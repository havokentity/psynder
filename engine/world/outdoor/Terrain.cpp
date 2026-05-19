// SPDX-License-Identifier: MIT
// Psynder — outdoor terrain public-API glue (DESIGN.md §9.2, ADR-008).
//
// The heavy math lives in the internal headers so unit tests can use it
// directly (tests/unit/CMakeLists.txt is owned by the build-system
// maintainer and links a fixed lane set). This TU wires up the public
// `TerrainMesh` / `TerrainRaymarch` / `load_spline_track` symbols.

#include "world/outdoor/Terrain.h"

#include "world/outdoor/CdlodMesh_internal.h"
#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Raymarch_internal.h"
#include "world/outdoor/Scatter_internal.h"
#include "world/outdoor/Spline_internal.h"

#include "asset/Vfs.h"
#include "core/Log.h"
#include "render/raster/Raster.h"

#include <cstring>

namespace psynder::world::outdoor {

namespace {

// Cached per-instance state. The class layouts in the public header are
// (intentionally) opaque — we keep state in this TU-local map keyed by
// `this` so we don't have to widen the public header (frozen Wave A).
//
// Wave A only stores the heightmap pointer + the precomputed chunk set;
// Wave B will plug in inner-frame CDLOD selection and a real DrawItem
// emit path against lane 07's queue.
struct MeshState {
    HeightmapDesc                    desc{};
    std::vector<detail::CdlodChunk>  chunks;
    bool                             built = false;
};

struct RaymarchState {
    HeightmapDesc desc{};
};

// We don't have a global ECS handle for the terrain object yet, so use
// `this`-keyed slot maps. They're touched once at load and once at draw —
// not on a hot path.
//
// NOTE: For Wave A we keep these as simple TU-local statics. Wave B will
// migrate to a properly-allocated arena slot tied to the scene root entity.
struct Slot {
    const void* key = nullptr;
    MeshState   mesh;
};
struct RaySlot {
    const void*   key = nullptr;
    RaymarchState ray;
};

constexpr usize kMaxTerrains = 16;
Slot     g_mesh_slots[kMaxTerrains];
RaySlot  g_ray_slots[kMaxTerrains];

MeshState* mesh_state_for(const TerrainMesh* tm) noexcept {
    for (auto& s : g_mesh_slots) {
        if (s.key == tm) return &s.mesh;
    }
    for (auto& s : g_mesh_slots) {
        if (s.key == nullptr) {
            s.key  = tm;
            s.mesh = MeshState{};
            return &s.mesh;
        }
    }
    return nullptr;
}

RaymarchState* ray_state_for(const TerrainRaymarch* tr) noexcept {
    for (auto& s : g_ray_slots) {
        if (s.key == tr) return &s.ray;
    }
    for (auto& s : g_ray_slots) {
        if (s.key == nullptr) {
            s.key = tr;
            s.ray = RaymarchState{};
            return &s.ray;
        }
    }
    return nullptr;
}

}  // namespace

// ─── TerrainMesh ─────────────────────────────────────────────────────────
void TerrainMesh::build(const HeightmapDesc& desc) {
    MeshState* st = mesh_state_for(this);
    if (!st) {
        PSY_LOG_WARN("world_outdoor: TerrainMesh slot table full");
        return;
    }
    st->desc   = desc;
    st->chunks = detail::build_all_chunks(desc);
    st->built  = true;
}

void TerrainMesh::render_cdlod(const math::Mat4& /*view*/,
                               const math::Mat4& /*proj*/) const {
    const MeshState* st = nullptr;
    for (auto& s : g_mesh_slots) {
        if (s.key == this) { st = &s.mesh; break; }
    }
    if (!st || !st->built) return;

    // Emit each leaf chunk into lane 07's submit queue. Wave A's chunk
    // morph is identity (no inter-level slope yet); the watertight
    // invariant comes from sharing integer texel positions between chunks.
    auto& rast = render::raster::Rasterizer::Get();
    for (const auto& c : st->chunks) {
        if (c.vertices.empty() || c.indices.empty()) continue;
        render::raster::DrawItem item{};
        item.vertices     = c.vertices.data();
        item.vertex_count = static_cast<u32>(c.vertices.size());
        item.indices      = c.indices.data();
        item.index_count  = static_cast<u32>(c.indices.size());
        item.model        = math::identity4();
        item.flags        = 0;
        rast.submit(item);
    }
}

// ─── TerrainRaymarch ─────────────────────────────────────────────────────
void TerrainRaymarch::set_heightmap(const HeightmapDesc& desc) {
    RaymarchState* st = ray_state_for(this);
    if (!st) {
        PSY_LOG_WARN("world_outdoor: TerrainRaymarch slot table full");
        return;
    }
    st->desc = desc;
}

void TerrainRaymarch::render(const math::Mat4& /*view*/,
                             const math::Mat4& /*proj*/) const {
    // Wave A ships the per-column raymarch as a callable kernel (see
    // Raymarch_internal.h). The framebuffer-bound integration goes through
    // lane 07's tile job system (a per-tile rectangle of columns is
    // dispatched). The scalar reference in Raymarch_internal::step_column
    // is the load-bearing piece pinned by the unit test.
    //
    // We intentionally do not synthesize a framebuffer here; the per-frame
    // render-tile entry point is lane 07's responsibility once the tile
    // bin queue carries a "terrain raymarch" item type. Issue filed in
    // the PR body if the orchestrator wants that DrawItem variant added.
    const RaymarchState* st = nullptr;
    for (auto& s : g_ray_slots) {
        if (s.key == this) { st = &s.ray; break; }
    }
    if (!st || !st->desc.heights) return;
}

// ─── Spline track loader ─────────────────────────────────────────────────
// Wave-A loader: reads a packed flat array of f32 control points + width +
// banking from the VFS. The file format is intentionally trivial for
// Wave A; the editor (lane 18) will replace this with the cooked .psylevel
// stream in Wave B.
//
// Binary layout (little-endian):
//   u32 magic = 'P','S','T','K' (PSynder Track)
//   u32 segment_count
//   for each segment:
//     f32 p0.x p0.y p0.z   p1.x p1.y p1.z
//     f32 p2.x p2.y p2.z   p3.x p3.y p3.z
//     f32 half_width
//     f32 banking_rad
//
// On any read error we leave the output vector untouched. This means the
// sample tracks bundled with the engine can ship inline (see Wave B).
void load_spline_track(std::string_view virtual_path,
                       std::vector<SplineRoadSegment>& segments_out) {
    auto& vfs  = asset::Vfs::Get();
    auto  blob = vfs.read(virtual_path);
    if (!blob.data || blob.bytes < 8) return;

    auto read_u32 = [](const u8* p) noexcept {
        u32 v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };
    auto read_f32 = [](const u8* p) noexcept {
        f32 v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };

    const u32 magic = read_u32(blob.data);
    if (magic != 0x4B545350u) return;       // 'PSTK' (little-endian)
    const u32 nseg  = read_u32(blob.data + 4);

    constexpr usize kBytesPerSeg = 14u * sizeof(f32);
    if (blob.bytes < 8u + static_cast<usize>(nseg) * kBytesPerSeg) return;

    segments_out.reserve(segments_out.size() + nseg);
    const u8* p = blob.data + 8;
    for (u32 i = 0; i < nseg; ++i) {
        SplineRoadSegment seg{};
        seg.p0          = math::Vec3{ read_f32(p+0),  read_f32(p+4),  read_f32(p+8)  };
        seg.p1          = math::Vec3{ read_f32(p+12), read_f32(p+16), read_f32(p+20) };
        seg.p2          = math::Vec3{ read_f32(p+24), read_f32(p+28), read_f32(p+32) };
        seg.p3          = math::Vec3{ read_f32(p+36), read_f32(p+40), read_f32(p+44) };
        seg.half_width  = read_f32(p+48);
        seg.banking_rad = read_f32(p+52);
        segments_out.push_back(seg);
        p += kBytesPerSeg;
    }
}

}  // namespace psynder::world::outdoor
