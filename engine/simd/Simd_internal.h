// SPDX-License-Identifier: MIT
// Psynder — SIMD internal header. Inline intrinsic kernels behind the
// frozen public Simd.h surface. Lane 03 owns this file; everything here is
// implementation detail and may freely change without coordination.
//
// Translation map:
//   - x86-64 baseline           SSE4.2  (`_mm_*`)
//   - x86-64 fast path          AVX2 + FMA (`_mm256_*`, `_mm_fmadd_ps`)
//   - x86-64 wide path          AVX-512F (`_mm512_*`, optional, behind detect)
//   - aarch64 / Apple Silicon   NEON (`v*q_*`)
//   - scalar fallback           plain C++ loop
//
// The runtime dispatcher (see Dispatch.h) selects the widest kernel for
// non-inlinable batched ops (e.g. reduce_add over a long buffer). Per-lane
// scalar/SIMD ops here are resolved at compile time by the host arch — one
// binary, one set of intrinsics, the platform that compiled is the platform
// that runs. (Multi-versioned dispatch is layered on top via Dispatch.cpp
// for routines big enough to recoup the function-pointer indirection.)

#pragma once

#include "Simd.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace psynder::simd {

// ─── Mask types ──────────────────────────────────────────────────────────
// `mask4` / `mask8` are SIMD predicate values produced by `cmp_*`. They
// carry one bit-pattern per lane (all-ones if true, all-zeros if false) so
// `blend` / logical ops collapse to a single intrinsic on every backend.

struct mask4 {
#if defined(__x86_64__) || defined(_M_X64)
    __m128 v;
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint32x4_t v;
#else
    psynder::u32 v[4];
#endif
};

struct mask8 {
#if defined(__AVX__)
    __m256 v;
#else
    mask4 lo, hi;
#endif
};

// ─── f32x4 — primary 4-wide kernel ───────────────────────────────────────
PSY_FORCEINLINE f32x4 add4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_add_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vaddq_f32(a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] + b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE f32x4 sub4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_sub_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vsubq_f32(a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] - b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE f32x4 mul4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_mul_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vmulq_f32(a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] * b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE f32x4 div4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_div_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vdivq_f32(a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] / b.v[i];
    return r;
#endif
}

