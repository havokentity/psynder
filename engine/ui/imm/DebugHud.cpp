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
// Packed via imm::rgba (R in the low byte); raw 0xRRGGBBAA literals would
// channel-swap against the framebuffer and render wrong (dark blue -> red).
constexpr u32 kColourPanelBg = rgba(0x0D, 0x11, 0x1A, 0xD8);  // dark slate
constexpr u32 kColourPanelFr = rgba(0x5A, 0x61, 0x72);        // soft slate frame
constexpr u32 kColourLabel = rgba(0xE0, 0xE5, 0xEE);          // snow
constexpr u32 kColourFps = rgba(0xA3, 0xBE, 0x8C);            // muted green
constexpr u32 kColourDiagLine = rgba(0xAE, 0xB6, 0xC2);       // soft grey
constexpr u32 kColourSection = rgba(0x8F, 0xBC, 0xD8);        // cool blue
constexpr u32 kColourValue = rgba(0xD8, 0xDE, 0xE9);          // bright slate
constexpr u32 kColourWarn = rgba(0xE0, 0xBC, 0x60);           // warm amber

// Formatter for "<value> ms" + FPS right-aligned. Returns the rendered
// FPS string width in pixels so the caller can position it.
void format_frame_stats(f32 frame_ms, char* out_ms, usize out_ms_cap, char* out_fps, usize out_fps_cap) noexcept {
    const f32 fps = (frame_ms > 0.0001f) ? (1000.0f / frame_ms) : 0.0f;
    std::snprintf(out_ms, out_ms_cap, "frame %.2fms", static_cast<double>(frame_ms));
    std::snprintf(out_fps, out_fps_cap, "%.2f fps", static_cast<double>(fps));
}

