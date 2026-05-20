// SPDX-License-Identifier: MIT
// Psynder — Q24.8 signed fixed-point. Lane 02.
//
// Used by the tiled rasterizer's edge functions (DESIGN.md §7.3 — "exact
// coverage, sub-pixel precision 1/256 px"). Integer-only arithmetic keeps
// the edge walk bit-deterministic across platforms; we deliberately do NOT
// pull in libm here.
//
// Range: ±8,388,607.996 (i32 / 256). Sufficient for screen-space coords on
// any sane resolution we ship — the rasterizer never sees coords past a few
// thousand pixels. Don't use Q24.8 for world coordinates; that's `f32` per
// ADR-005.

#pragma once

#include "core/Types.h"

#include <type_traits>

namespace psynder::math {

struct FxQ24_8 {
    static constexpr i32 kShift = 8;
    static constexpr i32 kOne = 1 << kShift;  // 256
    static constexpr f32 kInv = 1.0f / static_cast<f32>(kOne);

    i32 raw = 0;

    // ─── Construction ────────────────────────────────────────────────────
    constexpr FxQ24_8() noexcept = default;

    static constexpr FxQ24_8 from_raw(i32 r) noexcept {
        FxQ24_8 q;
        q.raw = r;
        return q;
    }

    static constexpr FxQ24_8 from_int(i32 v) noexcept { return from_raw(v * kOne); }

    // Truncating f32 → Q24.8 conversion. Round-to-nearest variant is
    // available as `from_f32_round`. Truncation is the rasterizer default
    // because it's what matches edge-function convention.
    static constexpr FxQ24_8 from_f32(f32 v) noexcept {
        return from_raw(static_cast<i32>(v * static_cast<f32>(kOne)));
    }

    static constexpr FxQ24_8 from_f32_round(f32 v) noexcept {
        // Branchless round-to-nearest, half-away-from-zero.
        f32 scaled = v * static_cast<f32>(kOne);
        f32 bias = scaled >= 0.0f ? 0.5f : -0.5f;
        return from_raw(static_cast<i32>(scaled + bias));
    }

    // ─── Conversion back to scalars ──────────────────────────────────────
    constexpr f32 to_f32() const noexcept { return static_cast<f32>(raw) * kInv; }
    constexpr i32 to_int_trunc() const noexcept { return raw >> kShift; }

    // ─── Arithmetic ──────────────────────────────────────────────────────
    constexpr FxQ24_8 operator+(FxQ24_8 o) const noexcept { return from_raw(raw + o.raw); }
    constexpr FxQ24_8 operator-(FxQ24_8 o) const noexcept { return from_raw(raw - o.raw); }
    constexpr FxQ24_8 operator-() const noexcept { return from_raw(-raw); }

    // Multiplication widens to i64 to avoid the intermediate overflow that
    // would silently corrupt the result for any non-trivial pair (the
    // product of two Q24.8 values is Q48.16 before the shift). The shift
    // is arithmetic — sign-preserving — even on the negative path.
    constexpr FxQ24_8 operator*(FxQ24_8 o) const noexcept {
        i64 wide = static_cast<i64>(raw) * static_cast<i64>(o.raw);
        return from_raw(static_cast<i32>(wide >> kShift));
    }

    // Division shifts the numerator left first so the quotient retains
    // sub-pixel precision. Returns 0 on divide-by-zero (rasterizer never
    // divides by an edge it already rejected, but defensive is cheap).
    constexpr FxQ24_8 operator/(FxQ24_8 o) const noexcept {
        if (o.raw == 0)
            return from_raw(0);
        i64 wide = (static_cast<i64>(raw) << kShift) / static_cast<i64>(o.raw);
        return from_raw(static_cast<i32>(wide));
    }

    constexpr FxQ24_8& operator+=(FxQ24_8 o) noexcept {
        raw += o.raw;
        return *this;
    }
    constexpr FxQ24_8& operator-=(FxQ24_8 o) noexcept {
        raw -= o.raw;
        return *this;
    }
    constexpr FxQ24_8& operator*=(FxQ24_8 o) noexcept {
        *this = *this * o;
        return *this;
    }
    constexpr FxQ24_8& operator/=(FxQ24_8 o) noexcept {
        *this = *this / o;
        return *this;
    }

    // ─── Comparison ──────────────────────────────────────────────────────
    constexpr bool operator==(FxQ24_8 o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(FxQ24_8 o) const noexcept { return raw != o.raw; }
    constexpr bool operator<(FxQ24_8 o) const noexcept { return raw < o.raw; }
    constexpr bool operator<=(FxQ24_8 o) const noexcept { return raw <= o.raw; }
    constexpr bool operator>(FxQ24_8 o) const noexcept { return raw > o.raw; }
    constexpr bool operator>=(FxQ24_8 o) const noexcept { return raw >= o.raw; }
};

static_assert(std::is_trivially_copyable_v<FxQ24_8>, "Q24.8 must be POD");
static_assert(sizeof(FxQ24_8) == 4, "Q24.8 is a thin wrapper over i32");

// Free-function constexpr converter — sometimes nicer at call sites than
// the static method (e.g. inside template metaprograms). Mirrors the
// truncating semantics of `FxQ24_8::from_f32`.
constexpr FxQ24_8 fx(f32 v) noexcept {
    return FxQ24_8::from_f32(v);
}

}  // namespace psynder::math
