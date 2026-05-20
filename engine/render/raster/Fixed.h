// SPDX-License-Identifier: MIT
// Psynder — Q24.8 fixed-point edge math, lane-07-local. Lane 02 will publish
// a global FxQ24_8 in math/Math.h when it lands the rest of the math lane;
// until then this header carries the type and the ops the rasterizer needs.
// Keeping the type lane-local also means hot-path inlining is guaranteed.
//
// 1/256 sub-pixel precision (DESIGN.md §7.3, §7.4) — kills the texture
// swimming common in old renderers. 24-bit integer range gives ±8 388 608
// pixels of headroom, plenty for any internal resolution we'll ever ship.

#pragma once

#include "core/Types.h"

#include <cmath>
#include <cstdint>

namespace psynder::render::raster {

inline constexpr i32 kSubPixelBits = 8;
inline constexpr i32 kSubPixelScale = 1 << kSubPixelBits;  // 256
inline constexpr i32 kSubPixelMask = kSubPixelScale - 1;

// Fixed-point Q24.8 scalar. Stored as int32 — top 24 bits integer, low 8
// bits fraction. Wraps at ±2^23 pixels; any input outside that range gets
// clipped by the caller upstream of triangle setup.
struct FxQ24_8 {
    i32 v = 0;

    constexpr FxQ24_8() noexcept = default;
    constexpr explicit FxQ24_8(i32 raw) noexcept : v(raw) {}

    static inline FxQ24_8 from_float(f32 x) noexcept {
        // Round-to-nearest. f32 can exactly represent any int that fits in
        // 24 bits, so the rounded fixed value is exact for any input that
        // fits the rasterizer's range.
        return FxQ24_8{static_cast<i32>(std::lround(x * static_cast<f32>(kSubPixelScale)))};
    }
    static inline FxQ24_8 from_int(i32 x) noexcept { return FxQ24_8{x << kSubPixelBits}; }

    inline f32 to_float() const noexcept {
        return static_cast<f32>(v) * (1.0f / static_cast<f32>(kSubPixelScale));
    }
    inline i32 floor_to_int() const noexcept { return v >> kSubPixelBits; }
    inline i32 ceil_to_int() const noexcept { return (v + kSubPixelMask) >> kSubPixelBits; }
};

PSY_FORCEINLINE FxQ24_8 operator+(FxQ24_8 a, FxQ24_8 b) noexcept {
    return FxQ24_8{a.v + b.v};
}
PSY_FORCEINLINE FxQ24_8 operator-(FxQ24_8 a, FxQ24_8 b) noexcept {
    return FxQ24_8{a.v - b.v};
}
PSY_FORCEINLINE FxQ24_8 operator-(FxQ24_8 a) noexcept {
    return FxQ24_8{-a.v};
}

PSY_FORCEINLINE bool operator==(FxQ24_8 a, FxQ24_8 b) noexcept {
    return a.v == b.v;
}
PSY_FORCEINLINE bool operator!=(FxQ24_8 a, FxQ24_8 b) noexcept {
    return a.v != b.v;
}
PSY_FORCEINLINE bool operator<(FxQ24_8 a, FxQ24_8 b) noexcept {
    return a.v < b.v;
}
PSY_FORCEINLINE bool operator<=(FxQ24_8 a, FxQ24_8 b) noexcept {
    return a.v <= b.v;
}
PSY_FORCEINLINE bool operator>(FxQ24_8 a, FxQ24_8 b) noexcept {
    return a.v > b.v;
}
PSY_FORCEINLINE bool operator>=(FxQ24_8 a, FxQ24_8 b) noexcept {
    return a.v >= b.v;
}

// 2D edge cross product, full 64-bit intermediate. Edge function for the
// directed line (a → b) evaluated at point p:
//
//     E(p) = (b.x - a.x)(p.y - a.y) - (b.y - a.y)(p.x - a.x)
//
// Positive on the left of the directed edge for CCW front-facing triangles
// in our right-handed coordinate system (DESIGN.md §10.1). The result is
// Q48.16 (two Q24.8s multiplied) — we keep it as i64.
PSY_FORCEINLINE i64 edge_func(FxQ24_8 ax, FxQ24_8 ay, FxQ24_8 bx, FxQ24_8 by, FxQ24_8 px, FxQ24_8 py) noexcept {
    const i64 dx = static_cast<i64>(bx.v) - static_cast<i64>(ax.v);
    const i64 dy = static_cast<i64>(by.v) - static_cast<i64>(ay.v);
    const i64 qx = static_cast<i64>(px.v) - static_cast<i64>(ax.v);
    const i64 qy = static_cast<i64>(py.v) - static_cast<i64>(ay.v);
    return dx * qy - dy * qx;
}

// Signed 2× triangle area in Q48.16. Sign is positive for CCW front-facing.
PSY_FORCEINLINE i64
tri_area_2x(FxQ24_8 ax, FxQ24_8 ay, FxQ24_8 bx, FxQ24_8 by, FxQ24_8 cx, FxQ24_8 cy) noexcept {
    return edge_func(ax, ay, bx, by, cx, cy);
}

// Top-left fill convention. For each edge (a → b), an edge sample is
// "inside" when E > 0, OR (E == 0 and the edge is a top or left edge).
// Returns the bias to add to E so the comparison is just `>= 0`.
PSY_FORCEINLINE i64 top_left_bias(FxQ24_8 ax, FxQ24_8 ay, FxQ24_8 bx, FxQ24_8 by) noexcept {
    // Top edge:  by == ay && bx <  ax  (horizontal, points left)
    // Left edge: by <  ay                (general edge going up)
    // Both classifications use the convention from the rasterizer chapter
    // of the Larrabee / Olano-Greer top-left rule.
    const bool top = (by.v == ay.v) && (bx.v < ax.v);
    const bool left = (by.v < ay.v);
    return (top || left) ? 0 : -1;
}

}  // namespace psynder::render::raster
