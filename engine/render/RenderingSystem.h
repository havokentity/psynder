// SPDX-License-Identifier: MIT
// Psynder — ECS/DOTS rendering system for hybrid raster + RT scenes.

#pragma once

#include "core/Log.h"
#include "render/FrameStats.h"
#include "render/Geometry.h"
#include "render/GeometryTools.h"
#include "render/MaterialBatcher.h"
#include "render/Material.h"
#include "render/raster/SurfaceCache.h"
#include "render/rt/FrameRenderer.h"
#include "scene/SceneEcs.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace psynder::render {

struct SceneRenderQueues {
    std::vector<scene::SceneRenderItem> all;
    std::vector<u32> raster_opaque;
    std::vector<u32> raster_transparent;
    std::vector<u32> raster_shadow_casters;
    std::vector<u32> raster_shadow_receivers;
    std::vector<u32> rt_visible;
    std::vector<u32> rt_shadow_casters;
    std::vector<u32> bake_static;
    std::vector<u32> bake_shadow_casters;
    std::vector<u32> bake_shadow_receivers;
    u32 dynamic_bake_rejected = 0;

    void clear() {
        all.clear();
        raster_opaque.clear();
        raster_transparent.clear();
        raster_shadow_casters.clear();
        raster_shadow_receivers.clear();
        rt_visible.clear();
        rt_shadow_casters.clear();
        bake_static.clear();
        bake_shadow_casters.clear();
        bake_shadow_receivers.clear();
        dynamic_bake_rejected = 0;
    }

    void reserve_like_all() {
        raster_opaque.reserve(all.size());
        raster_transparent.reserve(all.size());
        raster_shadow_casters.reserve(all.size() / 4u);
        raster_shadow_receivers.reserve(all.size());
        rt_visible.reserve(all.size());
        rt_shadow_casters.reserve(all.size());
        bake_static.reserve(all.size());
        bake_shadow_casters.reserve(all.size());
        bake_shadow_receivers.reserve(all.size());
    }

    [[nodiscard]] const scene::SceneRenderItem& item(u32 index) const noexcept {
        return all[index];
    }
};

struct SceneRenderPolicyIssue {
    Entity entity{};
    MaterialId material{};
    scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic;
    MaterialFlags material_flags = MaterialFlags::None;
};

inline void build_scene_render_queues(scene::Scene& scene, SceneRenderQueues& queues) {
    queues.clear();
    scene.update_transforms();
    scene.gather_render_items(queues.all);
    queues.reserve_like_all();

    const MaterialLibrary& materials = scene.materials();
    const MaterialView material_view = materials.view();
    for (u32 item_index = 0; item_index < static_cast<u32>(queues.all.size()); ++item_index) {
        const scene::SceneRenderItem& item = queues.all[item_index];
        u32 material_slot = 0;
        if (!materials.slot(item.material, material_slot))
            continue;
        const MaterialFlags flags = material_view.flags[material_slot];
        const bool is_static = item.mobility == scene::ObjectMobility::Static;
        if ((flags & MaterialFlags::RasterVisible) != 0u) {
            if (material_view.blend[material_slot] == MaterialBlendMode::AlphaBlend)
                queues.raster_transparent.push_back(item_index);
            else
                queues.raster_opaque.push_back(item_index);
        }
        if ((flags & MaterialFlags::CastsRasterShadow) != 0u &&
            material_view.raster_shadow_mode[material_slot] ==
                MaterialRasterShadowMode::ProjectedDecal) {
            queues.raster_shadow_casters.push_back(item_index);
        }
        if ((flags & MaterialFlags::ReceivesRasterShadow) != 0u)
            queues.raster_shadow_receivers.push_back(item_index);
        if ((flags & MaterialFlags::RtVisible) != 0u)
            queues.rt_visible.push_back(item_index);
        if ((flags & MaterialFlags::CastsRtShadow) != 0u)
            queues.rt_shadow_casters.push_back(item_index);
        const u32 bake_flags = flags & Material_BakedLightingMask;
        if (bake_flags != 0u) {
            if (!is_static) {
                ++queues.dynamic_bake_rejected;
            } else {
                if ((flags & MaterialFlags::BakeVisible) != 0u)
                    queues.bake_static.push_back(item_index);
                if ((flags & MaterialFlags::CastsBakedShadow) != 0u)
                    queues.bake_shadow_casters.push_back(item_index);
                if ((flags & MaterialFlags::ReceivesBakedShadow) != 0u)
                    queues.bake_shadow_receivers.push_back(item_index);
            }
        }
    }
}

