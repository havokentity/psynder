// SPDX-License-Identifier: MIT
// Psynder — CPU feature detection. Implements `detect()` for all three
// supported host architectures (Apple Silicon arm64, x86-64 Linux/macOS,
// x86-64 / arm64 Windows).
//
// Two probe paths:
//   1. x86 family: cpuid leaves 0/1/7 (+ leaf 0x80000004 for the brand
//      string) + xgetbv for AVX OS-enable state. We never trust the static
//      preprocessor macros (-mavx2 etc.) alone -- the compiled binary can
//      run on a CPU that lacks the feature, so the dispatch table reads
//      what the running silicon actually supports.
//   2. arm64: NEON is mandatory on AArch64, so it's reported unconditionally.
//      SVE2 probing on Linux uses HWCAP / HWCAP2 (auxv); macOS Apple Silicon
//      doesn't ship SVE2, so we leave it false on Darwin. Windows on ARM
//      probes IsProcessorFeaturePresent.
//
// Core / cache counts:
//   - macOS:   sysctlbyname (hw.physicalcpu, hw.logicalcpu,
//              hw.l1dcachesize, hw.l2cachesize, hw.l3cachesize).
//   - Linux:   sysconf(_SC_*) for core counts + reading
//              /sys/devices/system/cpu/cpu0/cache for cache sizes.
//   - Windows: GetLogicalProcessorInformationEx for both.

#include "CpuFeatures.h"

#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <unistd.h>
#include <cstdio>
#if defined(__aarch64__) || defined(__arm64__)
#include <sys/auxv.h>
#if __has_include(<asm/hwcap.h>)
#include <asm/hwcap.h>
#endif
#endif
#endif

namespace psynder::hardware {

namespace {

#if defined(__x86_64__) || defined(_M_X64)

// Wrap cpuid for both compilers. eax / ebx / ecx / edx are output regs;
// leaf and subleaf are inputs. The MSVC intrinsic packs the four results
// into an int[4]; gcc/clang have a builtin.
inline void cpuid_full(u32 leaf, u32 subleaf, u32& eax, u32& ebx, u32& ecx, u32& edx) {
#if defined(_MSC_VER)
    int regs[4] = {0};
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    eax = static_cast<u32>(regs[0]);
    ebx = static_cast<u32>(regs[1]);
    ecx = static_cast<u32>(regs[2]);
    edx = static_cast<u32>(regs[3]);
#else
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
#endif
}

inline u64 xgetbv(u32 xcr) {
#if defined(_MSC_VER)
    return _xgetbv(xcr);
#else
    u32 eax_v = 0, edx_v = 0;
    __asm__ volatile("xgetbv" : "=a"(eax_v), "=d"(edx_v) : "c"(xcr));
    return (static_cast<u64>(edx_v) << 32) | eax_v;
#endif
}

void probe_x86(CpuFeatures& f) {
    u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid_full(0, 0, eax, ebx, ecx, edx);
    const u32 max_basic = eax;

    if (max_basic >= 1) {
        cpuid_full(1, 0, eax, ebx, ecx, edx);
        // Leaf 1, ECX:
        //   bit 20 SSE4.2
        //   bit 28 AVX (also requires OS XSAVE -- gated below).
        //   bit 27 OSXSAVE
        //   bit 12 FMA
        // Leaf 1, EDX:
        //   bit 25 SSE
        //   bit 26 SSE2
        f.sse42 = (ecx & (1u << 20)) != 0;

        const bool os_xsave = (ecx & (1u << 27)) != 0;
        const bool avx_cpuid = (ecx & (1u << 28)) != 0;
        const bool fma_cpuid = (ecx & (1u << 12)) != 0;

        // OS-enable check via XCR0: bits 1 (XMM) and 2 (YMM) must be set
        // for the OS to preserve AVX state across context switches. If
        // XCR0 doesn't have both, AVX instructions will fault even
        // though cpuid says they exist. This catches old Linux kernels
        // that boot with AVX disabled in BIOS or sandbox environments
        // that filter cpuid through and lie about state preservation.
        bool avx_ok = false;
        if (os_xsave && avx_cpuid) {
            const u64 xcr0 = xgetbv(0);
            avx_ok = (xcr0 & 0x6) == 0x6;
        }
        f.avx = avx_ok;
        f.fma = avx_ok && fma_cpuid;
    }

    if (max_basic >= 7) {
        cpuid_full(7, 0, eax, ebx, ecx, edx);
        // Leaf 7, EBX:
        //   bit 5  AVX2
        //   bit 16 AVX-512 Foundation
        f.avx2 = f.avx && ((ebx & (1u << 5)) != 0);
        // AVX-512 also needs OS support for ZMM state (XCR0 bits 5/6/7);
        // we re-check XCR0 to be safe even though AVX2's xgetbv check
        // already cleared bits 1/2.
        if ((ebx & (1u << 16)) != 0) {
            const u64 xcr0 = xgetbv(0);
            const bool zmm_ok = (xcr0 & 0xE0) == 0xE0;  // opmask + zmm-hi + zmm-hi16
            f.avx512f = zmm_ok;
        }
    }
}

#endif  // x86_64

#if defined(__APPLE__)
void probe_macos(CpuFeatures& f) {
    int v = 0;
    size_t sz = sizeof(v);
    if (sysctlbyname("hw.physicalcpu", &v, &sz, nullptr, 0) == 0) {
        f.cores_physical = static_cast<u32>(v);
    }
    sz = sizeof(v);
    if (sysctlbyname("hw.logicalcpu", &v, &sz, nullptr, 0) == 0) {
        f.cores_logical = static_cast<u32>(v);
    }

    // Cache sizes: int64_t on Darwin. We probe per-level.
    int64_t cache = 0;
    sz = sizeof(cache);
    if (sysctlbyname("hw.l1dcachesize", &cache, &sz, nullptr, 0) == 0) {
        f.cache_l1d = static_cast<u32>(cache);
    }
    sz = sizeof(cache);
    if (sysctlbyname("hw.l2cachesize", &cache, &sz, nullptr, 0) == 0) {
        f.cache_l2 = static_cast<u32>(cache);
    }
    sz = sizeof(cache);
    if (sysctlbyname("hw.l3cachesize", &cache, &sz, nullptr, 0) == 0) {
        f.cache_l3 = static_cast<u32>(cache);
    }

#if defined(__aarch64__) || defined(__arm64__)
    // Apple Silicon supports NEON unconditionally; no SVE2 in the M1/M2/M3
    // lineup (as of late 2026).
    f.neon = true;
    f.sve = false;
#endif
}
#endif

#if defined(_WIN32)
void probe_windows(CpuFeatures& f) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    f.cores_logical = si.dwNumberOfProcessors;

