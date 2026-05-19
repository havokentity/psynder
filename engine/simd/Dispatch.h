// SPDX-License-Identifier: MIT
// Psynder — SIMD runtime dispatch. Lane 03 internal API.
//
// Per-lane intrinsics in Simd_internal.h compile to the widest path the
// translation unit was built for. For routines big enough to amortize a
// function-pointer indirection (e.g. reduce_add over a 1 MB buffer, mass
// blend during tile coverage emission), we expose a runtime-tiered entry
// point: the dispatcher inspects `psynder::hardware::detect()` at startup
// and points the slot at the kernel matching the host's widest available
// ISA.
//
// The four tiers we care about, in widening order:
//   Tier::Scalar    — no SIMD, plain C++ loop (only used if intrinsics off)
//   Tier::Sse42     — x86 baseline
//   Tier::Avx2      — AVX2 + FMA
//   Tier::Avx512    — AVX-512F/BW/VL (opportunistic)
//   Tier::Neon      — aarch64 fixed (no widening on Apple Silicon)
//
// `current_tier()` is a cheap atomic-load — callers may query it per-frame
// without measurable cost.

#pragma once

#include "Simd.h"

#include "core/Types.h"

namespace psynder::simd {

enum class Tier : psynder::u8 {
    Scalar  = 0,
    Sse42   = 1,
    Avx2    = 2,
    Avx512  = 3,
    Neon    = 4,
};

// Initialise dispatch state. Idempotent — calling repeatedly is safe and
// cheap (subsequent invocations short-circuit on a once-flag). Must be
// invoked before any of the tier-dispatched batched routines below; the
// auto-init via __attribute__((constructor)) in Simd.cpp also covers this.
void dispatch_init() noexcept;

// Currently-selected tier. After dispatch_init() this reflects the widest
// kernel available on the host CPU; before init it returns Tier::Scalar.
Tier current_tier() noexcept;

const char* tier_name(Tier t) noexcept;

// ─── Batched routines that benefit from runtime tier selection ──────────
// Compile-time-resolved per-lane ops live in Simd_internal.h. The routines
// here are the ones where a function-pointer indirection is comfortably
// hidden by the size of the inner loop.

// reduce_add over n floats. n need not be a multiple of any width.
psynder::f32 reduce_add(const psynder::f32* p, psynder::usize n) noexcept;

// y[i] = a[i] + b[i]. Aliasing is fine but undefined for partial overlap.
void add_buffer(const psynder::f32* a, const psynder::f32* b,
                psynder::f32* y, psynder::usize n) noexcept;

// y[i] = a[i] * b[i] + c[i]. Uses FMA where available.
void fma_buffer(const psynder::f32* a, const psynder::f32* b,
                const psynder::f32* c, psynder::f32* y,
                psynder::usize n) noexcept;

}  // namespace psynder::simd
