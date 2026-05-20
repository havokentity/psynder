// SPDX-License-Identifier: MIT
// Psynder — Lane 03 SIMD prefetch helpers. Header-only.
//
// Wraps the compiler intrinsics so call sites don't need to fork on
// __builtin_prefetch (Clang/GCC) vs _mm_prefetch (MSVC) every time they
// touch the streaming read paths. The four temporal-locality "tiers"
// map onto the canonical x86 prefetch instructions and onto the GCC
// `__builtin_prefetch` locality argument identically:
//
//   t0  — fetch into all caches (highest locality, will be reused soon)
//   t1  — skip L1, fetch into L2/L3
//   t2  — skip L1 and L2, fetch into LLC only
//   nta — non-temporal: fill but mark for replacement (one-shot read)
//
// `prefetch_range` walks a contiguous span on `stride` (default 64,
// one cache line) and issues a t0 prefetch at each step. It's tuned for
// the raster tile-coverage loops and the raytracer ray-payload sweeps:
// neither benefits from going wider, and both want every line touched.
//
// All helpers are noexcept and accept `const void*`; null is well-defined
// (the underlying intrinsic specs make prefetching a possibly-bogus
// address a no-op rather than a fault on every supported target).

#pragma once

#include "core/Types.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif
#endif

namespace psynder::simd {

PSY_FORCEINLINE void prefetch_t0(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, /*rw=*/0, /*locality=*/3);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
    (void)p;
#endif
}

PSY_FORCEINLINE void prefetch_t1(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, /*rw=*/0, /*locality=*/2);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T1);
#else
    (void)p;
#endif
}

PSY_FORCEINLINE void prefetch_t2(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, /*rw=*/0, /*locality=*/1);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T2);
#else
    (void)p;
#endif
}

PSY_FORCEINLINE void prefetch_nta(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, /*rw=*/0, /*locality=*/0);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_NTA);
#else
    (void)p;
#endif
}

// Walk a contiguous range and fire a t0 prefetch every `stride` bytes.
// `stride` defaults to a 64-byte cache line — the size of every x86-64
// and aarch64 (Apple Silicon, Cortex-A) line we target. Bytes is the
// *unconsumed* length we want resident; the helper rounds up to the
// stride so the tail line is always covered.
PSY_FORCEINLINE void prefetch_range(const void* p,
                                    psynder::usize bytes,
                                    psynder::usize stride = 64) noexcept {
    if (p == nullptr || bytes == 0 || stride == 0)
        return;
    const auto* cur = static_cast<const psynder::u8*>(p);
    const auto* end = cur + bytes;
    for (; cur < end; cur += stride)
        prefetch_t0(cur);
}

}  // namespace psynder::simd
