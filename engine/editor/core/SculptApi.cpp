// SPDX-License-Identifier: MIT
// Psynder — public sculpt_* API impl. Forwards to editor::sculpt::*.

#include "Editor.h"
#include "EditorState.h"
#include "Sculpt.h"

namespace psynder::editor {

namespace {
// Make sure the editor heightfield is at least minimally sized when the
// caller starts sculpting cold. Wave A scope: 257x257 at 1 m spacing,
// origin at world (0,0,0). Real authoring sets this from the loaded
// terrain header.
PSY_FORCEINLINE void ensure_heightfield() noexcept {
    auto& s = detail::get_state();
    if (s.heightfield.size_x == 0 || s.heightfield.size_z == 0) {
        s.heightfield.allocate(257, 257, 1.0f);
        s.heightfield.origin = { -128.0f, 0.0f, -128.0f };
    }
    if (s.splat.size_x == 0 || s.splat.size_z == 0) {
        s.splat.allocate(s.heightfield.size_x, s.heightfield.size_z);
    }
}
}  // namespace

void sculpt_raise(math::Vec3 wp, f32 radius, f32 strength) {
    ensure_heightfield();
    sculpt::raise(detail::get_state().heightfield, wp, radius, strength);
}

void sculpt_lower(math::Vec3 wp, f32 radius, f32 strength) {
    ensure_heightfield();
    sculpt::lower(detail::get_state().heightfield, wp, radius, strength);
}

void sculpt_smooth(math::Vec3 wp, f32 radius, f32 strength) {
    ensure_heightfield();
    sculpt::smooth(detail::get_state().heightfield, wp, radius, strength);
}

void sculpt_paint(math::Vec3 wp, f32 radius, u8 material_index, f32 weight) {
    ensure_heightfield();
    auto& s = detail::get_state();
    sculpt::paint(s.splat, wp, radius, s.heightfield, material_index, weight);
}

}  // namespace psynder::editor
