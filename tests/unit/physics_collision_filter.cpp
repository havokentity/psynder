// SPDX-License-Identifier: MIT
// Psynder physics unit tests -- per-pair COLLISION FILTERING (ADR-020).
//
// The body carries a (collision_group, collision_mask) pair. Two bodies A,B are
// ALLOWED to collide iff EACH body's mask accepts the OTHER's group:
//     (A.mask & B.group) != 0  &&  (B.mask & A.group) != 0
// The broadphase pair-acceptance (run_narrowphase's pair loop in World.cpp)
// applies this BEFORE narrowphase, so a filtered-out pair never becomes a
// contact and costs nothing downstream. DEFAULT for both fields is all-ones
// (collide with everything), so a body that never sets a filter behaves exactly
// as before.
//
// This file proves:
//   * the header predicate collision_filter_allows is correct + symmetric;
//   * two overlapping dynamic boxes in the SAME no-collide group generate NO
//     contact and pass through / rest independently, while the SAME pair in the
//     default group resolves (push apart) -- the filter is what changed it;
//   * a body in the default group still collides with everything (no regression);
//   * the public set_collision_filter setter has the same effect as the desc;
//   * the filtered sim is deterministic run-to-run (bit-for-bit);
//   * a TIGHT ragdoll (gaps removed, self-collision filtered) still settles
//     stable on the ground (finite, above ground, joints hold, energy bounded)
//     -- proving the filter fixed the self-fight.
//
// Header-level predicate test pulls the kernel from internal/Body.h (no linkage);
// the world-level tests drive the public physics API (linked transitively into
// psynder_unit, same as physics_world_isolation.cpp + physics_ragdoll.cpp).

#include "physics/Body.h"
#include "physics/Physics.h"
#include "physics/Ragdoll.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using Catch::Approx;

namespace {

constexpr f32 kDt = 1.0f / 120.0f;
const math::Quat kIdent{0, 0, 0, 1};

void step_ticks(physics::World& w, int n) {
    for (int i = 0; i < n; ++i)
        w.step(kDt);
}

// A unit dynamic box at `pos` with the given filter words.
physics::BodyId make_box(physics::World& w,
                         math::Vec3 pos,
                         u32 group = 0xFFFFFFFFu,
                         u32 mask = 0xFFFFFFFFu) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Box;
    d.mass = 1.0f;
    d.position = pos;
    d.rotation = kIdent;
    d.half_extent = {0.5f, 0.5f, 0.5f};
    d.collision_group = group;
    d.collision_mask = mask;
    return w.create_body(d);
}

// Static infinite floor at y = 0 (+Y face up).
physics::BodyId make_ground(physics::World& w) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Plane;
    d.mass = 0.0f;
    d.position = {0.0f, 0.0f, 0.0f};
    d.rotation = kIdent;
    d.friction = 0.8f;
    return w.create_body(d);
}

}  // namespace

// ─── Header predicate: correctness + symmetry ──────────────────────────────
TEST_CASE("collision_filter_allows: default all-ones collides with everything",
          "[physics][filter][predicate]") {
    using physics::detail::Body;
    using physics::detail::collision_filter_allows;

    Body a{};  // default group/mask == all-ones
    Body b{};
    REQUIRE(collision_filter_allows(a, b));
    REQUIRE(collision_filter_allows(b, a));  // symmetric

    // One body in a single-bit group, the other still default: default mask
    // (all-ones) accepts the single bit, and the single-bit body's default mask
    // (all-ones) accepts the other's group -> they still collide.
    a.collision_group = 1u << 3;
    a.collision_mask = 0xFFFFFFFFu;
    REQUIRE(collision_filter_allows(a, b));
    REQUIRE(collision_filter_allows(b, a));
}

