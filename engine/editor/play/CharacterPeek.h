// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE internal helper: read-only character pose peek.
//
// The public physics::character namespace exposes create / destroy / move but
// no position getter. The resolved capsule centre lives in the internal
// physics::detail character store. We cannot include physics/Character.h from
// the same TU as scene/SceneEcs.h: Character.h pulls physics/internal/Kernels.h
// + Shape.h, which define a namespace-local physics::quat_rotate that becomes
// ambiguous with math::quat_rotate (dragged in by scene's math/MathExt.h) under
// ADL. So this peek is isolated in its own TU (CharacterPeek.cpp) that includes
// ONLY the physics header, never the scene/math-ext headers.

#pragma once

#include "math/Math.h"
#include "physics/Physics.h"

namespace psynder::editor::play {

// Resolved capsule centre for a character handle, read from the engine's
// internal store. Returns {0,0,0} for an invalid / stale handle. Read-only.
[[nodiscard]] math::Vec3 peek_character_position(physics::character::CharacterId id) noexcept;

}  // namespace psynder::editor::play
