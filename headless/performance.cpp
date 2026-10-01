// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance.h"
#include "../src/fastmem.h"
#include "common/cpu_features.h"
#include "common/sparse_large_vector.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <set>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cpuid.h>
#include <cstring>
#include <map>
#include <new>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>
#include <latch>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef EDEN_DEV_PROFILE
#include <signal.h>
#endif
#ifdef PS5_NATIVE
#include <sys/param.h>
#include <sys/cpuset.h>
#endif

// Spread cores experiment (src: headless/CMakeLists.txt, k_thread.cpp derivation).
extern "C" {
bool eden_spread_cores = false;
unsigned long long eden_spread_widened = 0; // thread affinity masks given core 3
}

namespace Eden::Performance {
namespace {
constexpr std::array names{"CPUCore_0", "CPUCore_1", "CPUCore_2", "CPUCore_3",
                           "GPU", "HostTiming", "VSyncThread"};
struct Worker {
    pthread_t thread{};
    clockid_t clock{};
    int clock_error = ENOENT, affinity_error = ENOENT, allowed = 0;
    unsigned long long mask = 0;
    long long wall_ns = 0, mono_ns = 0;
    bool registered = false;
};
std::mutex workers_mutex;
std::array<Worker, names.size()> workers;
#ifdef EDEN_DEV_PROFILE
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
std::array<uintptr_t, 8192> sampled_pcs{};
std::atomic<unsigned> pc_count{};
// Guest core 0 host PCs (JIT code, HLE, memory callbacks), sampled with the GPU thread.
std::array<uintptr_t, 8192> sampled_core_pcs{};
std::atomic<unsigned> core_pc_count{};
unsigned core_pc_reported{};
pthread_t core_sample_thread{};
std::atomic<bool> core_sample_ready{};
unsigned pc_reported{};
bool pc_sampling{};
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
// Same firmware6.02 register offsets qualified by the C46 caller sampler.
constexpr size_t runtime_rsp_offset = 248, runtime_rbp_offset = 136;
using CallerChain = std::array<uintptr_t, 8>;
std::array<CallerChain, 8192> sampled_callers{};

CallerChain CaptureCallerChain(uintptr_t rsp, uintptr_t frame, uintptr_t handler_stack) {
    CallerChain callers{};
    // Read only aligned frames in the interrupted stack-top page. No allocation,
    // symbolization or logging in the signal handler. Reject unknown/alternate stacks.
    if (rsp <= 0x10000 || rsp < handler_stack || rsp - handler_stack >= 65536 ||
        (rsp & 4095) > 4096 - 8 * sizeof(uintptr_t)) return callers;
    for (unsigned i = 0; i < callers.size(); ++i) {
        if (frame < rsp || (frame >> 12) != (rsp >> 12) ||
            (frame & (alignof(uintptr_t) - 1)) ||
            (frame & 4095) > 4096 - 2 * sizeof(uintptr_t)) break;
        const auto* chain = reinterpret_cast<const uintptr_t*>(frame);
        callers[i] = chain[1];
        if (chain[0] <= frame || chain[0] - frame > 4096) break;
        frame = chain[0];
    }
    return callers;
}
#endif
// Core 0's A32 blocks in emission order (P0): dynarmic's code cache grows linearly until a full
// clear, so entries stay sorted and a sampled host PC finds its guest block by binary search.
struct JitBlock {
    uintptr_t entry;
    unsigned long long size, location;
};
constexpr std::size_t jit_block_capacity = std::size_t{1} << 20;
JitBlock* core0_blocks = nullptr;
std::atomic<std::size_t> core0_block_count{0};
// Guest-code profile (profile-start.txt, needs pc-sample.txt): every guest core's blocks and
// host PCs sampled at a fixed rate on cores 0-2 for a few seconds.
constexpr std::size_t profile_block_capacity = std::size_t{1} << 19;
constexpr std::size_t profile_sample_capacity = std::size_t{1} << 16;
std::array<JitBlock*, 3> profile_blocks{};
std::array<std::atomic<std::size_t>, 3> profile_block_count{};
std::array<std::array<uintptr_t, profile_sample_capacity>, 3>* profile_samples = nullptr;
std::array<std::atomic<std::size_t>, 3> profile_sample_count{};
std::array<pthread_t, 3> profile_threads{};
std::array<std::atomic<bool>, 3> profile_thread_ready{};
std::atomic<bool> profile_active{false};

void PcSignal(int, siginfo_t*, void* context) {
    if (profile_active.load(std::memory_order_acquire) && profile_samples) {
        for (unsigned core = 0; core < 3; ++core) {
            if (!profile_thread_ready[core].load(std::memory_order_acquire) ||
                !pthread_equal(pthread_self(), profile_threads[core])) continue;
            const std::size_t index = profile_sample_count[core].load(std::memory_order_relaxed);
            if (index < profile_sample_capacity) {
                (*profile_samples)[core][index] = static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)];
                profile_sample_count[core].store(index + 1, std::memory_order_release);
            }
            return;
        }
    }
    if (core_sample_ready.load(std::memory_order_acquire) &&
        pthread_equal(pthread_self(), core_sample_thread)) {
        const unsigned core_index = core_pc_count.load(std::memory_order_relaxed);
        if (core_index < sampled_core_pcs.size()) {
            sampled_core_pcs[core_index] = static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)];
            core_pc_count.store(core_index + 1, std::memory_order_release);
        }
        return;
    }
    const unsigned index = pc_count.load(std::memory_order_relaxed);
    if (index >= sampled_pcs.size()) return;
    // Native firmware 6.02 ABI, qualified by the standalone PC-sampling oracle.
    // SDK FreeBSD ucontext.mc_rip points at the wrong field on this firmware.
    sampled_pcs[index] = static_cast<const uintptr_t*>(context)[224 / sizeof(uintptr_t)];
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    const auto* words = static_cast<const uintptr_t*>(context);
    char handler_stack_marker;
    sampled_callers[index] = CaptureCallerChain(words[runtime_rsp_offset / sizeof(uintptr_t)],
        words[runtime_rbp_offset / sizeof(uintptr_t)], reinterpret_cast<uintptr_t>(&handler_stack_marker));
#endif
    pc_count.store(index + 1, std::memory_order_release);
}
#endif
#ifdef PS5_NATIVE
std::array<unsigned, 5> worker_cpus{};
bool worker_topology_ready = false;
// Physical core (x2APIC above the SMT shift) of each allowed CPU, -1 if unknown.
std::array<int, 64> cpu_core = [] { std::array<int, 64> c{}; c.fill(-1); return c; }();
// CPUs outside guest cores 0-2 and their SMT siblings, and not the GPU thread CPU.
unsigned long long secondary_cpus = 0;
// The process CPU set seen by the topology probe (threads may inherit narrower sets).
cpuset_t topology_allowed{};
bool topology_allowed_valid = false;
std::atomic<bool> placement_secondary{false};
std::atomic<unsigned> secondary_reports{};
// Iterations of integer multiply chains on `cpu` in a fixed time, optionally while another
// thread runs the same chain on `other` (-1: alone). An SMT sibling shares the core's
// execution units, so it slows the first thread far more than a CPU on another core does.
unsigned long long ContendedWork(int cpu, int other) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, stop{false};
    std::atomic<unsigned long long> result{0};
    const auto run = [&](int on, bool measure) {
        cpuset_t one{};
        CPU_SET(on, &one);
        const bool pinned = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one) == 0;
        sched_yield(); // the new affinity applies when the thread is next scheduled
        ready.fetch_add(1);
        if (!pinned) return; // measures nothing: no SMT pairs are inferred
        while (!go.load(std::memory_order_acquire)) {}
        // Six independent chains keep the multiplier busy every cycle (throughput bound).
        unsigned long long v[6] = {1, 2, 3, 4, 5, 6}, iterations = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 64; ++i)
                for (auto& value : v) value = value * 6364136223846793005ULL + 1442695040888963407ULL;
            ++iterations;
        }
        if (measure) result.store(iterations + ((v[0] ^ v[1] ^ v[2] ^ v[3] ^ v[4] ^ v[5]) & 1));
    };
    std::thread first(run, cpu, true);
    std::thread second;
    if (other >= 0) second = std::thread(run, other, false);
    const int threads = other >= 0 ? 2 : 1;
    while (ready.load() < threads) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    stop.store(true);
    first.join();
    if (second.joinable()) second.join();
    return result.load();
}

