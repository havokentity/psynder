// SPDX-License-Identifier: MIT
// Psynder — RmlUi binding for player HUDs / menus / cockpits. Lane 17 owns.

#pragma once

#include "core/Types.h"
#include "render/Framebuffer.h"

#include <string_view>

namespace psynder::ui::rml {

bool initialize();
void shutdown();

// Load .rml + .rcss into a named document; lane 17 implements the
// RenderInterface plugging into the software rasterizer.
bool load_document(std::string_view virtual_path, std::string_view name);
void show(std::string_view name);
void hide(std::string_view name);

void render(render::Framebuffer& target);
void update(f32 dt);

}  // namespace psynder::ui::rml
