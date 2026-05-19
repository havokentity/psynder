// SPDX-License-Identifier: MIT
// Psynder — SIMD abstraction. Lane 03 owns. Maps to SSE4.2 baseline /
// AVX2+FMA fast path / AVX-512 wide path on x86-64, and NEON on Apple
// Silicon. Runtime dispatch via psynder::hardware::detect().

#pragma once

#include "core/Types.h"

#if defined(__x86_64__) || defined(_M_X64)
#   include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#   include <arm_neon.h>
#endif

namespace psynder::simd {

// 4-wide and 8-wide float/int packs. Lane 03 implements the operator set
// (add/sub/mul/fma/cmp/blend/min/max/abs/rsqrt/sqrt/load/store/gather).
// The wider widths fall back to two halves on platforms that don't carry
// them natively (NEON has no 8-wide, AVX-512 is opt-in on x86).

struct f32x4 {
#if defined(__x86_64__) || defined(_M_X64)
    __m128 v;
#elif defined(__aarch64__) || defined(_M_ARM64)
    float32x4_t v;
#else
    f32 v[4];
#endif
};

struct f32x8 {
#if defined(__AVX__)
    __m256 v;
#else
    f32x4 lo, hi;
#endif
};

struct i32x4 {
#if defined(__x86_64__) || defined(_M_X64)
    __m128i v;
#elif defined(__aarch64__) || defined(_M_ARM64)
    int32x4_t v;
#else
    i32 v[4];
#endif
};

struct i32x8 {
#if defined(__AVX2__)
    __m256i v;
#else
    i32x4 lo, hi;
#endif
};

// Operator set declarations (lane 03 implements; many will be inline header-only).
f32x4 add(f32x4 a, f32x4 b) noexcept;
f32x4 sub(f32x4 a, f32x4 b) noexcept;
f32x4 mul(f32x4 a, f32x4 b) noexcept;
f32x4 div(f32x4 a, f32x4 b) noexcept;
f32x4 fma(f32x4 a, f32x4 b, f32x4 c) noexcept;
f32x4 load(const f32* p) noexcept;
void  store(f32* p, f32x4 v) noexcept;
f32x4 broadcast(f32 s) noexcept;

}  // namespace psynder::simd
