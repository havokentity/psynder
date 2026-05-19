// SPDX-License-Identifier: MIT
// Psynder — Lane 16 unit test (Wave B).
//
// Covers the three Wave-B headers carved off `engine/ui/imm/`:
//   • `Gizmo.h::gizmo_translate` — X-axis arm hit-test at the screen-
//     projected arrow tip returns true when the mouse-down state is on.
//   • `BrushPreview.h::brush_preview_box` — projects the 8 cube vertices
//     into screen space and emits 12 line segments via `imm::line`. We
//     verify by counting how many distinct corner pixels light up.
//   • `Heatmap.h::alloc_heatmap` — must not crash when the tag-stats
//     snapshot is "empty" (every row's budget == 0). We snapshot real
//     stats from `mem::tag_stats()` (which is always callable, even
//     with no allocators in flight) and just assert no abort + a sane
//     return value.

#include "ui/imm/Imm.h"
#include "ui/imm/Overlay.h"
#include "ui/imm/Gizmo.h"
#include "ui/imm/BrushPreview.h"
#include "ui/imm/Heatmap.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using psynder::i32;
using psynder::u32;
using psynder::u8;
using psynder::usize;
using psynder::f32;
using psynder::math::Vec2;
using psynder::math::Vec3;
using psynder::math::Vec4;
using psynder::math::Mat4;
using psynder::render::Framebuffer;
using psynder::render::PixelFormat;
namespace imm = psynder::ui::imm;

namespace {

constexpr u32 kW = 256;
constexpr u32 kH = 192;

struct TestFb {
    std::array<u32, kW * kH> pixels{};
    Framebuffer              fb{};

    TestFb() {
        fb.width  = kW;
        fb.height = kH;
        fb.format = PixelFormat::RGBA8;
        fb.pitch  = kW * 4U;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth  = nullptr;
        pixels.fill(0U);
    }

    u32 at(u32 x, u32 y) const {
        if (x >= kW || y >= kH) return 0U;
        return pixels[y * kW + x];
    }

    // Count non-zero pixels in a small square window centred on (cx,cy).
    usize count_ink_near(i32 cx, i32 cy, i32 r = 3) const {
        usize n = 0;
        for (i32 dy = -r; dy <= r; ++dy) {
            for (i32 dx = -r; dx <= r; ++dx) {
                const i32 x = cx + dx;
                const i32 y = cy + dy;
                if (x < 0 || y < 0) continue;
                if (static_cast<u32>(x) >= kW || static_cast<u32>(y) >= kH) continue;
                if (pixels[static_cast<u32>(y) * kW + static_cast<u32>(x)] != 0U) ++n;
            }
        }
        return n;
    }

    usize count_nonzero() const {
        usize n = 0;
        for (u32 v : pixels) if (v != 0U) ++n;
        return n;
    }
};

}  // namespace

TEST_CASE("ui_imm/wave-b: gizmo_translate hits the X-axis arrow at projected tip",
          "[ui_imm][gizmo][wave-b]") {
    TestFb t;
    imm::begin_frame(t.fb);

    // Use an identity projection: world XY → NDC XY → screen (with the
    // standard centre-and-flip mapping). With pos = (0,0,0) the gizmo
    // origin lands at the screen centre. A +X world step of 1.0 maps to
    // +X in NDC, which is +X (right) in screen pixels.
    const Mat4 vp = psynder::math::identity4();
    const Vec2 ss{ static_cast<f32>(kW), static_cast<f32>(kH) };

    // Screen-centre origin under identity vp + NDC mapping inside the
    // Gizmo.h projector: (0,0,0) → NDC (0,0,0) → screen
    // (0.5*W, (1-0.5)*H) = (W/2, H/2).
    const f32 cx = static_cast<f32>(kW) * 0.5f;
    const f32 cy = static_cast<f32>(kH) * 0.5f;

    // The X-arm is drawn at unit world-step's screen direction,
    // normalised to a fixed `kArmLengthPx` = 64. Identity projection
    // sends +X by ~(0.5*W) screen-pixels per world unit → direction is
    // pure +X. Tip lands at (cx + 64, cy). Mouse on the arm at half-arm.
    Vec3 pos{ 0.0f, 0.0f, 0.0f };
    const Vec2 mouse_on_x{ cx + 32.0f, cy };
    const bool grabbed = imm::gizmo_translate(pos, vp, mouse_on_x, /*down=*/true, ss);
    REQUIRE(grabbed);

    // Off the arm — far above the gizmo, well past the hit radius.
    Vec3 pos2{ 0.0f, 0.0f, 0.0f };
    const Vec2 mouse_off{ cx + 32.0f, cy + 40.0f };
    const bool not_grabbed = imm::gizmo_translate(pos2, vp, mouse_off, /*down=*/true, ss);
    REQUIRE_FALSE(not_grabbed);

    // The gizmo also draws three axis arrows via `imm::line`, so SOME
    // pixels must have been touched on the framebuffer.
    REQUIRE(t.count_nonzero() > 0U);

    imm::end_frame();
}

