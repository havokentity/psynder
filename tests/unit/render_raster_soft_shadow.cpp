// SPDX-License-Identifier: MIT
// Psynder -- M-HYB fidelity unit test (DESIGN.md 8): soft-penumbra shadow
// sampling + the heightmap-march MAX-combine occlusion. Three guards:
//
//   1. Soft vs hard: with softness > 0 AND samples > 1 the fragment stage
//      averages N jittered shadow rays toward a small disc around the light,
//      so a PARTIALLY occluded receiver lands strictly between fully-lit (1.0)
//      and fully-shadowed (1 - opacity). The hard path (samples == 1) is binary.
//   2. Default determinism: samples == 1 (or softness == 0) reproduces the
//      single-ray hard result BIT-for-bit -- this is the goldens-unchanged
//      invariant (the soft path must never perturb the default).
//   3. Heightmap-march occlusion: rising terrain between a point and the light
//      occludes; clear terrain does not -- and the ShadowOccluder trampoline
//      MAX-combines mesh-BVH and heightmap occlusion (occluded if EITHER hits).
//
// These exercise the helpers directly (trace_light_visibility_hard/_soft and
// trace_heightmap_shadow) rather than rendering a frame, so the penumbra
// gradient is asserted as a scalar in [1-opacity, 1] without pixel quantization.

#include "render/raster/RasterLighting.h"
#include "render/rt/Bvh.h"
#include "render/rt/HeightmapShadow.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

// ShadowOccluder trampoline mirroring engine/render/hybrid/ShadowScene.cpp's
// combined_occluded: a CombinedOccluder borrowing a TLAS and (optionally) a
// heightmap, MAX-combined as a logical OR of the two "blocked" results.
struct CombinedOccluder {
    const rt::Tlas* tlas = nullptr;
    rt::Heightmap heightmap{};
    bool has_heightmap = false;
};

bool combined_occluded(const void* occluder,
                       math::Vec3 origin,
                       math::Vec3 dir,
                       f32 t_min,
                       f32 t_max) noexcept {
    const auto* c = static_cast<const CombinedOccluder*>(occluder);
    if (!c)
        return false;
    rt::Ray ray{};
    ray.origin = origin;
    ray.direction = dir;
    ray.t_min = t_min;
    ray.t_max = t_max;
    if (c->tlas && c->tlas->occluded(ray))
        return true;
    if (c->has_heightmap)
        return rt::trace_heightmap_shadow(c->heightmap, ray);
    return false;
}

// Build a finite horizontal blade (two tris) at world height `y`, spanning
// [-half, +half] in X and Z. Used as a partial occluder: a receiver point at
// the blade's edge is in penumbra (some soft rays clear the edge, some hit).
rt::Bvh8 g_blas;                 // address-stable for the test's lifetime
std::vector<rt::Triangle> g_tris;

void build_blade(f32 y, f32 half) {
    g_tris = {
        {math::Vec3{-half, y, -half}, math::Vec3{half, y, -half}, math::Vec3{half, y, half}},
        {math::Vec3{-half, y, -half}, math::Vec3{half, y, half}, math::Vec3{-half, y, half}},
    };
    g_blas.build(g_tris.data(), static_cast<u32>(g_tris.size()));
}

RasterLight overhead_point() {
    RasterLight key{};
    key.kind = RasterLightKind::Point;
    key.position_world = math::Vec3{0.0f, 6.0f, 0.0f};
    key.color_linear = math::Vec3{1.0f, 1.0f, 1.0f};
    key.intensity = 1.0f;
    key.range = 64.0f;
    return key;
}

}  // namespace

// --- 1 + 2: soft penumbra gradient vs binary hard + default determinism ------