void draw_stat_row(math::Vec2 pos, std::string_view label_text, std::string_view value_text) noexcept {
    label(pos, label_text, kColourSection);
    label(math::Vec2{pos.x + 56.0f, pos.y}, value_text, kColourValue);
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

void DebugHudFrameHistory::push(f32 frame_ms) noexcept {
    samples[frame_index % kCapacity] = frame_ms;
    ++frame_index;
}

f32 DebugHudFrameHistory::average_ms() const noexcept {
    const u32 n = std::min(frame_index, kCapacity);
    if (n == 0u)
        return 0.0f;
    f32 sum = 0.0f;
    for (u32 i = 0; i < n; ++i)
        sum += samples[i];
    return sum / static_cast<f32>(n);
}

DebugHudStats DebugHudFrameHistory::make_stats(f32 frame_ms,
                                               usize draw_calls,
                                               usize triangles,
                                               usize active_voices) const noexcept {
    DebugHudStats stats{};
    stats.frame_ms = frame_ms;
    stats.avg_frame_ms = average_ms();
    stats.draw_calls = draw_calls;
    stats.triangles = triangles;
    stats.active_voices = active_voices;
    stats.raster_stats_valid = true;
    stats.render_stats_valid = true;
    return stats;
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
    if (stats.raster_stats_valid) {
        std::snprintf(meta_buf,
                      sizeof(meta_buf),
                      "avg %.2fms dc%zu tri%zu v%zu",
                      static_cast<double>(stats.avg_frame_ms),
                      static_cast<size_t>(stats.draw_calls),
                      static_cast<size_t>(stats.triangles),
                      static_cast<size_t>(stats.active_voices));
    } else if (stats.rt_stats_valid) {
        std::snprintf(meta_buf,
                      sizeof(meta_buf),
                      "avg %.2fms rt%u jobs%u v%zu",
                      static_cast<double>(stats.avg_frame_ms),
                      stats.rt_tiles,
                      stats.rt_jobs,
                      static_cast<size_t>(stats.active_voices));
    } else {
        std::snprintf(meta_buf,
                      sizeof(meta_buf),
                      "avg %.2fms dc-- tri-- v%zu",
                      static_cast<double>(stats.avg_frame_ms),
                      static_cast<size_t>(stats.active_voices));
    }
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
        // Full mode: strip chart + concrete stats + alloc heatmap + diag log.
        const f32 chart_y = panel_origin.y + 4.0f + 2.0f * kLineHeight + 2.0f;
        const math::Vec2 chart_origin{panel_origin.x + 4.0f, chart_y};
        const math::Vec2 chart_size{panel_size.x - 8.0f, 36.0f};
        graph(chart_origin,
              chart_size,
              stats.frame_ms,
              /*max_ms*/ 33.3f,
              std::string_view{"frame ms"});

        char row0[64]{};
        char row1[64]{};
        char row2[64]{};
        const f32 avg_fps = (stats.avg_frame_ms > 0.0001f) ? (1000.0f / stats.avg_frame_ms) : 0.0f;
        const f32 budget_delta = stats.frame_ms - 16.6667f;
        if (stats.raster_stats_valid) {
            std::snprintf(row0,
                          sizeof(row0),
                          "%zu draws / %zu tris",
                          static_cast<size_t>(stats.draw_calls),
                          static_cast<size_t>(stats.triangles));
        } else if (stats.rt_stats_valid) {
            std::snprintf(row0,
                          sizeof(row0),
                          "rt %u tiles / %u jobs",
                          stats.rt_tiles,
                          stats.rt_jobs);
        } else {
            std::snprintf(row0, sizeof(row0), "not reported");
        }
        std::snprintf(row1,
                      sizeof(row1),
                      "%.2f avg ms / %.1f fps",
                      static_cast<double>(stats.avg_frame_ms),
                      static_cast<double>(avg_fps));
        std::snprintf(row2,
                      sizeof(row2),
                      "%+.2f ms vs 60Hz, %zu voices",
                      static_cast<double>(budget_delta),
                      static_cast<size_t>(stats.active_voices));

        const f32 stats_y = chart_y + chart_size.y + 5.0f;
        label(math::Vec2{panel_origin.x + 4.0f, stats_y}, "render", kColourSection);
        draw_stat_row(math::Vec2{panel_origin.x + 4.0f, stats_y + kLineHeight}, "work", row0);
        draw_stat_row(math::Vec2{panel_origin.x + 4.0f, stats_y + 2.0f * kLineHeight}, "pace", row1);
        draw_stat_row(math::Vec2{panel_origin.x + 4.0f, stats_y + 3.0f * kLineHeight}, "budget", row2);

        const f32 heat_y = stats_y + 4.0f * kLineHeight + 4.0f;
        label(math::Vec2{panel_origin.x + 4.0f, heat_y}, "alloc heatmap", kColourSection);
        const math::Vec2 heat_origin{panel_origin.x + 4.0f, heat_y + kLineHeight};
        const math::Vec2 heat_size{panel_size.x - 8.0f, 34.0f};
        alloc_heatmap(heat_origin, heat_size);

        // Diag log lines (oldest first, newest at bottom).
        const f32 log_y = heat_origin.y + heat_size.y + 4.0f;
        label(math::Vec2{panel_origin.x + 4.0f, log_y}, "diag", kColourSection);
        const u32 visible = std::min(g_diag.count, kDiagRingCapacity);
        const u32 start = (g_diag.head + kDiagRingCapacity - visible) % kDiagRingCapacity;
        if (visible == 0u) {
            label(math::Vec2{panel_origin.x + 4.0f, log_y + kLineHeight},
                  "no diag lines",
                  kColourWarn);
        } else {
            const u32 max_lines = std::min(visible, 3u);
            for (u32 i = 0; i < max_lines; ++i) {
                const u32 src = visible - max_lines + i;
                const DiagLine& dl = g_diag.lines[(start + src) % kDiagRingCapacity];
                label(math::Vec2{panel_origin.x + 4.0f, log_y + (1.0f + static_cast<f32>(i)) * kLineHeight},
                      std::string_view{dl.buf, dl.len},
                      kColourDiagLine);
            }
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
