// SPDX-License-Identifier: MIT
// Psynder physics — character controller (DESIGN.md §10.1).
//
// Capsule kinematic body. Sweep-step-slide motion: try to move along `delta`,
// on hit project the remaining motion onto the impact plane and re-sweep. Up
// to 4 substeps. Slope limit + step-height climb-up are honoured.
//
// Wave B additions:
//   * Real stance state machine driven by `character_set_intent` (Character.h).
//   * Stair-step climb: when a horizontal sub-step hits a vertical wall, lift
//     by step_height, attempt the move from the lifted origin, then drop. If
//     the dropped position rests on floor, commit; otherwise revert to slide.
//   * Capsule height tracks current stance (crouch / prone / etc.).

#include "Physics.h"
#include "Character.h"
#include "WorldImpl.h"
#include "Narrowphase.h"
#include "internal/Kernels.h"

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

// Build the capsule body we use as the character's collider for narrowphase.
static Body make_capsule(const Character& c, math::Vec3 pos) noexcept {
    Body cap{};
    cap.position = pos;
    cap.rotation = {0, 0, 0, 1};
    cap.shape = 1;  // capsule
    cap.half_extent = {c.radius, std::max(0.0f, (c.height * 0.5f) - c.radius), 0.0f};
    cap.inv_mass = 1.0f;  // treat as dynamic for normal direction
    return cap;
}

// True if the capsule at `pos` overlaps any non-self body in the world.
static bool capsule_overlaps_world(const Character& c, math::Vec3 pos) noexcept {
    auto& w = world_state();
    Body cap = make_capsule(c, pos);
    for (u32 i = 0; i < w.bodies.size(); ++i) {
        const Body& b = w.bodies[i];
        if (b.gen == 0)
            continue;
        Contact ct;
        if (collide_pair(cap, b, ct))
            return true;
    }
    return false;
}

// Sweep one capsule against every body in the physics world. On any overlap
// push the capsule out along the contact normal. Returns true if a wall-like
// (steep-normal) collision blocked horizontal progress — the caller uses this
// signal to try a stair-step climb-up.
static bool sweep_and_resolve(Character& c, math::Vec3& position, math::Vec3 delta) {
    auto& w = world_state();

    math::Vec3 target = math::add(position, delta);
    position = target;

    bool blocked_horizontally = false;

    for (u32 i = 0; i < w.bodies.size(); ++i) {
        Body& b = w.bodies[i];
        if (b.gen == 0)
            continue;
        Body cap = make_capsule(c, position);
        Contact ct;
        if (collide_pair(cap, b, ct)) {
            // Normal points from cap (A) → b (B); push cap backwards.
            position = math::sub(position, math::mul(ct.normal_world, ct.depth + 1e-4f));
            // Floor classification: contact normal pointing up (cap pushed
            // downward → normal points -Y in our cap→b convention).
            if (-ct.normal_world.y >= c.slope_limit) {
                c.on_floor = true;
            } else if (std::fabs(ct.normal_world.y) < (1.0f - c.slope_limit)) {
                // Near-vertical contact: a wall. Note for stair-step.
                blocked_horizontally = true;
            }
        }
    }
    return blocked_horizontally;
}

void character_move(Character& c, math::Vec3 delta, f32 dt) {
    (void)dt;
    c.on_floor = false;

    // Special motion modes — Ladder and Water disable stair-step and pin the
    // body to the motion plane.
    if (c.stance == CharStance::Ladder) {
        // Move along the requested delta (gameplay produces vertical for
        // climb, lateral for shimmy); no stair-step, no gravity-driven slide.
        c.position = math::add(c.position, delta);
        return;
    }
    if (c.stance == CharStance::Water) {
        // In water we still collide but motion is unsubdivided plus damped
        // by buoyancy. Caller supplies a pre-damped delta.
        sweep_and_resolve(c, c.position, delta);
        return;
    }

    constexpr u32 kMaxSubsteps = 4;
    for (u32 i = 0; i < kMaxSubsteps; ++i) {
        math::Vec3 step = math::mul(delta, 1.0f / static_cast<f32>(kMaxSubsteps));

        // Snapshot the pre-move position so stair-step has a clean rollback
        // point if the elevated re-sweep also fails.
        math::Vec3 pre = c.position;
        bool blocked = sweep_and_resolve(c, c.position, step);

        // If we were blocked horizontally on this sub-step, attempt the
        // climb-up: lift by step_height, replay the horizontal motion, then
        // drop. We use the pure-algorithmic kernel so this is unit-testable.
        if (blocked) {
            math::Vec3 horiz = step;
            horiz.y = 0.0f;
            auto overlap = [&](math::Vec3 p) -> bool { return capsule_overlaps_world(c, p); };
            math::Vec3 stepped = kernels::kernel_stair_step_climb(pre, horiz, c.step_height, overlap);
            // Commit the climb only if it actually moved us further along
            // the requested horizontal motion (avoids infinitesimal jitter).
            math::Vec3 delta_climb = math::sub(stepped, pre);
            if (math::dot(delta_climb, horiz) > math::dot(math::sub(c.position, pre), horiz)) {
                c.position = stepped;
                c.on_floor = true;
            }
        }
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
    c.stand_height = d.height;
    c.height = d.height;
    c.radius = d.radius;
    c.velocity = {0, 0, 0};
    c.on_floor = false;
    c.stance = detail::CharStance::Stand;
    c.intent_crouch = c.intent_prone = c.env_ladder = c.env_water = false;
    if (c.gen == 0)
        c.gen = 1;
    return CharacterId{(c.gen << 24) | (idx & 0x00FFFFFFu)};
}

void destroy(CharacterId id) {
    auto& w = detail::character_world();
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.chars.size())
        return;
    w.chars[idx].gen = 0;
    w.free_slots.push_back(idx);
}

void move(CharacterId id, math::Vec3 delta, f32 dt) {
    auto& w = detail::character_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.chars.size() || w.chars[idx].gen == 0)
        return;
    detail::character_move(w.chars[idx], delta, dt);
}

}  // namespace psynder::physics::character
