// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE internal helper: read-only character pose peek.
//
// Isolated TU. It includes physics/Character.h (internal character store +
// Kernels.h) but NEVER scene/SceneEcs.h or math/MathExt.h, so the
// physics::quat_rotate / math::quat_rotate ambiguity that Kernels.h triggers
// against MathExt.h cannot arise here. PlayRuntime.cpp reaches the store only
// through this function.

#include "editor/play/CharacterPeek.h"

#include "physics/Character.h"

namespace psynder::editor::play {

math::Vec3 peek_character_position(physics::character::CharacterId id) noexcept {
    if (!id.valid())
        return math::Vec3{0.0f, 0.0f, 0.0f};
    const u32 idx = id.raw & 0x00FFFFFFu;
    auto& cw = physics::detail::character_world();
    if (idx < cw.chars.size())
        return cw.chars[idx].position;
    return math::Vec3{0.0f, 0.0f, 0.0f};
}

}  // namespace psynder::editor::play
