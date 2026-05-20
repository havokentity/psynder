// SPDX-License-Identifier: MIT
// Psynder physics unit tests — character controller stance machine + stair
// step (Wave B).
//
// Header-only: the stance kernel and the stair-step climb kernel both live
// in `physics/internal/Kernels.h`. Stair-step is templated on an overlap
// predicate, so the test fabricates an axis-aligned-box "obstacle" and an
// axis-aligned "floor" so the predicate is trivially cheap and the
// expected behaviour is exactly predictable.
//
// DESIGN.md §10.1 — character controller polish.

#include "physics/internal/Kernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

// ─── Stance state machine ────────────────────────────────────────────────

TEST_CASE("Stance: water environment overrides all player intent", "[physics][character][stance]") {
    // Water trumps both crouch and prone — you can't lay down in water,
    // the character pinches to swim height.
    auto next =
        kernels::kernel_char_next_stance(kernels::CharStanceK::Stand,
                                         kernels::CharIntent{true, true, false, /*in_water*/ true});
    REQUIRE(next == kernels::CharStanceK::Water);
}

TEST_CASE("Stance: ladder beats prone but not water", "[physics][character][stance]") {
    auto land =
        kernels::kernel_char_next_stance(kernels::CharStanceK::Prone,
                                         kernels::CharIntent{false, true, /*near_ladder*/ true, false});
    REQUIRE(land == kernels::CharStanceK::Ladder);
    auto water = kernels::kernel_char_next_stance(kernels::CharStanceK::Ladder,
                                                  kernels::CharIntent{false, false, true, true});
    REQUIRE(water == kernels::CharStanceK::Water);
}

TEST_CASE("Stance: crouch button transitions Stand → Crouch", "[physics][character][stance]") {
    auto next = kernels::kernel_char_next_stance(kernels::CharStanceK::Stand,
                                                 kernels::CharIntent{true, false, false, false});
    REQUIRE(next == kernels::CharStanceK::Crouch);
}

TEST_CASE("Stance: prone request from Prone state keeps you prone", "[physics][character][stance]") {
    auto next = kernels::kernel_char_next_stance(kernels::CharStanceK::Prone,
                                                 kernels::CharIntent{false, true, false, false});
    REQUIRE(next == kernels::CharStanceK::Prone);
}

TEST_CASE("Stance: prone → stand requires intermediate crouch step",
          "[physics][character][stance]") {
    // Releasing all stance modifiers while prone should drop us into
    // Crouch first, not all the way to Stand — that's the FPS feel.
    auto next = kernels::kernel_char_next_stance(kernels::CharStanceK::Prone,
                                                 kernels::CharIntent{false, false, false, false});
    REQUIRE(next == kernels::CharStanceK::Crouch);
}

TEST_CASE("Stance: prone height shorter than crouch, crouch shorter than stand",
          "[physics][character][stance]") {
    f32 h = 1.8f;
    REQUIRE(kernels::kernel_char_height_for_stance(kernels::CharStanceK::Prone, h) <
            kernels::kernel_char_height_for_stance(kernels::CharStanceK::Crouch, h));
    REQUIRE(kernels::kernel_char_height_for_stance(kernels::CharStanceK::Crouch, h) <
            kernels::kernel_char_height_for_stance(kernels::CharStanceK::Stand, h));
    REQUIRE(kernels::kernel_char_height_for_stance(kernels::CharStanceK::Stand, h) ==
            Approx(h).margin(1e-6f));
}

// ─── Stair-step climb-up ─────────────────────────────────────────────────

namespace {

// A minimal world: a floor at y=0 and a 0.3 m step at x>=1. The character
// at (0, 0, 0) heading +x should climb the step.
struct StepWorld {
    bool operator()(math::Vec3 p) const {
        // Floor at y=0 (anything below is inside the floor).
        if (p.y < 0.0f)
            return true;
        // Step block: 1 <= x <= 5, 0 <= y < step_top, anywhere in z.
        if (p.x >= 1.0f && p.x <= 5.0f && p.y < step_top)
            return true;
        return false;
    }
    f32 step_top = 0.30f;
};

}  // namespace

TEST_CASE("Stair-step climbs over a 0.30 m step when step_height = 0.35 m",
          "[physics][character][stair-step]") {
    StepWorld world{/*step_top=*/0.30f};

    math::Vec3 origin{0.5f, 0.05f, 0.0f};  // standing on the floor
    math::Vec3 horiz{0.8f, 0.0f, 0.0f};    // walk forward 0.8 m
    f32 step_height = 0.35f;

    math::Vec3 result = kernels::kernel_stair_step_climb(origin, horiz, step_height, world);

    // After climbing: x advanced into the step zone; y rests on the step's
    // top surface (probe drops body until it bumps the new floor).
    REQUIRE(result.x > 1.0f);
    REQUIRE(result.x == Approx(origin.x + horiz.x).margin(1e-3f));
    REQUIRE(result.y >= world.step_top);
    REQUIRE(result.y <= world.step_top + 0.05f);
}

TEST_CASE("Stair-step refuses a step that is taller than step_height",
          "[physics][character][stair-step]") {
    StepWorld tall{/*step_top=*/0.50f};

    math::Vec3 origin{0.5f, 0.05f, 0.0f};
    math::Vec3 horiz{0.8f, 0.0f, 0.0f};
    f32 step_height = 0.35f;

    math::Vec3 result = kernels::kernel_stair_step_climb(origin, horiz, step_height, tall);

    // Body refuses the climb — stays at origin.
    REQUIRE(result.x == Approx(origin.x).margin(1e-6f));
    REQUIRE(result.y == Approx(origin.y).margin(1e-6f));
}

TEST_CASE("Stair-step passes through open space without modification",
          "[physics][character][stair-step]") {
    // If there's no obstacle, the kernel should pass-through and return
    // origin + horiz (the elevated re-sweep clears, drop probe finds floor).
    StepWorld world{/*step_top=*/0.30f};
    math::Vec3 origin{-2.0f, 0.05f, 0.0f};  // far before the step
    math::Vec3 horiz{0.5f, 0.0f, 0.0f};     // moves to x = -1.5
    f32 step_height = 0.35f;

    math::Vec3 result = kernels::kernel_stair_step_climb(origin, horiz, step_height, world);
    REQUIRE(result.x == Approx(-1.5f).margin(1e-3f));
}
