// SPDX-License-Identifier: MIT
// Psynder physics unit tests — GJK-distance speculative contacts for convex
// pairs (box-box / capsule-box), closing the anti-tunnelling gap the closed-
// form separation kernels left open (Wave 8).
//
// kernel_pair_separation previously returned +inf for any pair without a
// closed-form distance (box-box, capsule-box, ...), so those pairs got NO
// speculative contact and a fast box could tunnel through another box in one
// sub-tick. A from-scratch GJK distance sub-algorithm (gjk_distance, Gilbert-
// Johnson-Keerthi 1988 / Ericson RTCD 9.5) now supplies the real gap + the
// separating normal for the SEPARATED case, while touching/penetrating pairs
// still return +inf -> the unchanged overlap (EPA) path, so resting stacks stay
// byte-identical.
//
// Coverage:
//   * kernel_pair_separation correctness: known box-box gaps (axis-aligned +
//     rotated) and capsule-box; overlapping boxes report "not separated" (the
//     +inf overlap path).
//   * kernel_collide_pair_spec emits a speculative contact for a fast CLOSING
//     box pair and none for a slow separated one.
//   * World-level: a fast box fired at a thin static box never crosses it.
//   * No regression: penetrating boxes take the +inf overlap path, and a box
//     resting on a static box settles without fall-through or phantom energy.

#include "physics/internal/Kernels.h"
#include "physics/Physics.h"
#include "physics/FpControl.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

// Internal Body in the box shape (code 2). Velocity/rotation default to rest.
Body make_box(math::Vec3 pos,
              math::Vec3 half,
              math::Quat rot = {0, 0, 0, 1},
              math::Vec3 vel = {0, 0, 0},
              f32 mass = 1.0f) {
    Body b{};
    b.position = pos;
    b.rotation = rot;
    b.linear_velocity = vel;
    b.shape = 2;  // box
    b.half_extent = half;
    b.mass = mass;
    b.inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
    // Solid-box inertia; the exact value is irrelevant to the distance /
    // speculation kernels under test, but keep it finite + invertible.
    const f32 ix = (1.0f / 3.0f) * mass * (half.y * half.y + half.z * half.z);
    const f32 iy = (1.0f / 3.0f) * mass * (half.x * half.x + half.z * half.z);
    const f32 iz = (1.0f / 3.0f) * mass * (half.x * half.x + half.y * half.y);
    b.inertia.local = {ix, iy, iz};
    b.inertia.inv_local =
        (mass > 0.0f) ? math::Vec3{1.0f / ix, 1.0f / iy, 1.0f / iz} : math::Vec3{0, 0, 0};
    return b;
}

// Internal Body in the capsule shape (code 1): half_extent.x = radius,
// half_extent.y = half-height (segment along local Y).
Body make_capsule(math::Vec3 pos, f32 radius, f32 half_height) {
    Body b{};
    b.position = pos;
    b.rotation = {0, 0, 0, 1};
    b.shape = 1;  // capsule
    b.half_extent = {radius, half_height, 0.0f};
    b.mass = 1.0f;
    b.inv_mass = 1.0f;
    b.inertia.local = {1.0f, 1.0f, 1.0f};
    b.inertia.inv_local = {1.0f, 1.0f, 1.0f};
    return b;
}

constexpr f32 kInf = std::numeric_limits<f32>::infinity();

}  // namespace

// ─── kernel_pair_separation: GJK distance correctness ────────────────────

TEST_CASE("GJK distance: two axis-aligned boxes report the exact gap + normal",
          "[physics][gjk][tunneling]") {
    // Unit boxes (half 0.5) at x = 0 and x = 3 -> surfaces at 0.5 and 2.5 -> a
    // 2.0 m gap along +x.
    Body a = make_box({0, 0, 0}, {0.5f, 0.5f, 0.5f});
    Body b = make_box({3, 0, 0}, {0.5f, 0.5f, 0.5f});
    Contact c{};
    const f32 sep = kernels::kernel_pair_separation(a, b, c);
    REQUIRE(sep == Approx(2.0f).margin(1e-3f));
    REQUIRE(c.normal_world.x == Approx(1.0f).margin(1e-3f));  // A -> B along +x
    REQUIRE(std::fabs(c.normal_world.y) < 1e-3f);
    REQUIRE(std::fabs(c.normal_world.z) < 1e-3f);
    REQUIRE(c.depth == Approx(-2.0f).margin(1e-3f));  // depth == -separation
}

