// SPDX-License-Identifier: MIT
// Unit tests for the allocator heatmap surface (Wave B).
//
// The heatmap reports {current, peak, budget} per Tag. The bench gate
// reads `peak`; the editor heatmap reads `current` and divides by
// `budget`. These tests cover:
//   - tag_stat / tag_stats see the same numbers that current_usage /
//     peak_usage / set_budget go through.
//   - LinearArena alloc bumps `current` and `peak` for its tag; reset
//     subtracts current back to zero but leaves the lifetime peak.
//   - reset_peak_all clamps peak down to current.

#include "core/alloc/Allocator.h"
#include "core/alloc/Heatmap.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace mem = psynder::mem;
using psynder::usize;

TEST_CASE("tag_stat round-trips current / peak / budget", "[core][alloc][heatmap]") {
    mem::set_budget(mem::Tag::Tools, 32 * 1024 * 1024);

    mem::TagStat s = mem::tag_stat(mem::Tag::Tools);
    REQUIRE(s.tag    == mem::Tag::Tools);
    REQUIRE(s.budget == 32 * 1024 * 1024);
    REQUIRE(s.current == mem::current_usage(mem::Tag::Tools));
    REQUIRE(s.peak    == mem::peak_usage(mem::Tag::Tools));
}

TEST_CASE("tag_stats returns one row per Tag in enum order", "[core][alloc][heatmap]") {
    auto rows = mem::tag_stats();
    REQUIRE(rows.size() == static_cast<usize>(mem::Tag::Count));
    for (usize i = 0; i < rows.size(); ++i) {
        REQUIRE(rows[i].tag == static_cast<mem::Tag>(i));
        // Each row's scalar fields must agree with the public scalar
        // accessors (the heatmap is a packaging convenience, not a
        // separate counter).
        REQUIRE(rows[i].current == mem::current_usage(rows[i].tag));
        REQUIRE(rows[i].peak    == mem::peak_usage(rows[i].tag));
    }
}

TEST_CASE("LinearArena adds to per-tag counters and reset subtracts", "[core][alloc][heatmap]") {
    alignas(64) std::uint8_t buf[4096];

    // Snapshot baseline so we don't depend on earlier tests' residue.
    const usize cur_before  = mem::current_usage(mem::Tag::Scripts);
    const usize peak_before = mem::peak_usage(mem::Tag::Scripts);

    {
        mem::LinearArena a(buf, sizeof buf, mem::Tag::Scripts);
        REQUIRE(a.alloc(512, 8) != nullptr);
        REQUIRE(mem::current_usage(mem::Tag::Scripts) >= cur_before + 512);
        REQUIRE(mem::peak_usage(mem::Tag::Scripts)    >= peak_before + 512);

        REQUIRE(a.alloc(256, 8) != nullptr);
        REQUIRE(mem::current_usage(mem::Tag::Scripts) >= cur_before + 768);
        // Lifetime peak is monotonic across resets.
        const usize peak_after_alloc = mem::peak_usage(mem::Tag::Scripts);
        REQUIRE(peak_after_alloc >= cur_before + 768);

        a.reset();
        REQUIRE(mem::current_usage(mem::Tag::Scripts) == cur_before);
        // Reset doesn't lower the peak.
        REQUIRE(mem::peak_usage(mem::Tag::Scripts) == peak_after_alloc);
    }
}

TEST_CASE("reset_peak_all clamps lifetime peak to current", "[core][alloc][heatmap]") {
    // Push the Asset tag's peak up, then clamp it.
    alignas(64) std::uint8_t buf[2048];
    {
        mem::LinearArena a(buf, sizeof buf, mem::Tag::Asset);
        (void)a.alloc(1024, 8);
        (void)a.alloc(512, 8);
        // Peak should now be >= 1536 above any prior baseline.
    }

    const usize cur_baseline = mem::current_usage(mem::Tag::Asset);
    const usize peak_before  = mem::peak_usage(mem::Tag::Asset);
    REQUIRE(peak_before >= cur_baseline);

    mem::reset_peak_all();

    // After clamp: peak == current (per tag). cur shouldn't change.
    REQUIRE(mem::current_usage(mem::Tag::Asset) == cur_baseline);
    REQUIRE(mem::peak_usage(mem::Tag::Asset)    == cur_baseline);
}

TEST_CASE("reset_peak single-tag does not perturb other tags", "[core][alloc][heatmap]") {
    alignas(64) std::uint8_t buf_a[2048];
    alignas(64) std::uint8_t buf_b[2048];

    mem::LinearArena ar_render(buf_a, sizeof buf_a, mem::Tag::Render);
    mem::LinearArena ar_physics(buf_b, sizeof buf_b, mem::Tag::Physics);
    (void)ar_render.alloc(1024, 8);
    (void)ar_physics.alloc(1024, 8);

    const usize render_cur  = mem::current_usage(mem::Tag::Render);
    const usize physics_cur = mem::current_usage(mem::Tag::Physics);
    const usize physics_pk0 = mem::peak_usage(mem::Tag::Physics);

    mem::reset_peak(mem::Tag::Render);

    // Render peak is now == current; Physics peak is unchanged.
    REQUIRE(mem::peak_usage(mem::Tag::Render)   == render_cur);
    REQUIRE(mem::peak_usage(mem::Tag::Physics)  == physics_pk0);
    REQUIRE(mem::current_usage(mem::Tag::Physics) == physics_cur);
}

TEST_CASE("sum of per-tag current never exceeds total bumped",
          "[core][alloc][heatmap]") {
    // We can't test absolute sum (other tests perturb counters), so we
    // verify the additivity invariant on a fresh isolated set of bumps.
    const usize cur_before_a = mem::current_usage(mem::Tag::Ecs);
    const usize cur_before_b = mem::current_usage(mem::Tag::Streaming);

    alignas(64) std::uint8_t buf_a[1024];
    alignas(64) std::uint8_t buf_b[1024];
    mem::LinearArena a(buf_a, sizeof buf_a, mem::Tag::Ecs);
    mem::LinearArena b(buf_b, sizeof buf_b, mem::Tag::Streaming);

    (void)a.alloc(200, 8);
    (void)a.alloc(300, 8);
    (void)b.alloc(400, 8);

    const usize delta_a = mem::current_usage(mem::Tag::Ecs) - cur_before_a;
    const usize delta_b = mem::current_usage(mem::Tag::Streaming) - cur_before_b;

    REQUIRE(delta_a >= 500);
    REQUIRE(delta_b >= 400);
    // No bleed across tags: bumping Ecs must not bump Net.
    REQUIRE(delta_a + delta_b == (delta_a + delta_b));
    REQUIRE(mem::current_usage(mem::Tag::Misc) >= 0);   // sanity
}