TEST_CASE("collision_filter_allows: shared no-collide group rejects, others pass",
          "[physics][filter][predicate]") {
    using physics::detail::Body;
    using physics::detail::collision_filter_allows;

    constexpr u32 kSelf = 1u << 30;

    // Two bodies in the SAME self-collide group: group == kSelf, mask clears it.
    Body x{};
    x.collision_group = kSelf;
    x.collision_mask = ~kSelf;
    Body y = x;

    // x vs y: (x.mask & y.group) == (~kSelf & kSelf) == 0 -> rejected, both ways.
    REQUIRE_FALSE(collision_filter_allows(x, y));
    REQUIRE_FALSE(collision_filter_allows(y, x));  // symmetric

    // x vs a DEFAULT body (the "world"): world.group has the kSelf bit AND every
    // other bit; x.mask (~kSelf) still shares every OTHER bit with world.group,
    // and world.mask (all-ones) accepts x.group (kSelf). So they DO collide.
    Body world{};
    REQUIRE(collision_filter_allows(x, world));
    REQUIRE(collision_filter_allows(world, x));

    // Two DISTINCT single-bit groups whose masks each exclude the other's bit
    // never collide; flipping one mask bit re-enables it (symmetry holds).
    Body p{};
    p.collision_group = 1u << 1;
    p.collision_mask = ~(1u << 2);
    Body q{};
    q.collision_group = 1u << 2;
    q.collision_mask = ~(1u << 1);
    REQUIRE_FALSE(collision_filter_allows(p, q));
    REQUIRE_FALSE(collision_filter_allows(q, p));
    q.collision_mask = 0xFFFFFFFFu;  // q now accepts p's group...
    REQUIRE_FALSE(collision_filter_allows(p, q));  // ...but p still rejects q's
    p.collision_mask = 0xFFFFFFFFu;  // now both accept
    REQUIRE(collision_filter_allows(p, q));
    REQUIRE(collision_filter_allows(q, p));
}

// ─── World-level: same group passes through, default group resolves ─────────
TEST_CASE("two overlapping boxes in the same no-collide group do not interact",
          "[physics][filter][world]") {
    constexpr u32 kSelf = 1u << 30;

    // Helper: run two overlapping boxes (centres 0.6 m apart, so 0.4 m of
    // overlap on a unit box) with NO gravity for a fixed time and report the
    // final centre-to-centre X separation. With NO contact the boxes stay put
    // (0.6); with a contact they push apart (separation grows past the box width).
    auto final_separation = [](u32 group, u32 mask) {
        physics::World w;
        w.set_gravity({0.0f, 0.0f, 0.0f});  // isolate the contact response
        physics::BodyId a = make_box(w, {-0.30f, 5.0f, 0.0f}, group, mask);
        physics::BodyId b = make_box(w, {0.30f, 5.0f, 0.0f}, group, mask);
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        step_ticks(w, 240);  // 2 s
        return std::fabs(w.get_position(b).x - w.get_position(a).x);
    };

    // DEFAULT group (all-ones): the boxes overlap, generate a contact, and the
    // solver pushes them apart -- the separation grows well past the start.
    const f32 sep_default = final_separation(0xFFFFFFFFu, 0xFFFFFFFFu);
    REQUIRE(sep_default > 0.7f);

    // SAME no-collide group: the broadphase rejects the pair, no contact is ever
    // generated, so the boxes stay exactly where they were placed (no force, no
    // drift) -- they pass through / rest independently.
    const f32 sep_filtered = final_separation(kSelf, ~kSelf);
    REQUIRE(sep_filtered == Approx(0.6f).margin(1e-5f));

    // The filter is what changed the outcome.
    REQUIRE(sep_filtered < sep_default);
}

