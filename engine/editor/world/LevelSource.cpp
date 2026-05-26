// SPDX-License-Identifier: MIT
// Psynder — editor "level source" loaders (impl). Lane 18 (editor) owns.

#include "editor/world/LevelSource.h"

#include "core/HashHelpers.h"
#include "core/Log.h"
#include "math/Bounds.h"
#include "world/bsp/BspFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace psynder::editor::world {

namespace {

using ::psynder::render::Vertex;

constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255u) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

// Bake a single fixed directional light into a vertex colour so adjacent
// same-coloured surfaces read as distinct (form / depth) under the flat-shaded
// editor view. Inlined here so the editor lib never depends on samples/common.
u32 shade_vertex(u32 base_rgba, math::Vec3 normal) noexcept {
    const math::Vec3 light_dir = math::normalize(math::Vec3{0.35f, 0.85f, 0.40f});
    const f32 n_dot_l = std::max(0.0f, math::dot(math::normalize(normal), light_dir));
    const f32 ambient = 0.35f;
    const f32 k = std::clamp(ambient + (1.0f - ambient) * n_dot_l, 0.0f, 1.0f);
    const u32 r = base_rgba & 0xFFu;
    const u32 g = (base_rgba >> 8) & 0xFFu;
    const u32 b = (base_rgba >> 16) & 0xFFu;
    const u32 a = (base_rgba >> 24) & 0xFFu;
    const auto scale = [k](u32 c) noexcept -> u32 {
        return static_cast<u32>(std::clamp(static_cast<f32>(c) * k, 0.0f, 255.0f));
    };
    return scale(r) | (scale(g) << 8) | (scale(b) << 16) | (a << 24);
}

math::Aabb vertices_bounds(std::span<const Vertex> verts) noexcept {
    math::Aabb b = math::aabb_empty();
    for (const Vertex& v : verts)
        b = math::aabb_union(b, v.position);
    return b;
}

// Finalize one pool slice into a LevelMesh + create the scene entity.
Entity emit_renderable(scene::Scene& scene,
                       LevelGeometry& geometry,
                       LevelGeometry::Pool& pool,
                       u32 first_vertex,
                       u32 first_index,
                       ::psynder::render::MaterialId material,
                       ::psynder::render::raster::CullMode cull,
                       scene::ObjectMobility mobility,
                       scene::SceneNode parent,
                       std::string_view name,
                       LevelMesh& out_mesh) {
    const u32 vertex_count = static_cast<u32>(pool.vertices.size()) - first_vertex;
    const u32 index_count = static_cast<u32>(pool.indices.size()) - first_index;
    const std::span<const Vertex> verts{pool.vertices.data() + first_vertex, vertex_count};
    const math::Aabb bounds = vertices_bounds(verts);

    LevelMesh mesh{};
    mesh.geometry_id = geometry.allocate_geometry_id();
    mesh.material = material;
    mesh.local_bounds = bounds;
    mesh.first_vertex = first_vertex;
    mesh.vertex_count = vertex_count;
    mesh.first_index = first_index;
    mesh.index_count = index_count;
    mesh.desc.vertices = pool.vertices.data() + first_vertex;
    mesh.desc.vertex_count = vertex_count;
    mesh.desc.indices = pool.indices.data() + first_index;
    mesh.desc.index_count = index_count;
    mesh.desc.cull = cull;
    mesh.desc.local_bounds = bounds;
    geometry.add_mesh(mesh);
    out_mesh = mesh;

    const scene::RenderableComponent renderable = scene::make_renderable(
        scene::GeometryKind::Mesh, mesh.geometry_id, material, bounds, mobility,
        scene::RenderableFlags::Visible);
    const Entity entity = scene.create_renderable(renderable, {}, parent);
    if (entity.valid() && !name.empty())
        scene.set_entity_name(entity, name);
    return entity;
}

}  // namespace

// ─── BSP loader ────────────────────────────────────────────────────────────

