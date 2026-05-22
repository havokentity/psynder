// SPDX-License-Identifier: MIT
// Psynder — Lane 18 (Wave C). ONE-CALL per-frame engine overlay suite for
// sample apps (lane 25) and any host that wants the dev overlays without
// wiring the console, debug HUD, editor toggle, and badge by hand.
//
// The overlays are an ENGINE FEATURE, not per-app boilerplate. A host adds a
// single late call (after the scene, before present) plus one input gate:
//
//     // ── once per frame, near the end of the loop ──
//     editor::frame_overlays(*input, fb);          // console + HUD + badge
//     window->present(fb);
//
//     // ── gate gameplay input on the console ──
//     if (!editor::overlays_capturing() && input->key_down(Escape)) break;
//     if (!editor::overlays_capturing()) controller.update(*input, dt);
//
// `frame_overlays` measures frame time itself, drives the `~` console + F1
// HUD cycle + F2 editor toggle, and draws the console panel, debug HUD, and
// PLAY/EDIT badge. The whole suite can be compiled out at runtime with
// `set_overlays_enabled(false)` (e.g. a shipping game build) — then it is a
// no-op and the console cannot open.
//
// Header-only so the sample / driver code keeps this as a build-time
// dependency without growing the editor_core public ABI. It fans out to:
//   - `ui::console` (lane 16 + core/console) — drop-down console
//   - `handle_input_frame` (HotKey.h)        — F2 editor toggle
//   - `ui::imm::draw_debug_hud` (lane 16)     — F1 debug HUD
//   - `imm::filled_rect` + `imm::label`       — bottom-right PLAY/EDIT badge

#pragma once

#include "Editor.h"
#include "HotKey.h"

#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "ui/console/ConsoleOverlay.h"
#include "ui/imm/DebugHud.h"
#include "ui/imm/Imm.h"

#include <array>

