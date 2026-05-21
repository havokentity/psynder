// SPDX-License-Identifier: MIT
// Psynder — Lane 16 (immediate-mode UI). DEBUG HUD impl — Wave C.

#include "DebugHud.h"

#include "Heatmap.h"  // imm::alloc_heatmap
#include "Imm.h"
#include "Overlay.h"

#include "detail/Context.h"
#include "detail/Draw.h"
#include "detail/Font.h"
#include "detail/Pixel.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace psynder::ui::imm {

namespace {

namespace d = ::psynder::ui::imm::detail;

// ─── Cvar mirror ──────────────────────────────────────────────────────────
// Default OFF so the HUD never blasts the scene unprompted; F1 (wired in
// editor::sample_step) cycles Off -> Compact -> Full, and the `r_debug_hud`
// cvar still sets it explicitly.
DebugHudMode g_mode = DebugHudMode::Off;

// ─── Diag-line ring ───────────────────────────────────────────────────────
//
// 6 visible lines, each capped at 64 chars + NUL. Storage is plain POD so
// it survives across frames without per-frame allocation (DESIGN.md §3.4).

constexpr u32 kDiagRingCapacity = 6;
constexpr u32 kDiagLineMax = 64;

struct DiagLine {
    char buf[kDiagLineMax + 1]{};
    u8 len = 0;
};

struct DiagRing {
    std::array<DiagLine, kDiagRingCapacity> lines{};
    u32 head = 0;  // next write slot
    u32 count = 0;
} g_diag;

void diag_push(std::string_view text) noexcept {
    DiagLine& slot = g_diag.lines[g_diag.head];
    const usize n = std::min(text.size(), static_cast<usize>(kDiagLineMax));
    std::memcpy(slot.buf, text.data(), n);
    slot.buf[n] = '\0';
    slot.len = static_cast<u8>(n);
    g_diag.head = (g_diag.head + 1U) % kDiagRingCapacity;
    if (g_diag.count < kDiagRingCapacity)
        ++g_diag.count;
}

// ─── Strip-chart ring (separate from Overlay.h's perf ring so the HUD
// owns its own 60-sample window without disturbing the editor's graph).
constexpr u32 kStripSamples = 60;

struct StripRing {
    std::array<f32, kStripSamples> samples{};
    u32 head = 0;
    u32 count = 0;
} g_strip;

void strip_push(f32 ms) noexcept {
    g_strip.samples[g_strip.head] = ms;
    g_strip.head = (g_strip.head + 1U) % kStripSamples;
    if (g_strip.count < kStripSamples)
        ++g_strip.count;
}

// ─── Layout constants ────────────────────────────────────────────────────
constexpr f32 kPad = 4.0f;
constexpr f32 kLineHeight = 10.0f;  // 6x8 font + 2px gutter
constexpr f32 kPanelW = 220.0f;

// Muted palette to match the console overlay — nothing glares over gameplay.
constexpr u32 kColourPanelBg = 0x0D111AD8u;   // dark slate, a touch more opaque
constexpr u32 kColourPanelFr = 0x5A6172FFu;   // soft slate frame (was mid-grey)
constexpr u32 kColourLabel = 0xE0E5EEFFu;     // snow (was pure white)
constexpr u32 kColourFps = 0xA3BE8CFFu;       // muted green (was glaring 0x80FF80)
constexpr u32 kColourDiagLine = 0xAEB6C2FFu;  // soft grey

// Formatter for "<value> ms" + FPS right-aligned. Returns the rendered
// FPS string width in pixels so the caller can position it.
void format_frame_stats(f32 frame_ms, char* out_ms, usize out_ms_cap, char* out_fps, usize out_fps_cap) noexcept {
    const f32 fps = (frame_ms > 0.0001f) ? (1000.0f / frame_ms) : 0.0f;
    std::snprintf(out_ms, out_ms_cap, "frame %.2fms", static_cast<double>(frame_ms));
    std::snprintf(out_fps, out_fps_cap, "%.2f fps", static_cast<double>(fps));
}

}  // namespace

// ─── Public surface ──────────────────────────────────────────────────────

DebugHudMode debug_hud_mode() noexcept {
    return g_mode;
}
void set_debug_hud_mode(DebugHudMode mode) noexcept {
    g_mode = mode;
}

void push_diag_line(std::string_view line) noexcept {
    diag_push(line);
}

void reset_debug_hud() noexcept {
    g_diag = DiagRing{};
    g_strip = StripRing{};
}