TEST_CASE("GJK distance: overlapping boxes are NOT separated (overlap path, +inf)",
          "[physics][gjk][tunneling]") {
    // Centres 0.5 apart, each half 0.5 -> the boxes overlap. The separation
    // kernel must return +inf so the caller falls through to the unchanged
    // overlap (EPA) path -> resting stacks stay byte-identical.
    Body a = make_box({0, 0, 0}, {0.5f, 0.5f, 0.5f});
    Body b = make_box({0.5f, 0, 0}, {0.5f, 0.5f, 0.5f});
    Contact c{};
    REQUIRE(kernels::kernel_pair_separation(a, b, c) == kInf);
}

TEST_CASE("GJK distance: a 45deg-rotated box pair reports the corner gap",
          "[physics][gjk][tunneling]") {
    // Box A axis-aligned at origin; box B rotated 45deg about Z at x = 3. B's
    // nearest feature toward -x is now a CORNER poking out to x = 3 - 0.5*sqrt2,
    // so the gap is smaller than the flat-face 2.0: 3 - 0.5 - 0.5*sqrt2 ~ 1.793.
    const math::Quat r45 = math::quat_from_axis_angle({0, 0, 1}, math::kHalfPi * 0.5f);
    Body a = make_box({0, 0, 0}, {0.5f, 0.5f, 0.5f});
    Body b = make_box({3, 0, 0}, {0.5f, 0.5f, 0.5f}, r45);
    Contact c{};
    const f32 sep = kernels::kernel_pair_separation(a, b, c);
    const f32 expected = 3.0f - 0.5f - 0.5f * std::sqrt(2.0f);  // ~1.793
    REQUIRE(sep == Approx(expected).margin(3e-2f));
    REQUIRE(sep < 2.0f);                // closer than the axis-aligned face gap
    REQUIRE(c.normal_world.x > 0.9f);   // separating normal still ~ +x
}

TEST_CASE("GJK distance: a separated capsule-box pair reports a positive gap",
          "[physics][gjk][tunneling]") {
    // Capsule (r 0.5, axis +Y) at origin -> +x surface at x = 0.5. Box (half
    // 0.5) at x = 3 -> -x surface at x = 2.5. Gap along +x = 2.0.
    Body cap = make_capsule({0, 0, 0}, 0.5f, 1.0f);
    Body box = make_box({3, 0, 0}, {0.5f, 0.5f, 0.5f});
    Contact c{};
    const f32 sep = kernels::kernel_pair_separation(cap, box, c);
    REQUIRE(sep == Approx(2.0f).margin(5e-2f));
    REQUIRE(c.normal_world.x > 0.9f);
}

// ─── kernel_collide_pair_spec: speculative contact gating ────────────────

TEST_CASE("kernel_collide_pair_spec emits a speculative contact for a fast closing box pair",
          "[physics][gjk][tunneling]") {
    const f32 dt = 1.0f / 120.0f;
    const f32 margin = kernels::kSpeculativeMargin;

    // Two unit boxes with a 1.0 m gap (surfaces at 0.5 and 2.0 -> wait, B at
    // x = 2 -> -x surface at 1.5 -> gap 1.0), closing at 120 m/s each. One tick
    // closes 2.0 m >> the 1.0 m gap, so without the speculative contact they
    // would pass clean through; the GJK gap must trip the speculative path.
    Body a = make_box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {120.0f, 0, 0});
    Body b = make_box({2, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {-120.0f, 0, 0});
    Contact c{};
    REQUIRE(kernels::kernel_collide_pair_spec(a, b, dt, margin, c));
    REQUIRE(c.speculative);
    REQUIRE(c.separation == Approx(1.0f).margin(1e-2f));  // gap = 2 - 0.5 - 0.5
    REQUIRE(c.depth == Approx(0.0f).margin(1e-6f));       // speculative: no penetration
}

