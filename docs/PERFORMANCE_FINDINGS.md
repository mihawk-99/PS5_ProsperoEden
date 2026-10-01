# CPU performance: findings (2026-09-30)

Why The Legend of Zelda: Tears of the Kingdom (1.1.1, 60 FPS cheat) holds about 40 fps at 4K, and
what would take it to 60. Measured on my console with the development build: Hyrule after loading
a save, standing still, 4K docked.

## Where the frame goes

| | In game |
| --- | --- |
| Game frames | 40.5 per second, steady |
| GPU busy | 20% (`gpu_time=on`) |
| GPU thread | 33% busy, about 80% of its samples asleep in the kernel |
| Waiting for a free swapchain buffer | about 9% |
| Guest cores 0, 1, 2 | 95-97% busy each; JIT compilation about 0% |

The GPU has room to spare; the three guest cores do not. TotK is CPU bound here.

Core 0's host samples (PC sampling, `pc-sample.txt`): 81% in JIT code, 8% in
`PhysicalCore::Idle` (a guest core waiting for its next thread), 6% in system libraries (mostly
kernel waits), the rest spread thinly over HLE services and the scheduler.

## What the JIT spends its time on

The hottest A64 blocks were dumped (development hook on the A64 block emitter) and disassembled.
Every guest load and store compiles to about 14 x86 instructions: an alignment test, a range
test, a load from the page table, a tag test, a sign extension and mask of the entry, a null
test, then the access itself. That lookup is 45% of the instructions in the hottest blocks and
about 38% of the samples landing in them (few samples, so a wide margin).

The reason is that **fastmem covers only 32-bit guests** in this port (`src/fastmem.h`,
`headless/checked-fastmem.cmake`); every 64-bit game, TotK included, uses the page-table path for
every memory access. With fastmem an access is a single instruction against a mapped window.

## Thread placement

The topology probe never pins anything: CPUID's x2APIC ID reads 0 on every CPU on the console,
so `CheckWorkerTopology` sees one core (`EDEN_WORKER_TOPOLOGY ready=0 distinct_cores=1`). Forced
placement (development setting `worker_cpus=`) measured:

| Guest cores 0-3 and GPU thread on CPUs | fps |
| --- | --- |
| Unpinned | 40.5 |
| 0,1,2,3,4 (sharing physical cores) | 34.5 |
| 0,2,4,6,8 (one per physical core) | 41.4 |

So SMT siblings are CPUs 2n and 2n+1, sharing a core costs about 15%, and the scheduler mostly
avoids it on its own. Pinning by measured sibling pairs is worth about 2%.

## Routes to 60 fps, by expected gain

1. **A64 fastmem.** A window over the 39-bit guest address space with guest memory aliased into
   it, as the A32 window already does, and either the checked form (one byte per page, about 4
   instructions per access) or the fault-based form upstream dynarmic uses. Removing most of the
   lookup overhead should cut guest-core time by roughly 20-35%, which is most of the way from
   40 to 60 fps. Open questions: reserving 512 GiB of address space on the console, aliasing at
   the kernel's 16 KiB granularity, and backing the window on first touch together with the lazy
   guest RAM. Aliasing must be introduced with short, watched runs: the console powered off once
   with a design that aliased one block many times (MEMORY_FINDINGS.md).
2. **Pin workers by measured SMT pairs** instead of CPUID: about 2%.
3. **Host overhead on guest cores** (kernel waits, SVC dispatch, scheduler): a few percent at most.

Excluded: lowering CPU accuracy or other accuracy trade-offs, and emulated-CPU underclocking.