// The measurement MeasuredSiblingPairs uses (host checks replace it).
unsigned long long (*contention_probe)(int cpu, int other) = ContendedWork;

// The console's CPUID reports x2APIC ID 0 on every CPU, so cores are told apart by contention:
// CPUs 2n and 2n+1 are taken as SMT siblings when CPU 1 slows CPU 0 and CPU 2 does not.
bool MeasuredSiblingPairs(const cpuset_t& allowed) {
    if (!CPU_ISSET(0, &allowed) || !CPU_ISSET(1, &allowed) || !CPU_ISSET(2, &allowed)) return false;
    unsigned long long alone = 0, with_1 = 0, with_2 = 0;
    for (int round = 0; round < 2; ++round) { // keep each case's best: other load only lowers it
        alone = std::max(alone, contention_probe(0, -1));
        with_1 = std::max(with_1, contention_probe(0, 1));
        with_2 = std::max(with_2, contention_probe(0, 2));
    }
    const bool pairs = alone && with_1 * 100 < alone * 85 && with_2 * 100 > alone * 92;
    std::printf("EDEN_WORKER_SMT_PROBE alone=%llu with_cpu1=%llu with_cpu2=%llu pairs=%d\n", alone, with_1, with_2, pairs);
    return pairs;
}

void CheckWorkerTopology() {
    cpuset_t original{};
    if (__get_cpuid_max(0, nullptr) < 0xb ||
        cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &original)) return;
    std::array<unsigned, 5> cores{};
    unsigned count = 0;
    bool changed = false;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &original)) continue;
        cpuset_t one{};
        CPU_SET(cpu, &one);
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one)) break;
        changed = true;
        unsigned a, b, c, d;
        __cpuid_count(0xb, 0, a, b, c, d);
        // Architectural SMT level: x2APIC ID above the SMT shift identifies
        // the physical core. Do not assume OS CPU numbering matches APIC IDs.
        if (!b || ((c >> 8) & 0xff) != 1 || (a & 31) >= 16) break;
        const unsigned core = d >> (a & 31);
        cpu_core[cpu] = static_cast<int>(core);
        if (count == cores.size() ||
            std::find(cores.begin(), cores.begin() + count, core) != cores.begin() + count) continue;
        cores[count] = core;
        worker_cpus[count++] = cpu;
    }
    if (changed) {
        cpuset_t restored{};
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &original) ||
            cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &restored) ||
            std::memcmp(&original, &restored, 8))
            throw std::runtime_error("Cannot restore topology-probe thread affinity");
    }
    if (count != cores.size() && MeasuredSiblingPairs(original)) {
        count = 0;
        for (unsigned cpu = 0; cpu < 64; ++cpu) {
            cpu_core[cpu] = CPU_ISSET(cpu, &original) ? static_cast<int>(cpu / 2) : -1;
            if (cpu_core[cpu] < 0 || cpu % 2 || count == cores.size()) continue;
            cores[count] = cpu / 2;
            worker_cpus[count++] = cpu;
        }
    }
    worker_topology_ready = count == cores.size();
    topology_allowed = original;
    topology_allowed_valid = true;
    secondary_cpus = 0;
    if (worker_topology_ready) {
        for (unsigned cpu = 0; cpu < 64; ++cpu) {
            if (!CPU_ISSET(cpu, &original) || cpu_core[cpu] < 0) continue;
            const bool guest_core = cpu_core[cpu] == static_cast<int>(cores[0]) ||
                cpu_core[cpu] == static_cast<int>(cores[1]) || cpu_core[cpu] == static_cast<int>(cores[2]);
            if (!guest_core && cpu != worker_cpus[3] && cpu != worker_cpus[4]) secondary_cpus |= 1ULL << cpu;
        }
        std::printf("EDEN_WORKER_SECONDARY mask=%llx\n", secondary_cpus);
    }
    std::printf("EDEN_WORKER_TOPOLOGY ready=%d distinct_cores=%u cpus=%u,%u,%u,%u,%u\n",
        worker_topology_ready, count, worker_cpus[0], worker_cpus[1], worker_cpus[2], worker_cpus[3], worker_cpus[4]);
}
void PlaceSecondary(const char* name) {
    if (!placement_secondary.load(std::memory_order_relaxed) || !secondary_cpus) return;
    cpuset_t mask{};
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if (secondary_cpus & (1ULL << cpu)) CPU_SET(cpu, &mask);
    const int result = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &mask);
    if (secondary_reports.fetch_add(1, std::memory_order_relaxed) < 64)
        std::printf("EDEN_WORKER_PLACED name=%s mask=%llx error=%d\n", name, secondary_cpus, result ? errno : 0);
}
void PinWorker(unsigned index, cpuset_t& affinity) {
    if (!worker_topology_ready || index >= worker_cpus.size() ||
        !CPU_ISSET(worker_cpus[index], topology_allowed_valid ? &topology_allowed : &affinity)) return;
    cpuset_t one{}, verified{};
    CPU_SET(worker_cpus[index], &one);
    if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &one)) {
        std::printf("EDEN_WORKER_PIN name=%s error=%d\n", names[index], errno);
        return;
    }
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &verified) ||
        std::memcmp(&one, &verified, 8)) {
        if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &affinity))
            throw std::runtime_error("Cannot restore worker affinity");
        return;
    }
    affinity = verified;
    std::printf("EDEN_WORKER_PIN name=%s cpu=%u verified=1\n", names[index], worker_cpus[index]);
}
#endif
bool cpu_clocks_valid = false;
bool owner_cpu_clock_valid = false;
std::atomic<unsigned> sample_epoch{};
struct CpuSample {
    unsigned epoch = ~0u;
    long long mono_ns = 0, cpu_ns = -ENOTSUP;
    unsigned long long thread = 0, pc = 0;
    unsigned svc = ~0u, fpcr = 0;
    unsigned long long compilations = 0, compile_ns = 0;
};
std::array<CpuSample, 4> cpu_samples;
// Read/written only by the worker owning this core; samples use workers_mutex.
std::array<unsigned, 4> sampled_epoch{~0u, ~0u, ~0u, ~0u};
clockid_t process_clock{};
int CurrentCpuClock(clockid_t* clock, bool process = false) {
#ifdef PS5_NATIVE
    // Native titles must use the imported kernel APIs. Firmware rejects direct
    // syscalls from the title. Qualification below rejects unusable CPU clocks.
    if (process) { *clock = CLOCK_PROCESS_CPUTIME_ID; return 0; }
    return pthread_getcpuclockid(pthread_self(), clock);
#else
    return process ? clock_getcpuclockid(0, clock) : pthread_getcpuclockid(pthread_self(), clock);
#endif
}
long long ClockNs(clockid_t clock) {
    timespec time{};
    if (clock_gettime(clock, &time) != 0) return -errno;
    return static_cast<long long>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
void CheckCpuClocks() {
    clockid_t worker_clock{};
    int worker_error = ENOENT;
    const int process_error = CurrentCpuClock(&process_clock, true);
    long long owner_begin = -1, owner_busy = -1, owner_idle = -1;
    std::binary_semaphore ready{0}, proceed{0};
    std::jthread worker([&] {
        worker_error = CurrentCpuClock(&worker_clock);
        ready.release();
        proceed.acquire();
        owner_begin = ClockNs(CLOCK_THREAD_CPUTIME_ID);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (std::chrono::steady_clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
        owner_busy = ClockNs(CLOCK_THREAD_CPUTIME_ID);
        ready.release();
        proceed.acquire();
        owner_idle = ClockNs(CLOCK_THREAD_CPUTIME_ID);
    });
    ready.acquire();
    const auto cpu_begin = worker_error ? -1 : ClockNs(worker_clock);
    proceed.release();
    ready.acquire();
    const auto cpu_busy = worker_error ? -1 : ClockNs(worker_clock);
    const auto process_idle = process_error ? -1 : ClockNs(process_clock);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto cpu_idle = worker_error ? -1 : ClockNs(worker_clock);
    const auto process_end = process_error ? -1 : ClockNs(process_clock);
    proceed.release();
    worker.join();
    const auto own_busy = owner_busy - owner_begin;
    const auto own_idle = owner_idle - owner_busy;
    owner_cpu_clock_valid = owner_begin >= 0 && own_busy >= 5'000'000 &&
        own_busy < 30'000'000 && own_idle >= 0 && own_idle < own_busy / 4 + 1'000'000;
    std::printf("EDEN_PERF_OWNER_CLOCK_CHECK valid=%d busy_ns=%lld idle_ns=%lld\n",
                owner_cpu_clock_valid, own_busy, own_idle);
    const auto busy = cpu_busy - cpu_begin;
    const auto idle = cpu_idle - cpu_busy;
    const auto process_sleep = process_end - process_idle;
    cpu_clocks_valid = !worker_error && !process_error && cpu_begin >= 0 && process_idle >= 0 &&
        busy >= 1'000'000 && idle >= 0 && idle < busy / 4 + 1'000'000 &&
        process_sleep >= 0 && process_sleep < 6'000'000;
    std::printf("EDEN_PERF_CPU_CLOCK_CHECK valid=%d worker_error=%d process_error=%d "
                "worker_clock=%d process_clock=%d busy_ns=%lld idle_ns=%lld process_sleep_ns=%lld\n",
                cpu_clocks_valid, worker_error, process_error, int(worker_clock), int(process_clock),
                busy, idle, process_sleep);
}
}

#ifdef PS5_NATIVE
// Development: pin guest cores 0-2, the GPU thread and core 3 to these CPUs (experiments).
void SetWorkerCpus(const std::array<unsigned, 5>& cpus) {
    worker_cpus = cpus;
    worker_topology_ready = true;
    std::printf("EDEN_WORKER_TOPOLOGY ready=1 forced=1 cpus=%u,%u,%u,%u,%u\n", cpus[0], cpus[1], cpus[2], cpus[3], cpus[4]);
}
#endif
void SetSecondaryPlacement(bool enabled) {
#ifdef PS5_NATIVE
    placement_secondary.store(enabled, std::memory_order_relaxed);
    if (!secondary_cpus || !topology_allowed_valid) return;
    cpuset_t mask{};
    if (enabled) {
        for (unsigned cpu = 0; cpu < 64; ++cpu)
            if (secondary_cpus & (1ULL << cpu)) CPU_SET(cpu, &mask);
    } else {
        mask = topology_allowed;
    }
    const int result = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, 8, &mask);
    std::printf("EDEN_WORKER_INHERIT enabled=%d mask=%llx error=%d\n", enabled,
                enabled ? secondary_cpus : 0ULL, result ? errno : 0);
#else
    (void)enabled;
#endif
}

