// SPDX-License-Identifier: MIT
// Psynder — lane 23 / platform-macos HID FFB descriptor builder.
//
// `ffb_build_constant_force` is a pure function: it clamps + scales the
// engine-side normalized floats into the DirectInput / ForceFeedback.framework
// integer convention (magnitude / direction in ±10000 units, planar Cartesian
// direction, FF_INFINITE for "play forever"). The test runs on any host
// because it never touches actual hardware.

#if defined(PSYNDER_PLATFORM_MACOS)

#include "platform/macos/MacPlatform_internal.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cmath>

namespace pm = psynder::platform::macos;

TEST_CASE("platform_macos: ffb_build_constant_force clamps + scales magnitude",
          "[platform-macos][ffb]") {
    SECTION("zero magnitude -> zero wire magnitude") {
        pm::FfbConstantForce d{};
        d.magnitude = 0.0f;
        const auto w = pm::ffb_build_constant_force(d);
        REQUIRE(w.magnitude == 0);
    }
    SECTION("unit magnitude -> +10000") {
        pm::FfbConstantForce d{};
        d.magnitude = 1.0f;
        d.gain = 1.0f;
        const auto w = pm::ffb_build_constant_force(d);
        REQUIRE(w.magnitude == 10000);
        REQUIRE(w.gain == 10000u);
    }
    SECTION("negative-unit magnitude -> -10000") {
        pm::FfbConstantForce d{};
        d.magnitude = -1.0f;
        const auto w = pm::ffb_build_constant_force(d);
        REQUIRE(w.magnitude == -10000);
    }
    SECTION("overrange magnitude clamps to +/-10000, never wraps") {
        pm::FfbConstantForce d{};
        d.magnitude = 4.2f;
        REQUIRE(pm::ffb_build_constant_force(d).magnitude == 10000);
        d.magnitude = -7.5f;
        REQUIRE(pm::ffb_build_constant_force(d).magnitude == -10000);
    }
    SECTION("0deg direction -> +X axis (10000, 0)") {
        pm::FfbConstantForce d{};
        d.magnitude = 0.5f;
        d.direction_deg = 0.0f;
        const auto w = pm::ffb_build_constant_force(d);
        REQUIRE(w.direction_x == 10000);
        REQUIRE(std::abs(w.direction_y) <= 1);  // round-to-int slop
    }
    SECTION("90deg direction -> +Y axis (0, 10000)") {
        pm::FfbConstantForce d{};
        d.direction_deg = 90.0f;
        const auto w = pm::ffb_build_constant_force(d);
        REQUIRE(std::abs(w.direction_x) <= 1);
        REQUIRE(w.direction_y == 10000);
    }
    SECTION("180deg direction -> -X axis (-10000, 0)") {
        pm::FfbConstantForce d{};
        d.direction_deg = 180.0f;
        const auto w = pm::ffb_build_constant_force(d);
        REQUIRE(w.direction_x == -10000);
        REQUIRE(std::abs(w.direction_y) <= 1);
    }
    SECTION("duration 0 -> FF_INFINITE sentinel, nonzero passes through") {
        pm::FfbConstantForce d{};
        d.duration_us = 0u;
        REQUIRE(pm::ffb_build_constant_force(d).duration == 0xFFFFFFFFu);
        d.duration_us = 250000u;
        REQUIRE(pm::ffb_build_constant_force(d).duration == 250000u);
    }
    SECTION("gain clamps to [0, 10000]") {
        pm::FfbConstantForce d{};
        d.gain = -0.1f;
        REQUIRE(pm::ffb_build_constant_force(d).gain == 0u);
        d.gain = 0.5f;
        REQUIRE(pm::ffb_build_constant_force(d).gain == 5000u);
        d.gain = 2.0f;
        REQUIRE(pm::ffb_build_constant_force(d).gain == 10000u);
    }
}

#endif  // PSYNDER_PLATFORM_MACOS
