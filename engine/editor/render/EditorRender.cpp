// SPDX-License-Identifier: MIT
// Psynder -- editor render glue implementation. See EditorRender.h.

#include "EditorRender.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace psynder::editor::render {

namespace {

namespace rrt = ::psynder::render::rt;
namespace bake = ::psynder::tools::bake;

// Transform a local-space point by a column-major Mat4 (w == 1).
math::Vec3 transform_point(const math::Mat4& m, math::Vec3 p) noexcept {
    const math::Vec4 r = math::mul(m, math::Vec4{p.x, p.y, p.z, 1.0f});
    return {r.x, r.y, r.z};
}

// One unique mesh slot turned into a BLAS, with its local-space triangles kept
// alive for the duration of the render (Bvh8 builds from caller-owned tris;
// keeping them here is harmless and keeps the BLAS rebuildable for refit).
//
// IMPORTANT: render::rt::Bvh8 keys its internal state on its own object
// address (the engine stores BVH state in a singleton registry keyed by `this`,
// and that registry does NOT erase an entry when a Bvh8 is destroyed). The BVH
// must therefore (a) be built only after the Bvh8 lives at its final, stable
// address, (b) live in storage that never relocates its elements, and (c) never
// be destroyed-then-reallocated at a possibly-reused address. We hold MeshBlas
// in a std::deque held alive for the whole module lifetime (stable element
// addresses across growth, never popped) and build in place.
//
// `fingerprint` captures the mesh's source geometry identity so we only rebuild
// a cached BLAS when its mesh actually changed. MeshLibrary recycles slots and
// rewrites them in place (MeshLibrary::update / create_mesh after destroy), but
// both paths change the source vertex/index pointers and/or counts, so a
// fingerprint over (generation-bearing pointers + counts) detects a real
// content change without the engine exposing a per-slot version.
struct MeshBlas {
    rrt::Bvh8 bvh{};
    std::vector<rrt::Triangle> tris{};
};

// Identity of the geometry a cached BLAS was built from.
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

// A cache entry: the address-stable BLAS plus the fingerprint it was built from.
struct CachedBlas {
    MeshBlas* blas = nullptr;      // points into the persistent deque (stable address)
    MeshFingerprint fingerprint{};
};

// Module-owned cache reused across render_scene_rt calls (the host calls every
// frame). All members keep address-stable / reusable storage so a steady scene
// allocates nothing after the first frame:
//   - blas_storage: address-stable BLAS objects, never relocated or popped
//     (see the Bvh8 address-keying note above).
//   - slot_to_blas: mesh slot -> cache entry (fingerprint + BLAS pointer).
//   - tlas: a single persistent Tlas; its registry state is keyed by its stable
//     address and reused across frames. Tlas::build re-assigns all internal
//     state, so rebuilding it each frame is correct (instance transforms change
//     per frame; the cheap-to-rebuild TLAS is rebuilt, the expensive BLAS are
//     cached). InstanceDesc entries are NOT self-address-keyed, so `instances`
//     is a plain reused vector (clear + refill).
//   - instances / instance_materials / lights: reused vectors (clear + refill).
struct RtCache {
    std::deque<MeshBlas> blas_storage;                 // stable element addresses, never popped
    std::unordered_map<u32, CachedBlas> slot_to_blas;  // mesh slot -> cached BLAS
    rrt::Tlas tlas{};                                  // persistent (address-keyed) TLAS
    std::vector<rrt::Tlas::InstanceDesc> instances;
    std::vector<::psynder::render::MaterialId> instance_materials;
    std::vector<rrt::FrameLight> lights;
    std::vector<u32> referenced_slots;  // distinct mesh slots that backed an instance this frame
};

// One process-wide cache, guarded by a mutex (the host drives one viewport from
// the frame thread, but render_scene_rt is a free function with no instance to
// hang state off, so the cache is a function-local static; the mutex keeps it
// safe if the entry point is ever called from more than one thread).
RtCache& rt_cache() {
    static RtCache cache;
    return cache;
}
std::mutex& rt_cache_mutex() {
    static std::mutex m;
    return m;
}

// Pull a mesh's local-space triangles out of the MeshLibrary view by slot.
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

// A bake triangle in local space with the authored shading normal averaged
// from its vertices. The bake kernel derives the lighting normal from triangle
// winding (build_basis), so we use the shading normal to orient the emitted
// winding consistently with the surface the artist authored.
struct BakeTri {
    math::Vec3 v0, v1, v2;
    math::Vec3 shading_normal;  // local space, averaged vertex normal
};

void gather_mesh_bake_triangles(const ::psynder::render::MeshView& mesh_view,
                                u32 mesh_slot,
                                std::vector<BakeTri>& out) {
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
        BakeTri t{};
        t.v0 = verts[i0].position;
        t.v1 = verts[i1].position;
        t.v2 = verts[i2].position;
        t.shading_normal = math::normalize(
            math::add(math::add(verts[i0].normal, verts[i1].normal), verts[i2].normal));
        out.push_back(t);
    }
}