void SampleGpuFrame(unsigned frame) {
    std::printf("EDEN_PERF_GPU_FRAME frame=%u mono_ns=%lld cpu_ns=%lld\n", frame,
                ClockNs(CLOCK_MONOTONIC),
                owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -static_cast<long long>(ENOTSUP));
}

extern "C" void ps5_opengl_heap_snapshot(const char* phase, unsigned iteration);
extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));
#ifdef PS5_NATIVE
extern "C" std::int64_t sceKernelGetDirectMemorySize();
extern "C" std::int32_t sceKernelAvailableDirectMemorySize(std::int64_t, std::int64_t, std::size_t, std::int64_t*,
                                                           std::size_t*);
#endif

extern "C" std::size_t eden_heap_committed_total(void) __attribute__((weak));
extern "C" std::uint64_t eden_lazy_committed_bytes(void) __attribute__((weak)); // src/memory_pages.cpp

namespace {
struct FreeMemory {
    std::uint64_t free_bytes = 0, largest_bytes = 0;
    unsigned ranges = 0;
};
[[maybe_unused]] FreeMemory ScanFreeMemory(std::int64_t total);
}

unsigned long long GpuMemoryLimit(unsigned long long vulkan_used) {
#ifdef PS5_NATIVE
    constexpr unsigned long long Headroom = 768ull << 20;
    static std::atomic<long long> last_ns{0};
    static std::atomic<unsigned long long> free_bytes{0};
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last_ns.load(std::memory_order_relaxed) >= 500'000'000) {
        last_ns.store(now, std::memory_order_relaxed);
        if (const std::int64_t total = sceKernelGetDirectMemorySize(); total > 0)
            free_bytes.store(ScanFreeMemory(total).free_bytes, std::memory_order_relaxed);
    }
    const unsigned long long reachable = vulkan_used + free_bytes.load(std::memory_order_relaxed);
    return reachable > Headroom ? reachable - Headroom : 1;
#else
    (void)vulkan_used;
    return 0;
#endif
}

namespace {
// The kernel answers with the largest free range in a span; the ranges either side of it
// are searched in turn (a bounded stack: the pool has at most a few hundred free ranges).
[[maybe_unused]] FreeMemory ScanFreeMemory(std::int64_t total) {
    FreeMemory result;
#ifdef PS5_NATIVE
    std::vector<std::pair<std::int64_t, std::int64_t>> spans{{0, total}};
    while (!spans.empty() && result.ranges < 4096) {
        const auto [low, high] = spans.back();
        spans.pop_back();
        std::int64_t start = 0;
        std::size_t size = 0;
        if (high - low < 0x4000 || sceKernelAvailableDirectMemorySize(low, high, 0x4000, &start, &size) != 0 ||
            size == 0)
            continue;
        ++result.ranges;
        result.free_bytes += size;
        result.largest_bytes = std::max<std::uint64_t>(result.largest_bytes, size);
        spans.emplace_back(low, start);
        spans.emplace_back(start + static_cast<std::int64_t>(size), high);
    }
#else
    (void)total;
#endif
    return result;
}
}

#ifdef PS5_NATIVE
// Read-only CPU state, with the prototypes of the payload SDK's hwinfo sample (and the CPU mode
// getter's public PS4 prototype): clock, mode and temperatures beside each memory report, so a
// run's log shows the console's thermal state (two runs ended with the console powering off).
extern "C" long sceKernelGetCpuFrequency(void);
extern "C" int sceKernelGetCpumode(void);
extern "C" int sceKernelGetCpuTemperature(int*);
extern "C" int sceKernelGetSocSensorTemperature(int, int*);
#endif