inline u32 collect_scene_render_policy_issues(scene::Scene& scene,
                                              std::vector<SceneRenderPolicyIssue>& out) {
    out.clear();
    std::vector<scene::SceneRenderItem> items;
    scene.update_transforms();
    scene.gather_render_items(items);

    const MaterialLibrary& materials = scene.materials();
    const MaterialView material_view = materials.view();
    for (const scene::SceneRenderItem& item : items) {
        u32 material_slot = 0;
        if (!materials.slot(item.material, material_slot))
            continue;
        const MaterialFlags flags = material_view.flags[material_slot];
        if (item.mobility == scene::ObjectMobility::Dynamic &&
            (flags & Material_BakedLightingMask) != 0u) {
            out.push_back({item.entity, item.material, item.mobility, flags});
        }
    }
    return static_cast<u32>(out.size());
}

inline u32 warn_scene_render_policy_issues(scene::Scene& scene) {
    std::vector<SceneRenderPolicyIssue> issues;
    const u32 count = collect_scene_render_policy_issues(scene, issues);
    for (const SceneRenderPolicyIssue& issue : issues) {
        PSY_LOG_WARN(
            "scene: dynamic entity {} uses baked material flags 0x{:08X}; bake shadow/lightmap "
            "participation is ignored until the object is Static",
            issue.entity.raw,
            issue.material_flags & Material_BakedLightingMask);
    }
    return count;
}

struct SceneRenderStats {
    u32 submitted = 0;
    u32 raster_draws = 0;
    u32 raster_triangles = 0;
    u32 raster_skipped = 0;
    u32 raster_shadow_casters = 0;
    u32 raster_shadow_receivers = 0;
    u32 rt_visible = 0;
    u32 rt_shadow_casters = 0;
    u32 bake_static = 0;
    u32 bake_shadow_casters = 0;
    u32 bake_shadow_receivers = 0;
    u32 dynamic_bake_rejected = 0;
    u32 material_batches = 0;
};

struct SceneMeshEntity {
    Entity entity{};
    MeshId mesh{};
    MaterialId material{};
};

enum class BuiltInMesh : u8 {
    TexturedTriangle,
    UnitCube,
    Pyramid,
    Cone,
    UvSphere,
    GeodesicSphere,
    SierpinskiTetrahedron,
    SierpinskiCarpet,
};

class RenderingSystem {
   public:
    [[nodiscard]] MeshLibrary& meshes() noexcept { return meshes_; }
    [[nodiscard]] const MeshLibrary& meshes() const noexcept { return meshes_; }
    [[nodiscard]] const SceneRenderQueues& queues() const noexcept { return queues_; }
    [[nodiscard]] const MaterialBatcher& material_batches() const noexcept {
        return material_batches_;
    }

    void reserve_scene_capacity(u32 renderables, u32 meshes = 0) {
        const u32 mesh_capacity = meshes == 0u ? renderables : meshes;
        meshes_.reserve(mesh_capacity);
        queues_.all.reserve(renderables);
        queues_.raster_opaque.reserve(renderables);
        queues_.raster_transparent.reserve(renderables / 4u);
        queues_.raster_shadow_casters.reserve(renderables / 4u);
        queues_.raster_shadow_receivers.reserve(renderables);
        queues_.rt_visible.reserve(renderables);
        queues_.rt_shadow_casters.reserve(renderables);
        queues_.bake_static.reserve(renderables);
        queues_.bake_shadow_casters.reserve(renderables);
        queues_.bake_shadow_receivers.reserve(renderables);
        material_batches_.reserve(renderables, renderables);
    }