// Gather the scene's lights from the ECS (scene::LightComponent) into the RT
// FrameLight representation. Uses the engine's gather_scene_lights helper,
// which runs the LightComponent query (parallel per chunk, mutex-merged) and
// resolves each light's world-space pose from the scene graph. The mapping is:
//   LightComponent.color_rgba8 -> FrameLight.r/g/b (R,G,B bytes -> [0,1])
//   LightComponent.intensity   -> FrameLight.intensity
//   LightComponent.range       -> FrameLight.range
//   world translation          -> FrameLight.position
// Directional lights have no finite world position, so they are pushed far
// along the negative light direction so the point-light kernel approximates a
// distant source; spot/point lights use their world translation directly.
void gather_scene_frame_lights(::psynder::scene::Scene& scene,
                               std::vector<rrt::FrameLight>& out) {
    out.clear();
    // gather_lights reads world matrices from the scene graph, so the cached
    // world transforms must be current before the light query runs.
    scene.update_transforms();
    std::vector<::psynder::scene::SceneLightItem> items;
    scene.gather_lights(items);
    out.reserve(items.size());
    for (const ::psynder::scene::SceneLightItem& item : items) {
        rrt::FrameLight light{};
        const u32 rgba = item.color_rgba8;
        light.r = static_cast<f32>(rgba & 0xFFu) / 255.0f;
        light.g = static_cast<f32>((rgba >> 8) & 0xFFu) / 255.0f;
        light.b = static_cast<f32>((rgba >> 16) & 0xFFu) / 255.0f;
        light.intensity = item.intensity;
        light.range = item.range > 0.0f ? item.range : 100.0f;
        if (item.kind == ::psynder::scene::LightKind::Directional) {
            // Place a distant proxy along -direction; the point kernel then
            // shades roughly parallel rays over the scene's local extent.
            const f32 distance = std::max(light.range, 100.0f);
            light.position = math::sub(item.position, math::mul(item.direction, distance));
        } else {
            light.position = item.position;
        }
        out.push_back(light);
    }
}

// Recover an rt::FrameCamera from a scene::SceneCameraView (view + projection
// matrices, column-major). The camera world matrix is the inverse of the view
// matrix; eye is its translation, forward is its -Z column, up is its +Y
// column. The vertical FOV is recovered from projection.m[5] (= 1/tan(fov/2)).
rrt::FrameCamera frame_camera_from_view(const ::psynder::scene::SceneCameraView& view,
                                        f32 aspect) {
    const math::Mat4 world = math::inverse_affine(view.view);
    const math::Vec3 eye{world.m[12], world.m[13], world.m[14]};
    const math::Vec3 forward{-world.m[8], -world.m[9], -world.m[10]};
    const math::Vec3 up{world.m[4], world.m[5], world.m[6]};
    const f32 proj_f = view.projection.m[5];
    const f32 fov_y = proj_f > 1.0e-4f ? 2.0f * std::atan(1.0f / proj_f)
                                       : 60.0f * math::kDegToRad;
    return rrt::make_frame_camera(eye, forward, aspect, fov_y, up);
}

