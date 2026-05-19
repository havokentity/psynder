// SPDX-License-Identifier: MIT
// Lane 24 tests — lm_bake direct-lighting baker + .lmlight format.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_bake/Bake.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::tools::bake;

TEST_CASE("lm_bake half-float round-trip is exact for representable values", "[tools][lm_bake]") {
    f32 inputs[] = { 0.0f, 1.0f, -1.0f, 65504.0f, 0.5f, -0.25f, 16.0f };
    for (f32 v : inputs) {
        u16 h = f32_to_f16(v);
        f32 back = f16_to_f32(h);
        f32 diff = std::fabs(back - v);
        f32 tol  = std::max(1e-3f, std::fabs(v) * 1e-3f);
        REQUIRE(diff <= tol);
    }
}

TEST_CASE("lm_bake produces direct illumination on a lit triangle", "[tools][lm_bake]") {
    BakeScene scene;
    // Single floor triangle on z=0, normal up.
    BakeTriangle floor{};
    floor.v0 = { 0, 0, 0 };
    floor.v1 = { 1, 0, 0 };
    floor.v2 = { 0, 1, 0 };
    floor.normal = { 0, 0, 1 };
    scene.triangles.push_back(floor);

    // Directional light shining straight down on the floor.
    BakeLight l;
    l.kind = LightKind::kDirectional;
    l.direction = { 0, 0, -1 };
    l.color = { 1, 1, 1 };
    l.intensity = 1.0f;
    scene.lights.push_back(l);

    BakeOptions opt;
    opt.lightmap_resolution = 4;
    BakedAtlas atlas = bake(scene, opt);
    REQUIRE(atlas.surfaces.size() == 1);
    const auto& surf = atlas.surfaces[0];
    REQUIRE(surf.width == 4);
    REQUIRE(surf.height == 4);

    // Texel (0,0) sits inside the triangle (u+v < 1). It should be lit.
    f32 r = surf.pixels[0];
    REQUIRE(r > 0.5f);
}

TEST_CASE("lm_bake shadow rays reject occluders", "[tools][lm_bake]") {
    BakeScene scene;
    // Floor triangle on z=0 sitting in the +x +y quadrant.
    BakeTriangle floor{};
    floor.v0 = { 0, 0, 0 };
    floor.v1 = { 4, 0, 0 };
    floor.v2 = { 0, 4, 0 };
    floor.normal = { 0, 0, 1 };
    scene.triangles.push_back(floor);

    // A horizontal blocker above the floor (full 16x16 rectangle as two
    // triangles, so any vertical ray from the floor hits it).
    BakeTriangle t1{};
    t1.v0 = { -8, -8, 2 };
    t1.v1 = {  8, -8, 2 };
    t1.v2 = { -8,  8, 2 };
    t1.normal = { 0, 0, -1 };
    scene.triangles.push_back(t1);

    BakeTriangle t2{};
    t2.v0 = {  8, -8, 2 };
    t2.v1 = {  8,  8, 2 };
    t2.v2 = { -8,  8, 2 };
    t2.normal = { 0, 0, -1 };
    scene.triangles.push_back(t2);

    BakeLight l;
    l.kind = LightKind::kPoint;
    l.position = { 1, 1, 5 };
    l.color = { 1, 1, 1 };
    l.intensity = 100.0f;
    scene.lights.push_back(l);

    BakeOptions opt;
    opt.lightmap_resolution = 4;
    BakedAtlas atlas = bake(scene, opt);
    REQUIRE(atlas.surfaces.size() == 3);
    // The floor (surface 0) should be entirely in shadow.
    f32 lit = 0;
    for (f32 v : atlas.surfaces[0].pixels) lit += v;
    REQUIRE(lit == 0.0f);
}

TEST_CASE("lm_bake .lmlight round-trips", "[tools][lm_bake]") {
    BakeScene scene;
    BakeTriangle tri{};
    tri.v0 = { 0, 0, 0 };
    tri.v1 = { 1, 0, 0 };
    tri.v2 = { 0, 1, 0 };
    tri.normal = { 0, 0, 1 };
    scene.triangles.push_back(tri);

    BakeLight l;
    l.kind = LightKind::kDirectional;
    l.direction = { 0, 0, -1 };
    l.color = { 1.0f, 0.5f, 0.25f };
    l.intensity = 1.0f;
    scene.lights.push_back(l);

    BakeOptions opt;
    opt.lightmap_resolution = 4;
    auto atlas = bake(scene, opt);

    std::vector<u8> bytes;
    write_lmlight(atlas, bytes);
    BakedAtlas back;
    std::string err;
    REQUIRE(read_lmlight(bytes, back, &err));
    REQUIRE(back.surfaces.size() == atlas.surfaces.size());
    REQUIRE(back.surfaces[0].width == atlas.surfaces[0].width);
    REQUIRE(back.surfaces[0].height == atlas.surfaces[0].height);
    // Tolerance: half-float precision.
    for (usize i = 0; i < back.surfaces[0].pixels.size(); ++i) {
        f32 a = atlas.surfaces[0].pixels[i];
        f32 b = back.surfaces[0].pixels[i];
        if (std::isfinite(a) && std::isfinite(b)) {
            f32 diff = std::fabs(a - b);
            REQUIRE(diff < 1e-2f);
        }
    }
}

TEST_CASE("lm_bake CLI --help returns 0", "[tools][lm_bake]") {
    const char* argv[] = {"lm_bake", "--help"};
    int rc = cli_main(2, const_cast<char**>(argv));
    REQUIRE(rc == 0);
}
