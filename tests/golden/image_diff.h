// SPDX-License-Identifier: MIT
// Psynder — minimal RGB8 image-diff utility, factored out so the golden
// runner and the lane-25 unit test can share the same comparator semantics.
// Header-only, no third-party deps.

#pragma once

#include "core/Types.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace psynder::testing {

struct Image {
    u32              width  = 0;
    u32              height = 0;
    std::vector<u8>  rgb;   // size = width*height*3, packed [R,G,B,R,G,B,...]
};

struct CompareResult {
    bool   sizes_match    = false;
    usize  mismatch_count = 0;
    usize  total_pixels   = 0;
    f64    mismatch_pct() const noexcept {
        return total_pixels == 0 ? 0.0
                                 : 100.0 * static_cast<f64>(mismatch_count)
                                         / static_cast<f64>(total_pixels);
    }
};

// A pixel mismatches when ANY channel differs by more than channel_eps.
// The default of 8 lets sRGB/quantization rounding noise slide while still
// flagging real regressions (a single-pixel hue shift > 8/255 is visible).
inline CompareResult compare_images(const Image& a, const Image& b,
                                    u32 channel_eps = 8) noexcept {
    CompareResult r{};
    if (a.width != b.width || a.height != b.height) return r;
    r.sizes_match  = true;
    r.total_pixels = static_cast<usize>(a.width) * a.height;
    for (usize i = 0; i < r.total_pixels; ++i) {
        const i32 dr = static_cast<i32>(a.rgb[i*3+0]) - static_cast<i32>(b.rgb[i*3+0]);
        const i32 dg = static_cast<i32>(a.rgb[i*3+1]) - static_cast<i32>(b.rgb[i*3+1]);
        const i32 db = static_cast<i32>(a.rgb[i*3+2]) - static_cast<i32>(b.rgb[i*3+2]);
        if (static_cast<u32>(std::abs(dr)) > channel_eps ||
            static_cast<u32>(std::abs(dg)) > channel_eps ||
            static_cast<u32>(std::abs(db)) > channel_eps) {
            ++r.mismatch_count;
        }
    }
    return r;
}

}  // namespace psynder::testing
