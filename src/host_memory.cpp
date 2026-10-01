// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/host_memory.h"
#include "fastmem.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef PS5_NATIVE
extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
std::int32_t sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
std::int32_t sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
std::int32_t sceKernelEnableDmemAliasing();
}
#endif

// Self-test probe: a load that may fault, and the fallback a fault is sent to.
extern "C" {
std::uint64_t eden_fastmem_probe_load(const void* address, std::uint64_t* rsp);
void eden_fastmem_probe_load_fault();
void eden_fastmem_probe_load_resume();
void eden_fastmem_probe_fallback();
}
asm(R"(
    .text
    .p2align 4
    .globl eden_fastmem_probe_load, eden_fastmem_probe_load_fault, eden_fastmem_probe_load_resume
eden_fastmem_probe_load:
    movq %rsp, (%rsi)
eden_fastmem_probe_load_fault:
    movq (%rdi), %rax
eden_fastmem_probe_load_resume:
    ret
    .p2align 4
    .globl eden_fastmem_probe_fallback
eden_fastmem_probe_fallback:
    movabsq $0x5a17ed5a17ed5a17, %rax
    ret
)");

namespace Common {
void* AllocateMemoryPages(size_t size) noexcept;
// Memory backed on first touch (src/memory_pages.cpp).
void* LazyReserve(std::size_t size, std::size_t chunk) noexcept;
bool LazyOwns(const void* pointer) noexcept;
bool LazyBacked(const void* pointer) noexcept;
bool LazyCommit(const void* pointer) noexcept;
void LazyRelease(void* pointer) noexcept;
void FreeMemoryPages(void* base, size_t size) noexcept;
#ifdef PS5_NATIVE
std::int64_t DirectMemoryStart(const void* pointer) noexcept;
#endif
} // namespace Common

namespace {
constexpr std::size_t GuestPage = 0x1000;
// The window covers as much of the 39-bit address space of 64-bit games as the console
// lets one reservation take (user space ends at 1 TiB, and RADV's PS5 winsys owns
// [256 GiB, 512 GiB)); addresses above it take the page-table path. 32-bit games use its
// first 4 GiB. One access byte per guest page of the window sits below it, which the
// checked JIT emitter tests before each direct access (headless/checked-fastmem.cmake).
constexpr std::size_t GuestPages = std::size_t{1} << 27; // the most pages a window can have
constexpr std::size_t Granule = 0x4000; // PS5 kernel page; host checks use the same groups
constexpr std::size_t PagesPerGranule = Granule / GuestPage;
// 2 MiB blocks aliased whole onto 2 MiB-aligned backing are mapped as one large mapping, so the
// kernel can use 2 MiB pages: 16 KiB pages cover only a few MiB of TLB reach, and the window
// is walked on every direct guest access.
constexpr std::size_t LargePage = 0x200000;
constexpr std::size_t ChunksPerLarge = LargePage / Granule;
std::size_t window_bits = 0; // fixed when the window is created
std::size_t WindowBytes = 0;
// One byte per guest page below the window: the JIT reads it at [r13 + page - MapBytes].
// A guard granule after the window stays reserved.
std::size_t MapPages = 0;
std::size_t MapBytes = 0;
std::size_t ReserveBytes = 0;
// Guest page state is kept per 256 MiB of address space, allocated when first mapped.
constexpr std::size_t LeafPages = std::size_t{1} << 16;
constexpr std::size_t LeafChunks = LeafPages / PagesPerGranule;
constexpr std::size_t Leaves = GuestPages / LeafPages;
constexpr std::uint8_t Readable = 1, Writable = 2; // Eden's requested page access
constexpr std::uint8_t ReadBlocked = 1, WriteBlocked = 2, Blocked = 3; // map byte bits
constexpr std::uint64_t ProbeMarker = 0x5a17ed5a17ed5a17ull;

std::atomic<bool> window_requested{false};
struct Counters {
    std::atomic<std::uint64_t> window, mapped_pages, aliased_chunks;
    std::atomic<std::uint64_t> map_calls, unmap_calls, protect_calls, kernel_calls, kernel_ns, failures;
    std::atomic<std::uint64_t> highest_mapped, outside_maps; // guest addresses Eden mapped
    std::atomic<std::uint64_t> large_blocks; // 2 MiB blocks mapped as one large mapping
} counters;
std::atomic<const std::uint8_t*> current_map{nullptr};

std::uint8_t Access(Common::MemoryPermission perms) {
    return (True(perms & Common::MemoryPermission::Read) ? Readable : 0) |
           (True(perms & Common::MemoryPermission::Write) ? Writable : 0);
}

[[noreturn]] void Fatal(const char* operation, std::size_t chunk, std::size_t count) {
    // A stale alias would expose freed memory to JIT code.
    std::printf("EDEN_FASTMEM_FATAL op=%s chunk=%zu count=%zu errno=%d\n", operation, chunk, count, errno);
    std::fflush(stdout);
    std::abort();
}

// Fault probe state; only the window self-test installs ProbeHandler.
struct Probe {
    std::uint64_t rsp;
    int signal;
    unsigned hits;
} probe;
void ProbeHandler(int signal, siginfo_t*, void* context) {
    auto& rip = Eden::Fastmem::ContextRip(context);
    if (rip != reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_load_fault)) {
        std::signal(signal, SIG_DFL); // Not a probe: repeat the fault with the default action.
        return;
    }
    // The same fake call dynarmic's handler makes for a fastmem fault.
    auto& rsp = Eden::Fastmem::ContextRsp(context);
    probe.rsp = rsp;
    probe.signal = signal;
    ++probe.hits;
    rsp -= sizeof(std::uint64_t);
    *reinterpret_cast<std::uint64_t*>(rsp) = reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_load_resume);
    rip = reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_fallback);
}