// fma — `a*b + c`. AVX2+FMA host gets a true FMA; on NEON we use vfmaq_f32
// which is a fused multiply-add per the ARMv8 spec. On SSE-only x86 we fall
// back to separate mul+add (slightly less accurate but faster than the SW
// FMA round-trip).
PSY_FORCEINLINE f32x4 fma4(f32x4 a, f32x4 b, f32x4 c) noexcept {
#if defined(__FMA__)
    return f32x4{_mm_fmadd_ps(a.v, b.v, c.v)};
#elif defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_add_ps(_mm_mul_ps(a.v, b.v), c.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    // vfmaq_f32 is `c + a*b` — note operand order.
    return f32x4{vfmaq_f32(c.v, a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] * b.v[i] + c.v[i];
    return r;
#endif
}

// Comparison returns mask4. SSE/AVX cmp produces an all-ones-or-zero mask
// per lane already, so we forward straight through.
PSY_FORCEINLINE mask4 cmp_eq4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_cmpeq_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vceqq_f32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = (a.v[i] == b.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

PSY_FORCEINLINE mask4 cmp_lt4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_cmplt_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vcltq_f32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = (a.v[i] < b.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

PSY_FORCEINLINE mask4 cmp_le4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_cmple_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vcleq_f32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = (a.v[i] <= b.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

PSY_FORCEINLINE mask4 cmp_gt4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_cmpgt_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vcgtq_f32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = (a.v[i] > b.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

PSY_FORCEINLINE mask4 cmp_ge4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_cmpge_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vcgeq_f32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = (a.v[i] >= b.v[i]) ? 0xFFFFFFFFu : 0u;
    return r;
#endif
}

// Blend — selects b where mask is set, a where it's clear.
// On SSE4.2 baseline we have `_mm_blendv_ps`. On AVX2 the same intrinsic
// folds to VBLENDVPS. On NEON we use `vbslq_f32` (bit-select).
PSY_FORCEINLINE f32x4 blend4(f32x4 a, f32x4 b, mask4 m) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_blendv_ps(a.v, b.v, m.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vbslq_f32(m.v, b.v, a.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = m.v[i] ? b.v[i] : a.v[i];
    return r;
#endif
}

PSY_FORCEINLINE f32x4 min4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_min_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vminq_f32(a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = std::min(a.v[i], b.v[i]);
    return r;
#endif
}

PSY_FORCEINLINE f32x4 max4(f32x4 a, f32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_max_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vmaxq_f32(a.v, b.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = std::max(a.v[i], b.v[i]);
    return r;
#endif
}

PSY_FORCEINLINE f32x4 abs4(f32x4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    // Clear sign bit via andnot with the sign-mask.
    const __m128 sign = _mm_set1_ps(-0.0f);
    return f32x4{_mm_andnot_ps(sign, a.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vabsq_f32(a.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = std::fabs(a.v[i]);
    return r;
#endif
}

PSY_FORCEINLINE f32x4 sqrt4(f32x4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_sqrt_ps(a.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vsqrtq_f32(a.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = std::sqrt(a.v[i]);
    return r;
#endif
}

// rsqrt — approximate 1/sqrt(x). ~12-bit precision on SSE, ~12-bit on NEON.
// Callers needing tighter precision should follow up with a Newton-Raphson
// step (`y = y * (1.5 - 0.5*x*y*y)`).
PSY_FORCEINLINE f32x4 rsqrt4(f32x4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_rsqrt_ps(a.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vrsqrteq_f32(a.v)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = 1.0f / std::sqrt(a.v[i]);
    return r;
#endif
}

// Loads / stores. Aligned variants assume 16-byte alignment; UB otherwise.
PSY_FORCEINLINE f32x4 load_aligned4(const f32* p) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_load_ps(p)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vld1q_f32(p)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = p[i];
    return r;
#endif
}

PSY_FORCEINLINE f32x4 load_unaligned4(const f32* p) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_loadu_ps(p)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    // vld1q_f32 doesn't require alignment on aarch64.
    return f32x4{vld1q_f32(p)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = p[i];
    return r;
#endif
}

PSY_FORCEINLINE void store_aligned4(f32* p, f32x4 v) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_store_ps(p, v.v);
#elif defined(__aarch64__) || defined(_M_ARM64)
    vst1q_f32(p, v.v);
#else
    for (int i = 0; i < 4; ++i)
        p[i] = v.v[i];
#endif
}

PSY_FORCEINLINE void store_unaligned4(f32* p, f32x4 v) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_storeu_ps(p, v.v);
#elif defined(__aarch64__) || defined(_M_ARM64)
    vst1q_f32(p, v.v);
#else
    for (int i = 0; i < 4; ++i)
        p[i] = v.v[i];
#endif
}

PSY_FORCEINLINE f32x4 broadcast4(f32 s) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return f32x4{_mm_set1_ps(s)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return f32x4{vdupq_n_f32(s)};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = s;
    return r;
#endif
}

// Gather — load 4 f32s from `base[idx[0..3]]`. No native gather on SSE4.2
// or NEON, so we synthesize from 4 scalar loads. On AVX2 we use a real
// VGATHERDPS; AVX-512 widens further.
PSY_FORCEINLINE f32x4 gather4(const f32* base, i32x4 idx) noexcept {
#if defined(__AVX2__)
    return f32x4{_mm_i32gather_ps(base, idx.v, 4)};
#elif defined(__x86_64__) || defined(_M_X64)
    alignas(16) psynder::i32 ix[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(ix), idx.v);
    return f32x4{_mm_setr_ps(base[ix[0]], base[ix[1]], base[ix[2]], base[ix[3]])};
#elif defined(__aarch64__) || defined(_M_ARM64)
    alignas(16) psynder::i32 ix[4];
    vst1q_s32(ix, idx.v);
    float32x4_t r = vdupq_n_f32(base[ix[0]]);
    r = vsetq_lane_f32(base[ix[1]], r, 1);
    r = vsetq_lane_f32(base[ix[2]], r, 2);
    r = vsetq_lane_f32(base[ix[3]], r, 3);
    return f32x4{r};
#else
    f32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = base[idx.v[i]];
    return r;
#endif
}

// Reductions — horizontal across the lanes. Returns a scalar.
PSY_FORCEINLINE f32 reduce_add4(f32x4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    // hadd_ps twice would also work but blocked-2 shuffle is one fewer uop.
    __m128 shuf = _mm_movehdup_ps(a.v);  // {1,1,3,3}
    __m128 sums = _mm_add_ps(a.v, shuf);
    shuf = _mm_movehl_ps(shuf, sums);  // {2+3, 3+3, …}
    sums = _mm_add_ss(sums, shuf);
    return _mm_cvtss_f32(sums);
#elif defined(__aarch64__) || defined(_M_ARM64)
    return vaddvq_f32(a.v);
#else
    return a.v[0] + a.v[1] + a.v[2] + a.v[3];
#endif
}

PSY_FORCEINLINE f32 reduce_min4(f32x4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __m128 shuf = _mm_movehdup_ps(a.v);
    __m128 m = _mm_min_ps(a.v, shuf);
    shuf = _mm_movehl_ps(shuf, m);
    m = _mm_min_ss(m, shuf);
    return _mm_cvtss_f32(m);
#elif defined(__aarch64__) || defined(_M_ARM64)
    return vminvq_f32(a.v);
#else
    return std::min({a.v[0], a.v[1], a.v[2], a.v[3]});
#endif
}

PSY_FORCEINLINE f32 reduce_max4(f32x4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __m128 shuf = _mm_movehdup_ps(a.v);
    __m128 m = _mm_max_ps(a.v, shuf);
    shuf = _mm_movehl_ps(shuf, m);
    m = _mm_max_ss(m, shuf);
    return _mm_cvtss_f32(m);
#elif defined(__aarch64__) || defined(_M_ARM64)
    return vmaxvq_f32(a.v);
#else
    return std::max({a.v[0], a.v[1], a.v[2], a.v[3]});
#endif
}

// ─── mask4 logical ops + extraction ──────────────────────────────────────
PSY_FORCEINLINE mask4 mask_and(mask4 a, mask4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_and_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vandq_u32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] & b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE mask4 mask_or(mask4 a, mask4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_or_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vorrq_u32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] | b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE mask4 mask_xor(mask4 a, mask4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return mask4{_mm_xor_ps(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{veorq_u32(a.v, b.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] ^ b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE mask4 mask_not(mask4 a) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    const __m128 ones = _mm_castsi128_ps(_mm_set1_epi32(-1));
    return mask4{_mm_xor_ps(a.v, ones)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return mask4{vmvnq_u32(a.v)};
#else
    mask4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = ~a.v[i];
    return r;
#endif
}

// Pack lane MSBs into an int — `lane i set -> bit i`. Tile coverage uses
// this directly to drive 8-bit tile masks.
PSY_FORCEINLINE int mask_to_int4(mask4 m) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return _mm_movemask_ps(m.v);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // Take the sign bit (MSB) of each lane and pack into bits 0..3.
    static const uint32x4_t shifts = {0, 1, 2, 3};
    uint32x4_t high = vshrq_n_u32(m.v, 31);
    uint32x4_t bits = vshlq_u32(high, vreinterpretq_s32_u32(shifts));
    return static_cast<int>(vaddvq_u32(bits));
#else
    int r = 0;
    for (int i = 0; i < 4; ++i)
        r |= (m.v[i] >> 31) << i;
    return r;
#endif
}

PSY_FORCEINLINE bool any_of(mask4 m) noexcept {
    return mask_to_int4(m) != 0;
}
PSY_FORCEINLINE bool all_of(mask4 m) noexcept {
    return mask_to_int4(m) == 0xF;
}
PSY_FORCEINLINE bool none_of(mask4 m) noexcept {
    return mask_to_int4(m) == 0;
}

// ─── f32x8 — AVX2-native or composed-of-two-halves ───────────────────────
// On AVX hosts the underlying vector is `__m256`. Elsewhere (NEON, scalar)
// the public header already defines f32x8 as two f32x4 — we lean on that.

PSY_FORCEINLINE f32x8 add8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_add_ps(a.v, b.v)};
#else
    return f32x8{add4(a.lo, b.lo), add4(a.hi, b.hi)};
#endif
}

PSY_FORCEINLINE f32x8 sub8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_sub_ps(a.v, b.v)};
#else
    return f32x8{sub4(a.lo, b.lo), sub4(a.hi, b.hi)};
#endif
}

PSY_FORCEINLINE f32x8 mul8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_mul_ps(a.v, b.v)};
#else
    return f32x8{mul4(a.lo, b.lo), mul4(a.hi, b.hi)};
#endif
}

PSY_FORCEINLINE f32x8 div8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_div_ps(a.v, b.v)};
#else
    return f32x8{div4(a.lo, b.lo), div4(a.hi, b.hi)};
#endif
}

PSY_FORCEINLINE f32x8 fma8(f32x8 a, f32x8 b, f32x8 c) noexcept {
#if defined(__FMA__)
    return f32x8{_mm256_fmadd_ps(a.v, b.v, c.v)};
#elif defined(__AVX__)
    return f32x8{_mm256_add_ps(_mm256_mul_ps(a.v, b.v), c.v)};
#else
    return f32x8{fma4(a.lo, b.lo, c.lo), fma4(a.hi, b.hi, c.hi)};
#endif
}

PSY_FORCEINLINE mask8 cmp_eq8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ)};
#else
    return mask8{cmp_eq4(a.lo, b.lo), cmp_eq4(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 cmp_lt8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ)};
#else
    return mask8{cmp_lt4(a.lo, b.lo), cmp_lt4(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 cmp_le8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ)};
#else
    return mask8{cmp_le4(a.lo, b.lo), cmp_le4(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 cmp_gt8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ)};
#else
    return mask8{cmp_gt4(a.lo, b.lo), cmp_gt4(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 cmp_ge8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ)};
#else
    return mask8{cmp_ge4(a.lo, b.lo), cmp_ge4(a.hi, b.hi)};
#endif
}

PSY_FORCEINLINE f32x8 blend8(f32x8 a, f32x8 b, mask8 m) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_blendv_ps(a.v, b.v, m.v)};
#else
    return f32x8{blend4(a.lo, b.lo, m.lo), blend4(a.hi, b.hi, m.hi)};
