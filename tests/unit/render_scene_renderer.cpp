// SPDX-License-Identifier: MIT
// Psynder — hybrid scene renderer queue tests.

#include <catch2/catch_test_macros.hpp>

#include "render/SceneRenderer.h"

#include <array>

using namespace psynder;

namespace {

constexpr std::array<render::raster::Vertex, 3> kVerts{{
    {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 0}, {0, 0}, 0xFFFFFFFFu},
    {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0}, {0, 0}, 0xFFFFFFFFu},
    {{0.0f, 0.5f, 0.0f}, {0, 0, 1}, {0, 1}, {0, 0}, 0xFFFFFFFFu},
}};

constexpr std::array<u32, 3> kIndices{0, 1, 2};

}  // namespace

TEST_CASE("scene renderer queues split raster, transparent, RT, and shadow work",
          "[render][scene_renderer]") {
    auto& world = scene::World::Get();
    world.set_structural_deferred(false);
    scene::RuntimeScene scene{world};
    render::SceneRenderer renderer;

    render::MaterialDesc opaque_desc{};
    opaque_desc.flags = render::Material_RasterVisible | render::Material_RtVisible |
                        render::Material_CastsRtShadow;
    const render::MaterialId opaque = scene.materials().create(opaque_desc);

    render::MaterialDesc glass_desc{};
    glass_desc.blend = render::MaterialBlendMode::AlphaBlend;
    glass_desc.flags = render::Material_RasterVisible | render::Material_RtVisible;
    const render::MaterialId glass = scene.materials().create(glass_desc);

    render::MaterialDesc probe_only_desc{};
    probe_only_desc.flags = render::Material_RtVisible;
    const render::MaterialId probe_only = scene.materials().create(probe_only_desc);

    render::MeshDesc mesh_desc{};
    mesh_desc.vertices = kVerts.data();
    mesh_desc.vertex_count = static_cast<u32>(kVerts.size());
    mesh_desc.indices = kIndices.data();
    mesh_desc.index_count = static_cast<u32>(kIndices.size());
    mesh_desc.local_bounds = math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    const render::MeshId mesh_a = renderer.meshes().create_mesh(mesh_desc);
    const render::MeshId mesh_b = renderer.meshes().create_mesh(mesh_desc);
    const render::MeshId mesh_c = renderer.meshes().create_mesh(mesh_desc);

    const Entity a = scene.create_renderable(renderer.make_mesh_renderable(mesh_a, opaque));
    const Entity b = scene.create_renderable(renderer.make_mesh_renderable(mesh_b, glass));
    const Entity c = scene.create_renderable(renderer.make_mesh_renderable(mesh_c, probe_only));

    render::SceneRenderQueues queues;
    render::build_scene_render_queues(scene, queues);

    REQUIRE(queues.all.size() == 3u);
    REQUIRE(queues.raster_opaque.size() == 1u);
    REQUIRE(queues.raster_transparent.size() == 1u);
    REQUIRE(queues.rt_visible.size() == 3u);
    REQUIRE(queues.rt_shadow_casters.size() == 1u);
    REQUIRE(queues.raster_opaque[0].entity == a);
    REQUIRE(queues.raster_transparent[0].entity == b);
    REQUIRE(queues.rt_shadow_casters[0].entity == a);

    scene.destroy_entity(a);
    scene.destroy_entity(b);
    scene.destroy_entity(c);
}

TEST_CASE("scene renderer emits raster draws from mesh handles", "[render][scene_renderer]") {
    auto& world = scene::World::Get();
    world.set_structural_deferred(false);
    scene::RuntimeScene scene{world};
    render::SceneRenderer renderer;

    render::MaterialDesc material_desc{};
    material_desc.flags = render::Material_RasterVisible;
    const render::MaterialId material = scene.materials().create(material_desc);

    render::MeshDesc mesh_desc{};
    mesh_desc.vertices = kVerts.data();
    mesh_desc.vertex_count = static_cast<u32>(kVerts.size());
    mesh_desc.indices = kIndices.data();
    mesh_desc.index_count = static_cast<u32>(kIndices.size());
    mesh_desc.local_bounds = math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    const render::MeshId mesh = renderer.meshes().create_mesh(mesh_desc);
    const Entity entity = scene.create_renderable(renderer.make_mesh_renderable(mesh, material));

    std::array<u32, 16 * 16> pixels{};
    render::Framebuffer fb{};
    fb.pixels = reinterpret_cast<u8*>(pixels.data());
    fb.width = 16;
    fb.height = 16;
    fb.pitch = 16 * sizeof(u32);
    fb.format = render::PixelFormat::RGBA8;

    render::raster::ViewState view{};
    view.target = fb;
    view.view = math::identity4();
    view.projection = math::identity4();

    const render::SceneRenderStats stats = renderer.render_raster(scene, view);
    REQUIRE(stats.submitted == 1u);
    REQUIRE(stats.raster_draws == 1u);
    REQUIRE(stats.raster_triangles == 1u);
    REQUIRE(stats.raster_skipped == 0u);

    scene.destroy_entity(entity);
}

TEST_CASE("scene renderer mesh entities use pooled handles", "[render][scene_renderer]") {
    auto& world = scene::World::Get();
    world.set_structural_deferred(false);
    scene::RuntimeScene scene{world};
    render::SceneRenderer renderer;
    renderer.reserve_scene_capacity(8u);

    render::MaterialDesc material_desc{};
    material_desc.flags = render::Material_RasterVisible;
    const render::MaterialId material = scene.materials().create(material_desc);

    render::MeshDesc mesh_desc{};
    mesh_desc.vertices = kVerts.data();
    mesh_desc.vertex_count = static_cast<u32>(kVerts.size());
    mesh_desc.indices = kIndices.data();
    mesh_desc.index_count = static_cast<u32>(kIndices.size());
    mesh_desc.local_bounds = math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};

    const render::SceneMeshEntity first = renderer.create_mesh_entity(scene, mesh_desc, material);
    REQUIRE(first.entity.valid());
    REQUIRE(first.mesh.valid());
    REQUIRE(renderer.meshes().live_count() == 1u);

    REQUIRE(renderer.meshes().destroy(first.mesh));
    REQUIRE(renderer.meshes().live_count() == 0u);
    REQUIRE(renderer.meshes().free_count() == 1u);

    const render::SceneMeshEntity second = renderer.create_mesh_entity(scene, mesh_desc, material);
    REQUIRE(second.entity.valid());
    REQUIRE(second.mesh.valid());
    REQUIRE((second.mesh.raw & 0x00FFFFFFu) == (first.mesh.raw & 0x00FFFFFFu));
    REQUIRE(second.mesh.raw != first.mesh.raw);

    scene.destroy_entity(first.entity);
    scene.destroy_entity(second.entity);
}
