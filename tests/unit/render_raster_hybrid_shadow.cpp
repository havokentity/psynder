// SPDX-License-Identifier: MIT
// Psynder — M-HYB unit test (DESIGN.md §8): the raster fragment stage, in
// Hybrid mode, fires a software-raytraced shadow ray per light per pixel and
// attenuates the occluded light's diffuse contribution. This is the load-
// bearing guard for "raster primary visibility + traced shadows in the SAME
// frame": render the SAME receiver twice — once plain raster (no occluder),
// once Hybrid (occluder bound between the light and the receiver) — and assert
// the Hybrid receiver pixel is meaningfully darker because the shadow ray hit.
//
// The occluder is a standalone render::rt::Tlas built from one BLAS (no scene
// needed); the ShadowOccluder trampoline casts the opaque pointer back to the
// Tlas and answers occlusion via Tlas::occluded — exactly the bridge the host
// uses. If the shadow ray never reached the shader, the two renders would be
// byte-identical and this test fails.

#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/RasterLighting.h"
#include "render/rt/Bvh.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

struct Image {
    std::vector<u32> pixels;
    std::vector<u32> depth;
    Framebuffer fb{};
    explicit Image(u32 w, u32 h)
        : pixels(static_cast<std::size_t>(w) * h, 0xFF000000u)
        , depth(static_cast<std::size_t>(w) * h, 0) {
        fb.width = w;
        fb.height = h;
        fb.pitch = w * 4;
        fb.format = PixelFormat::RGBA8;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth = depth.data();
    }
};

// ShadowOccluder trampoline mirroring engine/render/hybrid/ShadowScene.cpp:
// cast the opaque pointer to the concrete Tlas and answer the occlusion query.
bool tlas_occluded(const void* occluder,
                   math::Vec3 origin,
                   math::Vec3 dir,
                   f32 t_min,
                   f32 t_max) noexcept {
    const auto* tlas = static_cast<const rt::Tlas*>(occluder);
    if (!tlas)
        return false;
    rt::Ray ray{};
    ray.origin = origin;
    ray.direction = dir;
    ray.t_min = t_min;
    ray.t_max = t_max;
    return tlas->occluded(ray);
}

