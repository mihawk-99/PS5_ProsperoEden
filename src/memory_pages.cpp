// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <csignal>
#include <mutex>
#include <signal.h>
#include <vector>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

#ifdef PS5_NATIVE
// Keep CPU heaps/tables and JIT views out of RADV's high-word-2 GPU window.
// This is a non-fixed hint: the kernel retains ownership of collision handling.
constexpr std::uintptr_t cpu_mapping_hint = 0x1000000000ull;
static bool cpu_mapping_address(void* base) {
    return reinterpret_cast<std::uintptr_t>(base) >= 0x300000000ull && base != MAP_FAILED;
}
extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t,
                                         std::size_t, int, std::int64_t*);
std::int32_t sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
std::int32_t sceKernelEnableDmemAliasing();
int sceKernelDebugOutText(int, const char*);
std::int32_t sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
}
#endif

namespace Common {
namespace {
// The header sits in the page before the data. Blocks of at least LargePage start their data
// one LargePage in, with the address and direct memory LargePage-aligned, so the kernel can map
// them with 2 MiB pages: the guest backing, JIT caches and page tables are walked on every
// guest memory access, and 16 KiB pages cover only a few MiB of TLB reach.
struct Header { std::int64_t physical; std::size_t total; std::size_t lead; };
constexpr std::size_t LargePage = 0x200000;
// dev-settings lazy_memory=off (read with large_pages): back the guest RAM and the page tables
// densely, as before, instead of on first touch (LazyReserve below).
bool lazy_memory_off = false;
// Development A/B: dev-settings large_pages=off keeps every block 16 KiB-aligned. The heap takes
// its blocks before the frontend parses the file, so read it here with plain system calls.
bool LargePagesEnabled() {
    static std::atomic<int> state{0}; // 0 unknown, 1 on, 2 off
    int value = state.load(std::memory_order_acquire);
    if (value == 0) {
        value = 1;
        char text[4096];
        // An elevated process has no /app0 (headless/storage_paths.h): the install folder then.
        int fd = open("/app0/dev-settings.txt", O_RDONLY);
        if (fd < 0) fd = open("/data/homebrew/PPSA99008/dev-settings.txt", O_RDONLY);
        if (fd >= 0) {
            const auto count = read(fd, text, sizeof(text) - 1);
            close(fd);
            text[count > 0 ? count : 0] = '\0';
            if (std::strstr(text, "large_pages=off")) value = 2;
            if (std::strstr(text, "lazy_memory=off")) lazy_memory_off = true;
        }
        state.store(value, std::memory_order_release);
    }
    return value == 1;
}
std::size_t lead_size(std::size_t size, std::size_t page) {
    return size >= LargePage && LargePagesEnabled() ? LargePage : page;
}
std::size_t allocation_size(std::size_t size, std::size_t page, std::size_t lead) {
    if (!size || page < sizeof(Header) || size > std::numeric_limits<std::size_t>::max() - 2 * lead)
        return 0;
    return (size + lead - 1) / lead * lead + lead;
}
const Header& header_of(const void* pointer, std::size_t page) {
    return *reinterpret_cast<const Header*>(static_cast<const std::uint8_t*>(pointer) - page);
}
}

// ponytail: PS5 storage is dense and zeroed, including the 1 GiB page table.
// Add sparse native backing only when this measured overhead needs reducing.
void* AllocateMemoryPages(std::size_t size) noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    const std::size_t lead = page > 0 ? lead_size(size, static_cast<std::size_t>(page)) : 0;
    const auto total = page > 0 ? allocation_size(size, page, lead) : 0;
    if (!total) { errno = EINVAL; return nullptr; }
    void* base = nullptr;
    std::int64_t physical = -1;
#ifdef PS5_NATIVE
    base = reinterpret_cast<void*>(cpu_mapping_hint);
    const auto limit = sceKernelGetDirectMemorySize();
    auto rc = sceKernelAllocateDirectMemory(0, limit, total, lead, 12, &physical);
    if (rc != 0) {
        std::fprintf(stderr, "Direct allocation failed: rc=%08x bytes=%zu limit=%lld\n",
                     unsigned(rc), total, static_cast<long long>(limit));
        errno = ENOMEM;
        return nullptr;
    }
    rc = sceKernelMapDirectMemory(&base, total, PROT_READ | PROT_WRITE, 0, physical, lead);
    if (rc != 0 || !cpu_mapping_address(base)) {
        std::fprintf(stderr, "Direct mapping failed: rc=%08x bytes=%zu\n", unsigned(rc), total);
        if (rc == 0 && base && base != MAP_FAILED && munmap(base, total) != 0) std::abort();
        if (sceKernelReleaseDirectMemory(physical, total) != 0) std::abort();
        errno = ENOMEM;
        return nullptr;
    }
    if (lead == LargePage) {
        // The heap allocates through here: no stdio (it may allocate).
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_LARGE_ALLOC bytes=%zu va=%p pa=%llx\n",
                                         total, base, static_cast<unsigned long long>(physical));
        if (length > 0) (void)!write(2, line, static_cast<std::size_t>(length));
    }
    std::memset(base, 0, total);
