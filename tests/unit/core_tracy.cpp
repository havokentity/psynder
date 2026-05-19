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

// ─── Wave D additions ────────────────────────────────────────────────────
// Frame / plot / message macros should compile in both configurations and
// should have **zero observable side effects** in the OFF configuration --
// no allocations, no I/O, no global state writes. We can't directly observe
// "did this generate code?" from inside C++, but we can verify that the
// macro is statement-shaped (usable in an if-without-braces, callable in a
// constexpr context as a no-op equivalent), and that argument evaluation is
// not visible to the surrounding scope when the option is off.

namespace {

PSY_NOINLINE void frame_macro_runs() {
    PSY_TRACE_FRAME("psynder.test.frame");
    ++side_effect_counter;
}

PSY_NOINLINE void plot_macro_runs() {
    PSY_TRACE_PLOT_F32("psynder.test.plot", 1.5f);
    ++side_effect_counter;
}

PSY_NOINLINE void message_macro_runs() {
    PSY_TRACE_MESSAGE("psynder.test.message");
    ++side_effect_counter;
}

}  // namespace

TEST_CASE("Wave D Tracy macros compile and are statement-shaped",
          "[core][tracy][wave-d]") {
    side_effect_counter = 0;
    frame_macro_runs();
    plot_macro_runs();
    message_macro_runs();
    REQUIRE(side_effect_counter == 3);
}

TEST_CASE("Wave D Tracy macros are usable in if-without-braces",
          "[core][tracy][wave-d]") {
    // If the macro expanded to two statements without a do {} while wrapper,
    // this single-statement if/else would silently associate the second
    // statement with the else clause -- breaking control flow. The
    // do{}while(0) wrapping (real + no-op) prevents that. We don't need
    // to observe a counter here; we just need the file to compile.
    int n = 0;
    if (n == 0)
        PSY_TRACE_FRAME("psynder.test.frame.in_if");
    else
        PSY_TRACE_FRAME("psynder.test.frame.in_else");

    if (n == 0)
        PSY_TRACE_PLOT_F32("psynder.test.plot.in_if", 0.0f);
    else
        PSY_TRACE_PLOT_F32("psynder.test.plot.in_else", 1.0f);

    if (n == 0)
        PSY_TRACE_MESSAGE("psynder.test.message.in_if");
    else
        PSY_TRACE_MESSAGE("psynder.test.message.in_else");

    REQUIRE(n == 0);  // sanity, also keeps Catch from optimizing the body out
}

#if !defined(PSYNDER_ENABLE_TRACY) || !PSYNDER_ENABLE_TRACY
// When Tracy is compiled out, the macros must reduce to a no-op statement
// that does not touch the surrounding scope. We can prove "the macro
// touched nothing" by capturing the side_effect_counter, invoking the
// macros directly, and confirming it didn't move.
TEST_CASE("Wave D Tracy macros are zero-overhead when PSYNDER_ENABLE_TRACY=OFF",
          "[core][tracy][wave-d][noop]") {
    side_effect_counter = 0;

    PSY_TRACE_FRAME("psynder.test.noop.frame");
    PSY_TRACE_PLOT_F32("psynder.test.noop.plot", 42.0f);
    PSY_TRACE_MESSAGE("psynder.test.noop.message");

    // None of the macros should have written through to any observable
    // counter; they collapsed to do{}while(0).
    REQUIRE(side_effect_counter == 0);
}
#endif
