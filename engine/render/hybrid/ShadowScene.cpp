// SPDX-License-Identifier: MIT
// Psynder -- M-HYB hybrid-shadow occluder builder (see ShadowScene.h).
//
// Mirrors editor/render/EditorRender.cpp's RT cache: address-stable per-mesh
// BLAS (Bvh8) in a never-popped std::deque, fingerprint-gated rebuilds, and a
// single persistent Tlas rebuilt each frame. The only addition over the RT
// builder is the ShadowOccluder trampoline that lets the raster core (which
// does not link rt) trace occlusion through the opaque TLAS pointer.

#include "render/hybrid/ShadowScene.h"

#include "render/Geometry.h"
#include "render/rt/Bvh.h"
#include "render/rt/HeightmapShadow.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace psynder::render::hybrid {

namespace {

namespace rrt = ::psynder::render::rt;

// An address-stable BLAS plus the local-space triangles it was built from
// (kept alive so the Bvh8 stays rebuildable; Bvh8 builds from caller-owned
// tris). Lives in a deque, never relocated or popped.
struct MeshBlas {
    rrt::Bvh8 bvh{};
    std::vector<rrt::Triangle> tris{};
};

// Identity of the geometry a cached BLAS was built from. MeshLibrary recycles
// slots in place, but every reuse changes the source pointers and/or counts,
// so this fingerprint detects a real content change without a per-slot version.
struct MeshFingerprint {
    const ::psynder::render::raster::Vertex* vertices = nullptr;
    const u32* indices = nullptr;
    u32 vertex_count = 0;
    u32 index_count = 0;

    bool operator==(const MeshFingerprint& o) const noexcept {
        return vertices == o.vertices && indices == o.indices &&
               vertex_count == o.vertex_count && index_count == o.index_count;
    }
    bool operator!=(const MeshFingerprint& o) const noexcept { return !(*this == o); }
};

MeshFingerprint mesh_fingerprint(const ::psynder::render::MeshView& mesh_view,
                                 u32 mesh_slot) noexcept {
    return {mesh_view.vertices[mesh_slot], mesh_view.indices[mesh_slot],
            mesh_view.vertex_count[mesh_slot], mesh_view.index_count[mesh_slot]};
}

struct CachedBlas {
    MeshBlas* blas = nullptr;  // points into the persistent deque (stable address)
    MeshFingerprint fingerprint{};
};

// A combined occluder the trampoline reads through: the mesh BVH (TLAS) AND an
// optional borrowed terrain heightmap. Address-stable (lives in the module
// cache) so the ShadowOccluder can borrow a pointer to it across frames.
struct CombinedOccluder {
    const rrt::Tlas* tlas = nullptr;
    rrt::Heightmap heightmap{};  // valid() == false when no terrain is bound
    bool has_heightmap = false;
};

// Module-owned cache reused across build_shadow_scene calls (the host calls it
// every frame). All members keep address-stable / reusable storage so a steady
// scene allocates nothing after the first frame.
struct ShadowCache {
    std::deque<MeshBlas> blas_storage;                 // stable addresses, never popped
    std::unordered_map<u32, CachedBlas> slot_to_blas;  // mesh slot -> cached BLAS
    rrt::Tlas tlas{};                                  // persistent (address-keyed) TLAS
    std::vector<rrt::Tlas::InstanceDesc> instances;
    std::vector<u32> referenced_slots;
    CombinedOccluder combined{};                       // address-stable view handed out
};

ShadowCache& shadow_cache() {
    static ShadowCache cache;
    return cache;
}
std::mutex& shadow_cache_mutex() {
    static std::mutex m;
    return m;
}

void gather_mesh_triangles(const ::psynder::render::MeshView& mesh_view,
                           u32 mesh_slot,
                           std::vector<rrt::Triangle>& out) {
    out.clear();
    const ::psynder::render::raster::Vertex* verts = mesh_view.vertices[mesh_slot];
    const u32* indices = mesh_view.indices[mesh_slot];
    const u32 index_count = mesh_view.index_count[mesh_slot];
    const u32 vertex_count = mesh_view.vertex_count[mesh_slot];
    if (verts == nullptr || indices == nullptr || index_count < 3u || vertex_count == 0u)
        return;
    out.reserve(index_count / 3u);
    for (u32 i = 0; i + 2u < index_count; i += 3u) {
        const u32 i0 = indices[i + 0u];
        const u32 i1 = indices[i + 1u];
        const u32 i2 = indices[i + 2u];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
            continue;
        out.push_back({verts[i0].position, verts[i1].position, verts[i2].position});
    }
}

// The ShadowOccluder trampoline (DESIGN.md 8.2): cast the opaque pointer back
// to the module's CombinedOccluder and MAX-combine the two occlusion paths --
// the mesh BVH (TLAS) AND, when bound, a uniform-grid heightmap march. This is
// the single point where the raster shadow term reaches into the raytracer;
// everything upstream stays opaque. "MAX-combine per pixel" reduces to a
// logical OR of the two "blocked" results: a ray is occluded if EITHER path
// blocks it. The heightmap-only / mesh-only cases fall out naturally (the
// unbound path short-circuits), and an inactive heightmap leaves the mesh
// result byte-identical to the BVH-only build.
bool combined_occluded(const void* occluder,
                       math::Vec3 origin,
                       math::Vec3 dir,
                       f32 t_min,
                       f32 t_max) noexcept {
    const auto* combined = static_cast<const CombinedOccluder*>(occluder);
    if (combined == nullptr)
        return false;

    rrt::Ray ray{};
    ray.origin = origin;
    ray.direction = dir;
    ray.t_min = t_min;
    ray.t_max = t_max;

    // Mesh BVH first (cheap early-out on the common occluded-by-mesh case).
    if (combined->tlas != nullptr && combined->tlas->occluded(ray))
        return true;
    // Terrain heightmap march, MAX-combined. Inert when no field is bound.
    if (combined->has_heightmap)
        return rrt::trace_heightmap_shadow(combined->heightmap, ray);
    return false;
}

}  // namespace