#else
    base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;
#endif
    auto* data = static_cast<std::uint8_t*>(base) + lead;
    new (data - page) Header{physical, total, lead};
    return data;
}

#ifdef PS5_NATIVE
// Direct-memory start of an AllocateMemoryPages block's first data byte.
std::int64_t DirectMemoryStart(const void* pointer) noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    if (!pointer || page <= 0) std::abort();
    const auto& header = header_of(pointer, page);
    return header.physical + static_cast<std::int64_t>(header.lead);
}

// A second view of our own direct allocation; ownership stays with pointer.
void* MapExecutableAlias(void* pointer, std::size_t size) noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    if (!pointer || page <= 0) return nullptr;
    const auto header = header_of(pointer, page);
    if (header.total != allocation_size(size, page, header.lead)) std::abort();
    // This wrapper reports zero unconditionally; the mapping is the actual check.
    static const auto enabled = sceKernelEnableDmemAliasing();
    (void)enabled;
    const auto span = header.total - header.lead;
    void* alias = reinterpret_cast<void*>(cpu_mapping_hint);
    const auto rc = sceKernelMapDirectMemory(&alias, span, PROT_READ, 0,
                                              header.physical + static_cast<std::int64_t>(header.lead),
                                              header.lead);
    if (rc != 0 || !cpu_mapping_address(alias)) {
        std::printf("EDEN_JIT_ALIAS_MAP rc=%08x bytes=%zu\n", unsigned(rc), span);
        if (rc == 0 && alias && alias != MAP_FAILED && munmap(alias, span) != 0) std::abort();
        errno = rc ? unsigned(rc) & 0xffff : ENOMEM;
        return nullptr;
    }
    // Direct mapping wrappers reject EXEC on some versions. Use the same
    // checked mprotect contract already qualified for this owned memory type.
    if (mprotect(alias, span, PROT_READ | PROT_EXEC) != 0) {
        const auto error = errno;
        if (munmap(alias, span) != 0) std::abort();
        errno = error;
        return nullptr;
    }
    return alias;
}
#endif

void FreeMemoryPages(void* pointer, std::size_t size) noexcept {
    if (!pointer) return;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) std::abort();
    const auto header = header_of(pointer, page);
    auto* base = static_cast<std::uint8_t*>(pointer) - header.lead;
    if (header.total != allocation_size(size, page, header.lead) || munmap(base, header.total) != 0) std::abort();
#ifdef PS5_NATIVE
    if (sceKernelReleaseDirectMemory(header.physical, header.total) != 0) std::abort();
#endif
}
// Xbyak's allocator interface supplies only the pointer at release. The owned
// header already records the exact mapped size; retain the same checked free.
void FreeMemoryPages(void* pointer) noexcept {
    if (!pointer) return;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) std::abort();
    const auto header = header_of(pointer, page);
    if (header.total <= header.lead) std::abort();
    FreeMemoryPages(pointer, header.total - header.lead);
}

