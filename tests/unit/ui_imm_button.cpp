// SPDX-License-Identifier: MIT
// Psynder — Lane 16 unit test. Button hit-test invariants.
//
// Includes the IMM detail headers directly so the unit-test binary does
// not need to link `psynder_ui_imm` — `tests/unit/CMakeLists.txt` is
// owned by Lane 25, not 16, and we keep the cross-lane wiring untouched.

#include "ui/imm/detail/Context.h"
#include "ui/imm/detail/Widgets.h"

#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

namespace imm = psynder::ui::imm::detail;
using psynder::math::Vec2;

namespace {

// Re-prime the global IMM context before each test case so order doesn't
// matter when Catch2 shuffles.
void reset_context() {
    auto& ctx = imm::context();
    ctx = imm::Context{};
}

}  // namespace

TEST_CASE("ui_imm: rect hit-test includes the min edge and excludes the max", "[ui_imm][hit_test]") {
    const Vec2 pos{10.0f, 20.0f};
    const Vec2 size{8.0f, 4.0f};

    // Inside.
    REQUIRE(imm::hit_test(pos, size, Vec2{10.0f, 20.0f}));
    REQUIRE(imm::hit_test(pos, size, Vec2{14.0f, 22.0f}));

    // On the exclusive max edge.
    REQUIRE_FALSE(imm::hit_test(pos, size, Vec2{18.0f, 22.0f}));
    REQUIRE_FALSE(imm::hit_test(pos, size, Vec2{14.0f, 24.0f}));

    // Strictly outside.
    REQUIRE_FALSE(imm::hit_test(pos, size, Vec2{9.99f, 22.0f}));
    REQUIRE_FALSE(imm::hit_test(pos, size, Vec2{14.0f, 19.99f}));

    // Zero or negative size → never inside.
    REQUIRE_FALSE(imm::hit_test(pos, Vec2{0.0f, 4.0f}, Vec2{10.0f, 22.0f}));
    REQUIRE_FALSE(imm::hit_test(pos, Vec2{8.0f, -1.0f}, Vec2{10.0f, 22.0f}));
}

TEST_CASE("ui_imm: button triggers on click-release inside the rect", "[ui_imm][button]") {
    reset_context();
    auto& ctx = imm::context();
    const Vec2 pos{100.0f, 50.0f};
    const Vec2 size{40.0f, 16.0f};
    constexpr std::string_view label = "OK";

    // Frame 1: hover, no click yet.
    ctx.input.mouse_down_prev = false;
    ctx.input.mouse_down = false;
    ctx.input.mouse = Vec2{110.0f, 58.0f};
    REQUIRE_FALSE(imm::button_logic(ctx, pos, size, label));
    REQUIRE(ctx.hot_id != 0);
    REQUIRE(ctx.active_id == 0);

    // Frame 2: press.
    ctx.input.mouse_down_prev = false;
    ctx.input.mouse_down = true;
    REQUIRE_FALSE(imm::button_logic(ctx, pos, size, label));
    REQUIRE(ctx.active_id != 0);

    // Frame 3: still held inside.
    ctx.input.mouse_down_prev = true;
    ctx.input.mouse_down = true;
    REQUIRE_FALSE(imm::button_logic(ctx, pos, size, label));
    REQUIRE(ctx.active_id != 0);

    // Frame 4: release inside → trigger.
    ctx.input.mouse_down_prev = true;
    ctx.input.mouse_down = false;
    REQUIRE(imm::button_logic(ctx, pos, size, label));
    REQUIRE(ctx.active_id == 0);
}

TEST_CASE("ui_imm: button does NOT trigger if released outside the rect", "[ui_imm][button]") {
    reset_context();
    auto& ctx = imm::context();
    const Vec2 pos{100.0f, 50.0f};
    const Vec2 size{40.0f, 16.0f};
    constexpr std::string_view label = "Cancel";

    // Hover + press inside.
    ctx.input.mouse = Vec2{110.0f, 58.0f};
    ctx.input.mouse_down_prev = false;
    ctx.input.mouse_down = true;
    (void)imm::button_logic(ctx, pos, size, label);
    REQUIRE(ctx.active_id != 0);

    // Drag out, release.
    ctx.input.mouse = Vec2{0.0f, 0.0f};
    ctx.input.mouse_down_prev = true;
    ctx.input.mouse_down = false;
    REQUIRE_FALSE(imm::button_logic(ctx, pos, size, label));
    REQUIRE(ctx.active_id == 0);
}

TEST_CASE("ui_imm: widget_id is stable + collision-resistant for adjacent buttons",
          "[ui_imm][widget_id]") {
    const Vec2 size{32.0f, 12.0f};

    const auto a = imm::widget_id(Vec2{0.0f, 0.0f}, size, "A", 1);
    const auto b = imm::widget_id(Vec2{0.0f, 0.0f}, size, "B", 1);
    const auto c = imm::widget_id(Vec2{0.0f, 1.0f}, size, "A", 1);
    const auto d = imm::widget_id(Vec2{0.0f, 0.0f}, size, "A", 1);

    REQUIRE(a == d);     // Same position + label → same id.
    REQUIRE(a != b);     // Label differs.
    REQUIRE(a != c);     // Position differs.
    REQUIRE(a != 0ULL);  // Sentinel reserved for "no widget".
}