ShadowSceneStats build_shadow_scene(
    ::psynder::scene::Scene& scene,
    ::psynder::render::RenderingSystem& renderer,
    ::psynder::render::raster::ShadowOccluder& out_occluder) noexcept {
    return build_shadow_scene(scene, renderer, /*heightmap=*/nullptr, out_occluder);
}

ShadowSceneStats build_shadow_scene(
    ::psynder::scene::Scene& scene,
    ::psynder::render::RenderingSystem& renderer,
    const ::psynder::render::rt::Heightmap* heightmap,
    ::psynder::render::raster::ShadowOccluder& out_occluder) noexcept {
    ShadowSceneStats stats{};
    out_occluder.occluder = nullptr;
    out_occluder.occluded = nullptr;

    const bool has_heightmap = heightmap != nullptr && heightmap->valid();

    std::lock_guard<std::mutex> guard(shadow_cache_mutex());
    ShadowCache& cache = shadow_cache();

    // Gather the RT-visible renderables (world transforms already resolved).
    ::psynder::render::SceneRenderQueues queues;
    ::psynder::render::build_scene_render_queues(scene, queues);

    const ::psynder::render::MeshLibrary& meshes = renderer.meshes();
    const ::psynder::render::MeshView mesh_view = meshes.view();

    cache.instances.clear();
    cache.referenced_slots.clear();
    cache.instances.reserve(queues.rt_visible.size());

    u32 blas_built = 0;

    for (const u32 item_index : queues.rt_visible) {
        const ::psynder::scene::SceneRenderItem& item = queues.item(item_index);
        if (item.geometry != ::psynder::scene::GeometryKind::Mesh)
            continue;  // analytic geometry not traced here (matches the RT path)
        u32 mesh_slot = 0;
        if (!meshes.slot(::psynder::render::mesh_id_from_raw(item.geometry_id), mesh_slot))
            continue;

        const MeshFingerprint fp = mesh_fingerprint(mesh_view, mesh_slot);

        CachedBlas* entry = nullptr;
        auto found = cache.slot_to_blas.find(mesh_slot);
        if (found != cache.slot_to_blas.end())
            entry = &found->second;

        const bool needs_build =
            entry == nullptr || entry->blas == nullptr || entry->fingerprint != fp;

        if (needs_build) {
            // Allocate a persistent, address-stable BLAS the FIRST time a slot is
            // seen; thereafter rebuild the SAME Bvh8 object in place when its mesh
            // changed (Bvh8 is address-keyed; its registry entry is never erased).
            if (entry == nullptr || entry->blas == nullptr) {
                MeshBlas& blas = cache.blas_storage.emplace_back();
                CachedBlas slot_entry{};
                slot_entry.blas = &blas;
                entry = &cache.slot_to_blas.insert_or_assign(mesh_slot, slot_entry).first->second;
            }

            gather_mesh_triangles(mesh_view, mesh_slot, entry->blas->tris);
            entry->fingerprint = fp;
            if (entry->blas->tris.empty())
                continue;  // degenerate / empty mesh: no instance, no BVH build
            entry->blas->bvh.build(entry->blas->tris.data(),
                                   static_cast<u32>(entry->blas->tris.size()));
            ++blas_built;
        } else if (entry->blas->tris.empty()) {
            continue;  // cached as degenerate; still skip
        }

        if (std::find(cache.referenced_slots.begin(), cache.referenced_slots.end(), mesh_slot) ==
            cache.referenced_slots.end()) {
            cache.referenced_slots.push_back(mesh_slot);
        }

        rrt::Tlas::InstanceDesc desc{};
        desc.blas = &entry->blas->bvh;
        desc.transform = item.world;
        cache.instances.push_back(desc);
    }

    const bool has_mesh = !cache.instances.empty();
    // Nothing to trace against: no mesh geometry AND no terrain field. Leave the
    // occluder inactive so the caller falls back to plain raster (goldens safe).
    if (!has_mesh && !has_heightmap)
        return stats;

    cache.combined = CombinedOccluder{};
    if (has_mesh) {
        cache.tlas.build(cache.instances.data(), static_cast<u32>(cache.instances.size()));
        cache.combined.tlas = &cache.tlas;
    }
    if (has_heightmap) {
        cache.combined.heightmap = *heightmap;  // borrowed y_data; caller owns storage
        cache.combined.has_heightmap = true;
    }

    out_occluder.occluder = &cache.combined;
    out_occluder.occluded = &combined_occluded;

    stats.instance_count = static_cast<u32>(cache.instances.size());
    stats.blas_count = static_cast<u32>(cache.referenced_slots.size());
    stats.blas_built = blas_built;
    stats.heightmap = has_heightmap;
    stats.built = true;
    return stats;
}

}  // namespace psynder::render::hybrid