LoadResult load_bsp_into_scene(scene::Scene& scene,
                               const BspLevelSource& source,
                               LevelGeometry& geometry,
                               const LoadBspOptions& options,
                               std::vector<Entity>* out_entities) {
    LoadResult result{};
    const auto& map = source.map;

    if (source.face_index_offset.size() != map.faces.size() ||
        source.face_index_count.size() != map.faces.size()) {
        PSY_LOG_WARN("editor::world: BSP level source face/index arrays inconsistent ({} faces, "
                     "{} offsets, {} counts); aborting",
                     map.faces.size(),
                     source.face_index_offset.size(),
                     source.face_index_count.size());
        return result;
    }

    // One material per distinct BspFace::material id. The original face colour
    // is baked into the vertices, so the material just carries the editable
    // albedo + raster/RT visibility flags.
    std::vector<u32> material_keys{};
    std::vector<::psynder::render::MaterialId> material_ids{};
    const auto material_for = [&](u32 key) -> ::psynder::render::MaterialId {
        for (usize i = 0; i < material_keys.size(); ++i) {
            if (material_keys[i] == key)
                return material_ids[i];
        }
        ::psynder::render::MaterialDesc desc{};
        desc.albedo_rgba8 = 0xFFFFFFFFu;
        desc.winding = ::psynder::render::MaterialWinding::Ccw;
        const ::psynder::render::MaterialId id = scene.materials().create(desc);
        material_keys.push_back(key);
        material_ids.push_back(id);
        ++result.materials_created;
        return id;
    };

    // Copy a face's fan into `pool`, re-baking the shade and re-indexing local
    // to the pool. Returns the number of indices appended.
    const auto append_face = [&](usize face_index, LevelGeometry::Pool& pool) {
        const auto& face = map.faces[face_index];
        const u32 idx_off = source.face_index_offset[face_index];
        const u32 idx_cnt = source.face_index_count[face_index];
        if (idx_cnt == 0u)
            return;
        const u32 base = static_cast<u32>(pool.vertices.size());
        // Bring in the unique vertices this fan references, remapping indices.
        // Faces store small fans (quads); a linear remap table is fine.
        std::vector<u32> remap{};
        for (u32 k = 0; k < idx_cnt; ++k) {
            const usize src_index_pos = static_cast<usize>(idx_off) + k;
            if (src_index_pos >= source.indices.size())
                continue;
            const u32 src_vertex = source.indices[src_index_pos];
            // Find or add in this face's local remap.
            u32 local = 0xFFFFFFFFu;
            for (u32 r = 0; r < static_cast<u32>(remap.size()); ++r) {
                if (remap[r] == src_vertex) {
                    local = r;
                    break;
                }
            }
            if (local == 0xFFFFFFFFu) {
                local = static_cast<u32>(remap.size());
                remap.push_back(src_vertex);
                Vertex v = src_vertex < source.vertices.size() ? source.vertices[src_vertex]
                                                               : Vertex{};
                v.color = shade_vertex(v.color, v.normal);
                pool.vertices.push_back(v);
            }
            pool.indices.push_back(base + local);
        }
        (void)face;
    };

    const auto leaf_name = [&](usize leaf) {
        std::string name{options.name_prefix};
        name += " Leaf ";
        name += std::to_string(leaf);
        return name;
    };

    if (options.one_renderable_per_leaf) {
        for (usize li = 0; li < map.leaves.size(); ++li) {
            const auto& leaf = map.leaves[li];
            if (leaf.face_count == 0u)
                continue;
            LevelGeometry::Pool& pool = geometry.new_pool();
            const usize face_lo = leaf.first_face;
            const usize face_hi =
                std::min<usize>(face_lo + leaf.face_count, map.faces.size());
            for (usize fi = face_lo; fi < face_hi; ++fi)
                append_face(fi, pool);
            if (pool.indices.empty())
                continue;

            // One material per leaf: take the leaf's first non-empty face's
            // material as representative (faces in a leaf share a material in
            // the demo; mixed-material leaves still render with one batch).
            u32 mat_key = 0u;
            for (usize fi = face_lo; fi < face_hi; ++fi) {
                if (source.face_index_count[fi] != 0u) {
                    mat_key = map.faces[fi].material;
                    break;
                }
            }
            const ::psynder::render::MaterialId material = material_for(mat_key);
            LevelMesh mesh{};
            const Entity entity = emit_renderable(scene, geometry, pool, 0u, 0u, material,
                                                  options.cull, options.mobility, options.parent,
                                                  leaf_name(li), mesh);
            if (entity.valid()) {
                ++result.entities_created;
                ++result.meshes_created;
                result.bounds = math::aabb_union(result.bounds, mesh.local_bounds);
                if (out_entities)
                    out_entities->push_back(entity);
            }
        }
    } else {
        // Merge every leaf's faces into a single pool / renderable.
        LevelGeometry::Pool& pool = geometry.new_pool();
        u32 mat_key = 0u;
        bool have_key = false;
        for (const auto& leaf : map.leaves) {
            const usize face_lo = leaf.first_face;
            const usize face_hi =
                std::min<usize>(face_lo + leaf.face_count, map.faces.size());
            for (usize fi = face_lo; fi < face_hi; ++fi) {
                if (!have_key && source.face_index_count[fi] != 0u) {
                    mat_key = map.faces[fi].material;
                    have_key = true;
                }
                append_face(fi, pool);
            }
        }
        if (!pool.indices.empty()) {
            const ::psynder::render::MaterialId material = material_for(mat_key);
            std::string name{options.name_prefix};
            name += " Map";
            LevelMesh mesh{};
            const Entity entity =
                emit_renderable(scene, geometry, pool, 0u, 0u, material, options.cull,
                                options.mobility, options.parent, name, mesh);
            if (entity.valid()) {
                ++result.entities_created;
                ++result.meshes_created;
                result.bounds = math::aabb_union(result.bounds, mesh.local_bounds);
                if (out_entities)
                    out_entities->push_back(entity);
            }
        }
    }

    return result;
}

