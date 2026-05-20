// SPDX-License-Identifier: MIT
// Psynder — Wave B: heterogeneous P/E detection + per-worker hints. Lane 04.

#include "HeteroPool_internal.h"
#include "core/hardware/CpuFeatures.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#elif defined(__linux__)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <vector>
#endif

namespace psynder::jobs::detail {

namespace {

std::atomic<u32> g_topology_inited{0};
HeteroTopology g_topology;

#if defined(__APPLE__)

// Apple Silicon AMP topology is exposed via sysctlbyname:
//   hw.perflevel0.physicalcpu  → P-core count (perflevel 0 = highest perf)
//   hw.perflevel1.physicalcpu  → E-core count
//   hw.nperflevels             → number of perf levels (1 = homogeneous)
//
// Apple's docs are clear about ordering (level 0 is "highest performance",
// not "first index"). On x86 Macs hw.nperflevels is 1 and the perflevelN
// keys don't exist — return the homogeneous topology.
HeteroTopology detect_apple() noexcept {
    HeteroTopology t;
    u32 nperflevels = 0;
    size_t sz = sizeof(nperflevels);
    if (sysctlbyname("hw.nperflevels", &nperflevels, &sz, nullptr, 0) != 0) {
        nperflevels = 1;
    }
    int phys = 0;
    sz = sizeof(phys);
    sysctlbyname("hw.physicalcpu", &phys, &sz, nullptr, 0);
    t.total = phys > 0 ? static_cast<u32>(phys) : 0u;

    if (nperflevels >= 2u) {
        int p = 0, e = 0;
        sz = sizeof(p);
        if (sysctlbyname("hw.perflevel0.physicalcpu", &p, &sz, nullptr, 0) == 0 && p > 0) {
            t.p_cores = static_cast<u32>(p);
        }
        sz = sizeof(e);
        if (sysctlbyname("hw.perflevel1.physicalcpu", &e, &sz, nullptr, 0) == 0 && e > 0) {
            t.e_cores = static_cast<u32>(e);
        }
    }
    return t;
}

#elif defined(_WIN32)

// GetLogicalProcessorInformationEx(RelationProcessorCore) returns one
// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record per physical core,
// each with an EfficiencyClass byte. On hybrid SKUs class 0 is E-core
// and class ≥ 1 is P-core. On homogeneous parts every record has the
// same class — we treat that as "no hetero" and report p=e=0.
HeteroTopology detect_win32() noexcept {
    HeteroTopology t;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0)
        return t;
    std::vector<unsigned char> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                                              buf.data()),
                                          &len)) {
        return t;
    }
    u32 min_class = 0xFFu;
    u32 max_class = 0u;
    u32 cores = 0;
    u32 by_class[256] = {0};
    DWORD off = 0;
    while (off < len) {
        auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (rec->Relationship == RelationProcessorCore) {
            u32 ec = static_cast<u32>(rec->Processor.EfficiencyClass);
            ++by_class[ec & 0xFFu];
            if (ec < min_class)
                min_class = ec;
            if (ec > max_class)
                max_class = ec;
            ++cores;
        }
        off += rec->Size;
    }
    t.total = cores;
    if (min_class == max_class) {
        return t;  // homogeneous
    }
    t.e_cores = by_class[min_class & 0xFFu];
    t.p_cores = by_class[max_class & 0xFFu];
    return t;
}

#elif defined(__linux__)