    void prewarm_builtin_meshes(const TextureAsset* base_color_asset = nullptr) {
        [[maybe_unused]] const MeshId triangle =
            builtin_mesh(BuiltInMesh::TexturedTriangle, base_color_asset);
        [[maybe_unused]] const MeshId cube = builtin_mesh(BuiltInMesh::UnitCube, base_color_asset);
        [[maybe_unused]] const MeshId pyramid =
            builtin_mesh(BuiltInMesh::Pyramid, base_color_asset);
        [[maybe_unused]] const MeshId cone = builtin_mesh(BuiltInMesh::Cone, base_color_asset);
        [[maybe_unused]] const MeshId sphere =
            builtin_mesh(BuiltInMesh::UvSphere, base_color_asset);
        [[maybe_unused]] const MeshId geodesic =
            builtin_mesh(BuiltInMesh::GeodesicSphere, base_color_asset);
        [[maybe_unused]] const MeshId tetra =
            builtin_mesh(BuiltInMesh::SierpinskiTetrahedron, base_color_asset);
        [[maybe_unused]] const MeshId carpet =
            builtin_mesh(BuiltInMesh::SierpinskiCarpet, base_color_asset);
    }

    [[nodiscard]] MeshId builtin_mesh(
        BuiltInMesh mesh,
        const TextureAsset* base_color_asset = nullptr) {
        for (const BuiltInMeshEntry& entry : builtin_meshes_) {
            if (entry.kind == mesh && entry.base_color_asset == base_color_asset &&
                meshes_.valid(entry.mesh)) {
                return entry.mesh;
            }
        }
        const MeshId id = meshes_.create_mesh(builtin_mesh_desc(mesh, base_color_asset));
        builtin_meshes_.push_back({mesh, base_color_asset, id});
        return id;
    }

    [[nodiscard]] MeshId cached_mesh(const MeshDesc& mesh_desc) {
        const u64 key = mesh_desc_key(mesh_desc);
        for (const MeshCacheEntry& entry : mesh_cache_) {
            if (entry.key == key && meshes_.valid(entry.mesh))
                return entry.mesh;
        }
        const MeshId id = meshes_.create_mesh(mesh_desc);
        mesh_cache_.push_back({key, id});
        return id;
    }

    [[nodiscard]] MeshId cached_mesh(const geometry_tools::GeneratedMesh& mesh) {
        const u64 key = generated_mesh_key(mesh);
        for (const GeneratedMeshCacheEntry& entry : generated_mesh_cache_) {
            if (entry.key == key && meshes_.valid(entry.mesh))
                return entry.mesh;
        }
        generated_mesh_cache_.push_back({key, {}, mesh});
        GeneratedMeshCacheEntry& entry = generated_mesh_cache_.back();
        entry.mesh = meshes_.create_mesh(entry.storage.desc());
        return entry.mesh;
    }

    [[nodiscard]] scene::RenderableComponent make_mesh_renderable(
        MeshId mesh,
        MaterialId material,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        math::Aabb local_bounds = math::aabb_empty(),
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) const {
        scene::RenderableComponent out{};
        out.geometry = scene::GeometryKind::Mesh;
        out.geometry_id = mesh.raw;
        out.material = material;
        out.flags = flags;
        out.mobility = mobility;
        if (!math::is_empty(local_bounds)) {
            out.local_bounds = local_bounds;
        } else {
            const MeshDesc desc = meshes_.get(mesh);
            out.local_bounds = desc.local_bounds;
        }
        return out;
    }

