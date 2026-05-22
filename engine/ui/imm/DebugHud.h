// SPDX-License-Identifier: MIT
// Psynder — Lane 16 (immediate-mode UI). DEBUG HUD — Wave C.
//
// Callable debug overlay for sample binaries (and the editor when it
// wants a quick eyeball view of per-frame health). Draws into the
// top-left of a `render::Framebuffer` and is intentionally tiny:
//
//   • Frame time (ms) + FPS                  (always)
//   • 60-sample frame-time strip chart        (Compact + Full)
//   • Allocator heatmap (Wave-B)              (Full)
//   • 6-line perf log ring (last PSY_DIAG_TIER1 lines pushed by the
//     caller via `push_diag_line()`)          (Full)
//
// The HUD is driven via cvar `r_debug_hud` whose three values are
// modelled by `DebugHudMode`. Callers translate the cvar string into
// the enum (the IMM stays decoupled from the console registry per
// DESIGN.md §10.6 — Lane 16 must remain header-light).
//
// Public header (lives next to `Overlay.h`/`Heatmap.h` inside the
// lane-owned directory; the frozen `Imm.h` is untouched).

#pragma once

#include "core/Types.h"
#include "render/Framebuffer.h"

#include <string_view>

namespace psynder::ui::imm {

// cvar `r_debug_hud` decoded form.
enum class DebugHudMode : u8 {
    Off = 0,
    Compact = 1,
    Full = 2,
};

// Per-frame stats the caller fills in once, before `draw_debug_hud()`.
struct DebugHudStats {
    f32 frame_ms = 0.0f;
    f32 avg_frame_ms = 0.0f;
    usize draw_calls = 0;
    usize triangles = 0;
    usize active_voices = 0;
    u32 rt_tiles = 0;
    u32 rt_jobs = 0;
    bool raster_stats_valid = false;
    bool rt_stats_valid = false;
    bool render_stats_valid = false;
};

struct DebugHudFrameHistory {
    static constexpr u32 kCapacity = 60;

    f32 samples[kCapacity]{};
    u32 frame_index = 0;

    void push(f32 frame_ms) noexcept;
    [[nodiscard]] f32 average_ms() const noexcept;
    [[nodiscard]] DebugHudStats make_stats(f32 frame_ms,
                                           usize draw_calls = 0,
                                           usize triangles = 0,
                                           usize active_voices = 0) const noexcept;
};

// Stored cvar mirror. `r_debug_hud` host-side wires this up; tests poke
// the value directly.
DebugHudMode debug_hud_mode() noexcept;
void set_debug_hud_mode(DebugHudMode mode) noexcept;

// Push a one-line diagnostic message into the HUD's ring. Latest 6 lines
// are visible in `Full` mode. Lines are clipped to ~48 chars on render;
// the ring keeps the full string so callers can re-read if needed.
void push_diag_line(std::string_view line) noexcept;

// Drop all diag lines + reset the strip-chart ring. Tests call this so
// asserts on pixel counts aren't perturbed by earlier cases.
void reset_debug_hud() noexcept;

// Draws the HUD into `fb`. No-op when mode is `Off` or `fb` is not a
// supported pixel format. Safe to call without `begin_frame()` — it
// installs the framebuffer for the duration of the call and tears the
// pointer back down afterwards so it never leaks IMM context state.
//
// Returns the number of pixels written (post-clip), which the unit
// tests use to compare Compact vs Full bandwidth.
usize draw_debug_hud(render::Framebuffer& fb, const DebugHudStats& stats) noexcept;

}  // namespace psynder::ui::imm
