// SPDX-License-Identifier: MIT
// Psynder -- editor render glue tests: render_scene_rt + bake_lightmaps.
// Builds a tiny in-memory scene (two cubes + a point-light entity + a camera),
// renders it through the software raytracer into an off-screen framebuffer, and
// asserts the framebuffer is non-trivially written (lit pixels where the
// geometry is). The bake path is exercised on a static floor quad with an
// overhead point-light entity and asserted to produce a non-empty, plausibly
// lit lightmap. Lights are gathered from the scene ECS (scene::LightComponent),
// not passed in by the host.

#include <catch2/catch_test_macros.hpp>

#include "editor/render/EditorRender.h"
#include "render/Framebuffer.h"
#include "render/Material.h"
#include "render/RenderingSystem.h"
#include "scene/SceneEcs.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace psynder;

namespace {

// Unit cube centered at origin, half-extent 0.5, outward CCW winding.
// 12 triangles -> 36 indices. Normals are per-face approximations; RT uses
// geometric normals from the BVH, so only positions matter for hit testing.
struct CubeMesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

void push_quad(CubeMesh& m,
               math::Vec3 a,
               math::Vec3 b,
               math::Vec3 c,
               math::Vec3 d,
               math::Vec3 n) {
    const u32 base = static_cast<u32>(m.verts.size());
    const u32 col = 0xFFFFFFFFu;
    m.verts.push_back({a, n, {0, 0}, {0, 0}, col});
    m.verts.push_back({b, n, {1, 0}, {0, 0}, col});
    m.verts.push_back({c, n, {1, 1}, {0, 0}, col});
    m.verts.push_back({d, n, {0, 1}, {0, 0}, col});
    m.indices.push_back(base + 0u);
    m.indices.push_back(base + 1u);
    m.indices.push_back(base + 2u);
    m.indices.push_back(base + 0u);
    m.indices.push_back(base + 2u);
    m.indices.push_back(base + 3u);
}

CubeMesh make_cube() {
    CubeMesh m;
    const math::Vec3 c000{-0.5f, -0.5f, -0.5f}, c100{0.5f, -0.5f, -0.5f};
    const math::Vec3 c010{-0.5f, 0.5f, -0.5f}, c110{0.5f, 0.5f, -0.5f};
    const math::Vec3 c001{-0.5f, -0.5f, 0.5f}, c101{0.5f, -0.5f, 0.5f};
    const math::Vec3 c011{-0.5f, 0.5f, 0.5f}, c111{0.5f, 0.5f, 0.5f};
    push_quad(m, c101, c001, c011, c111, {0, 0, 1});   // +Z
    push_quad(m, c000, c100, c110, c010, {0, 0, -1});  // -Z
    push_quad(m, c100, c101, c111, c110, {1, 0, 0});   // +X
    push_quad(m, c001, c000, c010, c011, {-1, 0, 0});  // -X
    push_quad(m, c011, c010, c110, c111, {0, 1, 0});   // +Y
    push_quad(m, c000, c001, c101, c100, {0, -1, 0});  // -Y
    return m;
}

// A floor quad on the XZ plane at y = 0, facing +Y, spanning [-2, 2].
struct QuadMesh {
    std::array<render::raster::Vertex, 4> verts;
    std::array<u32, 6> indices{0, 1, 2, 0, 2, 3};
    QuadMesh() {
        const math::Vec3 n{0, 1, 0};
        verts = {{{{-2.0f, 0.0f, -2.0f}, n, {0, 0}, {0, 0}, 0xFFFFFFFFu},
                  {{2.0f, 0.0f, -2.0f}, n, {1, 0}, {0, 0}, 0xFFFFFFFFu},
                  {{2.0f, 0.0f, 2.0f}, n, {1, 1}, {0, 0}, 0xFFFFFFFFu},
                  {{-2.0f, 0.0f, 2.0f}, n, {0, 1}, {0, 0}, 0xFFFFFFFFu}}};
    }
};

struct FbStorage {
    std::vector<u32> pixels;
    std::vector<u32> depth;
    render::Framebuffer fb{};
    FbStorage(u32 w, u32 h, u32 clear) {
        pixels.assign(static_cast<usize>(w) * h, clear);
        depth.assign(static_cast<usize>(w) * h, 0u);
        fb.width = w;
        fb.height = h;
        fb.format = render::PixelFormat::RGBA8;
        fb.pitch = w * 4u;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth = depth.data();
    }
};

// White point light packed into a LightComponent.
scene::LightComponent make_point_light(f32 intensity, f32 range) {
    scene::LightComponent light{};
    light.kind = scene::LightKind::Point;
    light.color_rgba8 = 0xFFFFFFFFu;
    light.intensity = intensity;
    light.range = range;
    return light;
}

}  // namespace