void ReportDirectMemory(const char* when) {
#ifdef PS5_NATIVE
    {
        int cpu_temperature = -1, soc_temperature = -1;
        if (sceKernelGetCpuTemperature(&cpu_temperature) != 0) cpu_temperature = -1;
        if (sceKernelGetSocSensorTemperature(0, &soc_temperature) != 0) soc_temperature = -1;
        std::printf("EDEN_CPU_STATE when=%s mhz=%ld cpumode=%d cpu_c=%d soc_c=%d\n", when,
                    sceKernelGetCpuFrequency() / 1000000, sceKernelGetCpumode(), cpu_temperature, soc_temperature);
    }
    const std::int64_t total = sceKernelGetDirectMemorySize();
    if (total <= 0) return;
    const auto [free_bytes, largest_bytes, ranges] = ScanFreeMemory(total);
    std::printf("EDEN_MEMORY when=%s pool_mib=%lld free_mib=%llu largest_free_mib=%llu free_ranges=%u vulkan_mib=%llu "
                "heap_mib=%llu lazy_mib=%llu\n",
                when, static_cast<long long>(total >> 20), static_cast<unsigned long long>(free_bytes >> 20),
                static_cast<unsigned long long>(largest_bytes >> 20), ranges,
                vulkan_memory_used.load(std::memory_order_relaxed) >> 20,
                eden_heap_committed_total ? static_cast<unsigned long long>(eden_heap_committed_total() >> 20) : 0ull,
                eden_lazy_committed_bytes ? static_cast<unsigned long long>(eden_lazy_committed_bytes() >> 20) : 0ull);
    std::fflush(stdout);
#else
    (void)when;
#endif
}

namespace {
std::mutex hle_mutex;
// Keyed by the service's name pointer (one per service object) and command id.
std::map<std::pair<const char*, unsigned>, std::pair<unsigned long long, unsigned long long>> hle_calls;
} // namespace

void RecordHle(const char* service, unsigned command, long long ns) {
    std::lock_guard lock(hle_mutex);
    auto& entry = hle_calls[{service, command}];
    ++entry.first;
    entry.second += static_cast<unsigned long long>(ns > 0 ? ns : 0);
}

void ReportGpuThread(unsigned frame) {
    // Heap growth and arena use over the run (allocation failures abort the title).
    ps5_opengl_heap_snapshot("vulkan_report", frame);
    std::printf("EDEN_PERF_HEAP arenas=%u\n", eden_heap_arenas_created ? eden_heap_arenas_created() : 1u);
    {
        // Cumulative HLE handling time of every service command that has cost at least 1 ms.
        std::lock_guard lock(hle_mutex);
        for (const auto& [key, value] : hle_calls)
            if (value.second >= 1'000'000)
                std::printf("EDEN_DEV_HLE service=%s cmd=%u calls=%llu ns=%llu\n", key.first ? key.first : "?",
                            key.second, value.first, value.second);
    }
#ifdef PS5_NATIVE
    // Direct memory headroom (guest RAM, JIT, RADV and the caches share the pool).
    if (const std::int64_t total = sceKernelGetDirectMemorySize(); total > 0) {
        std::int64_t start = 0;
        std::size_t largest = 0;
        if (sceKernelAvailableDirectMemorySize(0, total, 0x4000, &start, &largest) == 0)
            std::printf("EDEN_PERF_DIRECT total=%lld largest_free=%zu\n", static_cast<long long>(total), largest);
    }
#endif
    const auto load = [](const Totals& totals, bool calls) {
        return (calls ? totals.calls : totals.nanoseconds).load(std::memory_order_relaxed);
    };
    // Cumulative totals; the analyzer takes deltas between consecutive reports.
    std::printf("EDEN_DEV_GPU frame=%u mono_ns=%lld cpu_ns=%lld idle_ns=%llu dispatch_calls=%llu "
                "dispatch_ns=%llu drain_calls=%llu drain_ns=%llu present_calls=%llu present_ns=%llu "
                "full_calls=%llu full_ns=%llu draws=%llu\n", frame, ClockNs(CLOCK_MONOTONIC),
                owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -static_cast<long long>(ENOTSUP),
                load(gpu_queue_wait, false), load(gpu_dispatch, true), load(gpu_dispatch, false),
                load(gpu_fence_drain, true), load(gpu_fence_drain, false),
                load(gpu_present_wait, true), load(gpu_present_wait, false),
                load(gpu_queue_full, true), load(gpu_queue_full, false), load(rasterizer_draw, true));
    std::printf("EDEN_DEV_GUEST cpu_write_calls=%llu cpu_write_ns=%llu cpu_read_calls=%llu cpu_read_ns=%llu "
                "sync_calls=%llu sync_ns=%llu dequeue_calls=%llu dequeue_ns=%llu",
                load(guest_cpu_write, true), load(guest_cpu_write, false),
                load(guest_cpu_read, true), load(guest_cpu_read, false),
                load(guest_sync_wait, true), load(guest_sync_wait, false),
                load(guest_dequeue_wait, true), load(guest_dequeue_wait, false));
    std::printf(" ipc_calls=%llu ipc_ns=%llu", load(guest_ipc_wait, true), load(guest_ipc_wait, false));
    std::printf(" fs_file_calls=%llu fs_file_ns=%llu fs_file_bytes=%llu fs_storage_calls=%llu fs_storage_ns=%llu "
                "fs_storage_bytes=%llu",
                load(guest_fs_file, true), load(guest_fs_file, false),
                guest_fs_file_bytes.load(std::memory_order_relaxed),
                load(guest_fs_storage, true), load(guest_fs_storage, false),
                guest_fs_storage_bytes.load(std::memory_order_relaxed));
    for (unsigned core = 0; core < jit_callbacks.size(); ++core)
        std::printf(" r%u=%llu w%u=%llu x%u=%llu", core, jit_callbacks[core].reads.load(std::memory_order_relaxed),
                    core, jit_callbacks[core].writes.load(std::memory_order_relaxed),
                    core, jit_callbacks[core].exclusive_writes.load(std::memory_order_relaxed));
    for (unsigned core = 0; core < 3; ++core)
        std::printf(" idle%u=%llu/%llu/%llu", core, guest_idle[core].calls.load(std::memory_order_relaxed),
                    guest_idle[core].nanoseconds.load(std::memory_order_relaxed) / 1000000,
                    guest_idle[core].sleeps.load(std::memory_order_relaxed));
    std::printf(" cond=");
    for (unsigned slot = 0; slot < render_conditions.size(); ++slot)
        std::printf("%s%llu", slot ? "," : "", render_conditions[slot].load(std::memory_order_relaxed));
    std::printf("\n");
    if (eden_spread_cores) std::printf("EDEN_SPREAD widened=%llu\n", eden_spread_widened);
    const auto window = Eden::Fastmem::WindowStats();
#ifdef EDEN_DEV_PROFILE
    {
        // Where the latest fastmem faults were: data address relative to the window, by code site.
        static std::uint64_t pcs[1024], addresses[1024];
        const std::size_t count = Eden::Fastmem::FaultSamples(pcs, addresses, 1024);
        std::map<std::uint64_t, unsigned> sites;
        unsigned outside = 0;
        for (std::size_t i = 0; i < count; ++i) {
            ++sites[pcs[i]];
            outside += addresses[i] < window.window || addresses[i] - window.window >= (1ull << 38);
        }
        std::vector<std::pair<unsigned, std::uint64_t>> ranked;
        for (const auto& [pc, n] : sites) ranked.emplace_back(n, pc);
        std::sort(ranked.rbegin(), ranked.rend());
        std::printf("EDEN_FASTMEM_FAULTS samples=%zu sites=%zu outside_window=%u top=", count, sites.size(), outside);
        for (std::size_t i = 0; i < ranked.size() && i < 8; ++i) {
            std::uint64_t address = 0;
            for (std::size_t j = 0; j < count; ++j) if (pcs[j] == ranked[i].second) { address = addresses[j]; break; }
            std::printf("%s%llx:%u@%llx", i ? "," : "", static_cast<unsigned long long>(ranked[i].second), ranked[i].first,
                        static_cast<unsigned long long>(address - window.window));
        }
        std::printf("\n");
    }
#endif
    std::printf("EDEN_FASTMEM window=%llx pages=%llu chunks=%llu direct_reads=%llu direct_writes=%llu "
                "faults=%llu maps=%llu unmaps=%llu protects=%llu kernel_calls=%llu kernel_ns=%llu failures=%llu highest=%llx outside=%llu large=%llu\n",
                static_cast<unsigned long long>(window.window), static_cast<unsigned long long>(window.mapped_pages),
                static_cast<unsigned long long>(window.aliased_chunks),
                static_cast<unsigned long long>(window.direct_reads),
                static_cast<unsigned long long>(window.direct_writes),
                static_cast<unsigned long long>(Eden::Fastmem::Faults()),
                static_cast<unsigned long long>(window.map_calls), static_cast<unsigned long long>(window.unmap_calls),
                static_cast<unsigned long long>(window.protect_calls),
                static_cast<unsigned long long>(window.kernel_calls), static_cast<unsigned long long>(window.kernel_ns),
                static_cast<unsigned long long>(window.failures),
                static_cast<unsigned long long>(window.highest_mapped),
                static_cast<unsigned long long>(window.outside_maps),
                static_cast<unsigned long long>(window.large_blocks));
    // Guest cores record their owner CPU clocks at their next JIT exit.
    Snapshot();
}