TEST_CASE("ui_imm/wave-b: brush_preview_box emits 12 cube-edge lines",
          "[ui_imm][brush_preview][wave-b]") {
    TestFb t;
    imm::begin_frame(t.fb);

    // Pick a box that, under identity projection, lands at known screen
    // positions. The Gizmo / BrushPreview projection sends world (x,y,z)
    // → NDC (x,y,z) → screen ((x+1)*0.5*W, (1-(y+1)*0.5)*H). With box
    // centre = (0,0,0) and full size = (1,1,1) the 8 corners are at
    // (±0.5, ±0.5, ±0.5). Their screen mappings are:
    //   X:  cx ± 0.25*W
    //   Y:  cy ∓ 0.25*H  (Y is flipped)
    // (Z does not influence x/y screen position under the identity vp.)
    const f32 W = static_cast<f32>(kW);
    const f32 H = static_cast<f32>(kH);
    const f32 cx = W * 0.5f;
    const f32 cy = H * 0.5f;
    const f32 dx = 0.25f * W;
    const f32 dy = 0.25f * H;

    constexpr u32 kInk = 0xFFFFFFFFu;
    // Default view_proj = identity, default screen_size = framebuffer
    // extent (the helper substitutes (1024, 768) when 0 is passed; pass
    // the real fb size so the corner math below matches kW/kH).
    const Mat4 vp = psynder::math::identity4();
    const Vec2 ss{ W, H };
    imm::brush_preview_box(Vec3{ 0.0f, 0.0f, 0.0f },
                           Vec3{ 1.0f, 1.0f, 1.0f },
                           kInk,
                           vp,
                           ss);

    // Each cube corner is shared by 3 edges → all 8 distinct screen-
    // corner positions (there are only 4 distinct (x,y) pairs under the
    // identity projection since +Z and -Z map to the same screen point,
    // so corners pair up: 8 corners → 4 distinct (x,y) positions). The
    // screen Y axis is flipped: world (-0.5, -0.5) → NDC (-0.5, -0.5) →
    // screen (cx - dx, cy + dy). We verify ink lands at all 4 distinct
    // screen positions.
    const std::array<Vec2, 4> kCorners = {
        Vec2{ cx - dx, cy + dy },  // world (-x,-y) → screen (left, bottom)
        Vec2{ cx + dx, cy + dy },  // world (+x,-y) → screen (right, bottom)
        Vec2{ cx + dx, cy - dy },  // world (+x,+y) → screen (right, top)
        Vec2{ cx - dx, cy - dy },  // world (-x,+y) → screen (left, top)
    };
    for (const Vec2& c : kCorners) {
        const i32 ix = static_cast<i32>(c.x + 0.5f);
        const i32 iy = static_cast<i32>(c.y + 0.5f);
        INFO("corner (" << ix << "," << iy << ")");
        // Widen the search window so we tolerate Bresenham endpoint
        // rounding (the line primitive snaps to integer pixels via
        // lround, which can be off by 1 from the projected float).
        REQUIRE(t.count_ink_near(ix, iy, /*r=*/5) > 0U);
    }

    // Sanity: drawing 12 line segments should leave a lot of lit
    // pixels — definitely more than 4 corners' worth. Most edges are
    // tens of pixels long under this projection.
    REQUIRE(t.count_nonzero() > 24U);

    imm::end_frame();
}

TEST_CASE("ui_imm/wave-b: alloc_heatmap does not crash with empty stats",
          "[ui_imm][heatmap][wave-b]") {
    TestFb t;
    imm::begin_frame(t.fb);

    // tag_stats() is always safe to call: it pure-reads the per-tag
    // atomics regardless of whether any allocator is currently in
    // flight. Drawing the heatmap with the default (often-zero) budget
    // values must not abort.
    const u32 rows = imm::alloc_heatmap(Vec2{ 10.0f, 10.0f },
                                        Vec2{ 120.0f, 80.0f });
    // tag_stats returns one row per Tag (excluding Count sentinel).
    REQUIRE(rows > 0U);

    // Background frame is drawn either way, so SOME pixels must be lit.
    REQUIRE(t.count_nonzero() > 0U);

    // Degenerate sizes are a no-op (return 0) and must not touch
    // anything beyond the framebuffer bounds.
    const u32 zero_rows = imm::alloc_heatmap(Vec2{ 0.0f, 0.0f },
                                             Vec2{ 0.0f, 0.0f });
    REQUIRE(zero_rows == 0U);

    imm::end_frame();
}
