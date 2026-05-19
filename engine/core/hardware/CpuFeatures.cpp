// SPDX-License-Identifier: MIT
// Psynder — CPU feature detection. Lane 01 owns the full cpuid / sysctl
// queries; Phase-0 stub returns a conservative profile so sample binaries
// link cleanly.

#include "CpuFeatures.h"

#if defined(_MSC_VER)
#   include <intrin.h>
#endif

#if defined(__APPLE__)
#   include <sys/sysctl.h>
#   include <sys/types.h>
#endif

namespace psynder::hardware {

namespace {
CpuFeatures probe() {
    CpuFeatures f;
#if defined(__aarch64__) || defined(_M_ARM64)
    f.neon = true;
#endif
#if defined(__x86_64__) || defined(_M_X64)
    f.sse42 = true;
#   if defined(__AVX2__) || defined(__AVX__)
    f.avx  = true;
    f.avx2 = true;
    f.fma  = true;
#   endif
#   if defined(__AVX512F__)
    f.avx512f = true;
#   endif
#endif

#if defined(__APPLE__)
    int cores = 0;
    size_t sz = sizeof(cores);
    if (sysctlbyname("hw.physicalcpu", &cores, &sz, nullptr, 0) == 0) {
        f.cores_physical = static_cast<u32>(cores);
    }
    if (sysctlbyname("hw.logicalcpu", &cores, &sz, nullptr, 0) == 0) {
        f.cores_logical = static_cast<u32>(cores);
    }
#endif
    if (f.cores_physical == 0) f.cores_physical = 4;
    if (f.cores_logical  == 0) f.cores_logical  = 8;
    return f;
}
}  // namespace

const CpuFeatures& detect() {
    static const CpuFeatures features = probe();
    return features;
}

}  // namespace psynder::hardware
