// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: HiZ buffer at 8×8 per tile (DESIGN.md §7.3).
// The HiZ stores the maximum (farthest) depth per 8×8 cell so a triangle
// whose min-z exceeds every cell's max-z can be early-rejected.

#include "render/Framebuffer.h"
#include "render/raster/HiZ.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

// Pack a [0,1] depth value the same way the rasterizer's pack_depth does
// (24-bit float Z + 8-bit stencil = 0).
u32 pack_z(f32 z) noexcept {
    u32 raw;
    std::memcpy(&raw, &z, sizeof(raw));
    return raw & 0xFFFFFF00u;
}

}  // namespace

TEST_CASE("HiZ tile clears to 1.0 (far plane)", "[raster][hiz][adr-002]") {
    HiZTile<64, 64> h;
    h.clear();
    for (u32 i = 0; i < h.kCells; ++i) {
        REQUIRE(h.max_z[i] == 1.0f);
    }
    REQUIRE(h.kCols == 8);
    REQUIRE(h.kRows == 8);
}

TEST_CASE("HiZ tile dimensions match ADR-002 / DESIGN.md sec 7.3", "[raster][hiz][adr-002]") {
    REQUIRE(HiZTile<32, 32>::kCols == 4);
    REQUIRE(HiZTile<32, 32>::kRows == 4);
    REQUIRE(HiZTile<64, 64>::kCols == 8);
    REQUIRE(HiZTile<64, 64>::kRows == 8);
    REQUIRE(HiZTile<128, 128>::kCols == 16);
    REQUIRE(HiZTile<128, 128>::kRows == 16);
}

TEST_CASE("HiZ rebuild_from_fb reads the depth slice", "[raster][hiz]") {
    constexpr u32 W = 64, H = 64;
    std::vector<u32> depth(W * H, pack_z(1.0f));
    Framebuffer fb{};
    fb.width = W;
    fb.height = H;
    fb.depth = depth.data();

    // Carve a foreground patch (z = 0.25) covering rows 0..7, cols 0..7
    // — exactly one HiZ cell.
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            depth[y * W + x] = pack_z(0.25f);
        }
    }

    HiZTile<64, 64> h;
    h.rebuild_from_fb(fb, 0, 0);

    // The cell covering (0..7, 0..7) sees max-z = 1.0 from neighbouring
    // pixels not in the patch — wait, actually, that cell is *entirely*
    // covered by the patch, so the cell's max IS 0.25. (The cell is
    // 8×8 = patch.)
    REQUIRE(h.max_z[0] == 0.25f);
    // Every other cell sees max-z = 1.0 (untouched far-plane).
    for (u32 i = 1; i < h.kCells; ++i) {
        REQUIRE(h.max_z[i] == 1.0f);
    }
}

TEST_CASE("HiZ any_cell_passes: a test_z behind every cell is rejected", "[raster][hiz][reject]") {
    HiZTile<64, 64> h;
    h.clear();
    // Set every cell to z = 0.4.
    for (u32 i = 0; i < h.kCells; ++i)
        h.max_z[i] = 0.4f;

    // A triangle with min-z = 0.5 is behind every cell — reject.
    REQUIRE_FALSE(h.any_cell_passes(0, 0, 64, 64, 0.5f));
    // A triangle with min-z = 0.3 passes — at least one cell.
    REQUIRE(h.any_cell_passes(0, 0, 64, 64, 0.3f));
}

TEST_CASE("HiZ any_cell_passes: partial bbox tests only its cells", "[raster][hiz][reject]") {
    HiZTile<64, 64> h;
    h.clear();
    // First cell only — push to 0.1; every other cell stays at 1.0.
    h.max_z[0] = 0.1f;

    // Test bbox only covering cell 0 (pixels 0..7) — a triangle behind
    // it gets rejected since the cell's max is 0.1.
    REQUIRE_FALSE(h.any_cell_passes(0, 0, 8, 8, 0.2f));
    // Same bbox, triangle in front — passes.
    REQUIRE(h.any_cell_passes(0, 0, 8, 8, 0.05f));
    // Bbox covering cell 0 and cell 1 — cell 1 has max-z = 1.0 so the
    // triangle at 0.2 passes via cell 1.
    REQUIRE(h.any_cell_passes(0, 0, 16, 8, 0.2f));
}

TEST_CASE("HiZ update_cell only tightens (max-z monotonically decreases)", "[raster][hiz]") {
    HiZTile<64, 64> h;
    h.clear();
    h.update_cell(0, 0.5f);
    REQUIRE(h.max_z[0] == 0.5f);
    // Trying to write a larger max-z must be a no-op (HiZ is the upper
    // bound on the current depth-buffer contents).
    h.update_cell(0, 0.8f);
    REQUIRE(h.max_z[0] == 0.5f);
    // Tighter value wins.
    h.update_cell(0, 0.3f);
    REQUIRE(h.max_z[0] == 0.3f);
}