// Refresh the module-owned BLAS cache + persistent TLAS/instance buffers from
// the scene's RT-visible renderables and (re)render. `cache` is locked by the
// caller. `lights` is the already-gathered light list (a span over either a
// host buffer or cache.lights). Returns stats including blas_built (0 when the
// BLAS cache fully hit, i.e. no geometry changed this frame).
SceneRtStats render_scene_rt_locked(RtCache& cache,
                                    ::psynder::scene::Scene& scene,
                                    const ::psynder::scene::SceneCameraView& view,
                                    ::psynder::render::RenderingSystem& renderer,
                                    std::span<const rrt::FrameLight> lights,
                                    ::psynder::render::Framebuffer& target,
                                    const SceneRtOptions& options) {
    SceneRtStats stats{};

    if (target.pixels == nullptr || target.width == 0u || target.height == 0u ||
        target.format != ::psynder::render::PixelFormat::RGBA8) {
        return stats;
    }

    // Build the scene queues: rt_visible holds indices into queues.all of the
    // RT-visible renderables, gathered with world transforms already resolved.
    ::psynder::render::SceneRenderQueues queues;
    ::psynder::render::build_scene_render_queues(scene, queues);
    if (queues.rt_visible.empty())
        return stats;

    const ::psynder::render::MeshLibrary& meshes = renderer.meshes();
    const ::psynder::render::MeshView mesh_view = meshes.view();

    // Reuse the persistent instance buffers: clear + refill, never reallocate
    // (steady state allocates nothing after the first frame).
    cache.instances.clear();
    cache.instance_materials.clear();
    cache.referenced_slots.clear();
    cache.instances.reserve(queues.rt_visible.size());
    cache.instance_materials.reserve(queues.rt_visible.size());

    u32 blas_built = 0;  // BLAS actually (re)built this frame

    for (const u32 item_index : queues.rt_visible) {
        const ::psynder::scene::SceneRenderItem& item = queues.item(item_index);
        if (item.geometry != ::psynder::scene::GeometryKind::Mesh)
            continue;  // analytic geometry path not wired here yet (see API gaps)
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
            // Allocate a persistent, address-stable BLAS the FIRST time a slot
            // is seen; thereafter rebuild the SAME Bvh8 object in place when its
            // mesh changed (Bvh8 is address-keyed and its registry entry is
            // never erased -- so we must never destroy/reallocate it; rebuilding
            // in place reuses the existing registry state).
            if (entry == nullptr || entry->blas == nullptr) {
                MeshBlas& blas = cache.blas_storage.emplace_back();
                CachedBlas slot_entry{};
                slot_entry.blas = &blas;
                entry = &cache.slot_to_blas.insert_or_assign(mesh_slot, slot_entry).first->second;
            }

            gather_mesh_triangles(mesh_view, mesh_slot, entry->blas->tris);
            // Record the fingerprint we just gathered from regardless of result,
            // so an unchanged (even degenerate) mesh is not re-gathered every
            // frame. tris.empty() below marks the entry as a skip; the Bvh8
            // object stays address-stable in the deque either way.
            entry->fingerprint = fp;
            if (entry->blas->tris.empty()) {
                continue;  // degenerate / empty mesh: no instance, no BVH build
            }
            entry->blas->bvh.build(entry->blas->tris.data(),
                                   static_cast<u32>(entry->blas->tris.size()));
            ++blas_built;
        } else if (entry->blas->tris.empty()) {
            continue;  // cached as degenerate; still skip
        }

        // Track distinct mesh slots that backed an instance this frame (for the
        // blas_count stat). N is the number of unique meshes in an editor view,
        // so the linear scan is cheap; referenced_slots is a reused buffer.
        if (std::find(cache.referenced_slots.begin(), cache.referenced_slots.end(), mesh_slot) ==
            cache.referenced_slots.end()) {
            cache.referenced_slots.push_back(mesh_slot);
        }

        rrt::Tlas::InstanceDesc desc{};
        desc.blas = &entry->blas->bvh;
        desc.transform = item.world;
        cache.instances.push_back(desc);
        cache.instance_materials.push_back(item.material);
    }

    if (cache.instances.empty())
        return stats;

    // Rebuild the persistent (address-stable) TLAS from this frame's instances.
    // Tlas::build re-assigns all internal state, so reusing the same object is
    // correct; the expensive per-mesh BLAS were cached above.
    cache.tlas.build(cache.instances.data(), static_cast<u32>(cache.instances.size()));

    const f32 aspect = static_cast<f32>(target.width) / static_cast<f32>(target.height);
    rrt::FrameCamera camera = frame_camera_from_view(view, aspect);

    rrt::FrameRenderInput input{};
    input.tlas = &cache.tlas;
    input.camera = camera;
    input.lights = lights.data();
    input.light_count = static_cast<u32>(lights.size());
    input.materials.library = &scene.materials();
    input.materials.instance_materials = cache.instance_materials.data();
    input.materials.instance_material_count =
        static_cast<u32>(cache.instance_materials.size());

    const u32 downscale = std::max<u32>(1u, options.trace_downscale);
    const u32 tile_size = std::max<u32>(1u, options.tile_size);
    const u32 trace_w = std::max<u32>(1u, target.width / downscale);
    const u32 trace_h = std::max<u32>(1u, target.height / downscale);

    rrt::FrameRenderConfig config =
        options.use_console_config
            ? rrt::frame_render_config_from_console(target.width, target.height, trace_w, trace_h,
                                                    tile_size)
            : rrt::FrameRenderConfig{};
    if (!options.use_console_config) {
        config.output_width = target.width;
        config.output_height = target.height;
        config.trace_width = trace_w;
        config.trace_height = trace_h;
        config.tile_size = tile_size;
    }

    renderer.render_rt(input, config, reinterpret_cast<u32*>(target.pixels), &stats.frame);

    stats.instance_count = static_cast<u32>(cache.instances.size());
    stats.blas_count = static_cast<u32>(cache.referenced_slots.size());
    stats.blas_built = blas_built;
    stats.light_count = static_cast<u32>(lights.size());
    stats.rendered = true;
    return stats;
}

