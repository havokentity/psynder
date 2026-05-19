// SPDX-License-Identifier: MIT
// Psynder — Lane 21 / platform-win32 force-feedback host-only tests.
//
// On Windows we exercise the DIEFFECT descriptor builders that back the
// constant-force and spring-centering effects — these are pure functions so
// they're testable without an actual force-feedback device attached.
//
// On Mac / Linux the unit binary doesn't link `psynder_platform_win32`, so
// the test TU compiles down to a single sentinel symbol that proves this
// file *is* still in the unit-test binary and Catch2 sees it. This matches
// the pattern in `platform_win32_basic.cpp`.

#include <catch2/catch_test_macros.hpp>

#if defined(PSYNDER_PLATFORM_WIN32)

#include "platform/win32/Win32Ffb.h"

using namespace psynder::platform::win32;

TEST_CASE("build_constant_force_effect populates DIEFFECT for a single-axis polar wheel",
          "[platform-win32][ffb]")
{
    DWORD            axes[1]{};
    LONG             dir [1]{};
    DICONSTANTFORCE  cf  {};

    FfbEffectDesc desc{};
    desc.constant_magnitude = 7500;        // strong right-tug
    desc.direction          = 9000;        // 90° polar = east in DI terms
    desc.spring_coefficient = 0;           // unused by this builder

    const DIEFFECT eff = build_constant_force_effect(desc, axes, dir, &cf);

    REQUIRE(eff.dwSize                 == sizeof(DIEFFECT));
    REQUIRE((eff.dwFlags & DIEFF_POLAR) != 0);
    REQUIRE((eff.dwFlags & DIEFF_OBJECTOFFSETS) != 0);
    REQUIRE(eff.cAxes                  == 1u);
    REQUIRE(eff.dwDuration             == INFINITE);
    REQUIRE(eff.dwGain                 == DI_FFNOMINALMAX);
    REQUIRE(eff.rgdwAxes               == axes);
    REQUIRE(eff.rglDirection           == dir);
    REQUIRE(eff.cbTypeSpecificParams   == sizeof(DICONSTANTFORCE));
    REQUIRE(eff.lpvTypeSpecificParams  == &cf);

    REQUIRE(axes[0] == DIJOFS_X);
    REQUIRE(dir[0]  == 9000);
    REQUIRE(cf.lMagnitude == 7500);
}

TEST_CASE("build_constant_force_effect clamps magnitude to the DirectInput full-scale",
          "[platform-win32][ffb]")
{
    DWORD            axes[1]{};
    LONG             dir [1]{};
    DICONSTANTFORCE  cf  {};

    FfbEffectDesc over_pos{};
    over_pos.constant_magnitude = 50000;        // beyond DI_FFNOMINALMAX
    (void)build_constant_force_effect(over_pos, axes, dir, &cf);
    REQUIRE(cf.lMagnitude == 10000);

    FfbEffectDesc over_neg{};
    over_neg.constant_magnitude = -50000;
    (void)build_constant_force_effect(over_neg, axes, dir, &cf);
    REQUIRE(cf.lMagnitude == -10000);
}

TEST_CASE("build_spring_effect populates a symmetric DICONDITION with the desired stiffness",
          "[platform-win32][ffb]")
{
    DWORD        axes[1]{};
    LONG         dir [1]{};
    DICONDITION  cond {};

    FfbEffectDesc desc{};
    desc.spring_coefficient = 6500;

    const DIEFFECT eff = build_spring_effect(desc, axes, dir, &cond);

    REQUIRE(eff.dwSize                == sizeof(DIEFFECT));
    REQUIRE(eff.cAxes                 == 1u);
    REQUIRE(eff.cbTypeSpecificParams  == sizeof(DICONDITION));
    REQUIRE(eff.lpvTypeSpecificParams == &cond);

    REQUIRE(axes[0] == DIJOFS_X);
    REQUIRE(dir[0]  == 0);              // centring is symmetric

    REQUIRE(cond.lOffset              == 0);
    REQUIRE(cond.lPositiveCoefficient == 6500);
    REQUIRE(cond.lNegativeCoefficient == 6500);  // matches positive — true spring
    REQUIRE(cond.dwPositiveSaturation == DI_FFNOMINALMAX);
    REQUIRE(cond.dwNegativeSaturation == DI_FFNOMINALMAX);
    REQUIRE(cond.lDeadBand            == 0);
}

#else  // !PSYNDER_PLATFORM_WIN32

// On non-Windows hosts the unit binary doesn't link the lane lib, so we
// can't call the DirectInput helpers. We still need *a* symbol in this TU
// so the .o ends up in the static-lib closure and the test binary links;
// emit a single hidden Catch2 case.
//
// Lane-25-owned tests/unit/CMakeLists.txt globs *.cpp so just having this
// file with a Catch2 macro is enough — the SUCCEED proves the sentinel.
TEST_CASE("platform-win32 FFB tests are Windows-only", "[.platform-win32-ffb]") {
    SUCCEED("platform-win32 FFB tests skipped on non-Windows host");
}

#endif  // PSYNDER_PLATFORM_WIN32
