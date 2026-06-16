// SPDX-License-Identifier: MIT
// Psynder physics unit tests -- articulated RAGDOLL primitive (Ragdoll.h).
//
// Drives the *public* path end to end on an owned World: build a humanoid
// ragdoll, drop it onto a static ground plane, and each frame run
//     world.step(dt); solve_ragdoll(world, rag, dt);
// (exactly how a game's death-ragdoll would run). We assert the rag:
//   * stays FINITE (no NaN/Inf, no positional blow-up);
//   * SETTLES on the ground (all bodies come to rest above the plane);
//   * stays CONNECTED (adjacent linked bodies' joint anchors track within a
//     small tolerance over the whole sim -- the joints hold);
//   * does NOT gain energy (total KE is bounded and trends down at rest);
//   * is DETERMINISTIC (same spawn reproduces bit-for-bit run to run);
//   * TEARS DOWN cleanly (destroy frees every body; rebuild from baseline).
//
// solve_joints lives in psynder_physics, which the unit binary links
// transitively (same as physics_world_isolation.cpp's create/step path).

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
// A ~65 deg tilt about +Z so the figure spawns leaning and collapses onto its
// side (the canonical death-ragdoll pose) rather than balancing upright first.
// q = (0, 0, sin(theta/2), cos(theta/2)) with theta = 1.2 rad.
const math::Quat kTilt{0.0f, 0.0f, 0.5646424f, 0.8253356f};

// Static infinite floor at y = 0 (the +Y face points up).
physics::BodyId make_ground(physics::World& w) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Plane;
    d.mass = 0.0f;  // static
    d.position = {0.0f, 0.0f, 0.0f};
    d.rotation = kIdent;
    d.friction = 0.8f;
    return w.create_body(d);
}

// Lowest body-centre Y over all ragdoll segments.
f32 min_body_y(physics::World& w, const physics::Ragdoll& rag) {
    f32 lo = 1e30f;
    for (physics::BodyId id : rag.bodies) {
        if (!id.valid())
            continue;
        f32 y = w.get_position(id).y;
        lo = std::min(lo, y);
    }
    return lo;
}

bool all_finite(physics::World& w, const physics::Ragdoll& rag) {
    for (physics::BodyId id : rag.bodies) {
        if (!id.valid())
            continue;
        math::Vec3 p = w.get_position(id);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
        // Positional blow-up guard: a stable ragdoll never flings a limb to
        // astronomical coordinates even before NaN sets in.
        if (std::fabs(p.x) > 1e4f || std::fabs(p.y) > 1e4f || std::fabs(p.z) > 1e4f)
            return false;
    }
    return true;
}

// World-space joint-anchor positions for the i-th segment's inbound joint:
// the anchor on the PARENT and the anchor on the CHILD. They must track each
// other (the ball-socket pin keeps them coincident).
struct AnchorPair {
    math::Vec3 on_parent;
    math::Vec3 on_child;
    bool valid = false;
};

math::Vec3 world_anchor(physics::World& w, physics::BodyId id, math::Vec3 local) {
    math::Quat q = w.get_rotation(id);
    // q * local (qualified math rotate via the same algebra the solver uses).
    // Rotate a vector by a quaternion: v + 2*cross(qv, cross(qv,v) + qw*v).
    math::Vec3 qv{q.x, q.y, q.z};
    math::Vec3 t = math::mul(math::cross(qv, local), 2.0f);
    math::Vec3 rotated = math::add(local, math::add(math::mul(t, q.w), math::cross(qv, t)));
    return math::add(w.get_position(id), rotated);
}

AnchorPair anchor_pair(physics::World& w,
                       const physics::RagdollDesc& desc,
                       const physics::Ragdoll& rag,
                       usize i) {
    AnchorPair ap{};
    const auto& s = desc.segments[i];
    if (s.parent == physics::kRagdollRoot || s.parent >= rag.bodies.size())
        return ap;
    physics::BodyId pid = rag.bodies[s.parent];
    physics::BodyId cid = rag.bodies[i];
    if (!pid.valid() || !cid.valid())
        return ap;
    ap.on_parent = world_anchor(w, pid, s.joint_local);
    ap.on_child = world_anchor(w, cid, s.child_joint_local);
    ap.valid = true;
    return ap;
}