TEST_CASE("editor render: render_scene_rt lights a scene into an offscreen framebuffer",
          "[editor][render][rt]") {
    scene::Scene scene;
    render::RenderingSystem renderer;

    render::MaterialDesc mat_desc{};
    mat_desc.albedo_rgba8 = 0xFF3060C0u;  // warm-ish; any non-clear color
    mat_desc.flags = render::Material_RtVisible | render::Material_CastsRtShadow |
                     render::Material_ReceivesRtShadow;
    const render::MaterialId material = scene.materials().create(mat_desc);

    const CubeMesh cube = make_cube();
    render::MeshDesc mesh_desc{};
    mesh_desc.vertices = cube.verts.data();
    mesh_desc.vertex_count = static_cast<u32>(cube.verts.size());
    mesh_desc.indices = cube.indices.data();
    mesh_desc.index_count = static_cast<u32>(cube.indices.size());
    mesh_desc.local_bounds = math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    const render::MeshId cube_mesh = renderer.meshes().create_mesh(mesh_desc);

    // Two instances of the SAME cube BLAS to exercise instance/BLAS reuse.
    const Entity a = scene.create_renderable(
        renderer.make_mesh_renderable(cube_mesh, material), scene::LocalTransform{});
    scene::LocalTransform right{};
    right.translation = {1.5f, 0.0f, 0.0f};
    const Entity b =
        scene.create_renderable(renderer.make_mesh_renderable(cube_mesh, material), right);

    // A point light gathered from the ECS (no host-supplied lights).
    const Entity light_entity = scene.create_entity();
    scene::LocalTransform light_xf{};
    light_xf.translation = {2.0f, 4.0f, 4.0f};
    REQUIRE(scene.set_transform(light_entity, light_xf));
    REQUIRE(scene.attach_light(light_entity, make_point_light(12.0f, 40.0f)));

    // Active camera entity at the eye, looking at the cubes.
    const Entity camera = scene.spawn_camera(scene::CameraDesc{
        /*position=*/{0.0f, 1.0f, 4.0f},
        /*look_at=*/{0.75f, 0.0f, 0.0f},
        /*up=*/{0.0f, 1.0f, 0.0f}});
    REQUIRE(camera.valid());

    constexpr u32 kW = 128, kH = 96;
    const u32 kClear = 0xFF202020u;
    FbStorage target(kW, kH, kClear);

    const f32 aspect = static_cast<f32>(kW) / static_cast<f32>(kH);
    scene::SceneCameraView view{};
    REQUIRE(scene.active_camera_view(aspect, view));

    editor::render::SceneRtOptions opts{};
    opts.trace_downscale = 2;
    opts.use_console_config = false;  // deterministic defaults, no console state

    // Asserts the framebuffer was non-trivially written: pixels differ from the
    // clear color, the image is not a single flat color, and something is lit.
    // Returns the brightest summed-channel value so successive frames can be
    // compared for stability.
    const auto check_lit = [&](const FbStorage& fb) -> u32 {
        usize changed = 0;
        u32 first = fb.pixels[0];
        bool any_different_from_first = false;
        u32 brightest = 0;
        for (u32 p : fb.pixels) {
            if (p != kClear)
                ++changed;
            if (p != first)
                any_different_from_first = true;
            const u32 r = p & 0xFFu, g = (p >> 8) & 0xFFu, b2 = (p >> 16) & 0xFFu;
            brightest = std::max(brightest, r + g + b2);
        }
        REQUIRE(changed > 0u);
        REQUIRE(any_different_from_first);  // not a uniform fill -- geometry shows
        REQUIRE(brightest > 0u);            // something is lit
        return brightest;
    };

    // Frame 1: cold cache. Geometry is new, so the cube BLAS is built once
    // (blas_built == 1) and shared by both instances (blas_count == 1).
    const editor::render::SceneRtStats stats =
        editor::render::render_scene_rt(scene, view, renderer, target.fb, opts);

    REQUIRE(stats.rendered);
    REQUIRE(stats.instance_count == 2u);
    REQUIRE(stats.blas_count == 1u);  // both instances reuse the one cube BLAS
    REQUIRE(stats.blas_built == 1u);  // cold cache: the one cube BLAS was built
    REQUIRE(stats.light_count == 1u);  // gathered from the LightComponent entity
    REQUIRE(stats.frame.hit_pixels > 0u);
    const u32 brightest1 = check_lit(target);

    // Frame 2: geometry unchanged, so the module-owned cache must fully hit --
    // the BLAS is reused (blas_built == 0) and the output is identical, while
    // the TLAS / instance / light buffers are refilled (no realloc) and the
    // image still renders correctly.
    FbStorage target2(kW, kH, kClear);
    const editor::render::SceneRtStats stats2 =
        editor::render::render_scene_rt(scene, view, renderer, target2.fb, opts);

    REQUIRE(stats2.rendered);
    REQUIRE(stats2.instance_count == 2u);
    REQUIRE(stats2.blas_count == 1u);
    REQUIRE(stats2.blas_built == 0u);  // cache hit: no BLAS rebuilt on frame 2
    REQUIRE(stats2.light_count == 1u);
    REQUIRE(stats2.frame.hit_pixels > 0u);
    const u32 brightest2 = check_lit(target2);

    // Cache reuse must not change the render: identical inputs -> the same lit
    // result across the two frames (brightest summed channel is a stable proxy;
    // exact per-pixel equality is avoided as RT tiles are traced in parallel).
    REQUIRE(brightest1 == brightest2);

    REQUIRE(scene.destroy_entity(a));
    REQUIRE(scene.destroy_entity(b));
    REQUIRE(scene.destroy_entity(light_entity));
    REQUIRE(scene.destroy_entity(camera));
}