// Memory backed on first touch: the guest RAM (4 GiB) and the page tables (1 GiB for a 64-bit
// game) held the pool the GPU draws from although games touch a fraction of them. A lazy range
// is reserved address space; the first access to a chunk faults, and the fault handler gives the
// chunk its own direct memory, zeroed before it is mapped (through a scratch address used once,
// so the memory is never visible at two addresses and never replaces a live mapping), then the
// access repeats. Code that writes commits first (CommitSparsePage, ClearBackingRegion).
// An earlier design mapped one zero block read-only across a table and replaced parts of it with
// MAP_FIXED; the console powered off during a run with it (docs/MEMORY_FINDINGS.md).
#ifdef PS5_NATIVE
namespace {
struct LazyRange {
    std::atomic<std::uintptr_t> base{0};
    std::size_t size = 0;
    std::size_t chunk = 0;
    std::atomic<std::int64_t>* physical = nullptr; // per chunk; -1 until backed
};
LazyRange lazy_ranges[8];
std::atomic_flag lazy_lock = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> lazy_committed{0};
// Scratch addresses, each used once to zero a chunk and then released.
std::uint8_t* lazy_scratch = nullptr;
std::size_t lazy_scratch_left = 0;
struct sigaction lazy_previous_segv{}, lazy_previous_bus{};

struct LazyGuard {
    LazyGuard() { while (lazy_lock.test_and_set(std::memory_order_acquire)) {} }
    ~LazyGuard() { lazy_lock.clear(std::memory_order_release); }
};

LazyRange* FindLazy(std::uintptr_t address) {
    for (auto& range : lazy_ranges) {
        const auto base = range.base.load(std::memory_order_acquire);
        if (base && address >= base && address - base < range.size) return &range;
    }
    return nullptr;
}

// Under lazy_lock. Gives chunk `index` of `range` zeroed direct memory. false on failure.
bool BackChunk(LazyRange& range, std::size_t index) {
    if (range.physical[index].load(std::memory_order_acquire) >= 0) return true;
    const std::size_t chunk = range.chunk;
    if (lazy_scratch_left < chunk) {
        constexpr std::size_t ScratchBytes = std::size_t{16} << 30; // address space only
        void* scratch = reinterpret_cast<void*>(cpu_mapping_hint);
        if (sceKernelReserveVirtualRange(&scratch, ScratchBytes, 0, LargePage) != 0 || !cpu_mapping_address(scratch))
            return false;
        lazy_scratch = static_cast<std::uint8_t*>(scratch);
        lazy_scratch_left = ScratchBytes;
    }
    std::int64_t physical = -1;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), chunk, chunk, 12, &physical) != 0) return false;
    void* zeroing = lazy_scratch;
    lazy_scratch += chunk;
    lazy_scratch_left -= chunk;
    if (sceKernelMapDirectMemory(&zeroing, chunk, PROT_READ | PROT_WRITE, MAP_FIXED, physical, chunk) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, chunk);
        return false;
    }
    std::memset(zeroing, 0, chunk);
    munmap(zeroing, chunk);
    void* at = reinterpret_cast<void*>(range.base.load(std::memory_order_relaxed) + index * chunk);
    if (sceKernelMapDirectMemory(&at, chunk, PROT_READ | PROT_WRITE, MAP_FIXED, physical, chunk) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, chunk);
        return false;
    }
    range.physical[index].store(physical, std::memory_order_release);
    lazy_committed.fetch_add(chunk, std::memory_order_relaxed);
    return true;
}

bool CommitLazy(std::uintptr_t address) {
    LazyRange* range = FindLazy(address);
    if (!range) return false;
    const std::size_t index = (address - range->base.load(std::memory_order_relaxed)) / range->chunk;
    if (range->physical[index].load(std::memory_order_acquire) >= 0) return true;
    LazyGuard guard;
    return BackChunk(*range, index);
}

void ForwardFault(int signal, siginfo_t* info, void* context) {
    const struct sigaction& previous = signal == SIGSEGV ? lazy_previous_segv : lazy_previous_bus;
    if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, context);
    } else if (previous.sa_handler == SIG_DFL) {
        std::signal(signal, SIG_DFL); // the access repeats and terminates as before
    } else if (previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
    }
}

void HandleFault(int signal, siginfo_t* info, void* context) {
    const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
    if (FindLazy(address)) {
        if (CommitLazy(address)) return; // the access repeats on the new memory
        static const char failed[] = "EDEN_LAZY_MEMORY commit failed (direct memory exhausted)\n";
        (void)!write(2, failed, sizeof(failed) - 1);
        (void)sceKernelDebugOutText(0, failed);
    }
    ForwardFault(signal, info, context);
}

bool InstallFaultHandler() {
    static const bool installed = [] {
        struct sigaction action{};
        action.sa_sigaction = HandleFault;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        return sigaction(SIGSEGV, &action, &lazy_previous_segv) == 0 &&
               sigaction(SIGBUS, &action, &lazy_previous_bus) == 0;
    }();
    return installed;
}
} // namespace
#endif

