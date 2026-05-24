// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge. Registers backtick-console commands that
// start the local editor IPC server and launch React editor panels.

#pragma once

#include "core/Types.h"
#include "editor/ipc/Ipc.h"

#include <span>
#include <string_view>

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
void publish_web_profiler_frame(const WebProfilerFrame& frame);
void pump_web_panels();

}  // namespace psynder::editor