#endif
}

PSY_FORCEINLINE f32x8 min8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_min_ps(a.v, b.v)};
#else
    return f32x8{min4(a.lo, b.lo), min4(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE f32x8 max8(f32x8 a, f32x8 b) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_max_ps(a.v, b.v)};
#else
    return f32x8{max4(a.lo, b.lo), max4(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE f32x8 abs8(f32x8 a) noexcept {
#if defined(__AVX__)
    const __m256 sign = _mm256_set1_ps(-0.0f);
    return f32x8{_mm256_andnot_ps(sign, a.v)};
#else
    return f32x8{abs4(a.lo), abs4(a.hi)};
#endif
}
PSY_FORCEINLINE f32x8 sqrt8(f32x8 a) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_sqrt_ps(a.v)};
#else
    return f32x8{sqrt4(a.lo), sqrt4(a.hi)};
#endif
}
PSY_FORCEINLINE f32x8 rsqrt8(f32x8 a) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_rsqrt_ps(a.v)};
#else
    return f32x8{rsqrt4(a.lo), rsqrt4(a.hi)};
#endif
}

PSY_FORCEINLINE f32x8 load_aligned8(const f32* p) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_load_ps(p)};
#else
    return f32x8{load_aligned4(p), load_aligned4(p + 4)};
