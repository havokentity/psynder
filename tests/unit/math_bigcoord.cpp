// SPDX-License-Identifier: MIT
// Psynder — Lane 02 Wave B tests: BigCoord driver + Pacejka monotonicity.

#include "math/BigCoord.h"
#include "math/Pacejka.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::math;

namespace {
bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}
}  // namespace

TEST_CASE("BigCoord no-snap when camera inside trigger_radius", "[math][bigcoord]") {
    BigCoordWorld w{};
    REQUIRE(approx_eq(w.trigger_radius, 1024.0f));

    // Camera at ~500 m: comfortably inside the 1024 m radius.
    Vec3 cam{300, -200, 400};  // |cam| = sqrt(90000+40000+160000) ≈ 538 m
    Vec3 shift = w.snap_to_camera(cam);

    REQUIRE(approx_eq(shift.x, 0.0f));
    REQUIRE(approx_eq(shift.y, 0.0f));
    REQUIRE(approx_eq(shift.z, 0.0f));
    REQUIRE(approx_eq(w.origin.x, 0.0f));
    REQUIRE(approx_eq(w.origin.y, 0.0f));
    REQUIRE(approx_eq(w.origin.z, 0.0f));
}

TEST_CASE("BigCoord snaps when camera drifts past trigger_radius", "[math][bigcoord]") {
    BigCoordWorld w{};

    // Camera at +3000 m on x: well beyond the 1024 m trigger radius.
    Vec3 cam{3000, 0, 0};
    Vec3 shift = w.snap_to_camera(cam);

    // Snap fired: x-axis got nudged onto the nearest cell.
    REQUIRE_FALSE(approx_eq(shift.x, 0.0f));
    REQUIRE(approx_eq(shift.y, 0.0f));
    REQUIRE(approx_eq(shift.z, 0.0f));

    // After snapping, the camera should sit inside one cell of the new
    // origin (cell size = 2 × radius = 2048 m). cam.x − origin.x ≤ cell.
    f32 cell = 2.0f * w.trigger_radius;
    REQUIRE(std::fabs(cam.x - w.origin.x) <= cell);

    // The new origin lies on the cell grid.
    REQUIRE(approx_eq(std::fmod(w.origin.x, cell), 0.0f, 1e-2f));

    // Idempotent: snapping again with the same camera does nothing.
    Vec3 shift2 = w.snap_to_camera(cam);
    REQUIRE(approx_eq(shift2.x, 0.0f));
    REQUIRE(approx_eq(shift2.y, 0.0f));
    REQUIRE(approx_eq(shift2.z, 0.0f));
}

TEST_CASE("Pacejka combined force is monotone in slip_ratio over the linear range",
          "[math][pacejka]") {
    // With B=10, C=1.9, the magic-formula peak sits around s ≈ 0.16
    // (the saturation slip). Below that, |F(s)| is strictly increasing
    // in |s|. We sweep a small range [0.0, 0.10] in 5 steps and require
    // monotone growth in the longitudinal magnitude.
    //
    // Use pure longitudinal slip (slip_y = 0) so the combined-force
    // resultant collapses to the per-axis magic formula on slip_x.
    const f32 load_N = 4000.0f;  // ~400 kgf per wheel
    const f32 mu = 1.0f;

    f32 prev_mag = -1.0f;
    for (int i = 0; i <= 5; ++i) {
        f32 s = 0.02f * static_cast<f32>(i);  // 0.00 .. 0.10
        Vec2 f = pacejka_combined_force(s, 0.0f, load_N, mu);
        f32 mag = std::sqrt(f.x * f.x + f.y * f.y);

        // Strictly greater than the previous step (and exactly 0 at s=0).
        if (i == 0) {
            REQUIRE(approx_eq(mag, 0.0f));
        } else {
            REQUIRE(mag > prev_mag);
        }
        prev_mag = mag;
    }

    // Sanity: the slip-ratio helper agrees with the SAE convention.
    // Pure rolling (ω·r == v) ⇒ slip ratio zero.
    f32 sr_rolling = pacejka_slip_ratio(/*ω*/ 10.0f, /*r*/ 0.3f, /*v*/ 3.0f);
    REQUIRE(approx_eq(sr_rolling, 0.0f));

    // Driving (ω·r > v) ⇒ positive slip ratio.
    f32 sr_drive = pacejka_slip_ratio(/*ω*/ 12.0f, /*r*/ 0.3f, /*v*/ 3.0f);
    REQUIRE(sr_drive > 0.0f);
}
