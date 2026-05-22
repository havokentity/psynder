// SPDX-License-Identifier: MIT
// Psynder — Lane 16 unit tests for the Wave-C debug HUD.
//
// Two cases:
//   1. HUD draws pixels in the expected top-left region when called.
//   2. Compact mode draws strictly fewer pixels than Full mode.

#include "ui/imm/DebugHud.h"

#include "core/Types.h"
#include "editor/core/SampleHook.h"
#include "render/Framebuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>

using psynder::u32;
using psynder::u8;
using psynder::usize;
using psynder::render::Framebuffer;
using psynder::render::PixelFormat;

namespace imm = psynder::ui::imm;

namespace {

constexpr u32 kW = 320;
constexpr u32 kH = 240;

struct TestFb {
    std::array<u32, kW * kH> pixels{};
    Framebuffer fb{};

    TestFb() {
        fb.width = kW;
        fb.height = kH;
        fb.format = PixelFormat::RGBA8;
        fb.pitch = kW * 4U;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth = nullptr;
        pixels.fill(0U);
    }

    u32 at(u32 x, u32 y) const { return pixels[y * kW + x]; }

    // Count non-zero pixels inside [x0,x1) × [y0,y1).
    usize count_nonzero(u32 x0, u32 y0, u32 x1, u32 y1) const {
        usize n = 0;
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                if (pixels[y * kW + x] != 0U)
                    ++n;
            }
        }
        return n;
    }
};

imm::DebugHudStats sample_stats() {
    imm::DebugHudStats s{};
    s.frame_ms = 16.67f;
    s.avg_frame_ms = 16.50f;
    s.draw_calls = 1234;
    s.triangles = 56789;
    s.active_voices = 4;
    s.raster_stats_valid = true;
    s.render_stats_valid = true;
    return s;
}

}  // namespace

TEST_CASE("ui_imm::DebugHud: Full mode paints pixels in the top-left panel", "[ui_imm][debug_hud]") {
    imm::reset_debug_hud();
    imm::set_debug_hud_mode(imm::DebugHudMode::Full);

    TestFb t;
    const auto stats = sample_stats();
    const usize written = imm::draw_debug_hud(t.fb, stats);

    // HUD must draw something.
    REQUIRE(written > 0U);

    // Header label "frame ... ms" is near the top-left corner of the
    // panel (panel origin ≈ (4,4), text inset another 4px, so glyphs
    // start around x≈8, y≈8). Verify at least one pixel inside the
    // expected text band is non-zero.
    REQUIRE(t.count_nonzero(8U, 8U, 60U, 18U) > 0U);

    // Panel frame outline at (4,4) → outline pixel at (4,4) must be set.
    REQUIRE(t.at(4U, 4U) != 0U);

    // Full mode should not be a mostly empty tall panel when no diagnostics
    // have been pushed; it still owns render stats, memory, and diag labels.
    REQUIRE(t.count_nonzero(8U, 72U, 210U, 180U) > 0U);

    // Region clearly *outside* the panel must remain untouched. The
    // panel is at most 224 px wide / 204 px tall. Pick a pixel well
    // outside that and assert it's still zero.
    REQUIRE(t.at(300U, 230U) == 0U);
}

TEST_CASE("editor overlay stats keep raster and RT reports separate", "[ui_imm][debug_hud]") {
    psynder::editor::FrameOverlayStats rt{};
    rt.rt_tiles = 512;
    rt.rt_jobs = 4;
    rt.rt_stats_valid = true;
    rt.render_stats_valid = true;

    const imm::DebugHudStats rt_hud = psynder::editor::make_debug_hud_stats(8.0f, 8.0f, rt);
    REQUIRE_FALSE(rt_hud.raster_stats_valid);
    REQUIRE(rt_hud.rt_stats_valid);
    REQUIRE(rt_hud.rt_tiles == 512U);
    REQUIRE(rt_hud.rt_jobs == 4U);

    psynder::editor::FrameOverlayStats legacy{};
    const imm::DebugHudStats legacy_hud =
        psynder::editor::make_debug_hud_stats_with_render(8.0f, 8.0f, legacy);
    REQUIRE(legacy_hud.raster_stats_valid);
    REQUIRE_FALSE(legacy_hud.rt_stats_valid);
}

TEST_CASE("ui_imm::DebugHud: Compact mode writes fewer pixels than Full", "[ui_imm][debug_hud]") {
    const auto stats = sample_stats();

    imm::reset_debug_hud();
    // Seed a few diag lines so the Full-mode log adds real glyphs.
    imm::push_diag_line("ent: spawn marker_alpha");
    imm::push_diag_line("phys: 12 islands awake");
    imm::push_diag_line("ai: graph rebuilt 2ms");
    imm::push_diag_line("net: snapshot 11kb");
    imm::push_diag_line("snd: 4 voices");
    imm::push_diag_line("gpu: tile bin 9ms");

    TestFb t_full;
    imm::set_debug_hud_mode(imm::DebugHudMode::Full);
    const usize full_written = imm::draw_debug_hud(t_full.fb, stats);

    imm::reset_debug_hud();
    TestFb t_compact;
    imm::set_debug_hud_mode(imm::DebugHudMode::Compact);
    const usize compact_written = imm::draw_debug_hud(t_compact.fb, stats);

    REQUIRE(compact_written > 0U);
    REQUIRE(full_written > 0U);
    REQUIRE(compact_written < full_written);

    // Sanity: Off mode produces zero writes.
    imm::set_debug_hud_mode(imm::DebugHudMode::Off);
    TestFb t_off;
    const usize off_written = imm::draw_debug_hud(t_off.fb, stats);
    REQUIRE(off_written == 0U);
    REQUIRE(t_off.count_nonzero(0U, 0U, kW, kH) == 0U);
}