#endif
}
PSY_FORCEINLINE f32x8 load_unaligned8(const f32* p) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_loadu_ps(p)};
#else
    return f32x8{load_unaligned4(p), load_unaligned4(p + 4)};
#endif
}
PSY_FORCEINLINE void store_aligned8(f32* p, f32x8 v) noexcept {
#if defined(__AVX__)
    _mm256_store_ps(p, v.v);
#else
    store_aligned4(p, v.lo);
    store_aligned4(p + 4, v.hi);
#endif
}
PSY_FORCEINLINE void store_unaligned8(f32* p, f32x8 v) noexcept {
#if defined(__AVX__)
    _mm256_storeu_ps(p, v.v);
#else
    store_unaligned4(p, v.lo);
    store_unaligned4(p + 4, v.hi);
#endif
}
PSY_FORCEINLINE f32x8 broadcast8(f32 s) noexcept {
#if defined(__AVX__)
    return f32x8{_mm256_set1_ps(s)};
#else
    return f32x8{broadcast4(s), broadcast4(s)};
#endif
}

PSY_FORCEINLINE f32x8 gather8(const f32* base, i32x8 idx) noexcept {
#if defined(__AVX2__)
    return f32x8{_mm256_i32gather_ps(base, idx.v, 4)};
#elif defined(__AVX__)
    // Have AVX vectors but no AVX2 gather → split into halves.
    i32x4 lo{_mm256_castsi256_si128(idx.v)};
    i32x4 hi{_mm256_extractf128_si256(idx.v, 1)};
    f32x4 g_lo = gather4(base, lo);
    f32x4 g_hi = gather4(base, hi);
    return f32x8{_mm256_insertf128_ps(_mm256_castps128_ps256(g_lo.v), g_hi.v, 1)};
#else
    return f32x8{gather4(base, idx.lo), gather4(base, idx.hi)};
#endif
}