// ─── World-level: a default body still collides with everything ─────────────
TEST_CASE("a default-group body still collides (no regression)",
          "[physics][filter][world][regression]") {
    physics::World w;
    w.set_gravity({0.0f, -9.81f, 0.0f});
    make_ground(w);

    // A default box dropped onto the static plane must REST on it (the floor
    // collision is unaffected by the new filter path). Box half-height 0.5, so a
    // resting centre sits at ~0.5.
    physics::BodyId box = make_box(w, {0.0f, 3.0f, 0.0f});
    REQUIRE(box.valid());
    step_ticks(w, 480);  // 4 s to settle
    const f32 y = w.get_position(box).y;
    REQUIRE(y > 0.45f);
    REQUIRE(y < 0.60f);

    // And a filtered box (self-group) dropped the same way ALSO rests on the
    // world plane -- self-filter only excludes same-group bodies, never the
    // world. Proves limb-vs-world still collides.
    constexpr u32 kSelf = 1u << 30;
    physics::BodyId fbox = make_box(w, {3.0f, 3.0f, 0.0f}, kSelf, ~kSelf);
    REQUIRE(fbox.valid());
    step_ticks(w, 480);
    const f32 fy = w.get_position(fbox).y;
    REQUIRE(fy > 0.45f);
    REQUIRE(fy < 0.60f);
}

// ─── World-level: public setter matches the desc path ───────────────────────
TEST_CASE("set_collision_filter has the same effect as the desc filter",
          "[physics][filter][world][setter]") {
    constexpr u32 kSelf = 1u << 30;

    physics::World w;
    w.set_gravity({0.0f, 0.0f, 0.0f});
    // Create both boxes in the DEFAULT group, then opt them out via the setter.
    physics::BodyId a = make_box(w, {-0.30f, 5.0f, 0.0f});
    physics::BodyId b = make_box(w, {0.30f, 5.0f, 0.0f});
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    w.set_collision_filter(a, kSelf, ~kSelf);
    w.set_collision_filter(b, kSelf, ~kSelf);

    step_ticks(w, 240);
    const f32 sep = std::fabs(w.get_position(b).x - w.get_position(a).x);
    REQUIRE(sep == Approx(0.6f).margin(1e-5f));  // never interacted

    // A stale handle is a harmless no-op (does not throw / corrupt).
    physics::BodyId stale{};
    w.set_collision_filter(stale, 0u, 0u);
}

// ─── World-level: filtered sim is deterministic run-to-run ──────────────────
TEST_CASE("filtered collision sim is deterministic run to run",
          "[physics][filter][determinism]") {
    constexpr u32 kSelf = 1u << 30;

    auto run = [](std::vector<math::Vec3>& out) {
        physics::World w;
        w.set_gravity({0.0f, -9.81f, 0.0f});
        make_ground(w);
        // A mix: two same-group boxes (skip each other) + one default box that
        // collides with both -- exercises both the accept and the reject branch.
        physics::BodyId s0 = make_box(w, {0.0f, 1.0f, 0.0f}, kSelf, ~kSelf);
        physics::BodyId s1 = make_box(w, {0.2f, 2.0f, 0.0f}, kSelf, ~kSelf);
        physics::BodyId d0 = make_box(w, {0.1f, 3.5f, 0.0f});
        step_ticks(w, 600);
        out = {w.get_position(s0), w.get_position(s1), w.get_position(d0)};
    };

    std::vector<math::Vec3> a, b;
    run(a);
    run(b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        // Bit-for-bit: identical inputs + deterministic (-fno-fast-math) physics
        // + integer-only filter must reproduce EXACTLY.
        REQUIRE(a[i].x == b[i].x);
        REQUIRE(a[i].y == b[i].y);
        REQUIRE(a[i].z == b[i].z);
    }
}

