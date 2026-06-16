// SPDX-License-Identifier: MIT
// Psynder — editor "level source" loaders. Lane 18 (editor) owns.
//
// The editor today can author primitive meshes and load .psyscene files, but
// it has no path to bring the engine's two world systems into a scene:
//   * the indoor BSP system (engine/world/bsp — BspMap, leaves, faces, PVS),
//   * the outdoor terrain system (engine/world/outdoor — heightfield).
//
// This module bridges that gap. It converts a BSP map (sample 03 / 13 build
// theirs in-memory) and a heightfield (sample 06) into ordinary scene mesh
// entities so the editor renders both through the normal scene render path
// (GeometryKind::Mesh renderables gathered by gather_scene_render_items).
//
// ─── Geometry ownership ───────────────────────────────────────────────────
// render::MeshDesc (render/Geometry.h) stores *pointers* into caller-owned
// vertex / index storage — the Scene and the host MeshLibrary never copy the
// geometry. So the loaders cannot stash geometry inside the Scene. Instead
// each loader appends its owned vertex / index pools + per-mesh descriptors
// into a caller-held `LevelGeometry` and creates scene entities whose
// RenderableComponent.geometry_id indexes into that pool. The host keeps the
// LevelGeometry alive for the lifetime of the scene and registers each
// LevelMesh with its render::MeshLibrary / mesh spawner under the SAME id, so
// the existing geometry_id -> mesh resolution the editor already performs for
// authored / loaded meshes works unchanged. Geometry ids are allocated from a
// caller-chosen base so they never collide with the host's own mesh ids.
//
// ─── Terrain backend choice: MESH (not raymarch) ─────────────────────────
// world::outdoor::TerrainRaymarch::render() paints pixels + packed Z directly
// into a bound framebuffer (TerrainTarget); it is a render-time backend and
// produces no scene entities, so it cannot populate a scene::Scene. A grid
// mesh built from the heightfield flows through the identical RenderableComponent
// path as every other editor mesh — lower risk, headless-testable, and it is
// what "render the terrain through the standard renderer" asks for. The
// raymarcher remains available to the host directly for a play-mode flyover.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Geometry.h"
#include "render/Material.h"
#include "render/raster/Raster.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"
#include "world/bsp/Bsp.h"
#include "world/outdoor/Terrain.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace psynder::editor::world {

// One renderable mesh produced by a loader. The vertex / index storage it
// points at is owned by the LevelGeometry that contains it (see below); these
// spans / the MeshDesc stay valid only while that LevelGeometry is alive and
// un-mutated past the point the mesh was appended.
struct LevelMesh {
    u32 geometry_id = 0u;                  // RenderableComponent.geometry_id
    ::psynder::render::MeshDesc desc{};    // pointers into the owning pools
    ::psynder::render::MaterialId material{};
    math::Aabb local_bounds = math::aabb_empty();
    u32 first_vertex = 0u;                 // offset into LevelGeometry.vertices
    u32 vertex_count = 0u;
    u32 first_index = 0u;                  // offset into LevelGeometry.indices
    u32 index_count = 0u;
};

// Owned geometry storage for one or more loads. Pointer-stable: the per-mesh
// pools live in stable heap blocks (std::unique_ptr<Pool>) so appending more
// meshes never invalidates the MeshDesc pointers of meshes already produced.
// The host keeps this alive for the scene's lifetime and registers each
// LevelMesh with its MeshLibrary under LevelMesh::geometry_id.
class LevelGeometry {
   public:
    struct Pool {
        std::vector<::psynder::render::Vertex> vertices;
        std::vector<u32> indices;
    };

    LevelGeometry() = default;
    explicit LevelGeometry(u32 geometry_id_base) noexcept : next_geometry_id_(geometry_id_base) {}

    // Allocate a fresh, pointer-stable pool. Returned reference stays valid for
    // the lifetime of this LevelGeometry.
    Pool& new_pool() {
        pools_.push_back(std::make_unique<Pool>());
        return *pools_.back();
    }

    [[nodiscard]] u32 allocate_geometry_id() noexcept { return next_geometry_id_++; }

    void add_mesh(const LevelMesh& mesh) { meshes_.push_back(mesh); }

    [[nodiscard]] std::span<const LevelMesh> meshes() const noexcept {
        return {meshes_.data(), meshes_.size()};
    }
    [[nodiscard]] usize mesh_count() const noexcept { return meshes_.size(); }
    [[nodiscard]] usize pool_count() const noexcept { return pools_.size(); }