PSY_FORCEINLINE f32 reduce_add8(f32x8 a) noexcept {
#if defined(__AVX__)
    __m128 lo = _mm256_castps256_ps128(a.v);
    __m128 hi = _mm256_extractf128_ps(a.v, 1);
    return reduce_add4(f32x4{_mm_add_ps(lo, hi)});
#else
    return reduce_add4(a.lo) + reduce_add4(a.hi);
#endif
}
PSY_FORCEINLINE f32 reduce_min8(f32x8 a) noexcept {
#if defined(__AVX__)
    __m128 lo = _mm256_castps256_ps128(a.v);
    __m128 hi = _mm256_extractf128_ps(a.v, 1);
    return reduce_min4(f32x4{_mm_min_ps(lo, hi)});
#else
    return std::min(reduce_min4(a.lo), reduce_min4(a.hi));
#endif
}
PSY_FORCEINLINE f32 reduce_max8(f32x8 a) noexcept {
#if defined(__AVX__)
    __m128 lo = _mm256_castps256_ps128(a.v);
    __m128 hi = _mm256_extractf128_ps(a.v, 1);
    return reduce_max4(f32x4{_mm_max_ps(lo, hi)});
#else
    return std::max(reduce_max4(a.lo), reduce_max4(a.hi));
#endif
}

