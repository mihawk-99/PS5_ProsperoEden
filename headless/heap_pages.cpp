// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "common/sparse_large_vector.h"

#ifdef PS5_NATIVE
extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
std::int32_t sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
std::int32_t sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
}
#endif

// Reuse the qualified, owned direct-memory backend for the C mspace heap.
extern "C" void* eden_heap_pages(std::size_t size) {
    return Common::AllocateMemoryPages(size);
}
extern "C" void eden_heap_pages_free(void* base, std::size_t size) {
    Common::FreeMemoryPages(base, size);
}

// The heap's address range, reserved without memory behind it (heap_arenas.inc commits it a
// segment at a time as the heap grows, so the GPU keeps the direct memory the heap has not
// needed yet). 8 MiB aligned, so each 8 MiB arena slot lies in one segment.
extern "C" void* eden_heap_reserve(std::size_t size) {
    constexpr std::size_t Alignment = 8u << 20;
#ifdef PS5_NATIVE
    // Between RADV's 32-bit window and its device-memory region, as the other CPU mappings.
    void* address = reinterpret_cast<void*>(0x1000000000ull);
    if (sceKernelReserveVirtualRange(&address, size, 0, Alignment) != 0) return nullptr;
    const auto placed = reinterpret_cast<std::uintptr_t>(address);
    if (placed < 0x300000000ull || placed + size > 0x4000000000ull) {
        munmap(address, size);
        return nullptr;
    }
    return address;
#else
    void* address = mmap(nullptr, size + Alignment, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (address == MAP_FAILED) return nullptr;
    const auto aligned = (reinterpret_cast<std::uintptr_t>(address) + Alignment - 1) & ~(Alignment - 1);
    return reinterpret_cast<void*>(aligned);
#endif
}

// Direct memory for [at, at + size) of the reservation, zeroed (the console hands released
// direct memory out as it was). 2 MiB aligned for large pages. 0 on success.
extern "C" int eden_heap_commit(void* at, std::size_t size) {
#ifdef PS5_NATIVE
    constexpr std::size_t LargePage = 2u << 20;
    std::int64_t physical = -1;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, LargePage, 12, &physical) != 0)
        return -1;
    void* address = at;
    if (sceKernelMapDirectMemory(&address, size, PROT_READ | PROT_WRITE, MAP_FIXED, physical, LargePage) != 0 ||
        address != at) {
        (void)sceKernelReleaseDirectMemory(physical, size);
        return -1;
    }
#else
    if (mprotect(at, size, PROT_READ | PROT_WRITE) != 0) return -1;
#endif
    std::memset(at, 0, size);
    return 0;
}