TEST_CASE("a slow well-separated box pair gets NO speculative contact",
          "[physics][gjk][tunneling]") {
    const f32 dt = 1.0f / 120.0f;
    const f32 margin = kernels::kSpeculativeMargin;

    // Same geometry, closing at only 1 m/s: one tick closes ~8 mm << the 1.0 m
    // gap, so no contact (no spurious impulse on a distant pair).
    Body a = make_box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {1.0f, 0, 0});
    Body b = make_box({2, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {-1.0f, 0, 0});
    Contact c{};
    REQUIRE_FALSE(kernels::kernel_collide_pair_spec(a, b, dt, margin, c));

    // Receding fast pair: also no contact (only CLOSING pairs can tunnel).
    Body ra = make_box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {-120.0f, 0, 0});
    Body rb = make_box({2, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 0, 1}, {120.0f, 0, 0});
    Contact rc{};
    REQUIRE_FALSE(kernels::kernel_collide_pair_spec(ra, rb, dt, margin, rc));
}

// ─── World-level: a fast box fired at a thin static box does NOT tunnel ───

TEST_CASE("a fast box fired at a thin static box never crosses it (no tunnelling)",
          "[physics][gjk][tunneling]") {
    FpGuard fp;
    physics::World world;
    world.set_gravity({0, 0, 0});  // isolate the wall interaction

    // Thin static wall box at x = 0: 0.2 m thick in x, large in y/z.
    physics::BodyDesc wall{};
    wall.shape = physics::Shape::Box;
    wall.mass = 0.0f;  // static
    wall.position = {0, 0, 0};
    wall.half_extent = {0.1f, 5.0f, 5.0f};
    world.create_body(wall);

    // Small fast box from the -x side at 120 m/s -> 1 m/tick, ~5x the wall
    // thickness; without the speculative GJK gap it would be on the +x side
    // after the first step.
    physics::BodyDesc bx{};
    bx.shape = physics::Shape::Box;
    bx.mass = 1.0f;
    bx.position = {-0.6f, 0, 0};
    bx.half_extent = {0.1f, 0.1f, 0.1f};
    const physics::BodyId bullet = world.create_body(bx);
    world.set_body_velocity(bullet, {120.0f, 0, 0});

    // Watch every tick: the bullet's centre must NEVER reach the far (+x) side
    // of the wall (its +x surface is at x = 0.1; centre past 0.2 means it
    // tunnelled). Without the fix it sails to x ~ +120.
    bool tunnelled = false;
    for (int i = 0; i < 120; ++i) {  // 1 s
        world.step(1.0f / 120.0f);
        if (world.get_position(bullet).x > 0.2f)
            tunnelled = true;
    }
    REQUIRE_FALSE(tunnelled);
    REQUIRE(world.get_position(bullet).x < 0.2f);
}

// ─── No regression: box-box resting contact is unaffected ────────────────

TEST_CASE("a box resting on a static box settles (no fall-through, no phantom launch)",
          "[physics][gjk][tunneling]") {
    FpGuard fp;
    physics::World world;
    world.set_gravity({0, -9.81f, 0});

    // Static ground box, top surface at y = 0.5.
    physics::BodyDesc ground{};
    ground.shape = physics::Shape::Box;
    ground.mass = 0.0f;
    ground.position = {0, 0, 0};
    ground.half_extent = {5.0f, 0.5f, 5.0f};
    world.create_body(ground);

    // Dynamic box (half 0.25) dropped just above -> rest centre y = 0.5 + 0.25.
    physics::BodyDesc box{};
    box.shape = physics::Shape::Box;
    box.mass = 1.0f;
    box.position = {0, 1.0f, 0};
    box.half_extent = {0.25f, 0.25f, 0.25f};
    const physics::BodyId id = world.create_body(box);

    for (int i = 0; i < 600; ++i)  // 5 s settle
        world.step(1.0f / 120.0f);

    const math::Vec3 p = world.get_position(id);
    REQUIRE(p.y > 0.5f);   // did not fall through the ground box
    REQUIRE(p.y < 0.95f);  // settled near rest (~0.75); no phantom energy launch
}
