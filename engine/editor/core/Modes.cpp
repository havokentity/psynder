// SPDX-License-Identifier: MIT
// Psynder — editor mode toggle. DESIGN.md §10.8: `~` or F2 flips play↔edit.
// The physics tick, the renderer, the ECS — none of them reload; the
// editor is a *mode* of the running engine, not a separate process.
//
// This TU is intentionally tiny: the actual key binding lives in lane
// 21/22/23 (platform input) and calls toggle_mode(); current_mode() is
// what the renderer / physics / script sample to gate edit-only paths.

#include "Editor.h"
#include "EditorState.h"

namespace psynder::editor {

void toggle_mode() {
    auto& s = detail::get_state();
    // Memory order: relaxed is fine; the mode flag is read by the engine
    // loop on the same thread that toggled it (or with a fence at the
    // top of the frame). Atomicity matters only because the editor IPC
    // server (lane 19) can request a flip from a worker.
    u8 cur = s.mode.load(std::memory_order_relaxed);
    s.mode.store(static_cast<u8>(cur ? 0 : 1), std::memory_order_relaxed);
}

Mode current_mode() {
    auto& s = detail::get_state();
    return static_cast<Mode>(s.mode.load(std::memory_order_relaxed));
}

}  // namespace psynder::editor