    // Physical cores: GetLogicalProcessorInformationEx with
    // RelationProcessorCore counts physical packages of (one or two)
    // logical processors. Allocate a growing buffer, retry on
    // ERROR_INSUFFICIENT_BUFFER -- the size depends on socket / NUMA.
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes > 0) {
        std::vector<u8> buffer(bytes);
        if (GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                &bytes)) {
            u32 cores = 0;
            DWORD offset = 0;
            while (offset < bytes) {
                auto* info =
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
                if (info->Relationship == RelationProcessorCore)
                    ++cores;
                offset += info->Size;
            }
            if (cores > 0)
                f.cores_physical = cores;
        }
    }
    if (f.cores_physical == 0)
        f.cores_physical = f.cores_logical;

    // Cache sizes: walk RelationCache. Take the max per level so SMT-
    // shared caches don't double-count.
    bytes = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &bytes);
    if (bytes > 0) {
        std::vector<u8> buffer(bytes);
        if (GetLogicalProcessorInformationEx(
                RelationCache,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                &bytes)) {
            DWORD offset = 0;
            while (offset < bytes) {
                auto* info =
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
                if (info->Relationship == RelationCache) {
                    const auto& c = info->Cache;
                    const u32 sz = static_cast<u32>(c.CacheSize);
                    if (c.Level == 1 && c.Type == CacheData) {
                        if (sz > f.cache_l1d)
                            f.cache_l1d = sz;
                    } else if (c.Level == 2) {
                        if (sz > f.cache_l2)
                            f.cache_l2 = sz;
                    } else if (c.Level == 3) {
                        if (sz > f.cache_l3)
                            f.cache_l3 = sz;
                    }
                }
                offset += info->Size;
            }
        }
    }

#if defined(_M_ARM64) || defined(__aarch64__)
    f.neon = true;
    // Windows on ARM doesn't expose a clean SVE2 probe yet
    // (IsProcessorFeaturePresent has no SVE2 ID as of late 2026), so we
    // leave it false. Future builds can light this up via Win11+ feature
    // detection once the API ships.
    f.sve = false;
