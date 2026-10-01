# CPU performance: findings (2026-09-30, 2026-10-01)

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

## What the three routes gave (2026-10-01)

All runs: TotK 1.1.1 with the 60 FPS cheat, 4K, standing after loading a save, ended through
Eden's own shutdown (`tools/console-run.py`), never by closing the title.

### Thread placement: works now, small gain

The probe now falls back to measuring contention when CPUID cannot tell cores apart: a
thread on CPU 0 doing throughput-bound integer work runs alone, with CPU 1 busy, and with CPU 2
busy. On the console CPU 1 halves CPU 0's rate and CPU 2 leaves it alone
(`EDEN_WORKER_SMT_PROBE alone=147058 with_cpu1=74142 with_cpu2=147335`), so CPUs 2n and 2n+1
are siblings: guest cores 0-2 go to CPUs 0, 2, 4, core 3 to 6, the GPU thread to 8, other
emulator threads to 7 and 9-12. TotK measured 39.7-41.9 fps with it against 40.5 without:
within run-to-run spread.

### Fastmem for 64-bit games: built, correct, no gain

- The console reserves at most 256 GiB in one piece above RADV's range (user space ends at
  1 TiB), so the window covers 38 of the 39 guest address bits; the JIT bounds fastmem to it.
- Unchecked, fault-based fastmem (upstream dynarmic) faulted about a million times per boot:
  thread stacks and TLS are scattered 4 KiB pages that cannot be aliased at the kernel's 16 KiB
  granularity, so every new block touching them faulted once and was recompiled. The boot
  took longer than the whole run.
- Checked fastmem (the 32-bit design extended: one access byte per page of the window, a
  range test, then the direct access; about 7 instructions instead of 14, no faults) ran with
  zero faults, 86% of mapped pages direct, and 2 MiB mappings for 2.5 GiB of the window.
  Reads through the window cost the same as through the backing (12.5 ns per random read).
- TotK: 32.6-33.0 fps with it against 39.7 without. Core 0's work per frame dropped about 7%,
  but its frames got longer: it spent more time blocked in the kernel (13.8% of its samples
  against 5.8%), for a reason not yet found.

It stays a development option (`dev-settings fastmem=on`): it costs the guest RAM up front
(4 GiB instead of what the game touches) and has not paid off.

### Where TotK's frame goes

Each guest core is idle 15-25% of the time even when it shows 95% busy: about 13,000 short
waits per 5 s per core (`EDEN_DEV_GUEST idleN`), the game's threads waiting on each other. The
work that remains is the game's own code; removing most of the page-table lookup did not
shorten it, which points at memory latency rather than instruction count. The console runs
its CPU at 3.2 GHz in this mode (`SceSystemStateMgr` lines in klog).

So 60 fps in TotK's open world is not reachable through these three routes. What is left
would change behaviour or the system (a higher CPU clock, which needs a system setting, or
accuracy trade-offs, which are excluded).

## CPU clock (2026-10-01)

- `sceKernelSetCpumodeGame` accepts 0 and 1 only (others `0x80020016`); 1 is the default
  (system CPU mode 5), 0 gives system CPU mode 1. Neither changes the clock.
- `sceSystemServiceChangeCpuClock` returns success for every argument tried (3500, 3200, 0-3)
  and changes nothing measurable.
- The kernel chooses the clock profile when a title starts (klog `[BAPM]` lines). ProsperoEden,
  built against SDK version 2.00, gets `Gen2 UB High CPU Frequency` (`proc gen:2 ... ub:1`); a
  current retail game (SDK 4.00) gets `Default` (`proc gen:3 ... ub:0`). A dependent-addition
  loop measures about 3.75 GHz in ProsperoEden. The `SceSystemStateMgr` lines show 3200 for
  both: a nominal value. So the clock is already the high-frequency profile; going further
  would mean changing the processor's power state from the kernel, which is not done here.

## Where TotK's frame goes, by thread (2026-10-01)

`tools/console-run.py --trace-at` records 3 s of guest thread runs, supervisor calls and
core idle periods; `tools/analyze-trace.py` summarises them. TotK at 4K, in Hyrule:

- Six worker threads (priority 44, two pinned to each guest core) run 8-10.6 ms per 25 ms
  frame each; they hand work to each other with `WaitForAddress`/`SignalToAddress` and yield
  (`SleepThread(0)`) about 490 times per frame.
- Per guest core: guest code 74-80%, idle 16-23%, the emulator's kernel and scheduler 3.7-3.9%
  (about 2.4 us per thread switch). Waking a signalled thread takes 2.5 us at the median;
  the long tail is the game's design (a woken worker waits for the other worker on its core).
- Spinning 1 ms instead of 0.1 ms before a core sleeps changed nothing (39.8 against 39.5 fps).

So the frame rate follows the JIT's speed: the idle time is the workers waiting on each
other's work and shrinks with it.

## Where the guest code time goes (2026-10-01)

`tools/console-run.py --profile-at` samples cores 0-2 at 2 kHz for 8 s with every core's JIT
block table; `tools/analyze-profile.py` attributes the samples. TotK at 4K: 72.5% of samples in
translated code, 7.5% in the idle spin, 13.7% in kernel waits (mostly idle sleeping), 3.4%
in JIT stubs outside blocks, about 3% elsewhere in the emulator. The translated code is flat:
the hottest block has 0.9% of the samples, the top 30 blocks 7.7%, the hottest 25 pages of
guest code 18%. No small set of guest routines dominates, so replacing a few with host code
would not help much; a gain has to come from all translated code. JIT code already sits on
2 MiB pages.

## More cores for the game's threads: no gain (2026-10-01)

Experiment (`dev-settings spread_cores=on`): application threads allowed on guest core 0, 1 or
2 may also run on core 3, which the system leaves idle; only the scheduler's affinity is
widened (the game still sees cores 0-2, and `GetCurrentProcessorNumber` reports a thread's home
core on core 3). Core 3 then gets a game core's JIT cache, and the heap is reserved at 4095 MiB
(TotK's fourth JIT instance took it past 3 GiB; the heap commits on demand).

TotK at 4K: core 3 went from 99% idle to 63% busy running the six workers, the other cores'
idle time rose to 27-46%, and the frame stayed at 25.1 ms (39.6 fps). Each worker still runs
8-11 ms per frame. The frame is a chain of dependent jobs across the workers, not threads
queuing for a core: more cores leave it unchanged, and only faster execution of each job
shortens it. 60 fps needs that chain about 1.5 times faster.
