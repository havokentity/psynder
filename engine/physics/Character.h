// SPDX-License-Identifier: MIT
// Psynder physics — character controller internal state (DESIGN.md §10.1).
//
// Capsule kinematic controller, sweep-step-slide motion.
//
// Wave A: motion algorithm only.
// Wave B: full stance state machine (crouch / prone / ladder / water) driven
// by player intent + collision queries, plus stair-step climb-up via the
// header-only kernel in `internal/Kernels.h`. The public `character::`
// namespace stays frozen — Wave B input flows through the internal
// `character_set_intent()` plumbing the test TUs reach for directly. (Lane 15
// wires this to gameplay once it ships its Wave B.)

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "internal/Kernels.h"

#include <vector>

namespace psynder::physics::detail {

enum class CharStance : u8 { Stand, Crouch, Prone, Ladder, Water };

struct Character {
    math::Vec3 position{0, 0, 0};
    math::Vec3 velocity{0, 0, 0};
    f32 stand_height = 1.8f;  // height in Stand stance
    f32 height = 1.8f;        // current height (derived from stance)
    f32 radius = 0.35f;
    f32 step_height = 0.35f;  // max obstacle height we walk over
    f32 slope_limit = 0.7f;   // cos(angle) above which a normal counts as floor
    bool on_floor = false;
    CharStance stance = CharStance::Stand;

    // Wave-B intent + environment flags. The motion code reads these to
    // pick the next stance via `kernels::kernel_char_next_stance`.
    bool intent_crouch = false;
    bool intent_prone = false;
    bool env_ladder = false;
    bool env_water = false;

    // `gen` is the slot's current generation (1..255, never 0), preserved
    // across destroy and bumped on reuse so a stale CharacterId fails the
    // decode equality check. `alive` marks a live slot vs a hole.
    u32 gen = 1;
    bool alive = false;
};

struct CharacterWorld {
    std::vector<Character> chars;
    std::vector<u32> free_slots;
};

// The per-world character sub-state now lives inside WorldImpl (one per World
// instance) — there is no longer a character_world() file-static singleton.
// detail::character_world() is the LEGACY default-world accessor: it returns
// World::Get()'s character sub-world (defined in World.cpp). Re-declared here
// — in addition to WorldImpl.h — so callers that include only this header
// (sample 09's resolved-pose read-back) keep compiling UNCHANGED.
CharacterWorld& character_world();

// Forward-declare WorldState so character_move can take it by reference (it
// sweeps the character capsule against that world's rigid bodies). The full
// definition lives in WorldImpl.h, which the .cpp includes.
struct WorldState;

// Sweep-step-slide kernel — runs N collide-and-slide iterations against the
// given world's rigid bodies. Public character::move() forwards into this.
void character_move(WorldState& w, Character& c, math::Vec3 delta, f32 dt);

// Wave-B: update the character's intent flags before the next `move()` call.
// Internal API only — lane 15 (gameplay) wires it. Stays out of the public
// header because lane 13's public surface is frozen.
inline void character_set_intent(
    Character& c, bool want_crouch, bool want_prone, bool near_ladder, bool in_water) noexcept {
    c.intent_crouch = want_crouch;
    c.intent_prone = want_prone;
    c.env_ladder = near_ladder;
    c.env_water = in_water;

    auto cur = static_cast<kernels::CharStanceK>(static_cast<u8>(c.stance));
    auto nxt = kernels::kernel_char_next_stance(
        cur,
        kernels::CharIntent{want_crouch, want_prone, near_ladder, in_water});
    c.stance = static_cast<CharStance>(static_cast<u8>(nxt));
    c.height = kernels::kernel_char_height_for_stance(nxt, c.stand_height);
}

}  // namespace psynder::physics::detail