// Render a white, untextured floor quad in the XZ plane (normal +Y) lit by a
// single point light directly above it. When `shadow` is active the fragment
// stage traces a shadow ray toward the light; the caller bound an occluder
// blade between the floor and the light, so the centre pixel ends up shadowed.
u32 render_floor_centre(const RasterLight* lights,
                        u32 light_count,
                        math::Vec3 ambient,
                        const ShadowOccluder* shadow) {
    constexpr u32 W = 96, H = 96;
    Image img(W, H);

    // Floor quad in the XZ plane at y = 0, normal +Y, centred at the origin.
    const Vertex verts[] = {
        Vertex{math::Vec3{-4, 0, -4}, math::Vec3{0, 1, 0}, math::Vec2{0, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{4, 0, -4}, math::Vec3{0, 1, 0}, math::Vec2{1, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{4, 0, 4}, math::Vec3{0, 1, 0}, math::Vec2{1, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{-4, 0, 4}, math::Vec3{0, 1, 0}, math::Vec2{0, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
    };
    const u32 idx[] = {0, 1, 2, 0, 2, 3};

    ViewState v{};
    // Camera looking straight down at the floor centre.
    v.view = math::look_at_rh(math::Vec3{0, 8, 0.01f}, math::Vec3{0, 0, 0}, math::Vec3{0, 0, -1});
    v.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                        static_cast<f32>(W) / static_cast<f32>(H),
                                        0.1f,
                                        100.0f);
    v.target = img.fb;
    v.tile_w = 64;
    v.tile_h = 64;
    v.lights = lights;
    v.light_count = light_count;
    v.ambient_linear = ambient;
    if (shadow)
        v.shadow = *shadow;

    clear_framebuffer(img.fb, 0xFF000000u);

    DrawItem d{};
    d.vertices = verts;
    d.vertex_count = 4;
    d.indices = idx;
    d.index_count = 6;
    d.model = math::identity4();
    d.cull = CullMode::None;

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    return img.pixels[static_cast<std::size_t>(H / 2) * W + (W / 2)];
}

PSY_FORCEINLINE u32 red_of(u32 rgba8) noexcept {
    return rgba8 & 0xFFu;
}

}  // namespace

TEST_CASE("hybrid shadows: a traced occluder darkens the receiver vs plain raster",
          "[render][raster][hybrid][shadows]") {
    // One point light 6 units directly above the floor centre.
    RasterLight key{};
    key.kind = RasterLightKind::Point;
    key.position_world = math::Vec3{0.0f, 6.0f, 0.0f};
    key.color_linear = math::Vec3{1.0f, 1.0f, 1.0f};
    key.intensity = 1.0f;
    key.range = 64.0f;

    const math::Vec3 ambient{0.1f, 0.1f, 0.1f};

    // Lit baseline: no occluder -> the floor centre is brightly lit (the point
    // light is straight overhead, n·l == 1).
    const u32 lit = render_floor_centre(&key, 1, ambient, nullptr);

    // Build an occluder: a small horizontal blade hovering between the floor
    // centre and the light (at y = 3). Two triangles spanning a few units in X/Z
    // so the centre shadow ray (origin ~y=0, dir +Y) passes through it.
    std::vector<rt::Triangle> tris = {
        {math::Vec3{-2, 3, -2}, math::Vec3{2, 3, -2}, math::Vec3{2, 3, 2}},
        {math::Vec3{-2, 3, -2}, math::Vec3{2, 3, 2}, math::Vec3{-2, 3, 2}},
    };
    rt::Bvh8 blas;
    blas.build(tris.data(), static_cast<u32>(tris.size()));

    rt::Tlas tlas;
    rt::Tlas::InstanceDesc inst{};
    inst.blas = &blas;
    inst.transform = math::identity4();
    tlas.build(&inst, 1);

    ShadowOccluder shadow{};
    shadow.occluder = &tlas;
    shadow.occluded = &tlas_occluded;
    shadow.opacity = 0.8f;  // strong shadow so the delta is unambiguous
    shadow.softness = 0.0f;
    shadow.samples = 1u;

    const u32 shadowed = render_floor_centre(&key, 1, ambient, &shadow);

    // Both pixels are covered (the floor, not the cleared background).
    REQUIRE(lit != 0xFF000000u);
    REQUIRE(shadowed != 0xFF000000u);
    // The shadow ray actually occluded: the Hybrid centre is distinctly darker.
    REQUIRE(red_of(shadowed) < red_of(lit));
    REQUIRE(red_of(lit) - red_of(shadowed) > 40u);
}

// Guard the default path: an INACTIVE occluder (null trampoline) must shade
// byte-identically to no-shadow raster. This is the goldens-unchanged invariant
// — if the hybrid evaluator ever perturbed the no-occluder case, this fails.
TEST_CASE("hybrid shadows: inactive occluder matches plain raster exactly",
          "[render][raster][hybrid][shadows]") {
    RasterLight key{};
    key.kind = RasterLightKind::Point;
    key.position_world = math::Vec3{0.0f, 6.0f, 0.0f};
    key.color_linear = math::Vec3{1.0f, 1.0f, 1.0f};
    key.intensity = 1.0f;
    key.range = 64.0f;
    const math::Vec3 ambient{0.1f, 0.1f, 0.1f};

    const u32 no_shadow = render_floor_centre(&key, 1, ambient, nullptr);

    ShadowOccluder inactive{};  // occluder == nullptr -> active() == false
    const u32 inactive_shadow = render_floor_centre(&key, 1, ambient, &inactive);

    REQUIRE(no_shadow == inactive_shadow);
}