// ─── Terrain loader ──────────────────────────────────────────────────────

LoadResult load_terrain_into_scene(scene::Scene& scene,
                                   const ::psynder::world::outdoor::HeightmapDesc& desc,
                                   LevelGeometry& geometry,
                                   const LoadTerrainOptions& options,
                                   std::vector<Entity>* out_entities) {
    LoadResult result{};
    if (desc.heights == nullptr || desc.size_x < 2u || desc.size_z < 2u ||
        !(desc.spacing > 0.0f)) {
        PSY_LOG_WARN("editor::world: terrain heightfield invalid (size {}x{}, spacing {}); "
                     "aborting",
                     desc.size_x,
                     desc.size_z,
                     desc.spacing);
        return result;
    }

    const u32 nx = desc.size_x;
    const u32 nz = desc.size_z;
    const f32 spacing = desc.spacing;
    const f32 hscale = desc.height_scale;

    const auto height_at = [&](u32 x, u32 z) noexcept -> f32 {
        const usize i = static_cast<usize>(z) * nx + x;
        return static_cast<f32>(desc.heights[i]) * hscale;
    };

    LevelGeometry::Pool& pool = geometry.new_pool();
    pool.vertices.reserve(static_cast<usize>(nx) * nz);
    pool.indices.reserve(static_cast<usize>(nx - 1u) * (nz - 1u) * 6u);

    // Vertex grid. Position in world space; normal from central differences so
    // the flat editor view reads the relief. UV stretches the map [0,1]^2.
    for (u32 z = 0; z < nz; ++z) {
        for (u32 x = 0; x < nx; ++x) {
            const f32 wx = static_cast<f32>(x) * spacing;
            const f32 wz = static_cast<f32>(z) * spacing;
            const f32 wy = height_at(x, z);

            const u32 xm = x > 0u ? x - 1u : x;
            const u32 xp = x + 1u < nx ? x + 1u : x;
            const u32 zm = z > 0u ? z - 1u : z;
            const u32 zp = z + 1u < nz ? z + 1u : z;
            const f32 dhx = height_at(xp, z) - height_at(xm, z);
            const f32 dhz = height_at(x, zp) - height_at(x, zm);
            const f32 dx = static_cast<f32>(xp - xm) * spacing;
            const f32 dz = static_cast<f32>(zp - zm) * spacing;
            math::Vec3 normal = math::normalize(math::Vec3{
                dx > 0.0f ? -dhx / dx : 0.0f, 1.0f, dz > 0.0f ? -dhz / dz : 0.0f});

            Vertex v{};
            v.position = {wx, wy, wz};
            v.normal = normal;
            v.uv = {static_cast<f32>(x) / static_cast<f32>(nx - 1u),
                    static_cast<f32>(z) / static_cast<f32>(nz - 1u)};
            v.lightmap_uv = {0.0f, 0.0f};
            v.color = shade_vertex(options.albedo_rgba8, normal);
            pool.vertices.push_back(v);
        }
    }

    // Two CCW (viewed from +Y) triangles per grid cell.
    for (u32 z = 0; z + 1u < nz; ++z) {
        for (u32 x = 0; x + 1u < nx; ++x) {
            const u32 i00 = z * nx + x;
            const u32 i10 = z * nx + (x + 1u);
            const u32 i01 = (z + 1u) * nx + x;
            const u32 i11 = (z + 1u) * nx + (x + 1u);
            pool.indices.push_back(i00);
            pool.indices.push_back(i01);
            pool.indices.push_back(i11);
            pool.indices.push_back(i00);
            pool.indices.push_back(i11);
            pool.indices.push_back(i10);
        }
    }

    ::psynder::render::MaterialDesc mat_desc{};
    mat_desc.albedo_rgba8 = options.albedo_rgba8;
    mat_desc.roughness = 0.9f;
    const ::psynder::render::MaterialId material = scene.materials().create(mat_desc);
    ++result.materials_created;

    LevelMesh mesh{};
    const Entity entity = emit_renderable(scene, geometry, pool, 0u, 0u, material, options.cull,
                                          options.mobility, options.parent, options.name, mesh);
    if (entity.valid()) {
        ++result.entities_created;
        ++result.meshes_created;
        result.bounds = mesh.local_bounds;
        if (out_entities)
            out_entities->push_back(entity);
    }
    return result;
}