// Guest pages aliased at 16 KiB granularity into one reserved window: a chunk is aliased
// when its four guest pages map consecutive backing pages that start on a 16 KiB backing
// boundary; everything else stays reserved. Access control is the per-page map byte the
// JIT tests before each direct access, so unaliased pages (thread stacks and TLS are often
// scattered 4 KiB pages), GPU tracking and guest permissions take the page-table path
// without faults, recompilation or system calls.
class Window {
public:
    std::uint8_t* base{}; // window start (fastmem arena); the map occupies the MiB below
#ifdef PS5_NATIVE
    using Backing = std::int64_t; // direct-memory start of the backing
#else
    using Backing = int; // shared file descriptor of the backing
#endif

    ~Window() {
        if (!reservation) return;
        current_map = nullptr;
        if (munmap(reservation, ReserveBytes) != 0) Fatal("release", 0, 0);
#ifdef PS5_NATIVE
        if (map_physical >= 0 && sceKernelReleaseDirectMemory(map_physical, MapBytes) != 0) Fatal("map-release", 0, 0);
#endif
        counters.window = 0;
        counters.mapped_pages = 0;
        counters.aliased_chunks = 0;
        counters.large_blocks = 0;
    }

    bool Create(std::uint8_t* backing_base, Backing backing_handle) {
        backing = backing_handle;
#ifdef PS5_NATIVE
        // RADV's PS5 winsys owns [0x2_0000_0000, 0x3_0000_0000) (its 32-bit window) and places
        // device memory with fixed mappings in [0x40_0000_0000, 0x80_0000_0000); user space ends
        // at 0x100_0000_0000. The window goes above RADV's range, as wide as will fit.
        void* address = nullptr;
        for (std::size_t bits = 38; bits >= 32 && !reservation; --bits) {
            const std::size_t map_bytes = (std::size_t{1} << bits) / GuestPage;
            const std::size_t bytes = map_bytes + (std::size_t{1} << bits) + Granule;
            address = reinterpret_cast<void*>(0x8000000000ull);
            const auto reserved = sceKernelReserveVirtualRange(&address, bytes, 0, Granule);
            const auto placed = reinterpret_cast<std::uintptr_t>(address);
            if (reserved == 0 && placed >= 0x8000000000ull) {
                reservation = static_cast<std::uint8_t*>(address);
                window_bits = bits;
                WindowBytes = std::size_t{1} << bits;
                MapPages = map_bytes;
                MapBytes = map_bytes;
                ReserveBytes = bytes;
            } else {
                std::printf("EDEN_FASTMEM_WINDOW bits=%zu reserve=%08x address=%p\n", bits, unsigned(reserved), address);
                if (reserved == 0) munmap(address, bytes);
            }
        }
        if (!reservation) return false;
        (void)sceKernelEnableDmemAliasing(); // Reports zero regardless; the self-test checks.
        void* map_at = reservation;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), MapBytes, Granule, 12, &map_physical) != 0 ||
            sceKernelMapDirectMemory(&map_at, MapBytes, PROT_READ | PROT_WRITE, MAP_FIXED, map_physical, Granule) != 0 ||
            map_at != reservation) {
            std::printf("EDEN_FASTMEM_WINDOW map=failed\n");
            return Abandon();
        }