    [[nodiscard]] SceneMeshEntity create_mesh_entity(
        scene::Scene& scene,
        const MeshDesc& mesh_desc,
        MaterialId material,
        const scene::LocalTransform& local = {},
        scene::SceneNode parent = scene::kInvalidSceneNode,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        const MeshId mesh = meshes_.create_mesh(mesh_desc);
        const Entity entity = scene.create_renderable(
            make_mesh_renderable(mesh, material, flags, math::aabb_empty(), mobility),
            local,
            parent);
        return {entity, mesh, material};
    }

    [[nodiscard]] SceneMeshEntity create_mesh_entity(
        scene::Scene& scene,
        const MeshDesc& mesh_desc,
        const MaterialDesc& material_desc,
        const scene::LocalTransform& local = {},
        scene::SceneNode parent = scene::kInvalidSceneNode,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        const MaterialId material = scene.materials().create(material_desc);
        return create_mesh_entity(scene, mesh_desc, material, local, parent, flags, mobility);
    }

    [[nodiscard]] SceneMeshEntity create_mesh_instance(
        scene::Scene& scene,
        MeshId mesh,
        MaterialId material,
        const scene::LocalTransform& local = {},
        scene::SceneNode parent = scene::kInvalidSceneNode,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        if (!material.valid())
            material = scene.materials().create(MaterialDesc{});
        const Entity entity = scene.create_renderable(
            make_mesh_renderable(mesh, material, flags, math::aabb_empty(), mobility),
            local,
            parent);
        return {entity, mesh, material};
    }

    [[nodiscard]] Entity spawn_mesh(
        scene::Scene& scene,
        const MeshDesc& mesh_desc,
        const scene::LocalTransform& local = {},
        scene::SceneNode parent = scene::kInvalidSceneNode,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        return create_mesh_entity(scene, mesh_desc, MaterialDesc{}, local, parent, flags, mobility)
            .entity;
    }

    [[nodiscard]] Entity spawn_mesh_instance(
        scene::Scene& scene,
        MeshId mesh,
        MaterialId material,
        const scene::LocalTransform& local = {},
        scene::SceneNode parent = scene::kInvalidSceneNode,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        return create_mesh_instance(scene, mesh, material, local, parent, flags, mobility).entity;
    }

    u32 spawn_mesh_batch(scene::Scene& scene,
                         MeshId mesh,
                         MaterialId material,
                         std::span<const scene::LocalTransform> local,
                         std::span<Entity> out_entities,
                         scene::SceneNode parent = scene::kInvalidSceneNode,
                         scene::RenderableFlags flags = scene::RenderableFlags::Visible,
                         scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        const usize count = std::min(local.size(), out_entities.size());
        if (count == 0u)
            return 0u;
        if (!material.valid())
            material = scene.materials().create(MaterialDesc{});
        u32 spawned = 0u;
        for (usize i = 0; i < count; ++i) {
            out_entities[i] =
                create_mesh_instance(scene, mesh, material, local[i], parent, flags, mobility).entity;
            if (out_entities[i].valid())
                ++spawned;
        }
        return spawned;
    }

    SceneRenderStats build(scene::Scene& scene) {
        build_scene_render_queues(scene, queues_);
        SceneRenderStats stats{};
        stats.submitted = static_cast<u32>(queues_.all.size());
        stats.raster_shadow_casters = static_cast<u32>(queues_.raster_shadow_casters.size());
        stats.raster_shadow_receivers = static_cast<u32>(queues_.raster_shadow_receivers.size());
        stats.rt_visible = static_cast<u32>(queues_.rt_visible.size());
        stats.rt_shadow_casters = static_cast<u32>(queues_.rt_shadow_casters.size());
        stats.bake_static = static_cast<u32>(queues_.bake_static.size());
        stats.bake_shadow_casters = static_cast<u32>(queues_.bake_shadow_casters.size());
        stats.bake_shadow_receivers = static_cast<u32>(queues_.bake_shadow_receivers.size());
        stats.dynamic_bake_rejected = queues_.dynamic_bake_rejected;
        material_batches_.build(queues_.all, scene.materials());
        stats.material_batches = material_batches_.batch_count();
        return stats;
    }

