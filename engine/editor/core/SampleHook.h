// SPDX-License-Identifier: MIT
// Psynder — Lane 18 (Wave C). One-call per-frame integration helper for
// sample apps (lane 25) and any host that wants the editor without
// wiring the input watcher and the badge overlay by hand.
//
// Usage from the sample's main loop:
//
//     auto mode = editor::sample_step(*input, fb);
//     if (mode == editor::Mode::Edit) {
//         // freeze physics, suppress AI, etc.
//     }
//
// Header-only so the sample / driver code can keep this dependency at
// build time without growing the editor_core public ABI. The work
// itself fans out to:
//   - `handle_input_frame` (HotKey.h) — edge-triggered mode toggle
//   - `imm::filled_rect` + `imm::label` (lane 16) — bottom-right badge
//   - `current_mode()`                — return for branch logic

#pragma once

#include "Editor.h"
#include "HotKey.h"

#include "render/Framebuffer.h"
#include "platform/Platform.h"
#include "ui/imm/Imm.h"
#include "ui/imm/DebugHud.h"
#include "ui/console/ConsoleOverlay.h"
#include "math/Math.h"
#include "core/Types.h"

namespace psynder::editor {

// Draw a small "PLAY" / "EDIT" badge in the bottom-right corner of `fb`.
// The badge is sized for the imm::label glyph (lane 16's 6×8 fixed font);
// we lay it out for a 4-character label including a one-pixel border.
//
// Colours are RGBA, low alpha so the badge is unobtrusive over gameplay:
//   - PLAY: dim green panel, white text
//   - EDIT: dim amber panel, white text
//
// Returns the resolved mode AFTER `handle_input_frame` has had a chance
// to flip it on this frame's input edge, so the caller's branch sees
// the same mode that was just rendered to the badge.
inline Mode sample_step(const platform::Input& input, render::Framebuffer& fb) noexcept {
    // Software drop-down console (lane 16 + core/console). Owns the backtick /
    // tilde key and, while open, swallows keystrokes so typing a command never
    // leaks into the editor toggle, the HUD cycle, or gameplay. update() runs
    // first (input capture); the matching draw() is a per-sample late call
    // before present() so the panel composites over the rendered scene.
    // Fixed 1/60 dt is fine — samples run at ~60 FPS and it only paces the
    // slide animation, caret blink, and key auto-repeat.
    const bool console_capturing = ui::console::update(input, 1.0f / 60.0f);

    if (!console_capturing) {
        handle_input_frame(input);

        // F1 cycles the debug HUD: Off -> Compact -> Full -> Off. (F2 is the
        // editor Play/Edit toggle, handled above; `~` is the console.)
        // Centralised here so every sample that calls sample_step() gets the
        // toggle with no per-sample input wiring.
        if (input.key_pressed(platform::KeyCode::F1)) {
            using ui::imm::DebugHudMode;
            const DebugHudMode m = ui::imm::debug_hud_mode();
            ui::imm::set_debug_hud_mode(m == DebugHudMode::Off       ? DebugHudMode::Compact
                                        : m == DebugHudMode::Compact ? DebugHudMode::Full
                                                                     : DebugHudMode::Off);
        }
    }

    const Mode mode = current_mode();

    // Badge layout: 6×8 font, 4 chars, 2-px inner padding, 4-px margin.
    // Origin is the badge's top-left in framebuffer pixels.
    constexpr u32 kBadgeW = 6 * 4 + 2 * 2;  // 28 px
    constexpr u32 kBadgeH = 8 + 2 * 2;      // 12 px
    constexpr u32 kMargin = 4;

    if (fb.width < kBadgeW + kMargin * 2 || fb.height < kBadgeH + kMargin * 2) {
        // Framebuffer too small to host the badge — skip the overlay
        // but still return the live mode so the caller's logic works.
        return mode;
    }

    const f32 x = static_cast<f32>(fb.width - kBadgeW - kMargin);
    const f32 y = static_cast<f32>(fb.height - kBadgeH - kMargin);

    // RGBA packed 0xAABBGGRR-ish per lane 16's convention. We pick the
    // standard 0xRRGGBBAA byte order used by `imm::label`'s rgba param.
    const u32 panel_rgba = (mode == Mode::Edit) ? 0xC07A30B0u   // amber, ~70% alpha
                                                : 0x2EA04AB0u;  // green, ~70% alpha
    const u32 text_rgba = 0xFFFFFFFFu;

    psynder::ui::imm::filled_rect(math::Vec2{x, y},
                                  math::Vec2{static_cast<f32>(kBadgeW), static_cast<f32>(kBadgeH)},
                                  panel_rgba);
    psynder::ui::imm::label(math::Vec2{x + 2.0f, y + 2.0f},
                            (mode == Mode::Edit) ? "EDIT" : "PLAY",
                            text_rgba);

    return mode;
}

}  // namespace psynder::editor
