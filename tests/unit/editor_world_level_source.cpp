// SPDX-License-Identifier: MIT
// Psynder — editor "level source" loaders: BSP + terrain into a scene::Scene.
// Headless (no window): build the demo fixtures, load each into an in-memory
// scene, assert the scene gains renderable mesh entities with non-empty
// geometry and sane bounds.

#include <catch2/catch_test_macros.hpp>

#include "editor/world/LevelSource.h"
#include "math/Bounds.h"
#include "scene/SceneEcs.h"

#include <span>
#include <vector>

using namespace psynder;

namespace {

struct RegistryScope {
    RegistryScope() {
        auto& registry = scene::EcsRegistry::Get();
        registry.clear();
        registry.set_structural_deferred(false);
    }
    ~RegistryScope() { scene::EcsRegistry::Get().clear(); }
};

// Count renderable mesh entities the scene would actually submit.
u32 count_mesh_render_items(scene::Scene& scene) {
    std::vector<scene::SceneRenderItem> items;
    scene.update_transforms();
    scene.gather_render_items(items);
    u32 n = 0u;
    for (const auto& item : items) {
        if (item.geometry == scene::GeometryKind::Mesh)
            ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("editor world: demo BSP fixture has consistent topology + geometry",
          "[editor][world][bsp]") {
    editor::world::BspLevelSource source;
    editor::world::build_demo_bsp_level(source);

    REQUIRE(source.map.leaves.size() == 4u);
    REQUIRE(source.map.nodes.size() == 4u);
    REQUIRE(source.map.faces.size() > 0u);
    // Per-face index bookkeeping is 1:1 with faces.
    REQUIRE(source.face_index_offset.size() == source.map.faces.size());
    REQUIRE(source.face_index_count.size() == source.map.faces.size());
    REQUIRE(source.vertices.size() > 0u);
    REQUIRE(source.indices.size() > 0u);
    // Three solid leaves carry faces; the 4th (solid-outside) carries none.
    u32 leaves_with_faces = 0u;
    for (const auto& leaf : source.map.leaves)
        leaves_with_faces += leaf.face_count != 0u ? 1u : 0u;
    REQUIRE(leaves_with_faces == 3u);
    REQUIRE_FALSE(math::is_empty(source.bounds));
    // Every index references a valid vertex.
    for (u32 idx : source.indices)
        REQUIRE(idx < source.vertices.size());
}

TEST_CASE("editor world: load_bsp_into_scene spawns one renderable per solid leaf",
          "[editor][world][bsp]") {
    RegistryScope scope;
    scene::Scene scene{scene::EcsRegistry::Get()};

    editor::world::BspLevelSource source;
    editor::world::build_demo_bsp_level(source);

    editor::world::LevelGeometry geometry{1000u};
    std::vector<Entity> entities;
    const editor::world::LoadResult result =
        editor::world::load_bsp_into_scene(scene, source, geometry, {}, &entities);

    // One renderable per solid leaf (the solid-outside leaf has no faces).
    REQUIRE(result.entities_created == 3u);
    REQUIRE(result.meshes_created == 3u);
    REQUIRE(result.materials_created > 0u);
    REQUIRE(entities.size() == 3u);
    REQUIRE_FALSE(math::is_empty(result.bounds));

    // Each produced mesh has non-empty geometry + sane bounds + stable id.
    REQUIRE(geometry.mesh_count() == 3u);
    for (const auto& mesh : geometry.meshes()) {
        REQUIRE(mesh.geometry_id >= 1000u);
        REQUIRE(mesh.vertex_count > 0u);
        REQUIRE(mesh.index_count > 0u);
        REQUIRE(mesh.desc.vertices != nullptr);
        REQUIRE(mesh.desc.indices != nullptr);
        REQUIRE(mesh.desc.vertex_count == mesh.vertex_count);
        REQUIRE(mesh.desc.index_count == mesh.index_count);
        REQUIRE_FALSE(math::is_empty(mesh.local_bounds));
        // Pointer-stable storage: the MeshDesc pointers stay readable.
        const auto& first = mesh.desc.vertices[0];
        (void)first;
    }

    // The scene actually submits the mesh renderables through the normal path.
    REQUIRE(count_mesh_render_items(scene) == 3u);

    // The RenderableComponent geometry ids match the produced LevelMeshes.
    auto& registry = scene.registry();
    for (Entity e : entities) {
        const auto* r = registry.get<scene::RenderableComponent>(e);
        REQUIRE(r != nullptr);
        REQUIRE(r->geometry == scene::GeometryKind::Mesh);
        REQUIRE(r->geometry_id >= 1000u);
        REQUIRE(scene.materials().valid(r->material));
        REQUIRE_FALSE(math::is_empty(r->local_bounds));
    }
}

TEST_CASE("editor world: load_bsp_into_scene merged mode emits a single renderable",
          "[editor][world][bsp]") {
    RegistryScope scope;
    scene::Scene scene{scene::EcsRegistry::Get()};

    editor::world::BspLevelSource source;
    editor::world::build_demo_bsp_level(source);

    editor::world::LevelGeometry geometry;
    editor::world::LoadBspOptions options;
    options.one_renderable_per_leaf = false;
    const editor::world::LoadResult result =
        editor::world::load_bsp_into_scene(scene, source, geometry, options);

    REQUIRE(result.entities_created == 1u);
    REQUIRE(geometry.mesh_count() == 1u);
    REQUIRE(geometry.meshes()[0].index_count == source.indices.size());
    REQUIRE(count_mesh_render_items(scene) == 1u);
}

TEST_CASE("editor world: demo heightmap fixture is deterministic + non-flat",
          "[editor][world][terrain]") {
    world::outdoor::HeightmapDesc desc{};
    const std::vector<u16> heights = editor::world::build_demo_heightmap(desc, 64u);

    REQUIRE(desc.size_x == 64u);
    REQUIRE(desc.size_z == 64u);
    REQUIRE(desc.heights == heights.data());
    REQUIRE(heights.size() == 64u * 64u);

    u16 lo = 0xFFFFu;
    u16 hi = 0u;
    for (u16 h : heights) {
        lo = h < lo ? h : lo;
        hi = h > hi ? h : hi;
    }
    REQUIRE(hi > lo);  // there is real relief (ridge + hills)
}

TEST_CASE("editor world: load_terrain_into_scene builds a grid mesh entity",
          "[editor][world][terrain]") {
    RegistryScope scope;
    scene::Scene scene{scene::EcsRegistry::Get()};

    world::outdoor::HeightmapDesc desc{};
    const std::vector<u16> heights = editor::world::build_demo_heightmap(desc, 32u);

    editor::world::LevelGeometry geometry{5000u};
    std::vector<Entity> entities;
    const editor::world::LoadResult result =
        editor::world::load_terrain_into_scene(scene, desc, geometry, {}, &entities);

    REQUIRE(result.entities_created == 1u);
    REQUIRE(result.meshes_created == 1u);
    REQUIRE(result.materials_created == 1u);
    REQUIRE(entities.size() == 1u);
    REQUIRE(geometry.mesh_count() == 1u);

    const auto& mesh = geometry.meshes()[0];
    // 32x32 grid → 1024 verts, 31x31 quads x 2 tris x 3 indices.
    REQUIRE(mesh.vertex_count == 32u * 32u);
    REQUIRE(mesh.index_count == 31u * 31u * 6u);
    REQUIRE(mesh.desc.vertices != nullptr);
    REQUIRE(mesh.desc.indices != nullptr);
    REQUIRE_FALSE(math::is_empty(mesh.local_bounds));
    // Mesh spans the heightfield footprint and has vertical extent (relief).
    REQUIRE(mesh.local_bounds.max.x > mesh.local_bounds.min.x);
    REQUIRE(mesh.local_bounds.max.z > mesh.local_bounds.min.z);
    REQUIRE(mesh.local_bounds.max.y > mesh.local_bounds.min.y);

    REQUIRE(count_mesh_render_items(scene) == 1u);

    const auto* r = scene.registry().get<scene::RenderableComponent>(entities[0]);
    REQUIRE(r != nullptr);
    REQUIRE(r->geometry == scene::GeometryKind::Mesh);
    REQUIRE(r->geometry_id == 5000u);
    REQUIRE(scene.materials().valid(r->material));
}

TEST_CASE("editor world: loaders reject malformed input without crashing",
          "[editor][world]") {
    RegistryScope scope;
    scene::Scene scene{scene::EcsRegistry::Get()};
    editor::world::LevelGeometry geometry;

    // Terrain with null heights / degenerate size → no entities.
    world::outdoor::HeightmapDesc bad{};
    bad.size_x = 0u;
    bad.size_z = 0u;
    const editor::world::LoadResult tr =
        editor::world::load_terrain_into_scene(scene, bad, geometry);
    REQUIRE(tr.entities_created == 0u);

    // BSP source with mismatched face/index arrays → no entities.
    editor::world::BspLevelSource src;
    editor::world::build_demo_bsp_level(src);
    src.face_index_count.pop_back();
    const editor::world::LoadResult br =
        editor::world::load_bsp_into_scene(scene, src, geometry);
    REQUIRE(br.entities_created == 0u);
}
