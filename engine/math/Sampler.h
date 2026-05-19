// SPDX-License-Identifier: MIT
// Psynder — sampling helpers built on top of the frozen Rng. Lane 02.
//
// The `Rng` (PCG) lives in Rng.h. Wave A asked for a `StratifiedSampler2D`
// helper "in Rng.h", but Rng.h is part of the frozen public surface (see
// wave-a-bar.md §1). The helper lives here, in an internal header, and
// callers wanting stratified sampling include both:
//
//     #include "math/Rng.h"
//     #include "math/Sampler.h"
//
// What it does: split [0,1)² into `nx × ny` cells, then return one sample
// per cell — jittered inside the cell. Used by:
//   - shadow ray stratification (DESIGN.md §8.2 — "low-discrepancy
//     4-sample stratified pattern across frames")
//   - lightmap bake (DESIGN.md §10 — `lm_bake`'s primary-ray distribution)
//   - debug spawn placement.
//
// One sampler instance carries an Rng + a cursor; you can `reset(nx, ny)`
// it between dispatches.

#pragma once

#include "Rng.h"

namespace psynder::math {

struct Sample2D { f32 u, v; };

struct StratifiedSampler2D {
    Rng rng{};
    u32 nx = 1;        // strata per row
    u32 ny = 1;        // strata per column
    u32 cursor = 0;    // next stratum index, walks 0 .. nx*ny - 1

    constexpr void reset(u32 nx_, u32 ny_) noexcept {
        nx = nx_ == 0 ? 1u : nx_;
        ny = ny_ == 0 ? 1u : ny_;
        cursor = 0;
    }

    constexpr u32 count() const noexcept { return nx * ny; }

    // Pull the next sample. When `cursor == count()` we wrap around — the
    // pattern repeats but with fresh jitter, so the caller can keep asking
    // without falling off the end of an array.
    constexpr Sample2D next() noexcept {
        u32 idx = cursor;
        cursor = (cursor + 1) % count();

        u32 ix = idx % nx;
        u32 iy = idx / nx;

        f32 jx = rng.next_f32();
        f32 jy = rng.next_f32();

        f32 inv_nx = 1.0f / static_cast<f32>(nx);
        f32 inv_ny = 1.0f / static_cast<f32>(ny);

        return Sample2D{
            (static_cast<f32>(ix) + jx) * inv_nx,
            (static_cast<f32>(iy) + jy) * inv_ny,
        };
    }

    // Fill an output buffer with a complete `nx*ny` stratified pattern in
    // a single call. Useful for the bake path which wants the whole set
    // up-front. The buffer must have room for `count()` samples; callers
    // typically allocate `kMaxSamples` from a scratch arena.
    constexpr void fill(Sample2D* out, u32 capacity) noexcept {
        u32 n = count();
        if (capacity < n) n = capacity;
        cursor = 0;
        for (u32 i = 0; i < n; ++i) out[i] = next();
    }
};

}  // namespace psynder::math