BakeResult bake_lightmaps_impl(::psynder::scene::Scene& scene,
                               ::psynder::render::RenderingSystem& renderer,
                               std::span<const rrt::FrameLight> lights,
                               const BakeOptions& options) {
    BakeResult result{};

    ::psynder::render::SceneRenderQueues queues;
    ::psynder::render::build_scene_render_queues(scene, queues);
    if (queues.bake_static.empty())
        return result;

    const ::psynder::render::MeshLibrary& meshes = renderer.meshes();
    const ::psynder::render::MeshView mesh_view = meshes.view();

    bake::BakeScene bake_scene;

    // Lights: the RT FrameLight set maps onto bake point lights. FrameLight
    // intensity/color carry through; range is advisory for RT and unused by
    // the direct-light bake kernel.
    bake_scene.lights.reserve(lights.size());
    for (const rrt::FrameLight& light : lights) {
        bake::BakeLight bl{};
        bl.kind = bake::LightKind::kPoint;
        bl.position = light.position;
        bl.color = {light.r, light.g, light.b};
        bl.intensity = light.intensity;
        bake_scene.lights.push_back(bl);
    }

    // Triangles: world-space geometry from each bake-eligible static renderable.
    const ::psynder::render::MaterialLibrary& material_lib = scene.materials();
    const ::psynder::render::MaterialView material_view = material_lib.view();
    std::vector<BakeTri> local_tris;
    for (const u32 item_index : queues.bake_static) {
        const ::psynder::scene::SceneRenderItem& item = queues.item(item_index);
        if (item.geometry != ::psynder::scene::GeometryKind::Mesh)
            continue;
        u32 mesh_slot = 0;
        if (!meshes.slot(::psynder::render::mesh_id_from_raw(item.geometry_id), mesh_slot))
            continue;
        gather_mesh_bake_triangles(mesh_view, mesh_slot, local_tris);
        if (local_tris.empty())
            continue;

        // Per-renderable albedo decoded from the material's RGBA8 (treated as
        // linear here; good enough for a plausibility check). Falls back to the
        // option default when the material is unresolved.
        math::Vec3 albedo = options.default_albedo;
        u32 material_slot = 0;
        if (material_lib.slot(item.material, material_slot)) {
            const u32 rgba = material_view.albedo_rgba8[material_slot];
            albedo = {static_cast<f32>(rgba & 0xFFu) / 255.0f,
                      static_cast<f32>((rgba >> 8) & 0xFFu) / 255.0f,
                      static_cast<f32>((rgba >> 16) & 0xFFu) / 255.0f};
        }

        for (const BakeTri& lt : local_tris) {
            bake::BakeTriangle bt{};
            bt.v0 = transform_point(item.world, lt.v0);
            bt.v1 = transform_point(item.world, lt.v1);
            bt.v2 = transform_point(item.world, lt.v2);

            // The bake kernel derives its lighting normal from triangle
            // winding (Bake.cpp::build_basis), so orient the emitted winding
            // so the geometric normal agrees with the authored shading normal
            // transformed to world space. Without this, a downward-wound floor
            // would self-shadow against an overhead light.
            const math::Vec3 geo_n =
                math::cross(math::sub(bt.v1, bt.v0), math::sub(bt.v2, bt.v0));
            const math::Vec3 world_shading_n =
                math::normalize(math::sub(transform_point(item.world, lt.shading_normal),
                                          transform_point(item.world, math::Vec3{0, 0, 0})));
            if (math::dot(geo_n, world_shading_n) < 0.0f) {
                std::swap(bt.v1, bt.v2);
            }
            bt.normal =
                math::normalize(math::cross(math::sub(bt.v1, bt.v0), math::sub(bt.v2, bt.v0)));
            bt.albedo = albedo;
            bt.material_flags = bake::BakeMaterial_DefaultFlags;
            bake_scene.triangles.push_back(bt);
        }
    }

    if (bake_scene.triangles.empty())
        return result;

    bake::BakeOptions opt{};
    opt.lightmap_resolution = std::max<u32>(1u, options.lightmap_resolution);
    opt.max_indirect_bounces = options.max_indirect_bounces;
    opt.indirect_samples_per_bounce = std::max<u32>(1u, options.indirect_samples_per_bounce);

    result.atlas = bake::bake(bake_scene, opt);
    bake::write_lmlight(result.atlas, result.lmlight);

    f32 max_luma = 0.0f;
    for (const bake::BakedSurface& surf : result.atlas.surfaces) {
        for (usize px = 0; px + 2u < surf.pixels.size(); px += 3u) {
            const f32 luma = 0.2126f * surf.pixels[px + 0u] + 0.7152f * surf.pixels[px + 1u] +
                             0.0722f * surf.pixels[px + 2u];
            max_luma = std::max(max_luma, luma);
        }
    }

    result.ok = true;
    result.triangle_count = static_cast<u32>(bake_scene.triangles.size());
    result.light_count = static_cast<u32>(lights.size());
    result.surface_count = static_cast<u32>(result.atlas.surfaces.size());
    result.max_luminance = max_luma;
    return result;
}

}  // namespace

