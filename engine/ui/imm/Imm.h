// SPDX-License-Identifier: MIT
// Psynder — immediate-mode UI. In-viewport overlays ONLY (gizmos, brush
// previews, debug HUD, perf graphs). Lane 16 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <string_view>

namespace psynder::ui::imm {

void begin_frame(render::Framebuffer& target);
void end_frame();

bool button(math::Vec2 position, math::Vec2 size, std::string_view label);
void label(math::Vec2 position, std::string_view text, u32 rgba = 0xFFFFFFFFu);
void line(math::Vec2 a, math::Vec2 b, u32 rgba);
void rect_outline(math::Vec2 origin, math::Vec2 size, u32 rgba);
void filled_rect(math::Vec2 origin, math::Vec2 size, u32 rgba);

// Gizmo support — lane 16 fleshes out the manipulator + brush preview API.
}  // namespace psynder::ui::imm