void* LazyReserve(std::size_t size, std::size_t chunk) noexcept {
#ifdef PS5_NATIVE
    (void)LargePagesEnabled(); // reads dev-settings.txt once
    if (lazy_memory_off || !InstallFaultHandler()) return nullptr;
    const std::size_t total = (size + chunk - 1) / chunk * chunk;
    void* base = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelReserveVirtualRange(&base, total, 0, chunk < LargePage ? LargePage : chunk) != 0) return nullptr;
    if (!cpu_mapping_address(base)) {
        munmap(base, total);
        return nullptr;
    }
    auto* physical = new (std::nothrow) std::atomic<std::int64_t>[total / chunk];
    if (!physical) {
        munmap(base, total);
        return nullptr;
    }
    for (std::size_t i = 0; i < total / chunk; ++i) physical[i].store(-1, std::memory_order_relaxed);
    LazyGuard guard;
    for (auto& range : lazy_ranges) {
        if (range.base.load(std::memory_order_relaxed)) continue;
        range.size = total;
        range.chunk = chunk;
        range.physical = physical;
        range.base.store(reinterpret_cast<std::uintptr_t>(base), std::memory_order_release);
        char line[96];
        const int length = std::snprintf(line, sizeof(line), "EDEN_LAZY_MEMORY reserve bytes=%zu chunk=%zu va=%p\n",
                                         total, chunk, base);
        if (length > 0) (void)sceKernelDebugOutText(0, line);
        return base;
    }
    delete[] physical;
    munmap(base, total);
#else
    (void)size;
    (void)chunk;
#endif
    return nullptr;
}

bool LazyOwns(const void* pointer) noexcept {
#ifdef PS5_NATIVE
    return FindLazy(reinterpret_cast<std::uintptr_t>(pointer)) != nullptr;
#else
    (void)pointer;
    return false;
#endif
}

bool LazyBacked(const void* pointer) noexcept {
#ifdef PS5_NATIVE
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const LazyRange* range = FindLazy(address);
    return !range || range->physical[(address - range->base.load(std::memory_order_relaxed)) / range->chunk]
                             .load(std::memory_order_acquire) >= 0;
#else
    (void)pointer;
    return true;
#endif
}

bool LazyCommit(const void* pointer) noexcept {
#ifdef PS5_NATIVE
    return CommitLazy(reinterpret_cast<std::uintptr_t>(pointer));
#else
    (void)pointer;
    return true;
#endif
}

std::uint64_t LazyCommittedBytes() noexcept {
#ifdef PS5_NATIVE
    return lazy_committed.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

void LazyRelease(void* pointer) noexcept {
#ifdef PS5_NATIVE
    LazyGuard guard;
    LazyRange* range = FindLazy(reinterpret_cast<std::uintptr_t>(pointer));
    if (!range) return;
    const auto base = range->base.load(std::memory_order_relaxed);
    range->base.store(0, std::memory_order_release);
    if (munmap(reinterpret_cast<void*>(base), range->size) != 0) std::abort();
    for (std::size_t i = 0; i < range->size / range->chunk; ++i) {
        const auto physical = range->physical[i].load(std::memory_order_relaxed);
        if (physical < 0) continue;
        if (sceKernelReleaseDirectMemory(physical, range->chunk) != 0) std::abort();
        lazy_committed.fetch_sub(range->chunk, std::memory_order_relaxed);
    }
    delete[] range->physical;
    range->physical = nullptr;
    range->size = range->chunk = 0;
#else
    (void)pointer;
#endif
}

// SparseLargeVector (the page tables): lazy in 64 KiB chunks when large, else dense.
void* AllocateSparsePages(std::size_t size) noexcept {
    if (size >= (64u << 20))
        if (void* base = LazyReserve(size, 64u << 10)) return base;
    return AllocateMemoryPages(size);
}

void FreeSparsePages(void* pointer, std::size_t size) noexcept {
    if (!pointer) return;
    if (LazyOwns(pointer)) {
        LazyRelease(pointer);
        return;
    }
    FreeMemoryPages(pointer, size);
}

void CommitSparsePage(void* page) noexcept {
    if (LazyOwns(page)) {
        if (!LazyCommit(page)) {
            std::fprintf(stderr, "Page table commit failed at %p (direct memory exhausted)\n", page);
            std::abort();
        }
        return;
    }
#ifndef PS5_NATIVE
    mprotect(page, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE);
#endif
}
} // namespace Common

// For other fault handlers (headless/fastmem_handler.cpp): true when the address is lazy memory
// and now backed, so the access can repeat.
extern "C" bool eden_lazy_memory_fault(void* address) {
#ifdef PS5_NATIVE
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return Common::FindLazy(value) && Common::CommitLazy(value);
#else
    (void)address;
    return false;
#endif
}

extern "C" std::uint64_t eden_lazy_committed_bytes(void) {
    return Common::LazyCommittedBytes();
}