SceneRtStats render_scene_rt(::psynder::scene::Scene& scene,
                             const ::psynder::scene::SceneCameraView& view,
                             ::psynder::render::RenderingSystem& renderer,
                             ::psynder::render::Framebuffer& target,
                             const SceneRtOptions& options) {
    // `scene` is mutable (the host owns it so), so no const_cast: the gather
    // path refreshes world transforms and runs ECS queries directly.
    std::lock_guard<std::mutex> lock(rt_cache_mutex());
    RtCache& cache = rt_cache();
    gather_scene_frame_lights(scene, cache.lights);  // fills the reused light buffer
    return render_scene_rt_locked(cache, scene, view, renderer, cache.lights, target, options);
}

SceneRtStats render_scene_rt(::psynder::scene::Scene& scene,
                             const ::psynder::scene::SceneCameraView& view,
                             ::psynder::render::RenderingSystem& renderer,
                             std::span<const rrt::FrameLight> lights,
                             ::psynder::render::Framebuffer& target,
                             const SceneRtOptions& options) {
    std::lock_guard<std::mutex> lock(rt_cache_mutex());
    RtCache& cache = rt_cache();
    return render_scene_rt_locked(cache, scene, view, renderer, lights, target, options);
}

BakeResult bake_lightmaps(::psynder::scene::Scene& scene,
                          ::psynder::render::RenderingSystem& renderer,
                          const BakeOptions& options) {
    std::vector<rrt::FrameLight> lights;
    gather_scene_frame_lights(scene, lights);
    return bake_lightmaps_impl(scene, renderer, lights, options);
}

BakeResult bake_lightmaps(::psynder::scene::Scene& scene,
                          ::psynder::render::RenderingSystem& renderer,
                          std::span<const rrt::FrameLight> lights,
                          const BakeOptions& options) {
    return bake_lightmaps_impl(scene, renderer, lights, options);
}

}  // namespace psynder::editor::render
