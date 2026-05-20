// SPDX-License-Identifier: MIT
// Psynder — CPU feature detection. Used by simd dispatch (lane 03) and the
// audio mixer (lane 12). Lane 01 owns; ripped from dmonte's Hardware/.

#pragma once

#include "../Types.h"

namespace psynder::hardware {

struct CpuFeatures {
    bool sse42 = false;
    bool avx = false;
    bool avx2 = false;
    bool fma = false;
    bool avx512f = false;
    bool neon = false;
    bool sve = false;  // ARMv9 SVE; future
    u32 cores_physical = 0;
    u32 cores_logical = 0;
    u32 cache_l1d = 0;  // bytes
    u32 cache_l2 = 0;
    u32 cache_l3 = 0;
};

const CpuFeatures& detect();

}  // namespace psynder::hardware