TEST_CASE("soft shadows: a partially occluded receiver is a penumbra gradient",
          "[render][raster][hybrid][shadows]") {
    // A blade hovering at y = 3 spanning x,z in [-2, 2]. The light is straight
    // overhead. A receiver near the blade's X edge (x = 2.0) has the hard ray
    // graze the boundary; the soft disc spreads rays both onto and off the
    // blade -> partial visibility.
    build_blade(/*y=*/3.0f, /*half=*/2.0f);
    rt::Tlas tlas;
    rt::Tlas::InstanceDesc inst{};
    inst.blas = &g_blas;
    inst.transform = math::identity4();
    tlas.build(&inst, 1);

    CombinedOccluder combo{};
    combo.tlas = &tlas;

    const RasterLight key = overhead_point();
    const math::Vec3 up{0.0f, 1.0f, 0.0f};
    const math::Vec3 light_dir{0.0f, 1.0f, 0.0f};  // toward the overhead light

    // Receiver right at the blade's X edge so the penumbra straddles it.
    const math::Vec3 edge{2.0f, 0.0f, 0.0f};

    // Fully-lit reference: a receiver well outside the blade footprint.
    const math::Vec3 outside{6.0f, 0.0f, 0.0f};
    // Fully-shadowed reference: a receiver under the blade centre.
    const math::Vec3 center{0.0f, 0.0f, 0.0f};

    ShadowOccluder hard{};
    hard.occluder = &combo;
    hard.occluded = &combined_occluded;
    hard.opacity = 0.8f;
    hard.softness = 0.0f;
    hard.samples = 1u;

    ShadowOccluder soft = hard;
    soft.softness = 1.0f;   // widest disc
    soft.samples = 16u;     // quality knob > 1 engages the soft path

    const f32 occluded_value = saturate(1.0f - hard.opacity);  // 1 - 0.8 = 0.2

    // Hard path is binary at the references.
    const f32 hard_outside = trace_light_visibility_hard(hard, key, outside, up, light_dir);
    const f32 hard_center = trace_light_visibility_hard(hard, key, center, up, light_dir);
    REQUIRE(hard_outside == 1.0f);
    REQUIRE(hard_center == occluded_value);

    // Soft path at the references collapses to the same fully-lit / fully-dark
    // values (every jittered ray agrees away from an edge). The averaged sum is
    // float-accumulated, so compare within a tiny epsilon rather than bitwise
    // (bit-exactness is the DEFAULT/hard path's job, asserted in the next case).
    const f32 soft_outside = trace_light_visibility_soft(soft, key, outside, up, light_dir);
    const f32 soft_center = trace_light_visibility_soft(soft, key, center, up, light_dir);
    REQUIRE(std::fabs(soft_outside - 1.0f) < 1e-5f);
    REQUIRE(std::fabs(soft_center - occluded_value) < 1e-5f);

    // The load-bearing claim: at the blade edge the SOFT receiver is a true
    // penumbra -- strictly between fully-lit and fully-shadowed -- whereas HARD
    // is one or the other (binary).
    const f32 soft_edge = trace_light_visibility_soft(soft, key, edge, up, light_dir);
    const f32 hard_edge = trace_light_visibility_hard(hard, key, edge, up, light_dir);
    REQUIRE(soft_edge > occluded_value);
    REQUIRE(soft_edge < 1.0f);
    // Hard is binary: it is exactly one of the two endpoints, never between.
    REQUIRE((hard_edge == 1.0f || hard_edge == occluded_value));
}

TEST_CASE("soft shadows: default knobs (samples==1 / softness==0) match hard bit-for-bit",
          "[render][raster][hybrid][shadows]") {
    build_blade(/*y=*/3.0f, /*half=*/2.0f);
    rt::Tlas tlas;
    rt::Tlas::InstanceDesc inst{};
    inst.blas = &g_blas;
    inst.transform = math::identity4();
    tlas.build(&inst, 1);

    CombinedOccluder combo{};
    combo.tlas = &tlas;

    const RasterLight key = overhead_point();
    const math::Vec3 up{0.0f, 1.0f, 0.0f};
    const math::Vec3 light_dir{0.0f, 1.0f, 0.0f};

    // Sweep a line of receiver positions crossing the blade edge.
    const math::Vec3 samples_pos[] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.9f, 0.0f, 0.0f},
        {2.0f, 0.0f, 0.0f}, {2.1f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f},
    };

    // The public dispatcher with default knobs (samples 1, softness 0) must
    // call the hard path and produce the EXACT same float as the hard helper.
    ShadowOccluder def{};
    def.occluder = &combo;
    def.occluded = &combined_occluded;
    def.opacity = 0.7f;
    def.softness = 0.0f;  // default-ish; soft path is OFF
    def.samples = 1u;

    for (const math::Vec3& p : samples_pos) {
        const f32 via_dispatch = trace_light_visibility(def, key, p, up, light_dir);
        const f32 via_hard = trace_light_visibility_hard(def, key, p, up, light_dir);
        // Bit-for-bit identical: the default never enters the averaging loop.
        REQUIRE(via_dispatch == via_hard);
    }

    // softness > 0 but samples == 1 also stays on the hard path (binary).
    ShadowOccluder soft_one = def;
    soft_one.softness = 1.0f;
    soft_one.samples = 1u;
    for (const math::Vec3& p : samples_pos) {
        const f32 via_dispatch = trace_light_visibility(soft_one, key, p, up, light_dir);
        const f32 via_hard = trace_light_visibility_hard(soft_one, key, p, up, light_dir);
        REQUIRE(via_dispatch == via_hard);
    }
}