// ─── Demo BSP fixture (port of samples/03_quake_room) ────────────────────

namespace {

void emit_demo_quad(BspLevelSource& w,
                    math::Vec3 a,
                    math::Vec3 b,
                    math::Vec3 c,
                    math::Vec3 d,
                    math::Vec3 normal,
                    u32 color,
                    u32 material_id) {
    const u32 base = static_cast<u32>(w.vertices.size());
    const auto push = [&](math::Vec3 p, math::Vec2 uv) {
        Vertex v{};
        v.position = p;
        v.normal = normal;
        v.uv = uv;
        v.lightmap_uv = {0.0f, 0.0f};
        v.color = color;
        w.vertices.push_back(v);
        w.bounds = math::aabb_union(w.bounds, p);
    };
    push(a, {0, 0});
    push(b, {1, 0});
    push(c, {1, 1});
    push(d, {0, 1});

    const u32 idx_base = static_cast<u32>(w.indices.size());
    w.indices.push_back(base + 0u);
    w.indices.push_back(base + 1u);
    w.indices.push_back(base + 2u);
    w.indices.push_back(base + 0u);
    w.indices.push_back(base + 2u);
    w.indices.push_back(base + 3u);

    ::psynder::world::bsp::BspFace face{};
    face.first_vertex = base;
    face.vertex_count = 4u;
    face.material = material_id;
    face.lightmap = 0xFFFFFFFFu;
    w.map.faces.push_back(face);
    w.face_index_offset.push_back(idx_base);
    w.face_index_count.push_back(6u);
}

}  // namespace

