// SPDX-License-Identifier: MIT
// Psynder physics — character controller internal state (DESIGN.md §10.1).
//
// Capsule kinematic controller, sweep-step-slide motion. Wave A ships the
// motion algorithm; the full state machine (crouch / prone / ladder / water)
// is API-only in Wave A and will be filled out in Wave B once lane 15 wires
// gameplay flags.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::physics::detail {

enum class CharStance : u8 { Stand, Crouch, Prone, Ladder, Water };

struct Character {
    math::Vec3 position{0, 0, 0};
    math::Vec3 velocity{0, 0, 0};
    f32        height   = 1.8f;
    f32        radius   = 0.35f;
    f32        step_height = 0.35f;   // max obstacle height we walk over
    f32        slope_limit = 0.7f;    // cos(angle) above which a normal counts as floor
    bool       on_floor = false;
    CharStance stance   = CharStance::Stand;
    u32        gen      = 1;
};

struct CharacterWorld {
    std::vector<Character> chars;
    std::vector<u32>       free_slots;
};

CharacterWorld& character_world();

// Sweep-step-slide kernel — runs N collide-and-slide iterations against the
// current physics world. Public character::move() forwards into this.
void character_move(Character& c, math::Vec3 delta, f32 dt);

}  // namespace psynder::physics::detail