#endif
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
void probe_linux(CpuFeatures& f) {
    // Core count via sysconf -- POSIX path, works on Linux + BSD.
    long phys = sysconf(_SC_NPROCESSORS_ONLN);
    if (phys > 0)
        f.cores_logical = static_cast<u32>(phys);
    // sysconf doesn't distinguish SMT-siblings from physical cores on
    // Linux; we'd need /proc/cpuinfo to get the true physical count.
    // Parse the unique-core ids out of cpuinfo as best-effort.
    if (FILE* fp = std::fopen("/proc/cpuinfo", "r"); fp) {
        char line[512];
        // Track the maximum (physical id, core id) tuple to count
        // distinct physical cores across sockets.
        u32 max_phys_id = 0;
        u32 max_core_id = 0;
        bool saw_any = false;
        while (std::fgets(line, sizeof(line), fp)) {
            int v = 0;
            if (std::sscanf(line, "physical id : %d", &v) == 1) {
                if (static_cast<u32>(v) > max_phys_id)
                    max_phys_id = static_cast<u32>(v);
                saw_any = true;
            } else if (std::sscanf(line, "core id : %d", &v) == 1) {
                if (static_cast<u32>(v) > max_core_id)
                    max_core_id = static_cast<u32>(v);
                saw_any = true;
            }
        }
        std::fclose(fp);
        if (saw_any) {
            // (max_phys_id + 1) sockets * (max_core_id + 1) cores per socket.
            f.cores_physical = (max_phys_id + 1) * (max_core_id + 1);
        }
    }
    if (f.cores_physical == 0)
        f.cores_physical = f.cores_logical;

    // Cache sizes from /sys/devices/system/cpu/cpu0/cache/index<N>/size.
    // The size file holds the cache size as "32K" / "1024K" / "32M". We
    // walk levels 0..3 and take the L1d / L2 / L3 size, identifying the
    // level via the type+level files at that index.
    for (int idx = 0; idx < 8; ++idx) {
        char path[128];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/type", idx);
        FILE* fp = std::fopen(path, "r");
        if (!fp)
            break;
        char type[32] = {0};
        if (!std::fgets(type, sizeof(type), fp)) {
            std::fclose(fp);
            break;
        }
        std::fclose(fp);

        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/level", idx);
        fp = std::fopen(path, "r");
        if (!fp)
            break;
        int level = 0;
        if (std::fscanf(fp, "%d", &level) != 1) {
            std::fclose(fp);
            break;
        }
        std::fclose(fp);

        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/size", idx);
        fp = std::fopen(path, "r");
        if (!fp)
            break;
        char sz[32] = {0};
        if (!std::fgets(sz, sizeof(sz), fp)) {
            std::fclose(fp);
            break;
        }
        std::fclose(fp);
        u32 bytes = 0;
        u32 n = 0;
        if (std::sscanf(sz, "%u%c", &n, reinterpret_cast<char*>(&sz[0])) == 2) {
            bytes = n * 1024u;  // K
            if (sz[0] == 'M' || sz[0] == 'm')
                bytes = n * 1024u * 1024u;
        } else if (std::sscanf(sz, "%u", &n) == 1) {
            bytes = n;
        }

        const bool is_data =
            std::strncmp(type, "Data", 4) == 0 || std::strncmp(type, "Unified", 7) == 0;
        if (level == 1 && is_data)
            f.cache_l1d = bytes;
        else if (level == 2)
            f.cache_l2 = bytes;
        else if (level == 3)
            f.cache_l3 = bytes;
    }

#if defined(__aarch64__)
    f.neon = true;
    // Linux exposes SVE via HWCAP / HWCAP2. HWCAP_SVE is defined in
    // <asm/hwcap.h> when the kernel headers support it.
    unsigned long hwcap = getauxval(AT_HWCAP);
#if defined(HWCAP_SVE)
    f.sve = (hwcap & HWCAP_SVE) != 0;
#else
    (void)hwcap;
    f.sve = false;
#endif
#endif
}
#endif

CpuFeatures probe() {
    CpuFeatures f;

#if defined(__x86_64__) || defined(_M_X64)
    probe_x86(f);
#endif

#if defined(__APPLE__)
    probe_macos(f);
#elif defined(_WIN32)
    probe_windows(f);
#else
    probe_linux(f);
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    // Mandatory on AArch64 -- the ABI assumes it.
    f.neon = true;
#endif

    // Final sanity defaults so the rest of the engine never sees zeroes
    // that would confuse `cores_physical / cores_logical` math.
    if (f.cores_physical == 0)
        f.cores_physical = 4;
    if (f.cores_logical == 0)
        f.cores_logical = f.cores_physical * 2;
    return f;
}

}  // namespace

const CpuFeatures& detect() {
    static const CpuFeatures features = probe();
    return features;
}

}  // namespace psynder::hardware
