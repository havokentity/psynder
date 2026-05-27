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
// address. The BVH must therefore be built only after the Bvh8 lives at its
// final, stable address, and the storage must never relocate its elements. We
// hold MeshBlas in a std::deque (stable element addresses across growth) and
// build in place.
struct MeshBlas {
    rrt::Bvh8 bvh{};
    std::vector<rrt::Triangle> tris{};
};

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

SceneRtStats render_scene_rt_impl(::psynder::scene::Scene& scene,
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

    // One BLAS per unique mesh slot; instances reference the shared BLAS by
    // per-entity world transform.
    std::unordered_map<u32, MeshBlas*> slot_to_blas;  // mesh slot -> stable BLAS
    std::deque<MeshBlas> blas_storage;                 // stable element addresses
    std::vector<rrt::Tlas::InstanceDesc> instances;
    std::vector<::psynder::render::MaterialId> instance_materials;
    instances.reserve(queues.rt_visible.size());
    instance_materials.reserve(queues.rt_visible.size());

    for (const u32 item_index : queues.rt_visible) {
        const ::psynder::scene::SceneRenderItem& item = queues.item(item_index);
        if (item.geometry != ::psynder::scene::GeometryKind::Mesh)
            continue;  // analytic geometry path not wired here yet (see API gaps)
        u32 mesh_slot = 0;
        if (!meshes.slot(::psynder::render::mesh_id_from_raw(item.geometry_id), mesh_slot))
            continue;

        auto found = slot_to_blas.find(mesh_slot);
        if (found == slot_to_blas.end()) {
            // Construct in place first (stable address), then build the BVH so
            // its address-keyed state stays valid for the lifetime here.
            MeshBlas& blas = blas_storage.emplace_back();
            gather_mesh_triangles(mesh_view, mesh_slot, blas.tris);
            if (blas.tris.empty()) {
                blas_storage.pop_back();
                continue;  // degenerate / empty mesh; skip the instance entirely
            }
            blas.bvh.build(blas.tris.data(), static_cast<u32>(blas.tris.size()));
            found = slot_to_blas.emplace(mesh_slot, &blas).first;
        }

        rrt::Tlas::InstanceDesc desc{};
        desc.blas = &found->second->bvh;
        desc.transform = item.world;
        instances.push_back(desc);
        instance_materials.push_back(item.material);
    }

    if (instances.empty())
        return stats;

    rrt::Tlas tlas;
    tlas.build(instances.data(), static_cast<u32>(instances.size()));

    const f32 aspect = static_cast<f32>(target.width) / static_cast<f32>(target.height);
    rrt::FrameCamera camera = frame_camera_from_view(view, aspect);

    rrt::FrameRenderInput input{};
    input.tlas = &tlas;
    input.camera = camera;
    input.lights = lights.data();
    input.light_count = static_cast<u32>(lights.size());
    input.materials.library = &scene.materials();
    input.materials.instance_materials = instance_materials.data();
    input.materials.instance_material_count = static_cast<u32>(instance_materials.size());

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

    stats.instance_count = static_cast<u32>(instances.size());
    stats.blas_count = static_cast<u32>(blas_storage.size());
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

SceneRtStats render_scene_rt(const ::psynder::scene::Scene& scene,
                             const ::psynder::scene::SceneCameraView& view,
                             ::psynder::render::RenderingSystem& renderer,
                             ::psynder::render::Framebuffer& target,
                             const SceneRtOptions& options) {
    // The gather/query path mutates ECS-internal bookkeeping (and refreshes
    // world transforms), so it needs a mutable Scene even though it does not
    // alter scene topology. The frozen entry point takes a const Scene&; the
    // host owns it mutably, so recovering the mutable reference here is safe.
    ::psynder::scene::Scene& mutable_scene = const_cast<::psynder::scene::Scene&>(scene);
    std::vector<rrt::FrameLight> lights;
    gather_scene_frame_lights(mutable_scene, lights);
    return render_scene_rt_impl(mutable_scene, view, renderer, lights, target, options);
}

SceneRtStats render_scene_rt(const ::psynder::scene::Scene& scene,
                             const ::psynder::scene::SceneCameraView& view,
                             ::psynder::render::RenderingSystem& renderer,
                             std::span<const rrt::FrameLight> lights,
                             ::psynder::render::Framebuffer& target,
                             const SceneRtOptions& options) {
    ::psynder::scene::Scene& mutable_scene = const_cast<::psynder::scene::Scene&>(scene);
    return render_scene_rt_impl(mutable_scene, view, renderer, lights, target, options);
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