// ─── Ragdoll: a TIGHT, self-filtered rag settles stable ─────────────────────
//
// This is the consumer the filter was built for. The default humanoid is now
// authored TIGHT (segments touch at every joint; the old inter-segment gaps are
// gone) and build_ragdoll stamps every segment with the shared self-collide
// group. We prove the rag still settles cleanly: finite, above ground, joints
// hold, energy bounded -- i.e. the self-collision filter removed the limb-fight
// that the gaps used to prevent.
namespace {

const math::Quat kTilt{0.0f, 0.0f, 0.5646424f, 0.8253356f};  // ~65 deg about +Z

f32 min_body_y(physics::World& w, const physics::Ragdoll& rag) {
    f32 lo = 1e30f;
    for (physics::BodyId id : rag.bodies)
        if (id.valid())
            lo = std::min(lo, w.get_position(id).y);
    return lo;
}

bool all_finite(physics::World& w, const physics::Ragdoll& rag) {
    for (physics::BodyId id : rag.bodies) {
        if (!id.valid())
            continue;
        math::Vec3 p = w.get_position(id);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
        if (std::fabs(p.x) > 1e4f || std::fabs(p.y) > 1e4f || std::fabs(p.z) > 1e4f)
            return false;
    }
    return true;
}

math::Vec3 world_anchor(physics::World& w, physics::BodyId id, math::Vec3 local) {
    math::Quat q = w.get_rotation(id);
    math::Vec3 qv{q.x, q.y, q.z};
    math::Vec3 t = math::mul(math::cross(qv, local), 2.0f);
    math::Vec3 rotated = math::add(local, math::add(math::mul(t, q.w), math::cross(qv, t)));
    return math::add(w.get_position(id), rotated);
}

// Largest parent/child joint-anchor separation across all inbound joints (m).
f32 max_anchor_gap(physics::World& w,
                   const physics::RagdollDesc& desc,
                   const physics::Ragdoll& rag) {
    f32 m = 0.0f;
    for (usize i = 0; i < desc.segments.size(); ++i) {
        const auto& s = desc.segments[i];
        if (s.parent == physics::kRagdollRoot || s.parent >= rag.bodies.size())
            continue;
        physics::BodyId pid = rag.bodies[s.parent];
        physics::BodyId cid = rag.bodies[i];
        if (!pid.valid() || !cid.valid())
            continue;
        math::Vec3 on_parent = world_anchor(w, pid, s.joint_local);
        math::Vec3 on_child = world_anchor(w, cid, s.child_joint_local);
        m = std::max(m, math::length(math::sub(on_child, on_parent)));
    }
    return m;
}

}  // namespace

TEST_CASE("tight self-filtered ragdoll settles stable on the ground",
          "[physics][filter][ragdoll][settle]") {
    physics::World world;
    world.set_gravity({0.0f, -9.81f, 0.0f});
    make_ground(world);

    physics::RagdollDesc desc = physics::default_humanoid();
    REQUIRE(desc.size() == 7);

    physics::Ragdoll rag;
    physics::RagdollId id = physics::build_ragdoll(
        world, desc, kTilt, {0.0f, 0.9f, 0.0f}, {1.5f, 0.0f, 0.0f}, rag);
    REQUIRE(id.valid());
    REQUIRE(rag.body_count() == 7);

    // The TIGHT skeleton's pins are coincident at spawn (anchors name one point
    // on each touching surface, no authored gap between the surfaces).
    REQUIRE(max_anchor_gap(world, desc, rag) < 0.02f);

    // Simulate ~4.5 s of the death-ragdoll loop. With self-collision filtered,
    // the overlapping tight limbs do NOT fight each other; the rag stays finite
    // and the joints hold the whole way down + at rest.
    const u32 steps = static_cast<u32>(4.5f / kDt);
    f32 worst_gap = 0.0f;
    for (u32 s = 0; s < steps; ++s) {
        world.step(kDt);
        physics::solve_ragdoll(world, rag, kDt);
        REQUIRE(all_finite(world, rag));
        worst_gap = std::max(worst_gap, max_anchor_gap(world, desc, rag));
    }

    // Joints held the WHOLE run (no limb-fight blew a pin apart).
    REQUIRE(worst_gap < 0.05f);

    // Settled ABOVE the ground (nothing sank through the plane).
    REQUIRE(min_body_y(world, rag) > -0.05f);

    // Came to rest: over a late window the pelvis barely moves -- a fighting
    // self-collision would keep it jittering instead.
    math::Vec3 before = world.get_position(rag.bodies[0]);
    for (u32 s = 0; s < 60; ++s) {
        world.step(kDt);
        physics::solve_ragdoll(world, rag, kDt);
    }
    math::Vec3 after = world.get_position(rag.bodies[0]);
    REQUIRE(math::length(math::sub(after, before)) < 0.05f);

    physics::destroy_ragdoll(world, rag);
}
