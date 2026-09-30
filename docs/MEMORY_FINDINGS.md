# Memory on the PS5: findings (2026-09-30)

What the GPU gets of the console's memory in ProsperoEden, why Super Mario Odyssey ran out of
it at 8K, and what changed. Measured on my console with the development build (`make dev`),
which prints an `EDEN_MEMORY` line at each startup stage, every 5 s beside the frame summary and
when rendering fails (free direct memory, largest free range, what the Vulkan heaps hold, the
heap's committed size).

## The pool

- The process has **12 GiB of direct memory** (`sceKernelGetDirectMemorySize`). RADV draws every
  `VkDeviceMemory` from it; the driver alone was measured using 11.625 GiB (PS5_Vulkan R88).
- RADV presents the pool as an integrated GPU's: an **8 GiB device-local heap** and a 4 GiB host
  heap. Eden sized its texture cache from the 8 GiB and budgeted 6 GiB.
- The emulator takes much of the same pool first. Before these changes (Super Mario Odyssey,
  1080p, MiB free of 12,288):

| Stage | Free | Taken by |
| --- | --- | --- |
| Process start | 9,208 | the C heap, **3 GiB committed up front** (Odyssey peaked at ~1 GiB of it) |
| Core initialised | 5,094 | guest RAM, 4 GiB |
| Game loaded | 2,877 | JIT code caches (0.69 GiB), the 64-bit page table (1 GiB, dense), code |
| In game | 2,043 | Vulkan 2,000 |

  So the GPU started with under 3 GiB. Odyssey at 8K after its opening cutscene took the Vulkan
  heaps to 5,804 MiB and left 69 MiB free when `CreateImage` failed with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` (Eden allocates images `VMA_ALLOCATION_CREATE_WITHIN_BUDGET`).

## Changes and what they measured

1. **The heap is committed on demand** (`headless/heap_arenas.inc`, `headless/heap_pages.cpp`):
   the 3 GiB address range is reserved, and direct memory backs it in segments (512 MiB, then
   256 MiB or the request), each its own mspace. Odyssey at 8K: the heap held 1.5 GiB instead of
   3 GiB; 2.5 GiB more free at startup. No failures in any run.
2. **The texture cache plans from what the pool can give** (`tools/prepare-vulkan-port.py`,
   `GpuMemoryLimit`): each tick its limits are recomputed from the Vulkan usage plus free direct
   memory, less 768 MiB, instead of the 8 GiB heap. At 8K the cache evicted as memory ran short
   (Vulkan usage moved between 4.5 and 8 GiB) instead of failing.
3. **Out of memory above 1080p restarts one step lower** (`headless/main.cpp`): the game gets a
   saved per-game limit (`games/<title>/resolution_limit`) and restarts; choosing a resolution
   again in Settings clears the limits. Not yet triggered on the console: with 1 and 2 the 8K run
   did not run out of memory.
4. **The page tables on sparse memory** (`src/memory_pages.cpp`, a derived
   `common/sparse_large_vector.h`): **off by default, `dev-settings sparse_tables=on`.** One
   zeroed 2 MiB block is mapped read-only across a table's range (reads of untouched entries see
   zeros) and each 2 MiB gets its own memory when Eden first commits a page in it. Odyssey
   committed 18 chunks (36 MiB) of its 1 GiB table.

With 1, 2 and 4 on, Odyssey at 8K played its first level for 150 s without running out of memory
(run s12-8k): 6,013 MiB free when the game started instead of 2,875, the level at 55-57 fps
(the GPU is ~55% busy at 8K; 4K holds 60 fps with it ~18% busy). Mario Kart 8 Deluxe at 4K
was unchanged (run m1-4k: 60 fps, the same slow windows as before).

## The console powered off

The next run, the same 8K sequence for 215 s with the sparse page tables on (run s14-8k), ended
with the console switching itself off. The last klog lines (16:20:04) are ordinary system-service
traffic: no crash record, fatal signal or GPU fault. A game's crash does not power a PS5 off, so
the system went down. Candidates, not yet separated:

- **The sparse page tables**: hundreds of read-only mappings of one direct-memory block, then
  `MAP_FIXED` replacements of them. Unusual kernel territory; a kernel panic writes nothing to
  klog. The main suspect, hence off by default.
- Closing the title at the end of the run (the runner kills it at its timeout), which tears all
  those mappings down together. The 150 s run was closed the same way without trouble.
- Heat after minutes of 8K rendering.

To separate them: short watched runs with `sparse_tables=on` that end without killing the title,
then the same with it off at 8K for as long as the run that powered off. If the aliasing is the
cause, back untouched entries without aliased mappings (for example a read fault handler that
commits, or one table level more).

## Mistakes worth remembering

- **ccache and generated header overrides.** ccache's direct mode does not see a new header
  that shadows an upstream one earlier in the include path, and returned objects built against
  the old class layout (heap corruption, `GraphicsPipeline` under-allocated). Every override now
  has its hash in the consumers' compile definitions (`EDEN_PORT_REVISION`,
  `EDEN_SPARSE_VECTOR_REVISION`).
- **A target's own `src/` include path comes before inherited ones**, so an override on
  `common` alone was not seen by `core`, `hid_core`, `audio_core` or the frontend; each puts
  `headless/recovery` first itself. Mixing the two layouts crashed Odyssey in `ZeroBlock`.
- **An elevated process has no `/app0`**, so development switches read before the frontend
  parses `dev-settings.txt` (`large_pages`, `sparse_tables`) also look in
  `/data/homebrew/PPSA99008`.
- **Mesa's disk cache is off when the real and effective ids differ**: elevation left the
  effective group at 1, so RADV never kept a shader cache. The frontend now matches them.