void RegisterWorker(const char* name) {
    for (unsigned i = 0; i < names.size(); ++i) {
        if (std::strcmp(name, names[i])) continue;
        Worker worker;
        worker.thread = pthread_self();
#ifdef EDEN_DEV_PROFILE
        if (pc_sampling && (i <= 2 || i == 4 || i == pc_sample_core.load())) {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGUSR2);
            if (pthread_sigmask(SIG_UNBLOCK, &mask, nullptr))
                throw std::runtime_error("Cannot enable development PC sampler");
            if (i <= 2) {
                profile_threads[i] = pthread_self();
                profile_thread_ready[i].store(true, std::memory_order_release);
            }
            if (i == pc_sample_core.load()) {
                core_sample_thread = pthread_self();
                core_sample_ready.store(true, std::memory_order_release);
            }
        }
#endif
        worker.clock_error = CurrentCpuClock(&worker.clock);
        worker.wall_ns = Common::g_wall_clock.GetTimeNS().count();
        worker.mono_ns = ClockNs(CLOCK_MONOTONIC);
#ifdef PS5_NATIVE
        cpuset_t affinity{};
        const int result = cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
                                             sizeof(unsigned long long), &affinity);
#else
        cpu_set_t affinity{};
        const int result = sched_getaffinity(0, sizeof(affinity), &affinity);
#endif
#ifdef PS5_NATIVE
        if (result == 0) PinWorker(i, affinity);
#endif
        worker.affinity_error = result == 0 ? 0 : errno;
        if (!worker.affinity_error) {
            for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (!CPU_ISSET(cpu, &affinity)) continue;
                ++worker.allowed;
                if (cpu < 64) worker.mask |= 1ULL << cpu;
            }
        }
        worker.registered = true;
        std::lock_guard lock(workers_mutex);
        workers[i] = worker;
        return;
    }
#ifdef PS5_NATIVE
    PlaceSecondary(name);
#endif
}

void SampleCpu(unsigned core, unsigned long long thread, unsigned long long pc, unsigned svc, unsigned fpcr) {
    const auto epoch = sample_epoch.load(std::memory_order_relaxed);
    if (sampled_epoch.at(core) == epoch) return;
    sampled_epoch[core] = epoch;
    const CpuSample sample{epoch, ClockNs(CLOCK_MONOTONIC),
        owner_cpu_clock_valid ? ClockNs(CLOCK_THREAD_CPUTIME_ID) : -ENOTSUP, thread, pc, svc, fpcr,
        compilation[core].calls.load(std::memory_order_relaxed),
        compilation[core].nanoseconds.load(std::memory_order_relaxed)};
    std::lock_guard lock(workers_mutex);
    cpu_samples[core] = sample;
}

void Snapshot() {
    // Request a fresh owner-written sample; report the last completed sample with
    // its own timestamp. An idle core may remain stale; never infer zero CPU use.
    sample_epoch.fetch_add(1, std::memory_order_relaxed);
    const auto mono = ClockNs(CLOCK_MONOTONIC);
    std::printf("EDEN_PERF_SAMPLE mono_ns=%lld process_cpu_ns=%lld wall_ns=%lld\n",
                mono, cpu_clocks_valid ? ClockNs(process_clock) : -static_cast<long long>(ENOTSUP),
                static_cast<long long>(Common::g_wall_clock.GetTimeNS().count()));
    std::lock_guard lock(workers_mutex);
#ifdef EDEN_DEV_PROFILE
    std::map<uintptr_t, unsigned> counts;
    const unsigned end = pc_count.load(std::memory_order_acquire);
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::map<std::array<uintptr_t, 9>, unsigned> caller_counts;
#endif
    while (pc_reported < end) {
        const unsigned index = pc_reported++;
        ++counts[sampled_pcs[index]];
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
        std::array<uintptr_t, 9> key{sampled_pcs[index]};
        std::copy(sampled_callers[index].begin(), sampled_callers[index].end(), key.begin() + 1);
        ++caller_counts[key];
#endif
    }
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    for (const auto& [chain, count] : caller_counts) {
        std::printf("EDEN_PERF_NATIVE_CALLERS mono_ns=%lld pc=%llx count=%u callers=", mono,
                    static_cast<unsigned long long>(chain[0]), count);
        for (unsigned i = 1; i < chain.size(); ++i)
            std::printf("%s%llx", i == 1 ? "" : ",", static_cast<unsigned long long>(chain[i]));
        std::printf("\n");
    }
#endif
    for (const auto& [pc, count] : counts)
        std::printf("EDEN_PERF_NATIVE_PC mono_ns=%lld pc=%llx count=%u\n", mono,
                    static_cast<unsigned long long>(pc), count);
    std::map<uintptr_t, unsigned> core_counts;
    const unsigned core_end = core_pc_count.load(std::memory_order_acquire);
    while (core_pc_reported < core_end) ++core_counts[sampled_core_pcs[core_pc_reported++]];
    for (const auto& [pc, count] : core_counts)
        std::printf("EDEN_PERF_CORE_PC mono_ns=%lld pc=%llx count=%u\n", mono,
                    static_cast<unsigned long long>(pc), count);
    // The same samples by guest block: location (A32 PC in the low word), block offset, count.
    if (const std::size_t blocks = core0_blocks ? core0_block_count.load(std::memory_order_acquire) : 0) {
        std::map<std::pair<unsigned long long, unsigned long long>, unsigned> guest_counts;
        for (const auto& [pc, count] : core_counts) {
            const JitBlock* begin = core0_blocks;
            const JitBlock* it = std::upper_bound(begin, begin + blocks, pc,
                                                  [](uintptr_t value, const JitBlock& block) { return value < block.entry; });
            if (it == begin) continue;
            --it;
            if (pc - it->entry < it->size) guest_counts[{it->location, pc - it->entry}] += count;
        }
        for (const auto& [key, count] : guest_counts)
            std::printf("EDEN_PERF_GUEST_PC mono_ns=%lld location=%llx host_offset=%llx count=%u\n", mono,
                        key.first, key.second, count);
        // P1: guest and host code of this window's six hottest blocks not dumped before.
        std::map<unsigned long long, unsigned> block_counts;
        for (const auto& [key, count] : guest_counts) block_counts[key.first] += count;
        std::vector<std::pair<unsigned, unsigned long long>> ranked;
        for (const auto& [location, count] : block_counts) ranked.emplace_back(count, location);
        std::sort(ranked.rbegin(), ranked.rend());
        static std::set<unsigned long long> dumped;
        unsigned printed = 0;
        for (const auto& [count, location] : ranked) {
            if (printed == 6) break;
            if (!dumped.insert(location).second) continue;
            ++printed;
            const JitBlock* block = nullptr;
            for (std::size_t i = blocks; i-- > 0;)
                if (core0_blocks[i].location == location) { block = &core0_blocks[i]; break; }
            std::string guest;
            const unsigned long long pc = location & 0xffffffffull;
            for (unsigned long long at = pc; at < pc + 256 && guest_read32; at += 4) {
                unsigned word = 0;
                if (!guest_read32(at, word)) break;
                char text[12];
                std::snprintf(text, sizeof(text), "%08x", word);
                guest += text;
            }
            std::printf("EDEN_PERF_BLOCK_GUEST location=%llx count=%u words=%s\n", location, count, guest.c_str());
            if (block) {
                std::string host;
                const auto* bytes = reinterpret_cast<const unsigned char*>(block->entry);
                for (unsigned long long i = 0; i < block->size && i < 4096; ++i) {
                    char text[4];
                    std::snprintf(text, sizeof(text), "%02x", bytes[i]);
                    host += text;
                }
                std::printf("EDEN_PERF_BLOCK_HOST location=%llx entry=%llx size=%llu bytes=%s\n", location,
                            static_cast<unsigned long long>(block->entry), block->size, host.c_str());
            }
        }
    }
#endif
    for (unsigned i = 0; i < workers.size(); ++i) {
        const auto& worker = workers[i];
        sched_param priority{};
        int policy = -1;
        const int priority_error = worker.registered ?
            pthread_getschedparam(worker.thread, &policy, &priority) : ENOENT;
        const auto cpu = !cpu_clocks_valid ? -static_cast<long long>(ENOTSUP) :
                        worker.clock_error ? -static_cast<long long>(worker.clock_error) :
                                             ClockNs(worker.clock);
        std::printf("EDEN_PERF_WORKER mono_ns=%lld name=%s cpu_ns=%lld clock_id=%d affinity_error=%d "
                    "allowed=%d mask_low64=%llx priority_error=%d policy=%d priority=%d "
                    "registered_wall_ns=%lld registered_mono_ns=%lld\n",
                    mono, names[i], cpu, int(worker.clock), worker.affinity_error, worker.allowed, worker.mask,
                    priority_error, policy, priority.sched_priority, worker.wall_ns, worker.mono_ns);
        if (i < compilation.size()) {
            const auto& sample = cpu_samples[i];
            std::printf("EDEN_PERF_CPU_POINT core=%u phase=%u epoch=%u mono_ns=%lld cpu_ns=%lld thread=%llu pc=%llx svc=%x fpcr=%u compilations=%llu compile_ns=%llu\n",
                        i, unsigned(cpu_state[i].phase.load(std::memory_order_relaxed)), sample.epoch,
                        sample.mono_ns, sample.cpu_ns, sample.thread, sample.pc, sample.svc, sample.fpcr,
                        sample.compilations, sample.compile_ns);
            std::printf("EDEN_PERF_PROGRESS mono_ns=%lld core=%u compilations=%llu compile_ns=%llu evacuations=%llu\n",
                        mono, i, compilation[i].calls.load(std::memory_order_relaxed),
                        compilation[i].nanoseconds.load(std::memory_order_relaxed),
                        evacuations[i].load(std::memory_order_relaxed));
        }
    }
    std::fflush(stdout);
}

