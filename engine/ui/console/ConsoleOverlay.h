// SPDX-License-Identifier: MIT
// Psynder — software drop-down developer console overlay.
//
// A fully CPU-rendered Quake-style console: the backtick / tilde key drops a
// dark panel from the top of the framebuffer, you type cvar / command lines,
// and they run through the engine console backend (psynder::console::Console).
// Scrollback, Up/Down history recall, and Tab autocomplete all render with
// the lane-16 immediate-mode 5x7 bitmap font — no native text widget, no GUI
// library. This overlay is the only console UI code to maintain; the heavy
// lifting (Execute / EnumerateCVars / History / Undo) lives in core/console.
//
// ── Host integration (call once per frame) ──────────────────────────────
//
//     const bool eat = ui::console::update(*input, dt);  // EARLY: after poll
//     if (!eat) {
//         // gameplay input: camera look, movement, fire, editor toggle, ...
//     }
//     // ... render the scene + any HUD ...
//     ui::console::draw(fb);                              // LATE: before present
//
// `update` MUST run before gameplay reads input so an open console swallows
// the keystrokes (otherwise typing 'w' would also walk the player). It
// returns true while the console is capturing input — the host gates its own
// input (including the escape-to-quit and F2 editor toggle) on `!eat`.
//
// `draw` runs after the scene so the panel composites on top. `step` fuses
// update+draw for hosts that can accept one frame of suppression latency.

#pragma once

#include "core/Types.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

#include <string_view>

namespace psynder::ui::console {

// Process input for this frame. Toggles open/closed on the backtick/tilde
// edge; while open, feeds typed text + editing keys into the prompt and runs
// submitted lines through the global console backend. Returns true while the
// console is capturing input (host should suppress gameplay input this frame).
bool update(const platform::Input& input, f32 dt) noexcept;

// Render the drop-down overlay into `fb` using the software imm renderer.
// No-op while fully closed. Call after the scene + HUD, before present().
void draw(render::Framebuffer& fb) noexcept;

// Convenience: update() then draw() in one call. Returns update()'s capturing
// flag. Note the host's gameplay input for THIS frame already ran, so input
// suppression takes effect next frame — prefer the split update/draw form for
// games where a stray keystroke matters.
bool step(const platform::Input& input, render::Framebuffer& fb, f32 dt) noexcept;

bool is_open() noexcept;
void set_open(bool open) noexcept;
void toggle() noexcept;

// Append a line to the scrollback (e.g. to mirror an engine log line).
void print_line(std::string_view text) noexcept;

// Clear overlay-owned state (scrollback, prompt line, history cursor,
// animation). Does NOT touch the backend's cvars / commands / history.
void reset() noexcept;

}  // namespace psynder::ui::console
