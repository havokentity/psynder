// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge. Registers backtick-console commands that
// start the local editor IPC server and launch React editor panels.

#pragma once

namespace psynder::editor {

void ensure_web_panel_commands_registered();
void pump_web_panels();

}  // namespace psynder::editor