#ifndef EDEN_DEV_PROFILE
// Release builds: the JIT's block hook (derived a32_interface.cpp) resolves here and does nothing.
extern "C" void eden_jit_block(unsigned, unsigned long long, const void*, unsigned long long) {}
#endif

#ifdef EDEN_DEV_PROFILE
extern "C" void eden_jit_block(unsigned core, unsigned long long location, const void* entry,
                               unsigned long long size) {
    if (!pc_sampling) return;
    if (core < 3) {
        if (!profile_blocks[core]) profile_blocks[core] = new (std::nothrow) JitBlock[profile_block_capacity];
        if (profile_blocks[core]) {
            std::size_t n = profile_block_count[core].load(std::memory_order_relaxed);
            const auto at = reinterpret_cast<uintptr_t>(entry);
            if (n && at < profile_blocks[core][n - 1].entry) n = 0; // the cache was cleared
            if (n < profile_block_capacity) {
                profile_blocks[core][n] = JitBlock{at, size, location};
                profile_block_count[core].store(n + 1, std::memory_order_release);
            }
        }
    }
    if (core != 0) return;
    if (!core0_blocks) core0_blocks = new JitBlock[jit_block_capacity];
    std::size_t count = core0_block_count.load(std::memory_order_relaxed);
    const auto address = reinterpret_cast<uintptr_t>(entry);
    if (count && address < core0_blocks[count - 1].entry) count = 0; // the cache was cleared
    if (count >= jit_block_capacity) return;
    core0_blocks[count] = JitBlock{address, size, location};
    core0_block_count.store(count + 1, std::memory_order_release);
}

namespace {
constexpr std::size_t trace_capacity = std::size_t{1} << 21; // 64 MiB
constexpr long long trace_seconds = 3;
TraceEvent* trace_events = nullptr;
std::atomic<std::size_t> trace_count{0};
long long trace_end_ns = 0;
} // namespace

void TraceRecord(TraceType type, unsigned core, unsigned long long thread, unsigned svc,
                 unsigned long long a0, unsigned long long a1) {
    const std::size_t index = trace_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= trace_capacity) return;
    trace_events[index] = TraceEvent{NowNs(), static_cast<unsigned>(thread), static_cast<unsigned short>(core),
                                     static_cast<unsigned char>(type), static_cast<unsigned char>(svc), a0, a1};
}

namespace {
// Writes the profile: per core, the block table then the samples; then the guest instructions
// (up to 64 words) of the 400 blocks with the most samples over all cores.
void WriteProfile() {
    const std::string path = "/data/prosperoeden/logs/profile.bin";
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    std::map<unsigned long long, unsigned> hot;
    for (unsigned core = 0; core < 3; ++core) {
        const std::size_t blocks = profile_blocks[core] ? std::min(profile_block_count[core].load(), profile_block_capacity) : 0;
        const std::size_t samples = std::min(profile_sample_count[core].load(), profile_sample_capacity);
        const unsigned long long counts[2] = {blocks, samples};
        std::fwrite(counts, sizeof(counts), 1, file);
        if (blocks) std::fwrite(profile_blocks[core], sizeof(JitBlock), blocks, file);
        std::fwrite((*profile_samples)[core].data(), sizeof(uintptr_t), samples, file);
        const JitBlock* begin = profile_blocks[core];
        for (std::size_t i = 0; i < samples && blocks; ++i) {
            const uintptr_t pc = (*profile_samples)[core][i];
            const JitBlock* it = std::upper_bound(begin, begin + blocks, pc,
                [](uintptr_t value, const JitBlock& block) { return value < block.entry; });
            if (it != begin && pc - (it - 1)->entry < (it - 1)->size) ++hot[(it - 1)->location];
        }
    }
    std::vector<std::pair<unsigned, unsigned long long>> ranked;
    for (const auto& [location, count] : hot) ranked.emplace_back(count, location);
    std::sort(ranked.rbegin(), ranked.rend());
    const unsigned long long code_blocks = std::min<std::size_t>(ranked.size(), 400);
    std::fwrite(&code_blocks, sizeof(code_blocks), 1, file);
    for (std::size_t i = 0; i < code_blocks; ++i) {
        const unsigned long long pc = ranked[i].second & ((1ull << 39) - 1);
        unsigned words[64]{};
        for (unsigned w = 0; w < 64 && guest_read32; ++w)
            if (!guest_read32(pc + w * 4, words[w])) break;
        std::fwrite(&ranked[i].second, sizeof(unsigned long long), 1, file);
        std::fwrite(words, sizeof(words), 1, file);
    }
    std::fclose(file);
    chmod(path.c_str(), 0666);
    std::printf("EDEN_PROFILE_DONE samples=%zu,%zu,%zu blocks=%zu,%zu,%zu hot=%llu\n",
                profile_sample_count[0].load(), profile_sample_count[1].load(), profile_sample_count[2].load(),
                profile_block_count[0].load(), profile_block_count[1].load(), profile_block_count[2].load(), code_blocks);
    std::fflush(stdout);
}
} // namespace

