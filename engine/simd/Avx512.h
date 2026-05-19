// SPDX-License-Identifier: MIT
// Psynder — Lane 03 AVX-512 scaffolding. Header-only.
//
// Wave-B parks the *structural* surface for the future AVX-512 wide path:
// the `f32x16` pack forward-declaration (gated on __AVX512F__ so a binary
// built without the flag won't accidentally pick up an unusable type) and
// the runtime `has_avx512f()` probe that consults the existing CPU-feature
// detector. The full operator set — add/sub/mul/fma/reduce — is deferred
// to a later wave; the dispatcher already routes through
// `current_tier() == Tier::Avx512` so calls land on the AVX2 widest-path
// kernel today.
//
// `has_avx512f()` is a thin wrapper around `psynder::hardware::detect()`
// rather than a separate cpuid probe — keeping the detection logic in one
// place means the rest of the engine and the dispatcher always agree on
// what the host can do.

#pragma once

#include "Simd.h"

#include "core/Types.h"
#include "core/hardware/CpuFeatures.h"

namespace psynder::simd {

#if defined(__AVX512F__)
// 16-wide f32 pack. Backed by `__m512` on x86 when the binary is compiled
// with -mavx512f / /arch:AVX512. The operator set lands in a follow-up
// wave; for now the type exists so client code can forward-declare members
// of this shape (e.g. raytracer ray packets) without forcing a rebuild
// when those kernels arrive.
struct f32x16 {
    __m512 v;
};
#endif

// Runtime probe: does the *current host* support AVX-512F?
//
// Returns false unconditionally on aarch64 and on x86 hosts where the
// detector didn't see the AVX512F bit. The compile-time __AVX512F__
// gate is *not* consulted — a binary built without the flag can still
// honestly report what the host could run, even though this binary won't
// use it. (The dispatcher in Dispatch.cpp consults both compile-time
// and runtime conditions before issuing AVX-512 kernels.)
PSY_FORCEINLINE bool has_avx512f() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return psynder::hardware::detect().avx512f;
#else
    return false;
#endif
}

}  // namespace psynder::simd
