// SPDX-License-Identifier: MIT
// Psynder — Lane 03 streaming (non-temporal) store helpers. Header-only.
//
// Non-temporal stores bypass the cache hierarchy, writing straight to
// memory through the write-combining buffers. We use them in the hot
// "produce a tile and never re-read it" paths: raster tile binning,
// raytracer hitbuf write-out, occlusion buffer dumps. Skipping the
// cache fills keeps the LLC from getting polluted with values we
// already know we won't touch again this frame.
//
// On x86 we map to `_mm_stream_ps` (the canonical 16-byte NT store).
// On aarch64 NEON has no architecturally non-temporal store — every
// vst1q variant goes through the cache. We approximate the contract
// by issuing the regular vst1q_f32 and then trusting the caller to
// pair it with `stream_fence()` if it needs the store globally visible
// before subsequent loads (the same pairing x86 callers need with
// `_mm_sfence`).
//
// The fence is a full store-store barrier:
//   - x86:        `_mm_sfence`
//   - aarch64:    `dmb ishst` via `__dmb(_ARM64_BARRIER_ISHST)` /
//                 `__asm__("dmb ishst")` on Clang.
// On anything else the fence collapses to a `std::atomic_thread_fence`
// release — close enough for the scalar fallback that has no streaming
// store to fence anyway.

#pragma once

#include "Simd.h"

#include "core/Types.h"

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64)
#   include <intrin.h>
#endif

#include <atomic>

namespace psynder::simd {

// Streaming (non-temporal) store of a 4-wide f32 vector. The destination
// pointer must be 16-byte-aligned on x86 — `_mm_stream_ps` faults under
// SSE if it isn't. aarch64 has no alignment requirement here but we
// preserve the contract so callers stay portable.
PSY_FORCEINLINE void stream_store_f32x4(psynder::f32* dst, f32x4 v) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_stream_ps(dst, v.v);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // NEON has no architectural NT store; vst1q_f32 is the closest match
    // and the cache pollution is the cost we pay on aarch64. Callers that
    // care still want stream_fence() to make the write globally visible.
    vst1q_f32(dst, v.v);
#else
    for (int i = 0; i < 4; ++i) dst[i] = v.v[i];
#endif
}

// Store-store fence. Pair after a batch of stream_store_* before any
// reader on another core may observe the buffer.
PSY_FORCEINLINE void stream_fence() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();
#elif defined(__aarch64__) || defined(_M_ARM64)
#   if defined(_MSC_VER) && !defined(__clang__)
    __dmb(_ARM64_BARRIER_ISHST);
#   else
    __asm__ __volatile__("dmb ishst" ::: "memory");
#   endif
#else
    std::atomic_thread_fence(std::memory_order_release);
#endif
}

}  // namespace psynder::simd