void PollTrace() {
    if (pc_sampling && !profile_active.load() &&
        (access("/app0/profile-start.txt", F_OK) == 0 || access("/data/homebrew/PPSA99008/profile-start.txt", F_OK) == 0)) {
        unlink("/app0/profile-start.txt");
        unlink("/data/homebrew/PPSA99008/profile-start.txt");
        if (!profile_samples) profile_samples = new (std::nothrow) std::array<std::array<uintptr_t, profile_sample_capacity>, 3>;
        if (profile_samples) {
            for (auto& count : profile_sample_count) count.store(0);
            profile_active.store(true, std::memory_order_release);
            std::printf("EDEN_PROFILE_START\n");
            std::fflush(stdout);
            std::thread([] {
                // 2 kHz per core for 8 s (16,000 samples a core), then the dump.
                for (unsigned tick = 0; tick < 16000; ++tick) {
                    for (unsigned core = 0; core < 3; ++core)
                        if (profile_thread_ready[core].load(std::memory_order_acquire)) pthread_kill(profile_threads[core], SIGUSR2);
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
                profile_active.store(false, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                WriteProfile();
            }).detach();
        }
    }
    if (trace_active.load(std::memory_order_relaxed)) {
        if (NowNs() < trace_end_ns) return;
        trace_active.store(false, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // writers in flight finish
        const std::size_t count = std::min(trace_count.load(), trace_capacity);
        const std::string path = std::string(getenv("EDEN_LOG_DIR") ? getenv("EDEN_LOG_DIR") : "/data/prosperoeden/logs") + "/sched.bin";
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(trace_events, sizeof(TraceEvent), count, file);
            std::fclose(file);
            chmod(path.c_str(), 0666);
        }
        std::printf("EDEN_TRACE_DONE events=%zu dropped=%zu path=%s\n", count,
                    trace_count.load() > trace_capacity ? trace_count.load() - trace_capacity : 0, path.c_str());
        std::fflush(stdout);
        return;
    }
    if (access("/app0/trace-start.txt", F_OK) != 0 && access("/data/homebrew/PPSA99008/trace-start.txt", F_OK) != 0) return;
    unlink("/app0/trace-start.txt");
    unlink("/data/homebrew/PPSA99008/trace-start.txt");
    if (!trace_events) trace_events = new (std::nothrow) TraceEvent[trace_capacity];
    if (!trace_events) return;
    trace_count.store(0);
    trace_end_ns = NowNs() + trace_seconds * 1000000000ll;
    std::printf("EDEN_TRACE_START seconds=%lld\n", trace_seconds);
    std::fflush(stdout);
    trace_active.store(true, std::memory_order_release);
}

void BeginPcSampling() {
    struct sigaction previous{}, action{};
    if (sigaction(SIGUSR2, nullptr, &previous) || previous.sa_handler != SIG_DFL)
        throw std::runtime_error("Development sampler signal already in use");
    action.sa_sigaction = PcSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, nullptr))
        throw std::runtime_error("Cannot install development PC sampler");
    // Development process lifetime; polling stops before guest-worker shutdown.
    pc_sampling = true;
#if defined(EDEN_DEV_WAIT_CALLERS) && defined(PS5_NATIVE)
    std::printf("EDEN_PERF_CALLER_MODE enabled=1 firmware=6.02 depth=8 page=4096\n");
#endif
    std::printf("EDEN_PERF_PC_ANCHOR address=%llx capacity=%zu\n",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&PcSignal)), sampled_pcs.size());
    std::printf("EDEN_PERF_LIBC memcpy=%p memset=%p memmove=%p memcmp=%p\n",
                reinterpret_cast<void*>(&std::memcpy), reinterpret_cast<void*>(&std::memset),
                reinterpret_cast<void*>(&std::memmove), reinterpret_cast<void*>(&std::memcmp));
}

void PollGpuPc() {
    std::lock_guard lock(workers_mutex);
    if (pc_sampling && workers[4].registered && pc_count.load() < sampled_pcs.size()) {
        const int error = pthread_kill(workers[4].thread, SIGUSR2);
        if (error) throw std::runtime_error("Cannot sample development render thread");
    }
    if (pc_sampling && core_sample_ready.load(std::memory_order_acquire) &&
        core_pc_count.load() < sampled_core_pcs.size())
        pthread_kill(core_sample_thread, SIGUSR2);
}
#endif

extern "C" unsigned eden_heap_arenas_created(void) __attribute__((weak));

void AllocatorCheck() {
    std::printf("EDEN_PERF_HEAP arenas=%u\n", eden_heap_arenas_created ? eden_heap_arenas_created() : 1u);
    constexpr unsigned ops = 50000, live_slots = 1024;
    const auto run = [](unsigned seed) {
        std::vector<void*> live(live_slots, nullptr);
        unsigned x = seed;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < ops; ++i) {
            x = x * 1664525u + 1013904223u;
            void*& slot = live[x % live_slots];
            std::free(slot);
            slot = std::malloc(16 + ((x >> 16) & 511));
        }
        const auto end = std::chrono::steady_clock::now();
        for (void* pointer : live) std::free(pointer);
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    };
    std::printf("EDEN_PERF_ALLOC threads=1 pairs=%u ns_per_pair=%.1f\n", ops, run(1) / double(ops));
    for (unsigned threads : {2u, 4u, 8u}) {
        std::vector<long long> elapsed(threads);
        std::latch start{static_cast<std::ptrdiff_t>(threads)};
        std::vector<std::thread> pool;
        for (unsigned index = 0; index < threads; ++index)
            pool.emplace_back([&, index] { start.arrive_and_wait(); elapsed[index] = run(index + 7); });
        for (auto& thread : pool) thread.join();
        long long worst = 0, sum = 0;
        for (auto value : elapsed) { worst = std::max(worst, value); sum += value; }
        std::printf("EDEN_PERF_ALLOC threads=%u pairs=%u ns_per_pair_mean=%.1f ns_per_pair_worst=%.1f\n",
                    threads, ops, sum / double(threads) / ops, worst / double(ops));
    }
}

#if defined(PS5_NATIVE) && defined(EDEN_DEV_PROFILE)
// The getter writes through its first argument (calling it without one faulted), so it is
// called with an output; the setter is tried with the mode as its only argument.
extern "C" int sceKernelGetCpumodeGame(int*);
extern "C" int sceKernelSetCpumodeGame(int);
// Development probe (cpumode-probe.txt in the app folder): each game CPU mode in turn, with
// what the getters and the clock report after it, then the original mode again. The klog's
// SceSystemStateMgr lines show the cores' actual clocks.
int GameCpumode(int& result) {
    int mode = -1;
    result = sceKernelGetCpumodeGame(&mode);
    return mode;
}
void ProbeCpumodes() {
    if (access("/app0/cpumode-probe.txt", F_OK) != 0 &&
        access("/data/homebrew/PPSA99008/cpumode-probe.txt", F_OK) != 0) return;
    int got = 0;
    const int original = GameCpumode(got);
    std::printf("EDEN_CPUMODE_PROBE original_game=%d get_result=%08x cpumode=%d mhz=%ld\n", original, unsigned(got),
                sceKernelGetCpumode(), sceKernelGetCpuFrequency() / 1000000);
    std::fflush(stdout);
    if (got != 0) return; // the getter failed: do not change anything
    for (int mode = 0; mode < 8; ++mode) {
        const int result = sceKernelSetCpumodeGame(mode);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const int now = GameCpumode(got);
        std::printf("EDEN_CPUMODE_PROBE set=%d result=%08x game=%d cpumode=%d mhz=%ld\n", mode, unsigned(result),
                    now, sceKernelGetCpumode(), sceKernelGetCpuFrequency() / 1000000);
        std::fflush(stdout);
    }
    const int restored = sceKernelSetCpumodeGame(original);
    std::printf("EDEN_CPUMODE_PROBE restore=%d result=%08x game=%d cpumode=%d\n", original, unsigned(restored),
                GameCpumode(got), sceKernelGetCpumode());
    std::fflush(stdout);
}
#endif