// Largest parent/child anchor separation across all inbound joints (m).
f32 max_anchor_gap(physics::World& w,
                   const physics::RagdollDesc& desc,
                   const physics::Ragdoll& rag) {
    f32 m = 0.0f;
    for (usize i = 0; i < desc.segments.size(); ++i) {
        AnchorPair ap = anchor_pair(w, desc, rag, i);
        if (!ap.valid)
            continue;
        m = std::max(m, math::length(math::sub(ap.on_child, ap.on_parent)));
    }
    return m;
}

}  // namespace

TEST_CASE("ragdoll drops, settles on a plane, and stays connected",
          "[physics][ragdoll][settle]") {
    physics::World world;
    world.set_gravity({0.0f, -9.81f, 0.0f});
    make_ground(world);

    physics::RagdollDesc desc = physics::default_humanoid();
    REQUIRE(desc.size() == 7);

    physics::Ragdoll rag;
    // Spawn leaning over, just above the floor, with a small sideways velocity:
    // the rag falls onto its side and settles (a death-ragdoll collapse).
    physics::RagdollId id = physics::build_ragdoll(
        world, desc, kTilt, {0.0f, 0.9f, 0.0f}, {1.5f, 0.0f, 0.0f}, rag);
    REQUIRE(id.valid());
    REQUIRE(rag.body_count() == 7);
    REQUIRE(rag.alive());

    // The pins are coincident at spawn (every joint anchor pair names one point).
    REQUIRE(max_anchor_gap(world, desc, rag) < 0.02f);

    // Simulate ~4.5 s: step the world, then solve the ragdoll joints, exactly
    // as a game's death-ragdoll loop would. Track the worst joint separation.
    const u32 steps = static_cast<u32>(4.5f / kDt);
    f32 worst_gap = 0.0f;
    for (u32 s = 0; s < steps; ++s) {
        world.step(kDt);
        physics::solve_ragdoll(world, rag, kDt);
        REQUIRE(all_finite(world, rag));
        worst_gap = std::max(worst_gap, max_anchor_gap(world, desc, rag));
    }

    // Connected the WHOLE run: no joint ever separated by more than a small
    // tolerance (the ball-socket pin held under the drop, impact + contact).
    REQUIRE(worst_gap < 0.05f);

    // Settled ABOVE the ground: every body centre is above the floor (minus a
    // small penetration allowance), i.e. nothing sank through the plane.
    REQUIRE(min_body_y(world, rag) > -0.05f);

    // Came to rest: over a late window the pelvis barely moves.
    math::Vec3 before = world.get_position(rag.bodies[0]);
    for (u32 s = 0; s < 30; ++s) {
        world.step(kDt);
        physics::solve_ragdoll(world, rag, kDt);
    }
    math::Vec3 after = world.get_position(rag.bodies[0]);
    REQUIRE(math::length(math::sub(after, before)) < 0.05f);

    physics::destroy_ragdoll(world, rag);
}

TEST_CASE("ragdoll does not gain energy (KE bounded, decreasing at rest)",
          "[physics][ragdoll][energy]") {
    physics::World world;
    world.set_gravity({0.0f, -9.81f, 0.0f});
    make_ground(world);

    physics::RagdollDesc desc = physics::default_humanoid();
    physics::Ragdoll rag;
    // Spawn LOW and leaning so it collapses onto its side near the plane: the
    // test measures the joint+contact interaction energy, not a huge drop.
    physics::build_ragdoll(world, desc, kTilt, {0.0f, 0.6f, 0.0f},
                           {0.4f, 0.0f, 0.0f}, rag);

    // Proxy for total kinetic energy: sum of body speed^2 (mass-weighting is not
    // needed for "bounded + decaying" -- the relative trend is what matters and
    // the solver damps every joint-touched body). We read speed via successive
    // position deltas so we don't need internal velocity access.
    auto sample_speed2 = [&](std::vector<math::Vec3>& prev) {
        f32 sum = 0.0f;
        for (usize i = 0; i < rag.bodies.size(); ++i) {
            math::Vec3 p = world.get_position(rag.bodies[i]);
            math::Vec3 v = math::mul(math::sub(p, prev[i]), 1.0f / kDt);
            sum += math::dot(v, v);
            prev[i] = p;
        }
        return sum;
    };

    std::vector<math::Vec3> prev(rag.bodies.size());
    for (usize i = 0; i < rag.bodies.size(); ++i)
        prev[i] = world.get_position(rag.bodies[i]);

    // Settle phase: track the PEAK speed^2 reached as the rag falls + impacts.
    f32 peak = 0.0f;
    for (u32 s = 0; s < static_cast<u32>(4.0f / kDt); ++s) {
        world.step(kDt);
        physics::solve_ragdoll(world, rag, kDt);
        REQUIRE(all_finite(world, rag));
        peak = std::max(peak, sample_speed2(prev));
    }
    // Bounded: a low gravitational drop + frictionless-joint-free assembly never
    // accumulates runaway speed. A diverging (energy-pumping) ragdoll would blow
    // through this by orders of magnitude; a real one peaks at a few m^2/s^2.
    REQUIRE(peak < 50.0f);

    // At rest: KE has decayed to a small residual and is NOT climbing back up.
    // Average speed^2 over a 1 s late window must be small in absolute terms AND
    // a small fraction of the peak (proving the assembly bled energy, not gained
    // it). The residual is the slow quasi-static settling drift on the ground.
    f32 late = 0.0f;
    const u32 win = 120;
    for (u32 s = 0; s < win; ++s) {
        world.step(kDt);
        physics::solve_ragdoll(world, rag, kDt);
        late += sample_speed2(prev);
    }
    late /= static_cast<f32>(win);
    REQUIRE(late < 0.2f);          // effectively stopped
    REQUIRE(late < peak * 0.5f);   // decreased well below its peak (no gain)

    physics::destroy_ragdoll(world, rag);
}