#else
        window_bits = 39;
        WindowBytes = std::size_t{1} << window_bits;
        MapPages = MapBytes = WindowBytes / GuestPage;
        ReserveBytes = MapBytes + WindowBytes + Granule;
        void* address = mmap(nullptr, ReserveBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (address == MAP_FAILED) return false;
        reservation = static_cast<std::uint8_t*>(address);
        if (mmap(reservation, MapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
            reservation)
            return Abandon();
#endif
        map = reservation;
        base = reservation + MapBytes;
        std::memset(map, Blocked, MapBytes);
        try {
            leaves.resize(Leaves);
        } catch (const std::bad_alloc&) {
            std::printf("EDEN_FASTMEM_WINDOW state=allocation\n");
            return Abandon();
        }
        if (const char* failure = SelfTest(backing_base)) {
            std::printf("EDEN_FASTMEM_CHECK ok=0 step=%s errno=%d\n", failure, errno);
            return Abandon();
        }
        counters.window = reinterpret_cast<std::uintptr_t>(base);
        current_map = map;
        return true;
    }

    void Map(std::size_t offset, std::size_t host_offset, std::size_t length, std::uint8_t access) {
        for (auto high = counters.highest_mapped.load(); offset + length > high &&
             !counters.highest_mapped.compare_exchange_weak(high, offset + length);) {}
        if (offset + length > WindowBytes) ++counters.outside_maps;
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.map_calls;
        const std::size_t host = host_offset / GuestPage;
        for (std::size_t p = first; p <= last; ++p) {
            Leaf& leaf = Make(p);
            auto& entry = leaf.backing[p % LeafPages];
            if (!entry) ++counters.mapped_pages;
            entry = static_cast<std::uint32_t>(host + (p - first) + 1);
            leaf.access[p % LeafPages] = access;
        }
        Update(first / PagesPerGranule, last / PagesPerGranule);
    }

    void Unmap(std::size_t offset, std::size_t length) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.unmap_calls;
        for (std::size_t p = first; p <= last; ++p) {
            Leaf* leaf = Find(p);
            if (!leaf) {
                p = (p / LeafPages + 1) * LeafPages - 1; // nothing mapped in this leaf
                continue;
            }
            auto& entry = leaf->backing[p % LeafPages];
            if (entry) --counters.mapped_pages;
            entry = 0;
            leaf->access[p % LeafPages] = 0;
        }
        Update(first / PagesPerGranule, last / PagesPerGranule);
    }

    void Protect(std::size_t offset, std::size_t length, std::uint8_t access) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.protect_calls;
        for (std::size_t p = first; p <= last; ++p) Make(p).access[p % LeafPages] = access;
        Refresh(first, last);
    }

private:
    struct Leaf {
        std::array<std::uint32_t, LeafPages> backing{}; // backing page + 1; 0 = unmapped
        std::array<std::uint8_t, LeafPages> access{};   // Readable | Writable as Eden requested
        std::array<std::uint32_t, LeafChunks> granule{}; // backing granule of an aliased chunk
        std::array<std::uint8_t, LeafChunks> aliased{};
        std::array<std::uint8_t, LeafChunks / ChunksPerLarge> large{};
    };

    std::mutex mutex;
    std::uint8_t* reservation{};
    std::uint8_t* map{};
#ifdef PS5_NATIVE
    std::int64_t map_physical = -1;