#if defined(PS5_NATIVE) && defined(EDEN_DEV_PROFILE)
extern "C" int sceSystemServiceChangeCpuClock(int);
// The clock this thread's core actually runs at: dependent additions take one cycle each.
long MeasuredMhz() {
    unsigned long long x = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 400000; ++i)
        asm volatile(".rept 100\n add $1, %0\n .endr" : "+r"(x));
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return ns > 0 ? static_cast<long>(40000000ull * 1000 / static_cast<unsigned long long>(ns)) : 0;
}
// Development probe (cpuclock-probe.txt in the app folder): sceSystemServiceChangeCpuClock with
// candidate arguments, the measured clock after each, then 3200 again.
void ProbeCpuClock() {
    if (access("/app0/cpuclock-probe.txt", F_OK) != 0 &&
        access("/data/homebrew/PPSA99008/cpuclock-probe.txt", F_OK) != 0) return;
    std::printf("EDEN_CPUCLOCK_PROBE before measured_mhz=%ld reported_mhz=%ld\n", MeasuredMhz(),
                sceKernelGetCpuFrequency() / 1000000);
    std::fflush(stdout);
    for (const int value : {3500, 3200, 0, 1, 2, 3, 3200}) {
        const int result = sceSystemServiceChangeCpuClock(value);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::printf("EDEN_CPUCLOCK_PROBE set=%d result=%08x measured_mhz=%ld reported_mhz=%ld\n", value,
                    unsigned(result), MeasuredMhz(), sceKernelGetCpuFrequency() / 1000000);
        std::fflush(stdout);
    }
}
#endif

void PlatformChecks() {
#if defined(PS5_NATIVE) && defined(EDEN_DEV_PROFILE)
    ProbeCpumodes();
    ProbeCpuClock();
#endif
#ifdef PS5_NATIVE
    CheckWorkerTopology();
#endif
    CheckCpuClocks();
#ifdef EDEN_DEV_PROFILE
    AllocatorCheck();
#endif
    using clock = std::chrono::steady_clock;
    const auto ns = [](auto start) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    };
    std::printf("EDEN_PERF_CLOCK leaf_max=%x invariant=%d tsc_hz=%llu native=%d\n",
                __get_cpuid_max(0, nullptr), Common::g_cpu_caps.invariant_tsc,
                static_cast<unsigned long long>(Common::g_cpu_caps.tsc_frequency),
                Common::g_wall_clock.IsNative());
    unsigned long long sum = 0;
    auto start = clock::now();
    for (unsigned i = 0; i < 100'000; ++i) sum += Common::g_wall_clock.GetCNTPCT();
    std::printf("EDEN_PERF_CLOCK_READ calls=100000 elapsed_ns=%lld checksum=%llu\n",
                static_cast<long long>(ns(start)), sum);
    // Bound the diagnostic overhead comparison; per-block timers use the
    // already-qualified fenced counter, with the upstream steady-clock fallback.
    start = clock::now();
    for (unsigned i = 0; i < 1000; ++i) sum += clock::now().time_since_epoch().count();
    const auto system_clock_ns = ns(start);
    start = clock::now();
    for (unsigned i = 0; i < 1000; ++i) sum += Common::g_wall_clock.GetTimeNS().count();
    std::printf("EDEN_PERF_DIAGNOSTIC_CLOCK calls=1000 system_ns=%lld counter_ns=%lld checksum=%llu\n",
                static_cast<long long>(system_clock_ns), static_cast<long long>(ns(start)), sum);
    for (const unsigned requested_us : {1000u, 16667u, 50000u}) {
        const auto wall = Common::g_wall_clock.GetTimeNS();
        start = clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(requested_us));
        const auto mono_delta = ns(start);
        const auto wall_delta = (Common::g_wall_clock.GetTimeNS() - wall).count();
        if (mono_delta <= 0 || wall_delta <= 0) throw std::runtime_error("Nonmonotonic clock");
        if (std::abs(wall_delta - mono_delta) > std::max<decltype(mono_delta)>(2'000'000, mono_delta / 20))
            throw std::runtime_error("Guest clock disagrees with monotonic time");
        std::printf("EDEN_PERF_SLEEP requested_us=%u mono_ns=%lld wall_ns=%lld\n",
                    requested_us, static_cast<long long>(mono_delta), static_cast<long long>(wall_delta));
    }
    constexpr std::size_t bytes = 16 * 1024 * 1024;
    auto* memory = static_cast<unsigned long long*>(Common::AllocateMemoryPages(bytes));
    if (!memory) throw std::bad_alloc{};
    struct Release {
        void* memory;
        ~Release() { Common::FreeMemoryPages(memory, bytes); }
    } release{memory};
    constexpr std::size_t words = bytes / sizeof(*memory);
    for (std::size_t i = 0; i < words; ++i) memory[i] = i;
    const volatile auto* reads = memory;
    sum = 0;
    start = clock::now();
    for (unsigned repeat = 0; repeat < 4; ++repeat)
        for (std::size_t i = 0; i < words; ++i) sum += reads[i];
    if (sum != 4ULL * words * (words - 1) / 2) throw std::runtime_error("Memory read checksum");
    std::printf("EDEN_PERF_MEMORY pattern=stream bytes=%zu elapsed_ns=%lld checksum=%llu\n",
                bytes * 4, static_cast<long long>(ns(start)), sum);
    // Full-cycle LCG, one node per cache line. Compare a small warm working set
    // with the same dependent-load loop across the larger owned mapping.
    for (const std::size_t nodes : {std::size_t{512}, bytes / 64}) {
        for (std::size_t i = 0; i < nodes; ++i) memory[i * 8] = (i * 5 + 1) & (nodes - 1);
        std::size_t index = 0;
        constexpr unsigned steps = 1'048'576;
        start = clock::now();
        for (unsigned i = 0; i < steps; ++i) index = reads[index * 8];
        if (index != 0) throw std::runtime_error("Memory chain checksum");
        std::printf("EDEN_PERF_MEMORY pattern=dependent working_bytes=%zu steps=%u elapsed_ns=%lld checksum=%zu\n",
                    nodes * 64, steps, static_cast<long long>(ns(start)), index);
    }
    std::fflush(stdout);
}
} // namespace Eden::Performance

// dynarmic A64 hooks (weak there): whole-cache evacuations and block compilation time.
extern "C" void eden_jit_evacuation(unsigned core) {
    if (core < Eden::Performance::evacuations.size())
        Eden::Performance::evacuations[core].fetch_add(1, std::memory_order_relaxed);
}
extern "C" void eden_jit_compile(unsigned core, unsigned long long ns) {
    if (core >= Eden::Performance::compilation.size()) return;
    Eden::Performance::compilation[core].calls.fetch_add(1, std::memory_order_relaxed);
    Eden::Performance::compilation[core].nanoseconds.fetch_add(ns, std::memory_order_relaxed);
}
