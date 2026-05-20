// SPDX-License-Identifier: MIT
// Psynder — Lane 18 (Wave C). Editor mode hot-key binding.
//
// DESIGN.md §10.8: pressing `~` (tilde) or F2 flips the running engine
// between Play and Edit. The flip itself is owned by `editor::toggle_mode()`
// in Modes.cpp; this TU is the edge-triggered input watcher that calls it
// from whatever frame loop wants to host the editor (lane 25 sample driver,
// dev-tool host, the eventual final game).
//
// We deliberately read `key_pressed()` (edge) rather than `key_down()`
// (level). Toggling on every frame the key is held would race against the
// player and flicker the mode 60+ times per second — exactly the bug a
// 2-frame test in this lane's unit suite is here to catch.

#pragma once

namespace psynder::platform {
class Input;
}

namespace psynder::editor {

// Watch the platform input device for the mode-toggle hot-keys (Tilde / F2)
// and call `toggle_mode()` on the leading edge of either press. Safe to
// call once per frame from the host driver; never toggles on a held key.
void handle_input_frame(const platform::Input& input) noexcept;

}  // namespace psynder::editor