#endif
    Backing backing{};
    std::vector<std::unique_ptr<Leaf>> leaves;

    Leaf* Find(std::size_t page) const { return leaves[page / LeafPages].get(); }
    Leaf& Make(std::size_t page) {
        auto& leaf = leaves[page / LeafPages];
        if (!leaf) leaf = std::make_unique<Leaf>();
        return *leaf;
    }
    std::uint32_t BackingOf(std::size_t page) const {
        const Leaf* leaf = Find(page);
        return leaf ? leaf->backing[page % LeafPages] : 0;
    }
    std::uint8_t AccessOf(std::size_t page) const {
        const Leaf* leaf = Find(page);
        return leaf ? leaf->access[page % LeafPages] : 0;
    }
    bool IsAliased(std::size_t chunk) const {
        const Leaf* leaf = Find(chunk * PagesPerGranule);
        return leaf && leaf->aliased[chunk % LeafChunks];
    }

    bool Abandon() {
        munmap(reservation, ReserveBytes);
#ifdef PS5_NATIVE
        if (map_physical >= 0) sceKernelReleaseDirectMemory(map_physical, MapBytes);
        map_physical = -1;
#endif
        reservation = map = base = nullptr;
        return false;
    }

    // Inclusive guest page range of a request, clipped to the window.
    static bool Pages(std::size_t offset, std::size_t length, std::size_t& first, std::size_t& last) {
        if (offset >= WindowBytes || !length) return false;
        first = offset / GuestPage;
        last = (std::min(offset + length, WindowBytes) - 1) / GuestPage;
        return true;
    }

    bool Aliasable(std::size_t chunk, std::uint32_t& granule) const {
        const std::size_t page = chunk * PagesPerGranule;
        const Leaf* leaf = Find(page);
        if (!leaf) return false;
        const std::size_t at = page % LeafPages;
        const std::uint32_t entry = leaf->backing[at];
        if (!entry || (entry - 1) % PagesPerGranule) return false;
        for (std::size_t i = 1; i < PagesPerGranule; ++i)
            if (leaf->backing[at + i] != entry + i) return false;
        granule = (entry - 1) / PagesPerGranule;
        return true;
    }

    // A32 map bytes: blocked bits of a page on its own; pages past the map are blocked.
    std::uint8_t Own(std::size_t page) const {
        if (page >= MapPages || !IsAliased(page / PagesPerGranule)) return Blocked;
        const auto access = AccessOf(page);
        return ((access & Readable) ? 0 : ReadBlocked) | ((access & Writable) ? 0 : WriteBlocked);
    }
    // A page's byte also carries its successor's blocks: direct accesses may cross into it.
    void Refresh(std::size_t first, std::size_t last) {
        if (first >= MapPages) return;
        last = std::min(last, MapPages - 1);
        for (std::size_t page = first ? first - 1 : 0; page <= last; ++page)
            map[page] = Own(page) | Own(page + 1);
    }
    void Block(std::size_t chunk, std::size_t count) {
        const std::size_t first = chunk * PagesPerGranule;
        if (first >= MapPages) return;
        const std::size_t last = std::min((chunk + count) * PagesPerGranule, MapPages) - 1;
        for (std::size_t page = first ? first - 1 : 0; page <= last; ++page) map[page] = Blocked;
    }

    template<class Operation>
    bool Kernel(Operation&& operation) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = operation();
        counters.kernel_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        ++counters.kernel_calls;
        return ok;
    }
    // Maps a run of chunks onto consecutive backing granules: whole aligned 2 MiB blocks as
    // large mappings, the rest in 16 KiB granules.
    bool MapChunks(std::size_t chunk, std::size_t count, std::uint32_t granule) {
        while (count) {
            const bool aligned = (reinterpret_cast<std::uintptr_t>(base + chunk * Granule) % LargePage) == 0 &&
                                 granule % ChunksPerLarge == 0;
            std::size_t piece;
            if (aligned && count >= ChunksPerLarge) {
                piece = count / ChunksPerLarge * ChunksPerLarge;
                if (!MapPiece(chunk, piece, granule, LargePage)) return false;
                for (std::size_t c = chunk; c < chunk + piece; c += ChunksPerLarge) SetLarge(c, true);
            } else {
                const std::size_t to_boundary = ChunksPerLarge - chunk % ChunksPerLarge;
                piece = std::min(count, to_boundary);
                if (!MapPiece(chunk, piece, granule, Granule)) return false;
            }
            chunk += piece;
            granule += static_cast<std::uint32_t>(piece);
            count -= piece;
        }
        return true;
    }
    bool IsLarge(std::size_t chunk) const {
        const Leaf* leaf = Find(chunk * PagesPerGranule);
        return leaf && leaf->large[(chunk % LeafChunks) / ChunksPerLarge];
    }
    void SetLarge(std::size_t chunk, bool large) {
        auto& flag = Make(chunk * PagesPerGranule).large[(chunk % LeafChunks) / ChunksPerLarge];
        if (flag != large) large ? ++counters.large_blocks : --counters.large_blocks;
        flag = large;
    }
    bool MapPiece(std::size_t chunk, std::size_t count, std::uint32_t granule, std::size_t alignment) {
        auto* at = base + chunk * Granule;
        const std::size_t bytes = count * Granule;
        return Kernel([&] {
#ifdef PS5_NATIVE
            void* address = at;
            return sceKernelMapDirectMemory(&address, bytes, PROT_READ | PROT_WRITE, MAP_FIXED,
                                            backing + std::int64_t(granule) * std::int64_t(Granule),
                                            alignment) == 0 &&
                   address == at;
#else
            (void)alignment;
            return mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, backing,
                        off_t(granule) * off_t(Granule)) == at;
#endif
        });
    }
    bool UnmapChunks(std::size_t chunk, std::size_t count) {
        auto* at = base + chunk * Granule;
        const std::size_t bytes = count * Granule;
        return Kernel([&] {
#ifdef PS5_NATIVE
            if (munmap(at, bytes) != 0) return false;
            void* address = at;
            return sceKernelReserveVirtualRange(&address, bytes, MAP_FIXED, Granule) == 0 && address == at;
#else
            return mmap(at, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                        -1, 0) == at;
#endif
        });
    }
    void Withdraw(std::size_t run, std::size_t count) {
        Block(run, count);
        if (!UnmapChunks(run, count)) Fatal("unmap", run, count);
        for (std::size_t c = run; c < run + count; ++c) {
            Make(c * PagesPerGranule).aliased[c % LeafChunks] = 0;
            SetLarge(c, false);
        }
        counters.aliased_chunks -= count;
    }

    // A 2 MiB block that became fully aliased onto one aligned 2 MiB of backing in pieces is
    // remapped as one large mapping. Its accesses are blocked while it is remapped.
    void Promote(std::size_t first, std::size_t last) {
        if (reinterpret_cast<std::uintptr_t>(base) % LargePage) return;
        for (std::size_t block = first / ChunksPerLarge * ChunksPerLarge; block <= last; block += ChunksPerLarge) {
            if (IsLarge(block)) continue;
            const Leaf* leaf = Find(block * PagesPerGranule);
            if (!leaf) continue;
            const std::size_t at = block % LeafChunks;
            const std::uint32_t granule = leaf->granule[at];
            bool whole = granule % ChunksPerLarge == 0;
            for (std::size_t i = 0; whole && i < ChunksPerLarge; ++i)
                whole = leaf->aliased[at + i] && leaf->granule[at + i] == granule + i;
            if (!whole) continue;
            Block(block, ChunksPerLarge);
            if (!UnmapChunks(block, ChunksPerLarge)) Fatal("promote-unmap", block, ChunksPerLarge);
            if (!MapChunks(block, ChunksPerLarge, granule)) {
                // Left reserved: accesses keep the page-table path.
                ++counters.failures;
                for (std::size_t c = block; c < block + ChunksPerLarge; ++c) Make(c * PagesPerGranule).aliased[c % LeafChunks] = 0;
                counters.aliased_chunks -= ChunksPerLarge;
            }
            Refresh(block * PagesPerGranule, (block + ChunksPerLarge) * PagesPerGranule - 1);
        }
    }

    void Update(std::size_t first, std::size_t last) {
        // 1. Withdraw aliases that no longer match their pages; block their bytes first.
        std::size_t run = 0, count = 0;
        const auto withdraw = [&] {
            if (!count) return;
            Withdraw(run, count);
            count = 0;
        };
        for (std::size_t c = first; c <= last; ++c) {
            std::uint32_t granule = 0;
            const Leaf* leaf = Find(c * PagesPerGranule);
            if (leaf && leaf->aliased[c % LeafChunks] &&
                (!Aliasable(c, granule) || granule != leaf->granule[c % LeafChunks])) {
                if (count++ == 0) run = c;
            } else {
                withdraw();
            }
        }
        withdraw();

        // 2. Alias chunks that became aliasable, in runs of consecutive backing granules.
        std::uint32_t run_granule = 0;
        const auto alias = [&] {
            if (!count) return;
            if (MapChunks(run, count, run_granule)) {
                for (std::size_t i = 0; i < count; ++i) {
                    Leaf& leaf = Make((run + i) * PagesPerGranule);
                    leaf.aliased[(run + i) % LeafChunks] = 1;
                    leaf.granule[(run + i) % LeafChunks] = run_granule + static_cast<std::uint32_t>(i);
                }
                counters.aliased_chunks += count;
            } else {
                ++counters.failures; // Left reserved: accesses keep the page-table path.
            }
            count = 0;
        };
        for (std::size_t c = first; c <= last; ++c) {
            std::uint32_t granule = 0;
            const bool candidate = !IsAliased(c) && Aliasable(c, granule);
            if (candidate && count && granule == run_granule + count) {
                ++count;
                continue;
            }
            alias();
            if (candidate) {
                run = c;
                run_granule = granule;
                count = 1;
            }
        }
        alias();

        // 3. Whole 2 MiB blocks as large mappings, then publish access for every page of the
        //    updated chunks (and the page before).
        Promote(first, last);
        Refresh(first * PagesPerGranule, (last + 1) * PagesPerGranule - 1);
    }

    // Exercise the kernel operations the window relies on, with the fake call the JIT
    // fault handler makes, before any guest code runs. Returns the failing step.
    const char* SelfTest(std::uint8_t* backing_base) {
        struct sigaction action{}, previous_segv{}, previous_bus{};
        action.sa_sigaction = ProbeHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, &previous_segv) != 0) return "install";
        if (sigaction(SIGBUS, &action, &previous_bus) != 0) {
            sigaction(SIGSEGV, &previous_segv, nullptr);
            return "install";
        }
        const char* failure = SelfTestSteps(backing_base);
        sigaction(SIGBUS, &previous_bus, nullptr);
        sigaction(SIGSEGV, &previous_segv, nullptr);
        return failure;
    }
    const char* SelfTestSteps(std::uint8_t* backing_base) {
        constexpr std::uint64_t pattern = 0x0123456789abcdefull;
        auto* word = reinterpret_cast<volatile std::uint64_t*>(backing_base + Granule + 8);
        std::uint64_t rsp = 0;
        if (map[MapBytes - 1] != Blocked) return "map";
        // Reserved memory faults; the fake call returns after the faulting load.
        probe = {};
        if (eden_fastmem_probe_load(base, &rsp) != ProbeMarker || probe.hits != 1) return "reserved-fault";
        if (probe.rsp != rsp) return "context-rsp";
        const int signal = probe.signal;
        // Two chunks alias the first two backing granules.
        if (!MapChunks(0, 2, 0)) return "alias";
        *word = pattern;
        if (eden_fastmem_probe_load(base + Granule + 8, &rsp) != pattern || probe.hits != 1) return "alias-read";
        // The top of the window aliases too (the 39-bit range is reachable).
        const std::size_t top = WindowBytes / Granule - 1;
        if (!MapChunks(top, 1, 1)) return "alias-top";
        if (eden_fastmem_probe_load(base + top * Granule + 8, &rsp) != pattern || probe.hits != 1) return "top-read";
        if (!UnmapChunks(top, 1)) return "unmap-top";
        // A whole 2 MiB block maps as one large mapping and reads through it.
        if (reinterpret_cast<std::uintptr_t>(base) % LargePage == 0) {
            constexpr std::size_t block = 2 * ChunksPerLarge;
            if (!MapChunks(block, ChunksPerLarge, 0) || !IsLarge(block)) return "alias-large";
            if (eden_fastmem_probe_load(base + (block + 1) * Granule + 8, &rsp) != pattern || probe.hits != 1)
                return "large-read";
            if (!UnmapChunks(block, ChunksPerLarge)) return "unmap-large";
            SetLarge(block, false);
        }
#ifdef EDEN_FASTMEM_BENCH
        {
            // The same 64 MiB of backing read at random through its own mapping and the window.
            constexpr std::size_t chunks = (64u << 20) / Granule, first = 4 * ChunksPerLarge;
            if (MapChunks(first, chunks, 0)) {
                const auto time = [&](const std::uint8_t* region) {
                    std::uint64_t x = 88172645463325252ull, sum = 0;
                    const auto start = std::chrono::steady_clock::now();
                    for (unsigned i = 0; i < (1u << 22); ++i) {
                        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                        sum += *reinterpret_cast<const volatile std::uint64_t*>(region + (x % ((64u << 20) / 8)) * 8);
                    }
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                    return std::pair{double(ns) / (1u << 22), sum};
                };
                const auto own = time(backing_base), window = time(base + first * Granule);
                std::printf("EDEN_FASTMEM_BENCH backing_ns=%.2f window_ns=%.2f large=%d\n", own.first, window.first, IsLarge(first) ? 1 : 0);
                UnmapChunks(first, chunks);
                for (std::size_t c = first; c < first + chunks; c += ChunksPerLarge) SetLarge(c, false);
            }
        }
#endif
        // Unmapping one chunk re-reserves it and keeps its neighbour.
        if (!UnmapChunks(0, 1)) return "partial-unmap";
        if (eden_fastmem_probe_load(base + 8, &rsp) != ProbeMarker || probe.hits != 2) return "unmapped-fault";
        if (eden_fastmem_probe_load(base + Granule + 8, &rsp) != pattern || probe.hits != 2)
            return "neighbour-read";
        if (!UnmapChunks(1, 1)) return "unmap";
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 16; ++i) eden_fastmem_probe_load(base + Granule + 8, &rsp);
        const auto fault_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count() / 16;
        if (probe.hits != 18) return "final-fault";
        *word = 0;
        std::printf("EDEN_FASTMEM_CHECK ok=1 window=%p bits=%zu signal=%d fault_ns=%lld kernel_calls=%llu\n",
                    static_cast<void*>(base), window_bits, signal, static_cast<long long>(fault_ns),
                    static_cast<unsigned long long>(counters.kernel_calls.load()));
        counters.kernel_calls = 0;
        counters.kernel_ns = 0;
        return nullptr;
    }
};
} // namespace

