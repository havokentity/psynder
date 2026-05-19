// SPDX-License-Identifier: MIT
// Psynder — Pacejka magic-formula tire helpers. Lane 02, Wave B.
//
// Lane 12 (physics) drives the racing/vehicle sim from per-wheel slip
// inputs. The classical formulation we want is Pacejka's "magic formula"
//
//     F(s) = D · sin( C · atan( B·s − E·(B·s − atan(B·s)) ) )
//
// where:
//   B   — stiffness factor
//   C   — shape factor
//   D   — peak force (typically μ·load_N)
//   E   — curvature factor (near 1; controls fall-off after peak)
//   s   — combined slip (slip ratio for longitudinal, slip angle for
//         lateral, or √(sx²+sy²) for combined)
//
// We expose two helpers:
//
//   `pacejka_slip_ratio()`     — kinematic slip ratio from wheel angular
//                                velocity, wheel radius, and ground speed.
//                                Returns the dimensionless ratio the
//                                magic formula consumes on the
//                                longitudinal axis.
//
//   `pacejka_combined_force()` — magic formula evaluated for combined
//                                longitudinal / lateral slip, returning
//                                the planar force vector (Fx, Fy) in
//                                newtons. The returned vector points
//                                opposite to the slip vector — i.e. the
//                                tire force that *opposes* slip.
//
// Defaults (B=10, C=1.9, D=μ·load, E=0.97) match the textbook starting
// values for a passenger-car summer tire on dry asphalt. Lane 12 will
// almost certainly tune per-vehicle but the defaults give a sensible
// out-of-the-box feel and are what the unit tests pin against.
//
// Header-only: pure arithmetic, no behavior worth a TU, and physics wants
// to inline these inside its per-wheel update.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::math {

// Default magic-formula coefficients (passenger-car summer tire, dry).
inline constexpr f32 kPacejkaB = 10.0f;
inline constexpr f32 kPacejkaC = 1.9f;
inline constexpr f32 kPacejkaE = 0.97f;

// Longitudinal slip ratio from kinematic inputs.
//
// `wheel_omega_rps`  — wheel angular velocity (radians/s).
// `wheel_radius_m`   — effective rolling radius (metres).
// `ground_speed_mps` — speed of the wheel hub over ground along the
//                     wheel's heading (metres/s). For typical use this is
//                     the longitudinal component of chassis velocity at
//                     the wheel attachment.
//
// Convention: SAE-style slip ratio s = (ω·r − v) / max(|v|, ε), so a
// positive slip ratio means the tire is *driving* (ω·r > v); a negative
// slip ratio means braking (ω·r < v). The ε floor avoids a divide-by-zero
// at standstill and just decays smoothly toward zero as speed → 0.
inline f32 pacejka_slip_ratio(f32 wheel_omega_rps,
                              f32 wheel_radius_m,
                              f32 ground_speed_mps) noexcept {
    // Floor on |v| so the ratio is well-defined and bounded near rest.
    // 0.1 m/s ≈ 0.4 km/h is well below typical sim minima; below that
    // the sim should be in a static-friction handover anyway.
    constexpr f32 kEps = 0.1f;
    f32 v = std::fabs(ground_speed_mps);
    f32 denom = v > kEps ? v : kEps;
    f32 omega_r = wheel_omega_rps * wheel_radius_m;
    return (omega_r - ground_speed_mps) / denom;
}

// Evaluate the magic formula on a single slip axis.
//
// Returns the scalar force magnitude for slip `s` given peak `D` and
// shape coefficients `B`, `C`, `E`. The result has the same sign as `s`
// (force opposing slip; the caller flips the sign on consumption).
inline f32 pacejka_axis_force(f32 s, f32 D,
                              f32 B = kPacejkaB,
                              f32 C = kPacejkaC,
                              f32 E = kPacejkaE) noexcept {
    f32 Bs = B * s;
    f32 inner = Bs - E * (Bs - std::atan(Bs));
    return D * std::sin(C * std::atan(inner));
}

// Combined-slip planar tire force, magic-formula version.
//
// `slip_x` — longitudinal slip ratio (dimensionless).
// `slip_y` — lateral slip (radians for slip angle, or matching units).
// `load_N` — vertical wheel load in newtons (Fz, always ≥ 0).
// `mu`     — friction coefficient between tire and ground (≥ 0).
//
// Returns (Fx, Fy) in newtons. The returned vector points in the slip
// direction's *opposing* sense — magnitude follows the magic-formula
// envelope and is monotone in |slip| over the linear range (|s| < ~0.1
// for typical B/C/D/E), peaking at the saturation slip and then falling
// off slightly toward the asymptote.
//
// Combined-slip handling: we project the per-axis force onto the
// normalized slip vector so the resultant vector magnitude tracks the
// magic formula evaluated on the combined slip magnitude. This is the
// "friction circle" approximation; lane 12 may swap in a more accurate
// combined-slip model later.
inline Vec2 pacejka_combined_force(f32 slip_x,
                                   f32 slip_y,
                                   f32 load_N,
                                   f32 mu) noexcept {
    // Peak force D = μ·Fz, clamped at zero for the no-load/no-mu edge.
    f32 D = mu * load_N;
    if (D <= 0.0f) return Vec2{0.0f, 0.0f};

    f32 s_mag = std::sqrt(slip_x * slip_x + slip_y * slip_y);
    if (s_mag <= 0.0f) return Vec2{0.0f, 0.0f};

    // Magnitude follows magic formula on the combined slip magnitude.
    f32 F_mag = pacejka_axis_force(s_mag, D);

    // Distribute along the slip vector direction. The minus sign means
    // the force opposes the slip vector (Newton's third on the tire).
    f32 inv_s = 1.0f / s_mag;
    return Vec2{
        -F_mag * slip_x * inv_s,
        -F_mag * slip_y * inv_s,
    };
}

}  // namespace psynder::math