   private:
    std::vector<std::unique_ptr<Pool>> pools_{};
    std::vector<LevelMesh> meshes_{};
    u32 next_geometry_id_ = 1u;
};

// ─── BSP level source ──────────────────────────────────────────────────────
//
// A BspMap stores only the topology (nodes / leaves / faces / pvs). The face
// geometry (positions / normals / colours / fan indices) lives in a separate
// pool the map references by face — sample 03 keeps it in its own World. This
// bundle pairs a BspMap with that pool so a loader can recover the geometry.
struct BspLevelSource {
    ::psynder::world::bsp::BspMap map{};
    std::vector<::psynder::render::Vertex> vertices{};   // shared vertex pool
    std::vector<u32> indices{};                          // fan indices, by face
    std::vector<u32> face_index_offset{};                // per BspFace
    std::vector<u32> face_index_count{};                 // per BspFace
    math::Aabb bounds = math::aabb_empty();
    f32 floor_y = 0.0f;
};

struct LoadBspOptions {
    // Emit one renderable per BSP leaf (true, default) or merge every leaf's
    // faces into a single renderable (false). Per-leaf keeps the BSP structure
    // visible / selectable in the editor outliner; merged is one draw.
    bool one_renderable_per_leaf = true;
    // Parent node for the spawned entities (kInvalidSceneNode = scene root).
    scene::SceneNode parent = scene::kInvalidSceneNode;
    // Renderables are static level geometry by default.
    scene::ObjectMobility mobility = scene::ObjectMobility::Static;
    // Interior surfaces face inward; cull front faces so you see the room from
    // inside, mirroring the indoor convention (CullMode::Front).
    ::psynder::render::raster::CullMode cull = ::psynder::render::raster::CullMode::Front;
    // Name prefix for spawned entities ("<prefix> Leaf 0", ...).
    std::string_view name_prefix = "BSP";
};

struct LoadResult {
    u32 entities_created = 0u;
    u32 meshes_created = 0u;
    u32 materials_created = 0u;
    math::Aabb bounds = math::aabb_empty();
};

// FROZEN ENTRY POINT.
// Convert a BSP level source into scene mesh entities. Materials are created in
// scene.materials(); geometry is appended to `geometry` (which the host keeps
// alive + registers with its MeshLibrary). Returns the spawned entities (one
// per renderable) appended to `out_entities` if non-null.
LoadResult load_bsp_into_scene(scene::Scene& scene,
                               const BspLevelSource& source,
                               LevelGeometry& geometry,
                               const LoadBspOptions& options = {},
                               std::vector<Entity>* out_entities = nullptr);

// ─── Terrain level source ──────────────────────────────────────────────────

struct LoadTerrainOptions {
    scene::SceneNode parent = scene::kInvalidSceneNode;
    scene::ObjectMobility mobility = scene::ObjectMobility::Static;
    ::psynder::render::raster::CullMode cull = ::psynder::render::raster::CullMode::Back;
    u32 albedo_rgba8 = 0xFF6F8F5Fu;  // mossy green
    std::string_view name = "Terrain";
};

// FROZEN ENTRY POINT.
// Build a grid mesh from a heightfield and add it as a single scene mesh
// entity. `desc.heights` must point at desc.size_x * desc.size_z u16 samples
// (kept alive only for the duration of this call — the mesh stores its own
// world-space vertices). World origin is the heightfield corner (0,0); the
// mesh spans [0, (size_x-1)*spacing] x [0, (size_z-1)*spacing].
LoadResult load_terrain_into_scene(scene::Scene& scene,
                                   const ::psynder::world::outdoor::HeightmapDesc& desc,
                                   LevelGeometry& geometry,
                                   const LoadTerrainOptions& options = {},
                                   std::vector<Entity>* out_entities = nullptr);

// ─── Demo fixtures (ports of samples 03 + 06 procedural generators) ─────────
//
// These give the editor a "New indoor map" / "New terrain" starting point and
// supply the unit-test fixtures. They build into in-memory structures only;
// no VFS / disk access.

// Build the sample-03 two-room-plus-doorway BSP map (4 leaves, 3 clusters).
void build_demo_bsp_level(BspLevelSource& out);

// Build the sample-06 256x256 value-noise + ridge heightfield. The returned
// vector owns the u16 samples; `out_desc.heights` points into it, so keep the
// vector alive while you use the desc.
std::vector<u16> build_demo_heightmap(::psynder::world::outdoor::HeightmapDesc& out_desc,
                                      u32 size = 256u,
                                      f32 spacing = 1.0f,
                                      f32 height_scale = 30.0f / 65535.0f);

}  // namespace psynder::editor::world