    SceneRenderStats render_raster(scene::Scene& scene, const raster::ViewState& view) {
        SceneRenderStats stats = build(scene);
        raster::Rasterizer& rasterizer = raster::Rasterizer::Get();
        rasterizer.begin_frame(view);
        emit_raster_queue(queues_.raster_opaque, scene.materials(), rasterizer, stats);
        emit_raster_queue(queues_.raster_transparent, scene.materials(), rasterizer, stats);
        rasterizer.end_frame();
        record_raster_work(stats.raster_draws, stats.raster_triangles);
        return stats;
    }

    SceneRenderStats render_raster_draws(const raster::ViewState& view,
                                         std::span<const raster::DrawItem> draws) {
        SceneRenderStats stats{};
        raster::Rasterizer& rasterizer = raster::Rasterizer::Get();
        rasterizer.begin_frame(view);
        for (const raster::DrawItem& draw : draws) {
            if (draw.vertices == nullptr || draw.vertex_count == 0u || draw.indices == nullptr ||
                draw.index_count < 3u) {
                ++stats.raster_skipped;
                continue;
            }
            rasterizer.submit(draw);
            ++stats.raster_draws;
            stats.raster_triangles += draw.index_count / 3u;
        }
        rasterizer.end_frame();
        stats.submitted = static_cast<u32>(draws.size());
        record_raster_work(stats.raster_draws, stats.raster_triangles);
        return stats;
    }

    void begin_raster_frame(const raster::ViewState& view) {
        direct_raster_stats_ = {};
        raster::Rasterizer::Get().begin_frame(view);
    }

    bool submit_raster_draw(const raster::DrawItem& draw) {
        ++direct_raster_stats_.submitted;
        if (draw.vertices == nullptr || draw.vertex_count == 0u || draw.indices == nullptr ||
            draw.index_count < 3u) {
            ++direct_raster_stats_.raster_skipped;
            return false;
        }
        raster::Rasterizer::Get().submit(draw);
        ++direct_raster_stats_.raster_draws;
        direct_raster_stats_.raster_triangles += draw.index_count / 3u;
        return true;
    }

    SceneRenderStats end_raster_frame() {
        raster::Rasterizer::Get().end_frame();
        record_raster_work(direct_raster_stats_.raster_draws,
                           direct_raster_stats_.raster_triangles);
        return direct_raster_stats_;
    }

    void render_rt(const rt::FrameRenderInput& input,
                   const rt::FrameRenderConfig& config,
                   u32* output_rgba8,
                   rt::FrameRenderStats* stats = nullptr) {
        rt_renderer_.render(input, config, output_rgba8, stats);
    }

   private:
    static raster::CullMode cull_from_material(MaterialWinding winding,
                                               raster::CullMode mesh_cull) noexcept {
        if (winding == MaterialWinding::DoubleSided)
            return raster::CullMode::None;
        if (winding == MaterialWinding::Cw)
            return mesh_cull == raster::CullMode::Front ? raster::CullMode::Back
                                                        : raster::CullMode::Front;
        return mesh_cull;
    }