// Linux: bucket cores by /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq.
// Hybrid topologies show distinct max-freq buckets per cluster (e.g. Alder
// Lake reports ~5.2 GHz on P, ~3.9 GHz on E). On homogeneous boxes every
// CPU reports the same max-freq and we return 0/0.
HeteroTopology detect_linux() noexcept {
    HeteroTopology t;
    DIR* d = opendir("/sys/devices/system/cpu");
    if (!d)
        return t;
    std::vector<u32> freqs;
    while (auto* ent = readdir(d)) {
        if (std::strncmp(ent->d_name, "cpu", 3) != 0)
            continue;
        bool all_digits = true;
        for (const char* p = ent->d_name + 3; *p; ++p) {
            if (*p < '0' || *p > '9') {
                all_digits = false;
                break;
            }
        }
        if (!all_digits || ent->d_name[3] == '\0')
            continue;
        char path[256];
        std::snprintf(path,
                      sizeof(path),
                      "/sys/devices/system/cpu/%s/cpufreq/cpuinfo_max_freq",
                      ent->d_name);
        FILE* fp = std::fopen(path, "r");
        if (!fp)
            continue;
        unsigned long khz = 0;
        if (std::fscanf(fp, "%lu", &khz) == 1 && khz > 0u) {
            freqs.push_back(static_cast<u32>(khz));
        }
        std::fclose(fp);
    }
    closedir(d);
    if (freqs.empty())
        return t;
    t.total = static_cast<u32>(freqs.size());
    u32 mn = freqs[0], mx = freqs[0];
    for (u32 f : freqs) {
        if (f < mn)
            mn = f;
        if (f > mx)
            mx = f;
    }
    if (mn == mx)
        return t;  // homogeneous
    u32 p = 0, e = 0;
    for (u32 f : freqs) {
        if (f == mx)
            ++p;
        else if (f == mn)
            ++e;
        else
            ++p;  // three-tier topologies lump middle into P
    }
    t.p_cores = p;
    t.e_cores = e;
    return t;
}

#endif

}  // namespace

HeteroTopology detect_hetero_topology() noexcept {
    // Lazy one-shot init. Concurrent callers either see the populated
    // topology or one wins and fills it.
    u32 state = g_topology_inited.load(std::memory_order_acquire);
    if (state == 2u)
        return g_topology;
    u32 expected = 0u;
    if (g_topology_inited.compare_exchange_strong(expected,
                                                  1u,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
        HeteroTopology t;
#if defined(__APPLE__)
        t = detect_apple();
#elif defined(_WIN32)
        t = detect_win32();
#elif defined(__linux__)
        t = detect_linux();
#else
        const auto& cpu = psynder::hardware::detect();
        t.total = cpu.cores_physical;
#endif
        if (t.total == 0) {
            const auto& cpu = psynder::hardware::detect();
            t.total = cpu.cores_physical ? cpu.cores_physical : std::thread::hardware_concurrency();
        }
        g_topology = t;
        g_topology_inited.store(2u, std::memory_order_release);
        return t;
    }
    while (g_topology_inited.load(std::memory_order_acquire) != 2u) {
        std::this_thread::yield();
    }
    return g_topology;
}

#if defined(__APPLE__)

void apply_worker_class_hint(WorkerClass cls) noexcept {
    // QoS is the primary AMP scheduling cue on macOS. USER_INTERACTIVE
    // strongly prefers P-cores; BACKGROUND prefers E-cores. UTILITY can
    // drift onto P under load — we use BACKGROUND because keeping
    // throughput work off the P-cores is the *desired* behavior.
    qos_class_t q = (cls == WorkerClass::Latency) ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_BACKGROUND;
    pthread_set_qos_class_self_np(q, 0);
}

#elif defined(_WIN32)

void apply_worker_class_hint(WorkerClass cls) noexcept {
    // PROCESS_POWER_THROTTLING_EXECUTION_SPEED in StateMask determines
    // whether the thread *can* be throttled (lands on E-cores). We set it
    // on for throughput threads and clear it (control mask still set) for
    // latency threads.
    THREAD_POWER_THROTTLING_STATE st{};
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = (cls == WorkerClass::Throughput) ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0u;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof(st));
    // Thread priority also feeds the scheduler's IDEAL processor pick.
    if (cls == WorkerClass::Latency) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    }
}

#else

void apply_worker_class_hint(WorkerClass /*cls*/) noexcept {
    // Linux / others: pool sizing alone provides the separation for Wave B.
    // A future revision can add sched_setaffinity once cpuset interactions
    // are tested.
}

#endif

}  // namespace psynder::jobs::detail
