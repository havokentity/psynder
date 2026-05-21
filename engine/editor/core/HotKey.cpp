// SPDX-License-Identifier: MIT
// Psynder — Lane 18 (Wave C). Hot-key watcher implementation.
//
// One frame, one read. The host driver calls `handle_input_frame(input)`
// once per main-loop iteration, after the platform has polled events and
// before gameplay / editor logic reads the current mode. Per the contract
// of `platform::Input::key_pressed()`, the predicate is true only on the
// frame the key transitioned from up→down, so a single edge produces a
// single `toggle_mode()` call regardless of how long the player holds it.
//
// Editor toggle is F2 only. The backtick / tilde key is reserved for the
// software console overlay (psynder::ui::console), matching the classic
// Quake convention where `~` drops the console, never the dev mode.

#include "HotKey.h"

#include "Editor.h"
#include "platform/Platform.h"

namespace psynder::editor {

void handle_input_frame(const platform::Input& input) noexcept {
    if (input.key_pressed(platform::KeyCode::F2)) {
        toggle_mode();
    }
}

}  // namespace psynder::editor