    bool build_raster_draw(const scene::SceneRenderItem& item,
                           const MaterialLibrary& materials,
                           raster::DrawItem& draw) const {
        if (item.geometry != scene::GeometryKind::Mesh)
            return false;
        u32 mesh_slot = 0;
        if (!meshes_.slot(mesh_id_from_raw(item.geometry_id), mesh_slot))
            return false;
        u32 material_slot = 0;
        if (!materials.slot(item.material, material_slot))
            return false;

        const MeshView mesh_view = meshes_.view();
        const MaterialView material_view = materials.view();
        draw = {};
        draw.vertices = mesh_view.vertices[mesh_slot];
        draw.vertex_count = mesh_view.vertex_count[mesh_slot];
        draw.indices = mesh_view.indices[mesh_slot];
        draw.index_count = mesh_view.index_count[mesh_slot];
        draw.model = item.world;
        draw.material = item.material;
        draw.cull =
            cull_from_material(material_view.winding[material_slot], mesh_view.cull[mesh_slot]);
        if (material_view.blend[material_slot] == MaterialBlendMode::AlphaTest)
            draw.flags |= raster::DrawFlags::AlphaTest;
        const TextureView texture = mesh_view.base_color_asset[mesh_slot] != nullptr
                                        ? mesh_view.base_color_asset[mesh_slot]->view()
                                        : mesh_view.base_color[mesh_slot];
        if (texture.valid()) {
            draw.lightmap_texels = texture.texels;
            draw.lightmap_w = texture.width;
            draw.lightmap_h = texture.height;
        }
        return draw.vertices != nullptr && draw.vertex_count != 0u && draw.indices != nullptr &&
               draw.index_count >= 3u;
    }

    void emit_raster_queue(std::span<const u32> item_indices,
                           const MaterialLibrary& materials,
                           raster::Rasterizer& rasterizer,
                           SceneRenderStats& stats) const {
        for (const u32 item_index : item_indices) {
            if (item_index >= queues_.all.size()) {
                ++stats.raster_skipped;
                continue;
            }
            const scene::SceneRenderItem& item = queues_.all[item_index];
            raster::DrawItem draw{};
            if (!build_raster_draw(item, materials, draw)) {
                ++stats.raster_skipped;
                continue;
            }
            rasterizer.submit(draw);
            ++stats.raster_draws;
            stats.raster_triangles += draw.index_count / 3u;
        }
    }

    struct BuiltInMeshEntry {
        BuiltInMesh kind = BuiltInMesh::TexturedTriangle;
        const TextureAsset* base_color_asset = nullptr;
        MeshId mesh{};
    };

    struct MeshCacheEntry {
        u64 key = 0;
        MeshId mesh{};
    };

    struct GeneratedMeshCacheEntry {
        u64 key = 0;
        MeshId mesh{};
        geometry_tools::GeneratedMesh storage{};
    };

    static MeshDesc builtin_mesh_desc(BuiltInMesh mesh,
                                      const TextureAsset* base_color_asset) noexcept {
        switch (mesh) {
            case BuiltInMesh::TexturedTriangle:
                return geometry_tools::textured_triangle(base_color_asset);
            case BuiltInMesh::UnitCube:
                return geometry_tools::unit_cube(base_color_asset);
            case BuiltInMesh::Pyramid:
                return geometry_tools::pyramid(base_color_asset);
            case BuiltInMesh::Cone:
                return geometry_tools::cone(base_color_asset);
            case BuiltInMesh::UvSphere:
                return geometry_tools::uv_sphere(base_color_asset);
            case BuiltInMesh::GeodesicSphere:
                return geometry_tools::geodesic_sphere(base_color_asset);
            case BuiltInMesh::SierpinskiTetrahedron:
                return geometry_tools::sierpinski_tetrahedron(base_color_asset);
            case BuiltInMesh::SierpinskiCarpet:
                return geometry_tools::sierpinski_carpet(base_color_asset);
        }
        return geometry_tools::textured_triangle(base_color_asset);
    }

    static u64 hash_bytes(const void* data, usize byte_count, u64 seed) noexcept {
        constexpr u64 kPrime = 1099511628211ull;
        const auto* bytes = static_cast<const u8*>(data);
        u64 hash = seed;
        for (usize i = 0; i < byte_count; ++i) {
            hash ^= bytes[i];
            hash *= kPrime;
        }
        return hash;
    }

