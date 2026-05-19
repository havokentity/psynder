// SPDX-License-Identifier: MIT
// Unit tests for the Tracy wrapper macros (Wave B).
//
// We can't talk to a running Tracy server from CI, so the meaningful
// thing to verify is that the macros:
//   - compile in both `PSYNDER_ENABLE_TRACY` on/off configurations
//   - have no observable side-effects when the option is off
//   - accept both name-only and name+colour variants
//
// In the `mac-release` preset the option is off, so this whole file
// expands to do{}while(0) bodies; the tests just confirm callability.

#include "core/Tracy.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

namespace {

int side_effect_counter = 0;

PSY_NOINLINE void traced_zone_runs() {
    PSY_TRACE_ZONE("psynder.test.zone");
    ++side_effect_counter;
}

PSY_NOINLINE void traced_zone_color_runs() {
    PSY_TRACE_ZONE_COLOR("psynder.test.zone.color", 0xFF0000u);
    ++side_effect_counter;
}

PSY_NOINLINE void existing_zone_runs() {
    PSY_ZONE("psynder.test.legacy_zone");
    ++side_effect_counter;
}

}  // namespace

TEST_CASE("Tracy macros compile and do not break control flow",
          "[core][tracy]") {
    side_effect_counter = 0;
    traced_zone_runs();
    traced_zone_color_runs();
    existing_zone_runs();
    REQUIRE(side_effect_counter == 3);
}

TEST_CASE("Tracy macros are usable inside loops and branches",
          "[core][tracy]") {
    int sum = 0;
    for (int i = 0; i < 4; ++i) {
        PSY_TRACE_ZONE("psynder.test.loop");
        if (i & 1) {
            PSY_TRACE_ZONE_COLOR("psynder.test.loop.odd", 0x00FF00u);
            sum += i;
        } else {
            PSY_TRACE_ZONE_COLOR("psynder.test.loop.even", 0x0000FFu);
            sum += i * 2;
        }
    }
    REQUIRE(sum == /*even: 0+4*/ 4 + /*odd: 1+3*/ 4);
}