void build_demo_bsp_level(BspLevelSource& w) {
    w = BspLevelSource{};
    w.bounds = math::aabb_empty();

    constexpr f32 kFloorY = 0.0f;
    constexpr f32 kCeilY = 3.0f;
    constexpr f32 kRoomAZ0 = -8.0f;
    constexpr f32 kRoomAZ1 = -2.0f;
    constexpr f32 kDoorZ0 = -2.0f;
    constexpr f32 kDoorZ1 = 0.0f;
    constexpr f32 kRoomBZ0 = 0.0f;
    constexpr f32 kRoomBZ1 = 6.0f;
    constexpr f32 kRoomX0 = -4.0f;
    constexpr f32 kRoomX1 = 4.0f;
    constexpr f32 kDoorX0 = -1.0f;
    constexpr f32 kDoorX1 = 1.0f;

    constexpr u32 kMatFloor = 1u;
    constexpr u32 kMatCeil = 2u;
    constexpr u32 kMatWallA = 3u;
    constexpr u32 kMatWallB = 4u;
    constexpr u32 kMatDoor = 5u;

    const u32 kColRoomAFloor = pack_rgba(110, 140, 180);
    const u32 kColRoomACeil = pack_rgba(70, 90, 130);
    const u32 kColRoomAWall = pack_rgba(150, 170, 200);
    const u32 kColRoomBFloor = pack_rgba(180, 140, 110);
    const u32 kColRoomBCeil = pack_rgba(130, 90, 70);
    const u32 kColRoomBWall = pack_rgba(200, 170, 150);
    const u32 kColDoorFloor = pack_rgba(160, 200, 130);
    const u32 kColDoorCeil = pack_rgba(110, 150, 90);
    const u32 kColDoorWall = pack_rgba(180, 210, 150);

    const u32 leaf0_first = static_cast<u32>(w.map.faces.size());

    // Leaf 0 — Room A.
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomAZ0}, {kRoomX1, kFloorY, kRoomAZ0},
                   {kRoomX1, kFloorY, kRoomAZ1}, {kRoomX0, kFloorY, kRoomAZ1}, {0, 1, 0},
                   kColRoomAFloor, kMatFloor);
    emit_demo_quad(w, {kRoomX0, kCeilY, kRoomAZ1}, {kRoomX1, kCeilY, kRoomAZ1},
                   {kRoomX1, kCeilY, kRoomAZ0}, {kRoomX0, kCeilY, kRoomAZ0}, {0, -1, 0},
                   kColRoomACeil, kMatCeil);
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomAZ1}, {kRoomX0, kCeilY, kRoomAZ1},
                   {kRoomX0, kCeilY, kRoomAZ0}, {kRoomX0, kFloorY, kRoomAZ0}, {1, 0, 0},
                   kColRoomAWall, kMatWallA);
    emit_demo_quad(w, {kRoomX1, kFloorY, kRoomAZ0}, {kRoomX1, kCeilY, kRoomAZ0},
                   {kRoomX1, kCeilY, kRoomAZ1}, {kRoomX1, kFloorY, kRoomAZ1}, {-1, 0, 0},
                   kColRoomAWall, kMatWallA);
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomAZ0}, {kRoomX0, kCeilY, kRoomAZ0},
                   {kRoomX1, kCeilY, kRoomAZ0}, {kRoomX1, kFloorY, kRoomAZ0}, {0, 0, 1},
                   kColRoomAWall, kMatWallA);
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomAZ1}, {kRoomX0, kCeilY, kRoomAZ1},
                   {kDoorX0, kCeilY, kRoomAZ1}, {kDoorX0, kFloorY, kRoomAZ1}, {0, 0, -1},
                   kColRoomAWall, kMatWallA);
    emit_demo_quad(w, {kDoorX1, kFloorY, kRoomAZ1}, {kDoorX1, kCeilY, kRoomAZ1},
                   {kRoomX1, kCeilY, kRoomAZ1}, {kRoomX1, kFloorY, kRoomAZ1}, {0, 0, -1},
                   kColRoomAWall, kMatWallA);

    const u32 leaf1_first = static_cast<u32>(w.map.faces.size());

    // Leaf 1 — Doorway corridor.
    emit_demo_quad(w, {kDoorX0, kFloorY, kDoorZ0}, {kDoorX1, kFloorY, kDoorZ0},
                   {kDoorX1, kFloorY, kDoorZ1}, {kDoorX0, kFloorY, kDoorZ1}, {0, 1, 0},
                   kColDoorFloor, kMatDoor);
    emit_demo_quad(w, {kDoorX0, kCeilY, kDoorZ1}, {kDoorX1, kCeilY, kDoorZ1},
                   {kDoorX1, kCeilY, kDoorZ0}, {kDoorX0, kCeilY, kDoorZ0}, {0, -1, 0},
                   kColDoorCeil, kMatDoor);
    emit_demo_quad(w, {kDoorX0, kFloorY, kDoorZ1}, {kDoorX0, kCeilY, kDoorZ1},
                   {kDoorX0, kCeilY, kDoorZ0}, {kDoorX0, kFloorY, kDoorZ0}, {1, 0, 0},
                   kColDoorWall, kMatDoor);
    emit_demo_quad(w, {kDoorX1, kFloorY, kDoorZ0}, {kDoorX1, kCeilY, kDoorZ0},
                   {kDoorX1, kCeilY, kDoorZ1}, {kDoorX1, kFloorY, kDoorZ1}, {-1, 0, 0},
                   kColDoorWall, kMatDoor);

    const u32 leaf2_first = static_cast<u32>(w.map.faces.size());

    // Leaf 2 — Room B.
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomBZ0}, {kRoomX1, kFloorY, kRoomBZ0},
                   {kRoomX1, kFloorY, kRoomBZ1}, {kRoomX0, kFloorY, kRoomBZ1}, {0, 1, 0},
                   kColRoomBFloor, kMatFloor);
    emit_demo_quad(w, {kRoomX0, kCeilY, kRoomBZ1}, {kRoomX1, kCeilY, kRoomBZ1},
                   {kRoomX1, kCeilY, kRoomBZ0}, {kRoomX0, kCeilY, kRoomBZ0}, {0, -1, 0},
                   kColRoomBCeil, kMatCeil);
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomBZ1}, {kRoomX0, kCeilY, kRoomBZ1},
                   {kRoomX0, kCeilY, kRoomBZ0}, {kRoomX0, kFloorY, kRoomBZ0}, {1, 0, 0},
                   kColRoomBWall, kMatWallB);
    emit_demo_quad(w, {kRoomX1, kFloorY, kRoomBZ0}, {kRoomX1, kCeilY, kRoomBZ0},
                   {kRoomX1, kCeilY, kRoomBZ1}, {kRoomX1, kFloorY, kRoomBZ1}, {-1, 0, 0},
                   kColRoomBWall, kMatWallB);
    emit_demo_quad(w, {kRoomX1, kFloorY, kRoomBZ1}, {kRoomX1, kCeilY, kRoomBZ1},
                   {kRoomX0, kCeilY, kRoomBZ1}, {kRoomX0, kFloorY, kRoomBZ1}, {0, 0, -1},
                   kColRoomBWall, kMatWallB);
    emit_demo_quad(w, {kRoomX0, kFloorY, kRoomBZ0}, {kRoomX0, kCeilY, kRoomBZ0},
                   {kDoorX0, kCeilY, kRoomBZ0}, {kDoorX0, kFloorY, kRoomBZ0}, {0, 0, 1},
                   kColRoomBWall, kMatWallB);
    emit_demo_quad(w, {kDoorX1, kFloorY, kRoomBZ0}, {kDoorX1, kCeilY, kRoomBZ0},
                   {kRoomX1, kCeilY, kRoomBZ0}, {kRoomX1, kFloorY, kRoomBZ0}, {0, 0, 1},
                   kColRoomBWall, kMatWallB);

    const u32 leaf_face_end = static_cast<u32>(w.map.faces.size());

    // Leaves.
    w.map.leaves.resize(4);
    w.map.leaves[0].cluster = 0;
    w.map.leaves[0].first_face = leaf0_first;
    w.map.leaves[0].face_count = leaf1_first - leaf0_first;
    w.map.leaves[0].bounds.min = {kRoomX0, kFloorY, kRoomAZ0};
    w.map.leaves[0].bounds.max = {kRoomX1, kCeilY, kRoomAZ1};

    w.map.leaves[1].cluster = 1;
    w.map.leaves[1].first_face = leaf1_first;
    w.map.leaves[1].face_count = leaf2_first - leaf1_first;
    w.map.leaves[1].bounds.min = {kDoorX0, kFloorY, kDoorZ0};
    w.map.leaves[1].bounds.max = {kDoorX1, kCeilY, kDoorZ1};

    w.map.leaves[2].cluster = 2;
    w.map.leaves[2].first_face = leaf2_first;
    w.map.leaves[2].face_count = leaf_face_end - leaf2_first;
    w.map.leaves[2].bounds.min = {kRoomX0, kFloorY, kRoomBZ0};
    w.map.leaves[2].bounds.max = {kRoomX1, kCeilY, kRoomBZ1};

    w.map.leaves[3].cluster = ::psynder::world::bsp::kBspSolidCluster;
    w.map.leaves[3].first_face = 0u;
    w.map.leaves[3].face_count = 0u;
    w.map.leaves[3].bounds.min = {0, 0, 0};
    w.map.leaves[3].bounds.max = {0, 0, 0};

    // BSP node tree (splits on Z then X for the doorway).
    w.map.nodes.resize(4);
    w.map.nodes[0].plane_normal = {0, 0, 1};
    w.map.nodes[0].plane_d = -2.0f;
    w.map.nodes[0].front_child = 1;
    w.map.nodes[0].back_child = ::psynder::world::bsp::bsp_encode_leaf(0);
    w.map.nodes[1].plane_normal = {0, 0, 1};
    w.map.nodes[1].plane_d = 0.0f;
    w.map.nodes[1].front_child = ::psynder::world::bsp::bsp_encode_leaf(2);
    w.map.nodes[1].back_child = 2;
    w.map.nodes[2].plane_normal = {1, 0, 0};
    w.map.nodes[2].plane_d = -1.0f;
    w.map.nodes[2].front_child = 3;
    w.map.nodes[2].back_child = ::psynder::world::bsp::bsp_encode_leaf(3);
    w.map.nodes[3].plane_normal = {1, 0, 0};
    w.map.nodes[3].plane_d = 1.0f;
    w.map.nodes[3].front_child = ::psynder::world::bsp::bsp_encode_leaf(3);
    w.map.nodes[3].back_child = ::psynder::world::bsp::bsp_encode_leaf(1);

    // PVS — 3 clusters, all mutually visible (single byte per row).
    constexpr u32 kClusters = 3u;
    w.map.pvs.assign(kClusters, 0u);
    w.map.pvs[0] = 0b0000'0111u;
    w.map.pvs[1] = 0b0000'0111u;
    w.map.pvs[2] = 0b0000'0111u;

    w.floor_y = kFloorY;
}

