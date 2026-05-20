// SPDX-License-Identifier: MIT
// Psynder — Lane 16 unit test. rect_outline pixel correctness.
//
// Drives the IMM drawing primitives against a stack-allocated 32-bit
// framebuffer and verifies the exact pixel mask produced. As with the
// button test we include the detail headers directly so the unit binary
// does not have to link `psynder_ui_imm`.

#include "ui/imm/detail/Draw.h"
#include "ui/imm/detail/Pixel.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using psynder::u32;
using psynder::u8;
using psynder::usize;
using psynder::math::Vec2;
using psynder::render::Framebuffer;
using psynder::render::PixelFormat;
namespace imm = psynder::ui::imm::detail;

namespace {

constexpr u32 kW = 16;
constexpr u32 kH = 12;

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
    void clear() { pixels.fill(0U); }
};

constexpr u32 kInk = 0xAABBCCDDu;

// Count how many pixels equal a target colour — useful for "no overdraw"
// invariants on the outline.
usize count_eq(const TestFb& t, u32 colour) {
    usize n = 0;
    for (u32 v : t.pixels)
        if (v == colour)
            ++n;
    return n;
}

}  // namespace

TEST_CASE("ui_imm: rect_outline draws only the perimeter, corners included",
          "[ui_imm][rect_outline]") {
    TestFb t;
    imm::rect_outline(t.fb, Vec2{2.0f, 3.0f}, Vec2{6.0f, 4.0f}, kInk);

    // Rect occupies x ∈ [2,7] inclusive (width 6, so 2..7), y ∈ [3,6].
    // Perimeter count = 2*W + 2*(H-2) = 12 + 4 = 16.
    REQUIRE(count_eq(t, kInk) == 16);

    // Corners.
    REQUIRE(t.at(2, 3) == kInk);
    REQUIRE(t.at(7, 3) == kInk);
    REQUIRE(t.at(2, 6) == kInk);
    REQUIRE(t.at(7, 6) == kInk);

    // Top + bottom edges.
    for (u32 x = 2; x <= 7; ++x) {
        REQUIRE(t.at(x, 3) == kInk);
        REQUIRE(t.at(x, 6) == kInk);
    }
    // Left + right edges (excluding the corner rows).
    for (u32 y = 4; y <= 5; ++y) {
        REQUIRE(t.at(2, y) == kInk);
        REQUIRE(t.at(7, y) == kInk);
    }
    // Interior pixels stay untouched.
    for (u32 y = 4; y <= 5; ++y) {
        for (u32 x = 3; x <= 6; ++x) {
            REQUIRE(t.at(x, y) == 0U);
        }
    }
    // Pixels outside the rect stay untouched.
    REQUIRE(t.at(1, 3) == 0U);
    REQUIRE(t.at(8, 3) == 0U);
    REQUIRE(t.at(2, 2) == 0U);
    REQUIRE(t.at(2, 7) == 0U);
}

TEST_CASE("ui_imm: rect_outline 1x1 rect plots exactly one pixel", "[ui_imm][rect_outline]") {
    TestFb t;
    imm::rect_outline(t.fb, Vec2{5.0f, 5.0f}, Vec2{1.0f, 1.0f}, kInk);
    REQUIRE(count_eq(t, kInk) == 1);
    REQUIRE(t.at(5, 5) == kInk);
}

TEST_CASE("ui_imm: rect_outline 2x1 rect plots two horizontal pixels", "[ui_imm][rect_outline]") {
    TestFb t;
    imm::rect_outline(t.fb, Vec2{3.0f, 7.0f}, Vec2{2.0f, 1.0f}, kInk);
    REQUIRE(count_eq(t, kInk) == 2);
    REQUIRE(t.at(3, 7) == kInk);
    REQUIRE(t.at(4, 7) == kInk);
}

TEST_CASE("ui_imm: rect_outline clips to the framebuffer", "[ui_imm][rect_outline]") {
    TestFb t;
    // Cross top-left corner: rect at (-2,-1), size 5x4 → only pixels with
    // x ∈ [0,2], y ∈ [0,2] should land. Specifically the right edge of
    // the rect (x=2) and bottom edge (y=2) are visible; the left + top
    // are clipped away.
    imm::rect_outline(t.fb, Vec2{-2.0f, -1.0f}, Vec2{5.0f, 4.0f}, kInk);

    REQUIRE(t.at(2, 0) == kInk);  // Right edge visible.
    REQUIRE(t.at(2, 1) == kInk);
    REQUIRE(t.at(2, 2) == kInk);
    REQUIRE(t.at(0, 2) == kInk);  // Bottom edge visible.
    REQUIRE(t.at(1, 2) == kInk);
    // Pixel inside the clipped (away) interior stays untouched.
    REQUIRE(t.at(0, 0) == 0U);
    REQUIRE(t.at(1, 1) == 0U);
}

TEST_CASE("ui_imm: filled_rect paints the half-open interior", "[ui_imm][filled_rect]") {
    TestFb t;
    imm::filled_rect(t.fb, Vec2{4.0f, 2.0f}, Vec2{3.0f, 2.0f}, kInk);

    // Interior = x ∈ [4,6], y ∈ [2,3]. Total 6 pixels.
    REQUIRE(count_eq(t, kInk) == 6);
    for (u32 y = 2; y <= 3; ++y) {
        for (u32 x = 4; x <= 6; ++x) {
            REQUIRE(t.at(x, y) == kInk);
        }
    }
    // Half-open: x=7 must NOT be painted, y=4 must NOT be painted.
    REQUIRE(t.at(7, 2) == 0U);
    REQUIRE(t.at(4, 4) == 0U);
}

TEST_CASE("ui_imm: line plots both endpoints", "[ui_imm][line]") {
    TestFb t;
    imm::line(t.fb, Vec2{1.0f, 1.0f}, Vec2{6.0f, 4.0f}, kInk);
    REQUIRE(t.at(1, 1) == kInk);
    REQUIRE(t.at(6, 4) == kInk);
    // Diagonal must hit at least one intermediate pixel.
    REQUIRE(count_eq(t, kInk) >= 4U);
}

TEST_CASE("ui_imm: plot_blend preserves opaque source", "[ui_imm][blend]") {
    TestFb t;
    imm::plot_blend(t.fb, 5, 5, imm::rgba(0xFF, 0x00, 0x00, 0xFF));
    REQUIRE(t.at(5, 5) == imm::rgba(0xFF, 0x00, 0x00, 0xFF));
}

TEST_CASE("ui_imm: plot_blend half-alpha blends towards the destination", "[ui_imm][blend]") {
    TestFb t;
    t.pixels[5 * kW + 5] = imm::rgba(0x00, 0x00, 0xFF, 0xFF);        // blue
    imm::plot_blend(t.fb, 5, 5, imm::rgba(0xFF, 0x00, 0x00, 0x80));  // red 50%
    // 0x80 ≈ 50%: red channel ≈ 128, blue channel ≈ 127, green 0.
    const u32 out = t.at(5, 5);
    const u32 out_r = (out >> 24) & 0xFFu;
    const u32 out_g = (out >> 16) & 0xFFu;
    const u32 out_b = (out >> 8) & 0xFFu;
    REQUIRE(out_r > 120U);
    REQUIRE(out_r < 136U);
    REQUIRE(out_g == 0U);
    REQUIRE(out_b > 120U);
    REQUIRE(out_b < 136U);
}
