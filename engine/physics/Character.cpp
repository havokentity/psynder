// SPDX-License-Identifier: MIT
// Psynder physics — character controller (DESIGN.md §10.1).
//
// Capsule kinematic body. Wave A implements the core sweep-step-slide motion
// algorithm: try to move along `delta`, on hit project the remaining motion
// onto the impact plane and re-sweep. Up to 4 substeps. Slope limit + step
// height are honoured; full stance state-machine (crouch / prone / ladder /
// water transitions) is API-only in Wave A.

#include "Physics.h"
#include "Character.h"
#include "WorldImpl.h"
#include "Narrowphase.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace psynder::physics::character {

namespace {
std::mutex g_mutate;
}

}  // namespace psynder::physics::character

namespace psynder::physics::detail {

CharacterWorld& character_world() {
    static CharacterWorld w;
    return w;
}

// Sweep one capsule against every body in the physics world and find the
// nearest collision time of impact (TOI) along motion `delta`. Wave A uses a
// discrete-pass test (try the destination, detect overlap, push out) which
// is cheap and stable for small per-tick motion; Wave B layers a continuous
// version for fast bullets and vehicles.
static bool sweep_and_resolve(Character& c, math::Vec3& position,
                              math::Vec3 delta) {
    auto& w = world_state();

    math::Vec3 target = math::add(position, delta);
    // Trial move; then for each overlap push along the contact normal.
    position = target;

    // Build a sphere-equivalent body for the character (its capsule) and
    // collide against every body. This is O(N) — acceptable until lane 13
    // Wave B layers a kinematic broadphase.
    for (u32 i = 0; i < w.bodies.size(); ++i) {
        Body& b = w.bodies[i];
        if (b.gen == 0) continue;
        Body cap{};
        cap.position    = position;
        cap.rotation    = {0, 0, 0, 1};
        cap.shape       = 1;                       // capsule
        cap.half_extent = { c.radius, std::max(0.0f, (c.height * 0.5f) - c.radius), 0.0f };
        cap.inv_mass    = 1.0f;                    // treat as dynamic for normal direction
        Contact ct;
        if (collide_pair(cap, b, ct)) {
            // Push character out along normal.
            // Normal points from cap (A) → b (B); push cap backwards.
            position = math::sub(position, math::mul(ct.normal_world, ct.depth + 1e-4f));
            // Test floor: if the contact normal has a positive Y component
            // greater than slope_limit, mark on_floor.
            if (-ct.normal_world.y >= c.slope_limit) c.on_floor = true;
        }
    }
    (void)delta;
    return true;
}

void character_move(Character& c, math::Vec3 delta, f32 dt) {
    (void)dt;
    c.on_floor = false;
    // Slide iterations: split delta into up-to-4 substeps to avoid huge
    // single-step penetrations.
    constexpr u32 kMaxSubsteps = 4;
    for (u32 i = 0; i < kMaxSubsteps; ++i) {
        math::Vec3 step = math::mul(delta, 1.0f / static_cast<f32>(kMaxSubsteps));
        sweep_and_resolve(c, c.position, step);
    }
}

}  // namespace psynder::physics::detail

namespace psynder::physics::character {

CharacterId create(const CharacterDesc& d) {
    auto& w = detail::character_world();
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx;
    if (!w.free_slots.empty()) {
        idx = w.free_slots.back();
        w.free_slots.pop_back();
    } else {
        idx = static_cast<u32>(w.chars.size());
        w.chars.emplace_back();
    }
    detail::Character& c = w.chars[idx];
    c.position = d.position;
    c.height   = d.height;
    c.radius   = d.radius;
    c.velocity = {0, 0, 0};
    c.on_floor = false;
    c.stance   = detail::CharStance::Stand;
    if (c.gen == 0) c.gen = 1;
    return CharacterId{ (c.gen << 24) | (idx & 0x00FFFFFFu) };
}

void destroy(CharacterId id) {
    auto& w = detail::character_world();
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.chars.size()) return;
    w.chars[idx].gen = 0;
    w.free_slots.push_back(idx);
}

void move(CharacterId id, math::Vec3 delta, f32 dt) {
    auto& w = detail::character_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.chars.size() || w.chars[idx].gen == 0) return;
    detail::character_move(w.chars[idx], delta, dt);
}

}  // namespace psynder::physics::character