TEST_CASE("editor render: bake_lightmaps produces a non-empty plausible lightmap",
          "[editor][render][bake]") {
    scene::Scene scene;
    render::RenderingSystem renderer;

    // Static, bake-eligible floor material.
    render::MaterialDesc floor_desc{};
    floor_desc.albedo_rgba8 = 0xFFB0B0B0u;
    floor_desc.flags = render::Material_BakeVisible | render::Material_CastsBakedShadow |
                       render::Material_ReceivesBakedShadow;
    const render::MaterialId floor_mat = scene.materials().create(floor_desc);

    const QuadMesh quad;
    render::MeshDesc mesh_desc{};
    mesh_desc.vertices = quad.verts.data();
    mesh_desc.vertex_count = static_cast<u32>(quad.verts.size());
    mesh_desc.indices = quad.indices.data();
    mesh_desc.index_count = static_cast<u32>(quad.indices.size());
    mesh_desc.local_bounds = math::Aabb{{-2.0f, 0.0f, -2.0f}, {2.0f, 0.0f, 2.0f}};
    const render::MeshId quad_mesh = renderer.meshes().create_mesh(mesh_desc);

    const Entity floor = scene.create_renderable(renderer.make_mesh_renderable(
        quad_mesh,
        floor_mat,
        scene::RenderableFlags::Visible,
        math::aabb_empty(),
        scene::ObjectMobility::Static));

    // Overhead point light, gathered from the ECS.
    const Entity light_entity = scene.create_entity();
    scene::LocalTransform light_xf{};
    light_xf.translation = {0.0f, 3.0f, 0.0f};  // directly above the floor
    REQUIRE(scene.set_transform(light_entity, light_xf));
    REQUIRE(scene.attach_light(light_entity, make_point_light(10.0f, 20.0f)));

    editor::render::BakeOptions bake_opts{};
    bake_opts.lightmap_resolution = 8;
    bake_opts.max_indirect_bounces = 0;

    const editor::render::BakeResult result =
        editor::render::bake_lightmaps(scene, renderer, bake_opts);

    REQUIRE(result.ok);
    REQUIRE(result.triangle_count == 2u);  // floor quad -> 2 tris
    REQUIRE(result.surface_count == 2u);
    REQUIRE(result.light_count == 1u);  // gathered from the LightComponent entity
    REQUIRE_FALSE(result.lmlight.empty());
    REQUIRE(result.max_luminance > 0.0f);  // the floor caught direct light

    // The atlas must carry actual texel storage.
    usize total_texels = 0;
    for (const auto& surf : result.atlas.surfaces)
        total_texels += surf.pixels.size();
    REQUIRE(total_texels > 0u);

    REQUIRE(scene.destroy_entity(floor));
    REQUIRE(scene.destroy_entity(light_entity));
}