TEST_CASE("soft shadows: jitter is deterministic across calls",
          "[render][raster][hybrid][shadows]") {
    // The low-discrepancy disc pattern is a pure function of the sample index
    // (no RNG / no time): two identical queries return identical floats, and
    // the per-sample offsets are stable run to run (golden-safe).
    f32 u0a, v0a, u0b, v0b, u7a, v7a, u7b, v7b;
    soft_disc_sample(0u, u0a, v0a);
    soft_disc_sample(0u, u0b, v0b);
    soft_disc_sample(7u, u7a, v7a);
    soft_disc_sample(7u, u7b, v7b);
    REQUIRE(u0a == u0b);
    REQUIRE(v0a == v0b);
    REQUIRE(u7a == u7b);
    REQUIRE(v7a == v7b);
    // Samples land inside the unit disc.
    REQUIRE(u0a * u0a + v0a * v0a <= 1.0f + 1e-5f);
    REQUIRE(u7a * u7a + v7a * v7a <= 1.0f + 1e-5f);
    // Distinct sample indices give distinct points (no degenerate collapse).
    REQUIRE((u0a != u7a || v0a != v7a));
}

// --- 3: heightmap-march occlusion through the MAX-combine trampoline ---------

namespace {

struct RampHm {
    std::vector<f32> data;
    rt::Heightmap hm;
    // y(x) = base + slope_x * x_world ; flat in z.
    RampHm(u32 w, u32 h, f32 base, f32 slope_x, f32 cell = 1.0f) {
        data.assign(static_cast<size_t>(w) * h, 0.0f);
        f32 ymin = 1e30f, ymax = -1e30f;
        for (u32 j = 0; j < h; ++j) {
            for (u32 i = 0; i < w; ++i) {
                const f32 x = cell * static_cast<f32>(i);
                const f32 y = base + slope_x * x;
                data[j * w + i] = y;
                ymin = std::fmin(ymin, y);
                ymax = std::fmax(ymax, y);
            }
        }
        hm.y_data = data.data();
        hm.width = w;
        hm.height = h;
        hm.origin_xz = {0.0f, 0.0f};
        hm.cell_size = cell;
        hm.y_min = ymin;
        hm.y_max = ymax;
    }
};

}  // namespace

TEST_CASE("heightmap occlusion: rising terrain between point and light occludes",
          "[render][raster][hybrid][shadows]") {
    // Ramp rising along +X (0..15). The combined occluder has NO mesh TLAS, so
    // any occlusion comes purely from the heightmap-march path.
    RampHm ramp(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f);
    REQUIRE(ramp.hm.valid());

    CombinedOccluder combo{};
    combo.tlas = nullptr;
    combo.heightmap = ramp.hm;
    combo.has_heightmap = true;

    // A near-horizontal ray at y = 5 starting at low x: the ramp rises through
    // y = 5 at x = 5, so the ray is occluded by terrain ahead.
    REQUIRE(combined_occluded(&combo, math::Vec3{0.5f, 5.0f, 8.0f},
                              math::Vec3{1.0f, 0.0f, 0.0f}, 1e-4f, 1e30f));

    // Clear: a ray well above the ramp's peak (y = 20 > max 15) never hits.
    REQUIRE_FALSE(combined_occluded(&combo, math::Vec3{1.0f, 20.0f, 8.0f},
                                    math::Vec3{1.0f, 0.0f, 0.0f}, 1e-4f, 1e30f));
}

TEST_CASE("heightmap occlusion: MAX-combine ORs mesh and terrain results",
          "[render][raster][hybrid][shadows]") {
    // Flat-ish ramp that does NOT occlude an overhead ray, plus a mesh blade
    // that DOES: the combine must report blocked (mesh wins), proving the OR.
    RampHm ramp(16, 16, /*base=*/0.0f, /*slope_x=*/0.0f);  // flat at y = 0
    build_blade(/*y=*/3.0f, /*half=*/8.0f);                // wide blade above
    rt::Tlas tlas;
    rt::Tlas::InstanceDesc inst{};
    inst.blas = &g_blas;
    inst.transform = math::identity4();
    tlas.build(&inst, 1);

    CombinedOccluder combo{};
    combo.tlas = &tlas;
    combo.heightmap = ramp.hm;
    combo.has_heightmap = true;

    const math::Vec3 origin{0.0f, 0.5f, 0.0f};  // above the flat terrain
    const math::Vec3 up_dir{0.0f, 1.0f, 0.0f};  // toward an overhead light

    // Terrain alone (flat at y=0, origin above it, ray going up) does NOT hit.
    CombinedOccluder terrain_only = combo;
    terrain_only.tlas = nullptr;
    REQUIRE_FALSE(combined_occluded(&terrain_only, origin, up_dir, 1e-4f, 2.5f));

    // With the mesh blade at y = 3 added, the combine reports occluded (OR).
    REQUIRE(combined_occluded(&combo, origin, up_dir, 1e-4f, 10.0f));
}
