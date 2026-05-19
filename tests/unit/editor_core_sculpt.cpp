// SPDX-License-Identifier: MIT
// Psynder — Lane 18 unit test: heightmap sculpt brushes shape the
// editor-side f32 heightfield. Header-only inclusion of editor::sculpt::*
// keeps the test linker-clean.

#include "editor/core/Sculpt.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::editor;

namespace {
sculpt::Heightfield make_hf(u32 nx, u32 nz, f32 spc = 1.0f) {
    sculpt::Heightfield hf;
    hf.allocate(nx, nz, spc);
    hf.origin = math::Vec3{0.0f, 0.0f, 0.0f};
    return hf;
}
}  // namespace

TEST_CASE("sculpt: raise pushes the centre vertex up", "[editor][sculpt]") {
    auto hf = make_hf(33, 33, 1.0f);
    REQUIRE(hf.sample(16, 16) == 0.0f);
    sculpt::raise(hf, math::Vec3{16.0f, 0.0f, 16.0f}, 5.0f, 1.0f);
    REQUIRE(hf.sample(16, 16) > 0.5f);   // centre + smoothstep at d=0 = strength
}

TEST_CASE("sculpt: lower inverts raise on a fresh heightfield", "[editor][sculpt]") {
    auto hf = make_hf(33, 33);
    sculpt::raise(hf, math::Vec3{16.0f, 0.0f, 16.0f}, 5.0f, 1.0f);
    const f32 h_after_raise = hf.sample(16, 16);
    sculpt::lower(hf, math::Vec3{16.0f, 0.0f, 16.0f}, 5.0f, 1.0f);
    REQUIRE(hf.sample(16, 16) < h_after_raise);
}

TEST_CASE("sculpt: flatten pulls neighbourhood toward centre height", "[editor][sculpt]") {
    auto hf = make_hf(33, 33);
    // Hand-paint a step: centre = 10 m, neighbours = 0 m.
    hf.store(16, 16, 10.0f);
    sculpt::flatten(hf, math::Vec3{16.0f, 0.0f, 16.0f}, 4.0f, 1.0f);
    const f32 c = hf.sample(16, 16);
    const f32 n = hf.sample(15, 16);
    REQUIRE(c == 10.0f);     // centre is the target, untouched
    REQUIRE(n > 0.0f);       // neighbour pulled up toward 10
    REQUIRE(n < 10.0f);
}

TEST_CASE("sculpt: paint normalises four channel weights", "[editor][sculpt]") {
    auto hf = make_hf(33, 33);
    sculpt::SplatGrid g;
    g.allocate(hf.size_x, hf.size_z);

    // Default = layer 0 only.
    REQUIRE(g.at(16,16)[0] == 1.0f);
    REQUIRE(g.at(16,16)[1] == 0.0f);

    sculpt::paint(g, math::Vec3{16.0f, 0.0f, 16.0f}, 4.0f, hf, /*material=*/1, /*weight=*/1.0f);

    // Centre should now blend layer 0 and layer 1, normalised.
    const auto& c = g.at(16, 16);
    const f32 sum = c[0] + c[1] + c[2] + c[3];
    REQUIRE(sum > 0.99f);
    REQUIRE(sum < 1.01f);
    REQUIRE(c[1] > 0.0f);
}
