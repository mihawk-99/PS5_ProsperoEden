// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
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
// dev-settings sparse_tables=on (read with large_pages): the sparse page-table backing is off by
// default. The console powered off (no crash record) at the end of a 3.5-minute 8K run with it
// on; until that is explained it is a development switch only.
bool sparse_tables_on = false;
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
            if (std::strstr(text, "sparse_tables=on")) sparse_tables_on = true;
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

// Sparse backing for SparseLargeVector (the page tables): a large vector's range reads as
// zeros from one shared 2 MiB block mapped read-only across it, and a 2 MiB chunk gets its own
// zeroed direct memory the first time Eden commits a page in it (every write commits first).
// Dense, a 64-bit game's page table alone held 1 GiB of the pool the GPU draws from.
#ifdef PS5_NATIVE
namespace {
constexpr std::size_t SparseChunk = 0x200000;
constexpr std::size_t SparseThreshold = 64u << 20; // smaller vectors stay dense
struct SparseRegion {
    std::uintptr_t base = 0;
    std::size_t size = 0;
    std::vector<std::int64_t> chunks; // direct memory of each committed chunk, -1 if none
};
std::mutex sparse_mutex;
SparseRegion sparse_regions[16];
std::int64_t sparse_zero = -1;

SparseRegion* FindSparseRegion(std::uintptr_t address) {
    for (auto& region : sparse_regions)
        if (region.size && address >= region.base && address - region.base < region.size) return &region;
    return nullptr;
}

// Zeroed direct memory, mapped nowhere yet (its zeroing view is released); -1 on failure.
std::int64_t ZeroedChunk() {
    std::int64_t physical = -1;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), SparseChunk, SparseChunk, 12, &physical) != 0)
        return -1;
    void* view = reinterpret_cast<void*>(cpu_mapping_hint);
    if (sceKernelMapDirectMemory(&view, SparseChunk, PROT_READ | PROT_WRITE, 0, physical, SparseChunk) != 0) {
        (void)sceKernelReleaseDirectMemory(physical, SparseChunk);
        return -1;
    }
    std::memset(view, 0, SparseChunk);
    munmap(view, SparseChunk);
    return physical;
}
}
#endif

void* AllocateSparsePages(std::size_t size) noexcept {
#ifdef PS5_NATIVE
    (void)LargePagesEnabled(); // reads dev-settings.txt once
    if (size >= SparseThreshold && sparse_tables_on) {
        const std::size_t total = (size + SparseChunk - 1) / SparseChunk * SparseChunk;
        std::lock_guard lock{sparse_mutex};
        static const auto aliasing = sceKernelEnableDmemAliasing();
        (void)aliasing;
        if (sparse_zero < 0) sparse_zero = ZeroedChunk();
        SparseRegion* slot = nullptr;
        for (auto& region : sparse_regions)
            if (!region.size) { slot = &region; break; }
        void* base = reinterpret_cast<void*>(cpu_mapping_hint);
        if (sparse_zero >= 0 && slot && sceKernelReserveVirtualRange(&base, total, 0, SparseChunk) == 0) {
            bool mapped = cpu_mapping_address(base);
            for (std::size_t offset = 0; mapped && offset < total; offset += SparseChunk) {
                void* at = static_cast<std::uint8_t*>(base) + offset;
                mapped = sceKernelMapDirectMemory(&at, SparseChunk, PROT_READ, MAP_FIXED, sparse_zero, SparseChunk) == 0;
            }
            if (mapped) {
                char line[96];
                const int length = std::snprintf(line, sizeof(line), "EDEN_SPARSE_VECTOR bytes=%zu va=%p\n", total, base);
                if (length > 0) (void)!write(2, line, static_cast<std::size_t>(length));
                (void)sceKernelDebugOutText(0, line);
                slot->base = reinterpret_cast<std::uintptr_t>(base);
                slot->size = total;
                slot->chunks.assign(total / SparseChunk, -1);
                return base;
            }
            munmap(base, total);
        }
        // No sparse backing: the qualified dense path.
    }
#endif
    return AllocateMemoryPages(size);
}

void FreeSparsePages(void* pointer, std::size_t size) noexcept {
    if (!pointer) return;
#ifdef PS5_NATIVE
    {
        std::lock_guard lock{sparse_mutex};
        if (auto* region = FindSparseRegion(reinterpret_cast<std::uintptr_t>(pointer))) {
            if (munmap(pointer, region->size) != 0) std::abort();
            for (const auto physical : region->chunks)
                if (physical >= 0 && sceKernelReleaseDirectMemory(physical, SparseChunk) != 0) std::abort();
            *region = SparseRegion{};
            return;
        }
    }
#endif
    FreeMemoryPages(pointer, size);
}

void CommitSparsePage(void* page) noexcept {
#ifdef PS5_NATIVE
    const auto address = reinterpret_cast<std::uintptr_t>(page);
    std::lock_guard lock{sparse_mutex};
    auto* region = FindSparseRegion(address);
    if (!region) return; // dense: already writable
    const std::size_t index = (address - region->base) / SparseChunk;
    if (region->chunks[index] >= 0) return;
    const auto physical = ZeroedChunk();
    void* at = reinterpret_cast<void*>(region->base + index * SparseChunk);
    // Replaces the zero view in one step: a reader sees zeros before and after.
    if (physical < 0 || sceKernelMapDirectMemory(&at, SparseChunk, PROT_READ | PROT_WRITE, MAP_FIXED, physical,
                                                 SparseChunk) != 0) {
        std::fprintf(stderr, "Sparse page table commit failed at %p\n", page);
        std::abort();
    }
    region->chunks[index] = physical;
#else
    mprotect(page, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE);
#endif
}
} // namespace Common