    template <class T>
    static u64 hash_value(const T& value, u64 seed) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        return hash_bytes(&value, sizeof(T), seed);
    }

    static u64 mesh_desc_key(const MeshDesc& desc) noexcept {
        u64 hash = 14695981039346656037ull;
        const auto vertices = reinterpret_cast<std::uintptr_t>(desc.vertices);
        const auto indices = reinterpret_cast<std::uintptr_t>(desc.indices);
        const auto texture_asset = reinterpret_cast<std::uintptr_t>(desc.base_color_asset);
        hash = hash_value(vertices, hash);
        hash = hash_value(desc.vertex_count, hash);
        hash = hash_value(indices, hash);
        hash = hash_value(desc.index_count, hash);
        hash = hash_texture_view(desc.base_color, hash);
        hash = hash_value(texture_asset, hash);
        hash = hash_value(desc.cull, hash);
        hash = hash_aabb(desc.local_bounds, hash);
        return hash;
    }

    static u64 generated_mesh_key(const geometry_tools::GeneratedMesh& mesh) noexcept {
        u64 hash = 14695981039346656037ull;
        const u64 tag = 0xA61C5EED5A77E11Full;
        hash = hash_value(tag, hash);
        const u32 vertex_count = static_cast<u32>(mesh.vertices.size());
        const u32 index_count = static_cast<u32>(mesh.indices.size());
        const auto texture_asset = reinterpret_cast<std::uintptr_t>(mesh.base_color_asset);
        hash = hash_value(vertex_count, hash);
        if (!mesh.vertices.empty()) {
            for (const Vertex& vertex : mesh.vertices)
                hash = hash_vertex(vertex, hash);
        }
        hash = hash_value(index_count, hash);
        if (!mesh.indices.empty())
            hash = hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32), hash);
        hash = hash_texture_view(mesh.base_color, hash);
        hash = hash_value(texture_asset, hash);
        hash = hash_value(mesh.cull, hash);
        hash = hash_aabb(mesh.local_bounds, hash);
        return hash;
    }

    static u64 hash_vec2(math::Vec2 v, u64 hash) noexcept {
        hash = hash_value(v.x, hash);
        return hash_value(v.y, hash);
    }

    static u64 hash_vec3(math::Vec3 v, u64 hash) noexcept {
        hash = hash_value(v.x, hash);
        hash = hash_value(v.y, hash);
        return hash_value(v.z, hash);
    }

    static u64 hash_aabb(const math::Aabb& bounds, u64 hash) noexcept {
        hash = hash_vec3(bounds.min, hash);
        return hash_vec3(bounds.max, hash);
    }

    static u64 hash_texture_view(TextureView texture, u64 hash) noexcept {
        const auto texels = reinterpret_cast<std::uintptr_t>(texture.texels);
        hash = hash_value(texels, hash);
        hash = hash_value(texture.width, hash);
        hash = hash_value(texture.height, hash);
        return hash_value(texture.pitch, hash);
    }

    static u64 hash_vertex(const Vertex& vertex, u64 hash) noexcept {
        hash = hash_vec3(vertex.position, hash);
        hash = hash_vec3(vertex.normal, hash);
        hash = hash_vec2(vertex.uv, hash);
        hash = hash_vec2(vertex.lightmap_uv, hash);
        return hash_value(vertex.color, hash);
    }

    MeshLibrary meshes_{};
    std::vector<BuiltInMeshEntry> builtin_meshes_{};
    std::vector<MeshCacheEntry> mesh_cache_{};
    std::vector<GeneratedMeshCacheEntry> generated_mesh_cache_{};
    SceneRenderQueues queues_{};
    MaterialBatcher material_batches_{};
    SceneRenderStats direct_raster_stats_{};
    rt::FrameRenderer rt_renderer_{};
};

using HybridRenderer = RenderingSystem;

}  // namespace psynder::render