// ─── mask8 logical ops + extraction ──────────────────────────────────────
PSY_FORCEINLINE mask8 mask_and(mask8 a, mask8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_and_ps(a.v, b.v)};
#else
    return mask8{mask_and(a.lo, b.lo), mask_and(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 mask_or(mask8 a, mask8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_or_ps(a.v, b.v)};
#else
    return mask8{mask_or(a.lo, b.lo), mask_or(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 mask_xor(mask8 a, mask8 b) noexcept {
#if defined(__AVX__)
    return mask8{_mm256_xor_ps(a.v, b.v)};
#else
    return mask8{mask_xor(a.lo, b.lo), mask_xor(a.hi, b.hi)};
#endif
}
PSY_FORCEINLINE mask8 mask_not(mask8 a) noexcept {
#if defined(__AVX__)
    const __m256 ones = _mm256_castsi256_ps(_mm256_set1_epi32(-1));
    return mask8{_mm256_xor_ps(a.v, ones)};
#else
    return mask8{mask_not(a.lo), mask_not(a.hi)};
#endif
}

PSY_FORCEINLINE int mask_to_int8(mask8 m) noexcept {
#if defined(__AVX__)
    return _mm256_movemask_ps(m.v);
#else
    return mask_to_int4(m.lo) | (mask_to_int4(m.hi) << 4);
#endif
}
PSY_FORCEINLINE bool any_of(mask8 m) noexcept {
    return mask_to_int8(m) != 0;
}
PSY_FORCEINLINE bool all_of(mask8 m) noexcept {
    return mask_to_int8(m) == 0xFF;
}
PSY_FORCEINLINE bool none_of(mask8 m) noexcept {
    return mask_to_int8(m) == 0;
}

// ─── i32x4 / i32x8 — integer ops needed for gather + bench ───────────────
PSY_FORCEINLINE i32x4 load_i32x4(const psynder::i32* p) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return i32x4{_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return i32x4{vld1q_s32(p)};
#else
    i32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = p[i];
    return r;
#endif
}
PSY_FORCEINLINE void store_i32x4(psynder::i32* p, i32x4 v) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v.v);
#elif defined(__aarch64__) || defined(_M_ARM64)
    vst1q_s32(p, v.v);
#else
    for (int i = 0; i < 4; ++i)
        p[i] = v.v[i];
#endif
}
PSY_FORCEINLINE i32x4 broadcast_i32x4(psynder::i32 s) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return i32x4{_mm_set1_epi32(s)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return i32x4{vdupq_n_s32(s)};
#else
    i32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = s;
    return r;
#endif
}
PSY_FORCEINLINE i32x4 add_i32x4(i32x4 a, i32x4 b) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return i32x4{_mm_add_epi32(a.v, b.v)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    return i32x4{vaddq_s32(a.v, b.v)};
#else
    i32x4 r{};
    for (int i = 0; i < 4; ++i)
        r.v[i] = a.v[i] + b.v[i];
    return r;
#endif
}

PSY_FORCEINLINE i32x8 load_i32x8(const psynder::i32* p) noexcept {
#if defined(__AVX2__)
    return i32x8{_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p))};
#elif defined(__AVX__)
    // AVX-only host: build a __m256i from two 128-bit loads.
    __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4));
    return i32x8{_mm256_insertf128_si256(_mm256_castsi128_si256(lo), hi, 1)};
#else
    return i32x8{load_i32x4(p), load_i32x4(p + 4)};
#endif
}

PSY_FORCEINLINE i32x8 broadcast_i32x8(psynder::i32 s) noexcept {
#if defined(__AVX2__)
    return i32x8{_mm256_set1_epi32(s)};
#elif defined(__AVX__)
    return i32x8{_mm256_castps_si256(_mm256_set1_ps(std::bit_cast<float>(s)))};
#else
    return i32x8{broadcast_i32x4(s), broadcast_i32x4(s)};
#endif
}

// ─── AVX-512 opportunistic f32x16 ────────────────────────────────────────
// Gated by `__AVX512F__` — the compiler must have been invoked with -mavx512f
// (or the host was an AVX-512 toolchain). Fallback is two AVX2 halves; the
// runtime dispatcher still inspects CpuFeatures::avx512f before calling any
// routine that depends on this width.

