// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge. Registers backtick-console commands that
// start the local editor IPC server and launch React editor panels.

#pragma once

#include "core/Types.h"

namespace psynder::editor {

struct WebProfilerFrame {
    u64 frame_index = 0;
    f32 cpu_ms = 0.0f;
    u32 draw_calls = 0;
    u32 entities = 0;
};

void ensure_web_panel_commands_registered();
void publish_web_profiler_frame(const WebProfilerFrame& frame);
void pump_web_panels();

}  // namespace psynder::editor