// ─── Demo heightmap fixture (port of samples/06_tactical_map) ────────────

namespace {

PSY_FORCEINLINE f32 fade5(f32 t) noexcept {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

PSY_FORCEINLINE f32 value_noise(f32 wx, f32 wz, f32 cell, u32 seed) noexcept {
    const f32 fx = wx / cell;
    const f32 fz = wz / cell;
    const i32 ix = static_cast<i32>(std::floor(fx));
    const i32 iz = static_cast<i32>(std::floor(fz));
    const f32 tx = fade5(fx - static_cast<f32>(ix));
    const f32 tz = fade5(fz - static_cast<f32>(iz));
    const f32 h00 = hash_helpers::murmur_mix2_unit32(static_cast<u32>(ix), static_cast<u32>(iz),
                                                     seed);
    const f32 h10 = hash_helpers::murmur_mix2_unit32(static_cast<u32>(ix + 1),
                                                     static_cast<u32>(iz), seed);
    const f32 h01 = hash_helpers::murmur_mix2_unit32(static_cast<u32>(ix),
                                                     static_cast<u32>(iz + 1), seed);
    const f32 h11 = hash_helpers::murmur_mix2_unit32(static_cast<u32>(ix + 1),
                                                     static_cast<u32>(iz + 1), seed);
    const f32 a = h00 + (h10 - h00) * tx;
    const f32 b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

}  // namespace

std::vector<u16> build_demo_heightmap(::psynder::world::outdoor::HeightmapDesc& out_desc,
                                      u32 size,
                                      f32 spacing,
                                      f32 height_scale) {
    if (size < 2u)
        size = 2u;
    std::vector<u16> heights(static_cast<usize>(size) * size, 0u);

    const f32 map_m = static_cast<f32>(size) * spacing;
    const f32 cx = map_m * 0.5f;
    const f32 cz = map_m * 0.5f;

    for (u32 z = 0; z < size; ++z) {
        for (u32 x = 0; x < size; ++x) {
            const f32 wx = static_cast<f32>(x) * spacing;
            const f32 wz = static_cast<f32>(z) * spacing;

            f32 n = 0.0f;
            n += 0.55f * value_noise(wx, wz, 64.0f, 1u);
            n += 0.25f * value_noise(wx, wz, 24.0f, 2u);
            n += 0.10f * value_noise(wx, wz, 8.0f, 3u);
            n = n / 0.90f;
            n = std::clamp(n, 0.0f, 1.0f);

            const f32 dz = wz - cz;
            const f32 ridge = std::exp(-(dz * dz) / (2.0f * 22.0f * 22.0f));
            const f32 ridge_mod = 0.55f + 0.45f * value_noise(wx, 0.0f, 32.0f, 7u);

            const f32 bdx = wx - (cx + 60.0f);
            const f32 bdz = wz - (cz + 80.0f);
            const f32 bump = std::exp(-(bdx * bdx + bdz * bdz) / (2.0f * 28.0f * 28.0f));

            const f32 base = 0.18f + 0.42f * n;
            const f32 sum = base + 0.85f * ridge * ridge_mod + 0.35f * bump;
            const f32 hf = sum > 1.0f ? 1.0f : sum;

            heights[static_cast<usize>(z) * size + x] = static_cast<u16>(hf * 65535.0f);
        }
    }

    out_desc = ::psynder::world::outdoor::HeightmapDesc{};
    out_desc.size_x = size;
    out_desc.size_z = size;
    out_desc.spacing = spacing;
    out_desc.height_scale = height_scale;
    out_desc.heights = heights.data();
    return heights;
}

}  // namespace psynder::editor::world