#if defined(__AVX512F__)
struct f32x16 {
    __m512 v;
};
PSY_FORCEINLINE f32x16 add16(f32x16 a, f32x16 b) noexcept {
    return f32x16{_mm512_add_ps(a.v, b.v)};
}
PSY_FORCEINLINE f32x16 sub16(f32x16 a, f32x16 b) noexcept {
    return f32x16{_mm512_sub_ps(a.v, b.v)};
}
PSY_FORCEINLINE f32x16 mul16(f32x16 a, f32x16 b) noexcept {
    return f32x16{_mm512_mul_ps(a.v, b.v)};
}
PSY_FORCEINLINE f32x16 fma16(f32x16 a, f32x16 b, f32x16 c) noexcept {
    return f32x16{_mm512_fmadd_ps(a.v, b.v, c.v)};
}
PSY_FORCEINLINE f32x16 load_aligned16(const psynder::f32* p) noexcept {
    return f32x16{_mm512_load_ps(p)};
}
PSY_FORCEINLINE f32x16 load_unaligned16(const psynder::f32* p) noexcept {
    return f32x16{_mm512_loadu_ps(p)};
}
PSY_FORCEINLINE void store_aligned16(psynder::f32* p, f32x16 v) noexcept {
    _mm512_store_ps(p, v.v);
}
PSY_FORCEINLINE void store_unaligned16(psynder::f32* p, f32x16 v) noexcept {
    _mm512_storeu_ps(p, v.v);
}
PSY_FORCEINLINE f32x16 broadcast16(psynder::f32 s) noexcept {
    return f32x16{_mm512_set1_ps(s)};
}
PSY_FORCEINLINE psynder::f32 reduce_add16(f32x16 a) noexcept {
    return _mm512_reduce_add_ps(a.v);
}
#else
// Composed fallback so code referring to f32x16 still compiles. The widest-
// path consumers should still gate this with the dispatch tier, but a
// portable definition makes generic templates / tests cleaner.
struct f32x16 {
    f32x8 lo, hi;
};
PSY_FORCEINLINE f32x16 add16(f32x16 a, f32x16 b) noexcept {
    return f32x16{add8(a.lo, b.lo), add8(a.hi, b.hi)};
}
PSY_FORCEINLINE f32x16 sub16(f32x16 a, f32x16 b) noexcept {
    return f32x16{sub8(a.lo, b.lo), sub8(a.hi, b.hi)};
}
PSY_FORCEINLINE f32x16 mul16(f32x16 a, f32x16 b) noexcept {
    return f32x16{mul8(a.lo, b.lo), mul8(a.hi, b.hi)};
}
PSY_FORCEINLINE f32x16 fma16(f32x16 a, f32x16 b, f32x16 c) noexcept {
    return f32x16{fma8(a.lo, b.lo, c.lo), fma8(a.hi, b.hi, c.hi)};
}
PSY_FORCEINLINE f32x16 load_aligned16(const psynder::f32* p) noexcept {
    return f32x16{load_aligned8(p), load_aligned8(p + 8)};
}
PSY_FORCEINLINE f32x16 load_unaligned16(const psynder::f32* p) noexcept {
    return f32x16{load_unaligned8(p), load_unaligned8(p + 8)};
}
PSY_FORCEINLINE void store_aligned16(psynder::f32* p, f32x16 v) noexcept {
    store_aligned8(p, v.lo);
    store_aligned8(p + 8, v.hi);
}
PSY_FORCEINLINE void store_unaligned16(psynder::f32* p, f32x16 v) noexcept {
    store_unaligned8(p, v.lo);
    store_unaligned8(p + 8, v.hi);
}
PSY_FORCEINLINE f32x16 broadcast16(psynder::f32 s) noexcept {
    return f32x16{broadcast8(s), broadcast8(s)};
}
PSY_FORCEINLINE psynder::f32 reduce_add16(f32x16 a) noexcept {
    return reduce_add8(a.lo) + reduce_add8(a.hi);
}
#endif

}  // namespace psynder::simd