TEST_CASE("ragdoll spawn is deterministic run to run", "[physics][ragdoll][determinism]") {
    auto run = [](std::vector<math::Vec3>& out) {
        physics::World world;
        world.set_gravity({0.0f, -9.81f, 0.0f});
        make_ground(world);
        physics::RagdollDesc desc = physics::default_humanoid();
        physics::Ragdoll rag;
        physics::build_ragdoll(world, desc, kIdent, {0.3f, 0.8f, -0.2f},
                               {0.5f, -1.0f, 0.25f}, rag);
        for (u32 s = 0; s < 240; ++s) {
            world.step(kDt);
            physics::solve_ragdoll(world, rag, kDt);
        }
        out.clear();
        for (physics::BodyId id : rag.bodies)
            out.push_back(world.get_position(id));
        physics::destroy_ragdoll(world, rag);
    };

    std::vector<math::Vec3> a, b;
    run(a);
    run(b);
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == 7);
    for (usize i = 0; i < a.size(); ++i) {
        // Bit-for-bit: identical inputs + deterministic (-fno-fast-math) physics
        // must reproduce EXACTLY.
        REQUIRE(a[i].x == b[i].x);
        REQUIRE(a[i].y == b[i].y);
        REQUIRE(a[i].z == b[i].z);
    }
}

TEST_CASE("ragdoll teardown frees every body and returns to baseline",
          "[physics][ragdoll][teardown]") {
    physics::World world;
    world.set_gravity({0.0f, -9.81f, 0.0f});
    physics::BodyId ground = make_ground(world);
    REQUIRE(ground.valid());

    physics::RagdollDesc desc = physics::default_humanoid();

    // Build + tear down repeatedly. If teardown leaked or left stale handles,
    // the body free-list would mis-recycle and a later read would crash or read
    // garbage. We assert each rebuild produces a fresh, valid, finite rag and
    // that a torn-down rag is empty + a no-op on a second destroy.
    physics::Ragdoll rag;
    for (int iter = 0; iter < 5; ++iter) {
        physics::RagdollId id = physics::build_ragdoll(
            world, desc, kIdent, {0.0f, 0.7f, 0.0f}, {0.0f, 0.0f, 0.0f}, rag);
        REQUIRE(id.valid());
        REQUIRE(rag.body_count() == 7);

        // Handles from THIS build resolve to live bodies.
        for (physics::BodyId b : rag.bodies) {
            REQUIRE(b.valid());
            math::Vec3 p = world.get_position(b);
            REQUIRE(std::isfinite(p.y));
        }

        // Step a little, then tear down.
        for (u32 s = 0; s < 20; ++s) {
            world.step(kDt);
            physics::solve_ragdoll(world, rag, kDt);
        }
        physics::destroy_ragdoll(world, rag);

        // Baseline: the ragdoll is empty + reports not alive.
        REQUIRE(rag.body_count() == 0);
        REQUIRE_FALSE(rag.alive());

        // Idempotent: a second destroy is a harmless no-op.
        physics::destroy_ragdoll(world, rag);
        REQUIRE(rag.body_count() == 0);
    }

    // The ground body created at the top is untouched by ragdoll teardown.
    REQUIRE(world.get_position(ground).y == Approx(0.0f).margin(1e-5f));
}