// The JIT's fastmem bound for 64-bit code (core/arm/dynarmic/arm_dynarmic_64.cpp, derived)
// and the size of the access map below the window, once created.
extern "C" unsigned eden_fastmem_window_bits() { return static_cast<unsigned>(window_bits); }
extern "C" unsigned long long eden_fastmem_map_bytes() { return MapBytes; }

namespace Eden::Fastmem {
void Request(bool enabled) noexcept { window_requested = enabled; }
bool Requested() noexcept { return window_requested; }
Stats WindowStats() noexcept {
    std::uint64_t direct_reads = 0, direct_writes = 0;
    if (const auto* map = current_map.load()) {
        for (std::size_t page = 0; page < MapPages; ++page) {
            direct_reads += !(map[page] & ReadBlocked);
            direct_writes += !(map[page] & WriteBlocked);
        }
    }
    return {counters.window, counters.mapped_pages, counters.aliased_chunks, direct_reads, direct_writes,
            counters.map_calls, counters.unmap_calls, counters.protect_calls,
            counters.kernel_calls, counters.kernel_ns, counters.failures,
            counters.highest_mapped, counters.outside_maps, counters.large_blocks};
}
} // namespace Eden::Fastmem

namespace Common {
// ponytail: backing plus, on request, the A32 fastmem window above. NCE is unavailable.
class HostMemory::Impl {
public:
    u8* base{};
    size_t size;
    std::unique_ptr<Window> window;
#ifndef PS5_NATIVE
    int fd = -1; // Host checks: a shared backing the window can alias.
#endif
    Impl(size_t requested, bool fastmem) : size(requested) {
#ifndef PS5_NATIVE
        if (fastmem) {
            fd = memfd_create("eden-backing", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, off_t(requested)) != 0)
                throw std::system_error(errno, std::generic_category(), "Backing file");
            void* mapped = mmap(nullptr, requested, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mapped == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "Backing map");
            base = static_cast<u8*>(mapped);
        }
#endif
        // Without a fastmem window (which aliases the backing's one direct allocation), the
        // guest RAM is backed on first touch in 2 MiB chunks: games touch a fraction of the 4 GiB.
        if (!base && !fastmem) base = static_cast<u8*>(LazyReserve(requested, 2u << 20));
        if (!base) base = static_cast<u8*>(AllocateMemoryPages(requested));
        if (!base) throw std::system_error(errno, std::generic_category(), "Backing allocation");
        if (!fastmem) return;
        window = std::make_unique<Window>();
#ifdef PS5_NATIVE
        const Window::Backing handle = DirectMemoryStart(base);
#else
        const Window::Backing handle = fd;
#endif
        if (!window->Create(base, handle)) window.reset();
    }
    ~Impl() {
        window.reset();
#ifndef PS5_NATIVE
        if (fd >= 0) {
            if (munmap(base, size) != 0 || close(fd) != 0) std::abort();
            return;
        }
#endif
        if (LazyOwns(base)) LazyRelease(base);
        else FreeMemoryPages(base, size);
    }
};

