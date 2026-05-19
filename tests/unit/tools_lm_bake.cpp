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

// ─── Wave-B: multi-bounce indirect ───────────────────────────────────────

namespace {
// Sum all RGB pixel values across every surface — a proxy for total
// integrated radiance after the bake.
f32 total_atlas_energy(const BakedAtlas& atlas) {
    f32 sum = 0;
    for (const auto& s : atlas.surfaces) {
        for (f32 v : s.pixels) sum += v;
    }
    return sum;
}
}  // anon

TEST_CASE("lm_bake multi-bounce produces brighter scene than direct only", "[tools][lm_bake][wave-b]") {
    // A horizontal floor with a parallel ceiling directly above, both
    // perfect Lambertian reflectors. A small bright point light sits
    // between them. Direct lighting hits both surfaces; under multi-
    // bounce, light bouncing between the parallel plates causes a strict
    // increase in total integrated atlas radiance.
    //
    // Geometry note: the test triangle's vertices are wound so the cross
    // product e1 × e2 (used by both build_basis and indirect closest-hit
    // direction tests) points along the outward normal — floor up, ceiling
    // down. lm_bake doesn't backface-cull on visibility rays, but the
    // hemisphere gather samples are oriented by the *basis normal*, so the
    // winding alignment ensures the floor's hemisphere actually faces the
    // ceiling and vice-versa.
    BakeScene scene;
    BakeTriangle floor{};
    floor.v0 = { -1, -1, 0 };
    floor.v1 = {  1, -1, 0 };
    floor.v2 = { -1,  1, 0 };
    floor.normal = { 0, 0, 1 };
    floor.albedo = { 1, 1, 1 };
    scene.triangles.push_back(floor);

    // Second floor triangle so the floor is a full 2x2 square (matters for
    // hemisphere rays from the ceiling hitting back the floor).
    BakeTriangle floor2{};
    floor2.v0 = {  1, -1, 0 };
    floor2.v1 = {  1,  1, 0 };
    floor2.v2 = { -1,  1, 0 };
    floor2.normal = { 0, 0, 1 };
    floor2.albedo = { 1, 1, 1 };
    scene.triangles.push_back(floor2);

    // Ceiling: wound CCW when viewed from +z so e1×e2 = -z.
    BakeTriangle ceil1{};
    ceil1.v0 = { -1, -1, 2 };
    ceil1.v1 = { -1,  1, 2 };
    ceil1.v2 = {  1, -1, 2 };
    ceil1.normal = { 0, 0, -1 };
    ceil1.albedo = { 1, 1, 1 };
    scene.triangles.push_back(ceil1);
    BakeTriangle ceil2{};
    ceil2.v0 = {  1, -1, 2 };
    ceil2.v1 = { -1,  1, 2 };
    ceil2.v2 = {  1,  1, 2 };
    ceil2.normal = { 0, 0, -1 };
    ceil2.albedo = { 1, 1, 1 };
    scene.triangles.push_back(ceil2);

    BakeLight l;
    l.kind = LightKind::kPoint;
    l.position = { 0, 0, 1 };
    l.color = { 1, 1, 1 };
    l.intensity = 10.0f;
    scene.lights.push_back(l);

    BakeOptions opt;
    opt.lightmap_resolution = 4;
    opt.indirect_samples_per_bounce = 32;

    opt.max_indirect_bounces = 0;
    auto direct = bake(scene, opt);
    f32 e_direct = total_atlas_energy(direct);

    opt.max_indirect_bounces = 2;
    auto indirect = bake(scene, opt);
    f32 e_indirect = total_atlas_energy(indirect);

    REQUIRE(std::isfinite(e_direct));
    REQUIRE(std::isfinite(e_indirect));
    REQUIRE(e_direct > 0.0f);
    REQUIRE(e_indirect > e_direct);
}

TEST_CASE("lm_bake direct-only matches Wave-A baseline (bounces=0)", "[tools][lm_bake][wave-b]") {
    // Sanity: bounce_count = 0 must yield exactly the Wave-A direct result
    // (no extra surface modifications happen in the bake() loop).
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
    l.color = { 1, 1, 1 };
    l.intensity = 1.0f;
    scene.lights.push_back(l);

    BakeOptions opt;
    opt.lightmap_resolution = 4;
    opt.max_indirect_bounces = 0;
    auto atlas = bake(scene, opt);
    auto reference = bake_triangle_direct(scene, 0, opt);
    REQUIRE(atlas.surfaces.size() == 1);
    REQUIRE(atlas.surfaces[0].pixels == reference.pixels);
}
