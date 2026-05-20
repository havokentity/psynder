// SPDX-License-Identifier: MIT
// Psynder — PCG RNG. Ported from dmonte (see DESIGN.md §5: borrowed PCG).
// Small, standalone, deterministic, well-understood. Used by stratified
// shadow-ray sampling, lightmap bake stratification, debug spawn placement.

#pragma once

#include "core/Types.h"

namespace psynder::math {

struct Rng {
    u64 state = 0x853c49e6748fea9bULL;
    u64 inc = 0xda3e39cb94b95bdbULL;

    constexpr void seed(u64 s, u64 stream = 1) noexcept {
        state = 0;
        inc = (stream << 1u) | 1u;
        (void)next_u32();
        state += s;
        (void)next_u32();
    }

    constexpr u32 next_u32() noexcept {
        u64 old = state;
        state = old * 6364136223846793005ULL + inc;
        u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
    }

    constexpr f32 next_f32() noexcept {
        return (next_u32() >> 8) * (1.0f / static_cast<f32>(1u << 24));
    }

    constexpr f32 in_range(f32 lo, f32 hi) noexcept { return lo + (hi - lo) * next_f32(); }
};

}  // namespace psynder::math