HostMemory::HostMemory(size_t size, size_t virtual_size_)
    : backing_size(size),
      impl(std::make_unique<Impl>(size, virtual_size_ && Eden::Fastmem::Requested())),
      backing_base(impl->base) {
    if (impl->window) {
        virtual_base = impl->window->base;
        virtual_size = WindowBytes;
    }
}
HostMemory::~HostMemory() = default;
HostMemory::HostMemory(HostMemory&& other) noexcept { *this = std::move(other); }
HostMemory& HostMemory::operator=(HostMemory&& other) noexcept {
    if (this != &other) {
        impl = std::move(other.impl);
        backing_size = std::exchange(other.backing_size, 0);
        backing_base = std::exchange(other.backing_base, nullptr);
        virtual_base = std::exchange(other.virtual_base, nullptr);
        virtual_size = std::exchange(other.virtual_size, 0);
    }
    return *this;
}
void HostMemory::ClearBackingRegion(size_t offset, size_t length, u32 fill) {
    if (offset > backing_size || length > backing_size - offset)
        throw std::out_of_range("Backing clear range");
    if (!length) return;
    if (!LazyOwns(backing_base)) {
        std::memset(backing_base + offset, fill, length);
        return;
    }
    // Lazy backing: a chunk not backed yet is zero already, so a zero fill skips it rather than
    // backing the whole allocation; other fills (debug) back it first.
    constexpr size_t Chunk = 2u << 20;
    for (size_t at = offset; at < offset + length;) {
        const size_t end = std::min(offset + length, (at / Chunk + 1) * Chunk);
        u8* pointer = backing_base + at;
        if (fill != 0 && !LazyCommit(pointer)) throw std::bad_alloc{};
        if (LazyBacked(pointer)) std::memset(pointer, fill, end - at);
        at = end;
    }
}
// Eden calls these only while a page table uses VirtualBasePointer() as its fastmem
// arena. Without a window they are rejected rather than presented as no-ops.
void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool) {
    if (!impl || !impl->window) throw std::logic_error("Native aliases unavailable without a fastmem window");
    if (host_offset > backing_size || length > backing_size - host_offset)
        throw std::out_of_range("Backing map range");
    impl->window->Map(virtual_offset, host_offset, length, Access(perms));
}
void HostMemory::Unmap(size_t virtual_offset, size_t length, bool) {
    if (!impl || !impl->window) throw std::logic_error("Native aliases unavailable without a fastmem window");
    impl->window->Unmap(virtual_offset, length);
}
void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perms) {
    if (!impl || !impl->window) throw std::logic_error("Native protection unavailable without a fastmem window");
    impl->window->Protect(virtual_offset, length, Access(perms));
}
void HostMemory::EnableDirectMappedAddress() {
    throw std::logic_error("NCE unavailable in x64 backing-only adapter");
}
} // namespace Common