namespace psynder::editor {

// ─── Master enable + capture query ──────────────────────────────────────────
namespace detail {
struct OverlayState {
    bool enabled = true;          // master switch for the whole overlay suite
    bool capturing = false;       // input was consumed by overlays this frame
    u64 last_tick = 0;            // for the internal frame-time measurement
    std::array<f32, 120> ring{};  // ~2 s rolling window for avg frame ms
    u32 head = 0;
    u32 count = 0;
};
// Single shared instance across all TUs (inline-function-local static).
inline OverlayState& overlay_state() noexcept {
    static OverlayState s;
    return s;
}
}  // namespace detail

inline bool overlays_enabled() noexcept {
    return detail::overlay_state().enabled;
}
inline void set_overlays_enabled(bool enabled) noexcept {
    auto& st = detail::overlay_state();
    st.enabled = enabled;
    if (!enabled)
        st.capturing = false;
    if (!enabled)
        ui::console::set_open(false);  // don't strand an open console
}

// True while the console is capturing keystrokes. Hosts gate their gameplay
// input (camera, fire, escape-to-quit) on `!overlays_capturing()`.
inline bool overlays_capturing() noexcept {
    return overlays_enabled() && (detail::overlay_state().capturing || ui::console::is_open());
}

// ─── Badge ──────────────────────────────────────────────────────────────────
// Draw a small "PLAY" / "EDIT" badge in the bottom-right corner of `fb`,
// sized for the lane-16 6×8 fixed font (4 chars + 2-px padding + border).
// Low-alpha panel so it stays unobtrusive over gameplay.
inline void draw_mode_badge(render::Framebuffer& fb, Mode mode) noexcept {
    constexpr u32 kBadgeW = 6 * 4 + 2 * 2;  // 28 px
    constexpr u32 kBadgeH = 8 + 2 * 2;      // 12 px
    constexpr u32 kMargin = 4;
    if (fb.width < kBadgeW + kMargin * 2 || fb.height < kBadgeH + kMargin * 2)
        return;

    const f32 x = static_cast<f32>(fb.width - kBadgeW - kMargin);
    const f32 y = static_cast<f32>(fb.height - kBadgeH - kMargin);
    // Packed via imm::rgba (R in the low byte) so the panel reads true colour;
    // filled_rect overwrites, so the badge is opaque regardless of alpha.
    const u32 panel_rgba = (mode == Mode::Edit) ? psynder::ui::imm::rgba(0xC0, 0x7A, 0x30)  // amber
                                                : psynder::ui::imm::rgba(0x2E, 0xA0, 0x4A);  // green
    psynder::ui::imm::filled_rect(math::Vec2{x, y},
                                  math::Vec2{static_cast<f32>(kBadgeW), static_cast<f32>(kBadgeH)},
                                  panel_rgba);
    psynder::ui::imm::label(math::Vec2{x + 2.0f, y + 2.0f},
                            (mode == Mode::Edit) ? "EDIT" : "PLAY",
                            psynder::ui::imm::rgba(0xFF, 0xFF, 0xFF));
}

namespace detail {
inline ui::imm::DebugHudMode next_debug_hud_mode(ui::imm::DebugHudMode mode) noexcept {
    using ui::imm::DebugHudMode;
    switch (mode) {
        case DebugHudMode::Off:
            return DebugHudMode::Compact;
        case DebugHudMode::Compact:
            return DebugHudMode::Full;
        case DebugHudMode::Full:
            return DebugHudMode::Off;
    }
    return DebugHudMode::Off;
}
}  // namespace detail

inline Mode sample_update(const platform::Input& input, f32 dt) noexcept {
    auto& st = detail::overlay_state();
    if (!st.enabled) {
        st.capturing = false;
        return current_mode();
    }

    // The console owns the backtick/tilde key and, while open, swallows
    // keystrokes so typing a command never leaks into the toggles or gameplay.
    const bool console_capturing = ui::console::update(input, dt);
    st.capturing = console_capturing;

    if (!console_capturing) {
        handle_input_frame(input);  // F2 Play/Edit toggle

        // F1 cycles the debug HUD: Off -> Compact -> Full -> Off.
        if (input.key_pressed(platform::KeyCode::F1)) {
            ui::imm::set_debug_hud_mode(detail::next_debug_hud_mode(ui::imm::debug_hud_mode()));
        }
    }

    return current_mode();
}

inline void sample_draw(render::Framebuffer& fb) noexcept {
    if (!overlays_enabled())
        return;
    draw_mode_badge(fb, current_mode());
}

// ─── Input + badge primitive ────────────────────────────────────────────────
// Drives the console (input capture), the F2 editor toggle, the F1 HUD cycle,
// and draws the PLAY/EDIT badge. Returns the resolved mode. `dt` paces the
// console slide / caret blink / key auto-repeat. Most hosts call the
// higher-level `frame_overlays` instead of this directly.
inline Mode sample_step(const platform::Input& input, render::Framebuffer& fb, f32 dt) noexcept {
    const Mode mode = sample_update(input, dt);
    draw_mode_badge(fb, mode);
    return mode;
}

// ─── One-call overlay frame ─────────────────────────────────────────────────
// Optional per-frame scene counters surfaced on the debug HUD. All default
// to 0 — a host that doesn't track them still gets frame-time + FPS.
struct FrameOverlayStats {
    usize draw_calls = 0;
    usize triangles = 0;
    usize active_voices = 0;
};

inline ui::imm::DebugHudStats make_debug_hud_stats(f32 frame_ms,
                                                   f32 avg_frame_ms,
                                                   const FrameOverlayStats& stats = {}) noexcept {
    ui::imm::DebugHudStats hud{};
    hud.frame_ms = frame_ms;
    hud.avg_frame_ms = avg_frame_ms;
    hud.draw_calls = stats.draw_calls;
    hud.triangles = stats.triangles;
    hud.active_voices = stats.active_voices;
    return hud;
}

inline void draw_frame_overlays(render::Framebuffer& fb, const ui::imm::DebugHudStats& hud) noexcept {
    if (!overlays_enabled())
        return;
    draw_mode_badge(fb, current_mode());
    ui::imm::draw_debug_hud(fb, hud);  // early-returns when the HUD is Off
    ui::console::draw(fb);             // drop-down console composites on top
}

// THE host entry point. Call once near the end of the loop (after the scene
// is rendered, before present). Internally measures frame_ms from the wall
// clock between calls, drives the console + F1 HUD + F2 toggle, and draws the
// console panel, debug HUD, and badge over `fb`. Returns the editor Mode
// (so hosts can freeze physics / AI in Edit mode). No-op returning
// current_mode() when `set_overlays_enabled(false)`.
inline Mode frame_overlays(const platform::Input& input,
                           render::Framebuffer& fb,
                           const FrameOverlayStats& stats = {}) noexcept {
    auto& st = detail::overlay_state();
    if (!st.enabled)
        return current_mode();

    // Wall-clock frame time between successive calls (clamped against the
    // first frame + hitches), plus a rolling average for the HUD.
    const u64 now = platform::Clock::ticks_now();
    f32 frame_ms = (st.last_tick != 0)
                       ? static_cast<f32>(platform::Clock::seconds(now - st.last_tick) * 1000.0)
                       : 1000.0f / 60.0f;
    if (frame_ms <= 0.0f || frame_ms > 250.0f)
        frame_ms = 1000.0f / 60.0f;
    st.last_tick = now;
    st.ring[st.head] = frame_ms;
    st.head = (st.head + 1u) % static_cast<u32>(st.ring.size());
    if (st.count < static_cast<u32>(st.ring.size()))
        ++st.count;
    f32 avg = 0.0f;
    for (u32 i = 0; i < st.count; ++i)
        avg += st.ring[i];
    avg = (st.count > 0u) ? avg / static_cast<f32>(st.count) : frame_ms;

    const Mode mode = sample_step(input, fb, frame_ms / 1000.0f);
    draw_frame_overlays(fb, make_debug_hud_stats(frame_ms, avg, stats));
    return mode;
}

}  // namespace psynder::editor
