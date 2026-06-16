// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge. Registers backtick-console commands that
// start the local editor IPC server and launch React editor panels.

#pragma once

#include "core/Types.h"
#include "editor/ipc/Ipc.h"

#include <span>
#include <string>
#include <string_view>

namespace psynder::scene {
class Scene;
}

namespace psynder::editor {

using WebProfilerSection = ipc::StatsSection;

struct WebProfilerFrame {
    u64 frame_index = 0;
    f32 cpu_ms = 0.0f;
    f32 render_ms = 0.0f;
    u32 draw_calls = 0;
    u32 entities = 0;
    std::span<const WebProfilerSection> sections;
};

void ensure_web_panel_commands_registered();
void close_web_panel_windows();
void clear_web_scene_authoring_state();
std::string web_entity_label(Entity entity);
void set_web_entity_label(Entity entity, std::string_view label);
std::string web_material_texture_name(u32 material_raw);
void set_web_material_texture_name(u32 material_raw, std::string_view texture_name);
void clear_web_material_texture_names();

struct WebSceneDirtyState {
    bool dirty = false;
    u32 generation = 0;
};

[[nodiscard]] WebSceneDirtyState web_scene_dirty_state();
void set_web_scene_dirty(bool dirty);
void mark_web_scene_dirty();
void publish_web_scene_load_failed(std::string_view path, std::string_view error);
void publish_web_scene_hierarchy(scene::Scene* scene);
void publish_web_scene_dirty();
void publish_web_selection_command_ack(std::string_view command,
                                       bool ok,
                                       std::string_view text,
                                       Entity entity = {},
                                       std::string_view component = {},
                                       std::string_view field = {});
void publish_web_profiler_frame(const WebProfilerFrame& frame);
void pump_web_panels();

}  // namespace psynder::editor