usize draw_debug_hud(render::Framebuffer& fb, const DebugHudStats& stats) noexcept {
    if (g_mode == DebugHudMode::Off)
        return 0;
    if (!d::framebuffer_drawable(fb))
        return 0;

    // Push the latest frame-time sample onto our private strip-chart ring
    // so the chart scrolls regardless of whether the host also pushes to
    // the global `imm::graph` ring.
    strip_push(stats.frame_ms);

    // Snapshot pixels in the target region so we can return a write
    // count without depending on context state.
    const u32 panel_w = static_cast<u32>(kPanelW);
    const u32 panel_h = (g_mode == DebugHudMode::Full) ? 200U : 80U;
    const u32 x1 = std::min(fb.width, panel_w + 4U);
    const u32 y1 = std::min(fb.height, panel_h + 4U);
    // Static so we don't burn 180 KB of stack each call. Lane 16 is
    // single-threaded (DESIGN.md §3.4 — UI runs on the main thread only).
    static std::array<u32, 224 * 204> snapshot{};
    const usize snapshot_cap = snapshot.size();
    for (u32 y = 0; y < y1; ++y) {
        const u32* row = reinterpret_cast<const u32*>(fb.pixels + static_cast<usize>(fb.pitch) * y);
        for (u32 x = 0; x < x1; ++x) {
            const u32 idx = y * fb.width + x;
            // Aux snapshot is indexed densely (panel-local) to bound size.
            const usize sidx = static_cast<usize>(y) * x1 + x;
            if (sidx < snapshot_cap)
                snapshot[sidx] = row[x];
        }
    }

    // Install framebuffer into the IMM context so the lane's drawing
    // helpers (label/graph/heatmap/etc.) hit `fb`. Save+restore the prior
    // pointer so we never trample a caller's open frame.
    auto& ctx = d::context();
    render::Framebuffer* prev_target = ctx.target;
    bool prev_frame_open = ctx.frame_open;
    ctx.target = &fb;
    ctx.frame_open = true;

    // ─── Panel background ────────────────────────────────────────────
    const math::Vec2 panel_origin{kPad, kPad};
    const math::Vec2 panel_size{
        static_cast<f32>(panel_w),
        static_cast<f32>(panel_h),
    };
    filled_rect(panel_origin, panel_size, kColourPanelBg);
    rect_outline(panel_origin, panel_size, kColourPanelFr);

    // ─── Header line: "frame X.XXms" + right-aligned FPS ─────────────
    char ms_buf[32]{};
    char fps_buf[24]{};
    format_frame_stats(stats.frame_ms, ms_buf, sizeof(ms_buf), fps_buf, sizeof(fps_buf));
    label(math::Vec2{panel_origin.x + 4.0f, panel_origin.y + 4.0f},
          std::string_view{ms_buf},
          kColourLabel);

    const std::string_view fps_view{fps_buf};
    const u32 fps_px = d::text_width(fps_view);
    const f32 fps_x = panel_origin.x + panel_size.x - 4.0f - static_cast<f32>(fps_px);
    label(math::Vec2{fps_x, panel_origin.y + 4.0f}, fps_view, kColourFps);

    // Second header line: avg ms + draw/tri/voice counts.
    char meta_buf[64]{};
    std::snprintf(meta_buf,
                  sizeof(meta_buf),
                  "avg %.2fms dc%zu tri%zu v%zu",
                  static_cast<double>(stats.avg_frame_ms),
                  static_cast<size_t>(stats.draw_calls),
                  static_cast<size_t>(stats.triangles),
                  static_cast<size_t>(stats.active_voices));
    label(math::Vec2{panel_origin.x + 4.0f, panel_origin.y + 4.0f + kLineHeight},
          std::string_view{meta_buf},
          kColourLabel);

    if (g_mode == DebugHudMode::Compact) {
        // Strip chart only (Compact). Use the lane's graph() so the
        // chart shares its rendering with the editor overlay.
        const math::Vec2 chart_origin{
            panel_origin.x + 4.0f,
            panel_origin.y + 4.0f + 2.0f * kLineHeight + 2.0f,
        };
        const math::Vec2 chart_size{
            panel_size.x - 8.0f,
            panel_size.y - (4.0f + 2.0f * kLineHeight + 6.0f),
        };
        graph(chart_origin,
              chart_size,
              stats.frame_ms,
              /*max_ms*/ 33.3f,
              std::string_view{});
    } else {
        // Full mode: strip chart + alloc heatmap + diag log.
        const f32 chart_y = panel_origin.y + 4.0f + 2.0f * kLineHeight + 2.0f;
        const math::Vec2 chart_origin{panel_origin.x + 4.0f, chart_y};
        const math::Vec2 chart_size{panel_size.x - 8.0f, 36.0f};
        graph(chart_origin,
              chart_size,
              stats.frame_ms,
              /*max_ms*/ 33.3f,
              std::string_view{"frame ms"});

        const f32 heat_y = chart_y + chart_size.y + 4.0f;
        const math::Vec2 heat_origin{panel_origin.x + 4.0f, heat_y};
        const math::Vec2 heat_size{panel_size.x - 8.0f, 48.0f};
        alloc_heatmap(heat_origin, heat_size);

        // Diag log lines (oldest first, newest at bottom).
        const f32 log_y = heat_y + heat_size.y + 4.0f;
        const u32 visible = std::min(g_diag.count, kDiagRingCapacity);
        const u32 start = (g_diag.head + kDiagRingCapacity - visible) % kDiagRingCapacity;
        for (u32 i = 0; i < visible; ++i) {
            const DiagLine& dl = g_diag.lines[(start + i) % kDiagRingCapacity];
            label(math::Vec2{panel_origin.x + 4.0f, log_y + static_cast<f32>(i) * kLineHeight},
                  std::string_view{dl.buf, dl.len},
                  kColourDiagLine);
        }
    }

    // Tear down our context manipulation.
    ctx.target = prev_target;
    ctx.frame_open = prev_frame_open;

    // Count pixels written so the test can compare modes. Densely-indexed
    // snapshot covers panel-local coords (x ∈ [0,x1), y ∈ [0,y1)).
    usize written = 0;
    for (u32 y = 0; y < y1; ++y) {
        const u32* row = reinterpret_cast<const u32*>(fb.pixels + static_cast<usize>(fb.pitch) * y);
        for (u32 x = 0; x < x1; ++x) {
            const usize sidx = static_cast<usize>(y) * x1 + x;
            if (sidx >= snapshot_cap)
                continue;
            if (row[x] != snapshot[sidx])
                ++written;
        }
    }
    return written;
}

}  // namespace psynder::ui::imm
