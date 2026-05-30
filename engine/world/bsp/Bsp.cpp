// SPDX-License-Identifier: MIT
// Psynder — BSP loader + PVS leaf walk. Lane 10 owns.
//
// Loader is a header-validated, bounds-checked, byte-pump over a Vfs blob.
// `locate` is the canonical id-engine point-in-tree descent; `walk_visible_leaves`
// finds the eye's cluster, indexes the PVS bit-vector row, and emits every
// other leaf whose cluster bit is set.
//
// The runtime accepts blobs produced by `lm_qbsp` (lane 24) only — we reject
// anything whose magic / version doesn't match exactly.

// NOTE: the public Bsp.h header uses std::vector without including <vector>
// (frozen public header — see AGENTS.md "Public-header contracts"). Every
// internal TU that pulls in Bsp.h therefore pre-includes <vector> here.
#include <vector>

#include "Bsp.h"

#include "BspFormat.h"
#include "BspDraw.h"
#include "Portal.h"

#include "asset/Vfs.h"
#include "core/Log.h"

#include <cstring>

namespace psynder::world::bsp {

namespace {

// ─── Helpers ─────────────────────────────────────────────────────────────

// Bounds-checked memcpy of `count * sizeof(T)` bytes from `blob + chunk.offset`
// into `dst`. Returns false if the chunk runs off the end of the blob.
template <class T>
bool copy_chunk(const u8* blob, u32 blob_bytes, const BspFileChunk& chunk, std::vector<T>& dst) {
    if (chunk.count == 0) {
        dst.clear();
        return true;
    }
    const u64 byte_count = static_cast<u64>(chunk.count) * sizeof(T);
    if (chunk.offset > blob_bytes || byte_count > static_cast<u64>(blob_bytes - chunk.offset)) {
        return false;
    }
    dst.resize(chunk.count);
    std::memcpy(dst.data(), blob + chunk.offset, byte_count);
    return true;
}

// Bounds-checked memcpy of a flat byte chunk (PVS is stored row-packed).
bool copy_byte_chunk(const u8* blob, u32 blob_bytes, const BspFileChunk& chunk, std::vector<u8>& dst) {
    if (chunk.count == 0) {
        dst.clear();
        return true;
    }
    if (chunk.offset > blob_bytes || chunk.count > blob_bytes - chunk.offset) {
        return false;
    }
    dst.resize(chunk.count);
    std::memcpy(dst.data(), blob + chunk.offset, chunk.count);
    return true;
}

// Convert file-record nodes/leaves/faces into the runtime ABI shapes. The on-
// disk format mirrors the runtime fields one-for-one; we still pay the copy
// because the runtime struct can drift from the file struct independently.
void unpack_nodes(const std::vector<BspFileNode>& src, std::vector<BspNode>& dst) {
    dst.resize(src.size());
    for (usize i = 0; i < src.size(); ++i) {
        dst[i].plane_normal = math::Vec3{src[i].nx, src[i].ny, src[i].nz};
        dst[i].plane_d = src[i].d;
        dst[i].front_child = src[i].front_child;
        dst[i].back_child = src[i].back_child;
    }
}

void unpack_leaves(const std::vector<BspFileLeaf>& src, std::vector<BspLeaf>& dst) {
    dst.resize(src.size());
    for (usize i = 0; i < src.size(); ++i) {
        dst[i].cluster = src[i].cluster;
        dst[i].first_face = src[i].first_face;
        dst[i].face_count = src[i].face_count;
        dst[i].bounds.min = math::Vec3{src[i].bbox_min_x, src[i].bbox_min_y, src[i].bbox_min_z};
        dst[i].bounds.max = math::Vec3{src[i].bbox_max_x, src[i].bbox_max_y, src[i].bbox_max_z};
    }
}

void unpack_faces(const std::vector<BspFileFace>& src, std::vector<BspFace>& dst) {
    dst.resize(src.size());
    for (usize i = 0; i < src.size(); ++i) {
        dst[i].first_vertex = src[i].first_vertex;
        dst[i].vertex_count = src[i].vertex_count;
        dst[i].material = src[i].material;
        dst[i].lightmap = src[i].lightmap;
    }
}

// Validate child indices stay within bounds. A negative child encodes a leaf
// reference; the decoded index must be a valid leaf. A non-negative child is a
// node index and must point at a real node. Catches malformed (or maliciously
// crafted) .psybsp blobs before they cause an OOB read in `locate`.
bool validate_topology(const BspMap& map) {
    const i32 node_count = static_cast<i32>(map.nodes.size());
    const i32 leaf_count = static_cast<i32>(map.leaves.size());
    if (node_count == 0 && leaf_count == 0) {
        return true;  // empty map is valid
    }
    if (leaf_count == 0) {
        return false;  // can't have nodes without leaves
    }

    auto check_child = [&](i32 child) {
        if (bsp_is_leaf(child)) {
            const i32 li = bsp_leaf_index(child);
            return li >= 0 && li < leaf_count;
        }
        return child >= 0 && child < node_count;
    };

    for (const BspNode& n : map.nodes) {
        if (!check_child(n.front_child) || !check_child(n.back_child)) {
            return false;
        }
    }

    // PVS table must either be empty (no PVS info) or have exactly one row per
    // non-solid cluster. We accept either; per-leaf cluster ids are validated
    // lazily in walk_visible_leaves.
    return true;
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────────

bool load(std::string_view virtual_path, BspMap& out) {
    out.nodes.clear();
    out.leaves.clear();
    out.faces.clear();
    out.pvs.clear();

    asset::Blob blob = asset::Vfs::Get().read(virtual_path);
    if (blob.data == nullptr || blob.bytes < sizeof(BspFileHeader)) {
        PSY_LOG_ERROR("[bsp] load({}): empty or truncated blob ({} bytes)", virtual_path, blob.bytes);
        return false;
    }

    BspFileHeader header{};
    std::memcpy(&header, blob.data, sizeof(BspFileHeader));

    if (header.magic != kBspFileMagic) {
        PSY_LOG_ERROR("[bsp] load({}): bad magic 0x{:08x}", virtual_path, header.magic);
        return false;
    }
    if (header.version != kBspFileVersion) {
        PSY_LOG_ERROR("[bsp] load({}): version {} != expected {}",
                      virtual_path,
                      header.version,
                      kBspFileVersion);
        return false;
    }
    if (header.total_bytes > blob.bytes) {
        PSY_LOG_ERROR("[bsp] load({}): header claims {} bytes, blob is {}",
                      virtual_path,
                      header.total_bytes,
                      blob.bytes);
        return false;
    }

    const u32 used_bytes = header.total_bytes;

    std::vector<BspFileNode> file_nodes;
    std::vector<BspFileLeaf> file_leaves;
    std::vector<BspFileFace> file_faces;
    if (!copy_chunk(blob.data, used_bytes, header.nodes, file_nodes) ||
        !copy_chunk(blob.data, used_bytes, header.leaves, file_leaves) ||
        !copy_chunk(blob.data, used_bytes, header.faces, file_faces) ||
        !copy_byte_chunk(blob.data, used_bytes, header.pvs, out.pvs)) {
        PSY_LOG_ERROR("[bsp] load({}): chunk offset out of range", virtual_path);
        return false;
    }

    // Cross-check the PVS row size: rows == cluster_count, bytes == cluster_count
    // * pvs_row_bytes. A non-zero cluster count with a row of zero bytes is
    // a degenerate file we won't trust.
    if (header.cluster_count > 0) {
        const u64 expected = static_cast<u64>(header.cluster_count) * header.pvs_row_bytes;
        if (header.pvs_row_bytes == 0 || expected != out.pvs.size()) {
            PSY_LOG_ERROR(
                "[bsp] load({}): PVS row mismatch (clusters={}, "
                "row_bytes={}, total={})",
                virtual_path,
                header.cluster_count,
                header.pvs_row_bytes,
                out.pvs.size());
            return false;
        }
    }

    unpack_nodes(file_nodes, out.nodes);
    unpack_leaves(file_leaves, out.leaves);
    unpack_faces(file_faces, out.faces);

    if (!validate_topology(out)) {
        PSY_LOG_ERROR("[bsp] load({}): topology validation failed", virtual_path);
        out.nodes.clear();
        out.leaves.clear();
        out.faces.clear();
        out.pvs.clear();
        return false;
    }

    PSY_LOG_INFO("[bsp] load({}): nodes={}, leaves={}, faces={}, clusters={}",
                 virtual_path,
                 out.nodes.size(),
                 out.leaves.size(),
                 out.faces.size(),
                 header.cluster_count);
    return true;
}

// Standard id-engine point-in-tree descent. We walk from node 0; at each
// node we evaluate dot(normal, point) - d. Positive (or zero) → front child;
// negative → back child. We stop when a child is a leaf reference.
BspLeaf locate(const BspMap& map, math::Vec3 point) {
    if (map.leaves.empty()) {
        return BspLeaf{};
    }

    // Special case: no internal nodes — the BSP is a single leaf.
    if (map.nodes.empty()) {
        return map.leaves.front();
    }

    i32 node_index = 0;
    // Guard against pathological topologies (cycles produced by a broken
    // compiler) — cap the descent depth at 2 * leaf_count.
    const i32 max_depth = static_cast<i32>(map.leaves.size()) * 2 + 64;
    for (i32 step = 0; step < max_depth; ++step) {
        const BspNode& n = map.nodes[static_cast<usize>(node_index)];
        const f32 d = math::dot(n.plane_normal, point) - n.plane_d;
        const i32 child = (d >= 0.0f) ? n.front_child : n.back_child;
        if (bsp_is_leaf(child)) {
            const i32 li = bsp_leaf_index(child);
            return map.leaves[static_cast<usize>(li)];
        }
        node_index = child;
    }
    // Shouldn't happen with a well-formed tree.
    return map.leaves.front();
}

// PVS-driven visibility walk. Find the eye's leaf, fetch its cluster row, and
// emit every leaf whose cluster bit is set. Edge cases:
//   * Solid leaf (cluster == kBspSolidCluster): no PVS row → emit nothing.
//   * No PVS data at all (clusters == 0): emit all leaves (conservative).
//   * cluster id ≥ row_count: treated as missing data → emit all leaves.
void walk_visible_leaves(const BspMap& map,
                         math::Vec3 eye,
                         void (*emit)(const BspLeaf&, void*),
                         void* user) {
    if (emit == nullptr || map.leaves.empty()) {
        return;
    }

    const BspLeaf eye_leaf = locate(map, eye);
    if (eye_leaf.cluster < 0) {
        // Solid leaf — Quake convention is "see nothing". (The camera shouldn't
        // be inside a wall, but we don't punish callers for clipping bugs.)
        return;
    }

    // Reconstitute the PVS row width. We know map.pvs.size() == clusters * row_bytes
    // (validated in load()), so derive the row count from the eye leaf's bbox of
    // possible cluster ids: search up to the maximum cluster id we see.
    i32 max_cluster = 0;
    for (const BspLeaf& l : map.leaves) {
        if (l.cluster > max_cluster)
            max_cluster = l.cluster;
    }
    const u32 cluster_count = static_cast<u32>(max_cluster + 1);
    if (cluster_count == 0 || map.pvs.empty()) {
        // No PVS info — fall back to "everything visible" (conservative).
        for (const BspLeaf& l : map.leaves) {
            emit(l, user);
        }
        return;
    }
    const u32 row_bytes = static_cast<u32>(map.pvs.size()) / cluster_count;
    if (row_bytes == 0 || static_cast<u32>(eye_leaf.cluster) >= cluster_count) {
        for (const BspLeaf& l : map.leaves) {
            emit(l, user);
        }
        return;
    }

    const u8* row = map.pvs.data() + static_cast<usize>(eye_leaf.cluster) * row_bytes;
    for (const BspLeaf& l : map.leaves) {
        if (l.cluster < 0)
            continue;  // skip solid leaves
        const u32 ci = static_cast<u32>(l.cluster);
        if (ci >= cluster_count)
            continue;
        const u8 byte = row[ci >> 3];
        const u8 mask = static_cast<u8>(1u << (ci & 7u));
        if (byte & mask) {
            emit(l, user);
        }
    }
}

// ─── BSP face → DrawItem converter (BspDraw.h) ───────────────────────────

void build_face_draws(const BspGeometry& geom,
                      std::span<const BspFace> faces,
                      const BspMaterialResolve& resolve,
                      std::vector<render::raster::DrawItem>& out,
                      u32 first_face) {
    out.reserve(out.size() + faces.size());
    for (usize fi = 0; fi < faces.size(); ++fi) {
        const BspFace& f = faces[fi];
        if (f.vertex_count == 0)
            continue;
        render::raster::DrawItem di{};
        di.vertices =
            (f.first_vertex < geom.vertices.size()) ? &geom.vertices[f.first_vertex] : nullptr;
        di.vertex_count = f.vertex_count;
        // BSP convention: each face is an n-gon already fan-triangulated by
        // lm_qbsp into `vertex_count - 2` triangles. The index buffer for a
        // single face is therefore (vertex_count - 2) * 3 indices long; we
        // expose a contiguous slice into geom.indices addressed by first_vertex
        // (lm_qbsp writes the index slab parallel to the vertex slab).
        const u32 tri_indices = (f.vertex_count >= 3) ? (f.vertex_count - 2) * 3 : 0;
        di.indices = (f.first_vertex < geom.indices.size()) ? &geom.indices[f.first_vertex] : nullptr;
        di.index_count = tri_indices;
        di.model = math::identity4();
        if (resolve.table != nullptr && f.material < resolve.count) {
            di.material = resolve.table[f.material];
        } else {
            di.material = render::raster::MaterialId{f.material};
        }
        di.flags = render::raster::DrawFlags::None;
        // W12-2: wire the baked lightmap chunk for this face (if any). The
        // rasterizer's per-draw lightmap path samples the RGBA8 chunk through
        // the face base UV and modulates: final = vertexColor * lumel. A face
        // with no baked lightmap leaves lightmap_texels null -> full-bright.
        const u32 global_face = first_face + static_cast<u32>(fi);
        if (global_face < geom.face_lightmap.size()) {
            const u32 lm_idx = geom.face_lightmap[global_face];
            if (lm_idx != BspGeometry::kNoLightmap && lm_idx < geom.lightmaps.size()) {
                const BspFaceLightmap& lm = geom.lightmaps[lm_idx];
                const usize texel_count = static_cast<usize>(lm.width) * lm.height;
                if (texel_count > 0u &&
                    static_cast<usize>(lm.first_texel) + texel_count <=
                        geom.lightmap_texels.size()) {
                    di.lightmap_texels = &geom.lightmap_texels[lm.first_texel];
                    di.lightmap_w = lm.width;
                    di.lightmap_h = lm.height;
                }
            }
        }
        out.push_back(di);
    }
}

void build_leaf_draws(const BspMap& map,
                      const BspGeometry& geom,
                      const BspLeaf& leaf,
                      const BspMaterialResolve& resolve,
                      std::vector<render::raster::DrawItem>& out) {
    if (leaf.face_count == 0 || leaf.first_face >= map.faces.size()) {
        return;
    }
    const usize lo = leaf.first_face;
    const usize hi = std::min<usize>(lo + leaf.face_count, map.faces.size());
    std::span<const BspFace> faces{map.faces.data() + lo, hi - lo};
    build_face_draws(geom, faces, resolve, out, static_cast<u32>(lo));
}

// --- Geometry loader (W10-2) ----------------------------------------------
// Read the vertex + index chunks of the on-disk `.psybsp` (the same blob
// `load` reads for nodes/leaves/faces/pvs) into a BspGeometry so the runtime
// can render the BSP faces. The on-disk vertex stride mirrors the rasterizer
// Vertex packed layout (44 bytes); we bulk-memcpy straight into the runtime
// struct. The static_assert below pins that stride so any change to the
// rasterizer Vertex layout surfaces here (and demands a format bump) rather
// than silently corrupting the load.
bool load_geometry(std::string_view virtual_path, BspGeometry& out) {
    static_assert(sizeof(render::raster::Vertex) == 44u,
                  "on-disk BSP vertex stride (44) must match render::raster::Vertex");

    out.vertices.clear();
    out.indices.clear();

    asset::Blob blob = asset::Vfs::Get().read(virtual_path);
    if (blob.data == nullptr || blob.bytes < sizeof(BspFileHeader)) {
        return false;
    }
    BspFileHeader header{};
    std::memcpy(&header, blob.data, sizeof(BspFileHeader));
    if (header.magic != kBspFileMagic || header.version != kBspFileVersion ||
        header.total_bytes > blob.bytes) {
        return false;
    }
    const u32 used_bytes = header.total_bytes;

    // Vertices: raw 44-byte records -> render::raster::Vertex (bulk memcpy).
    if (header.vertices.count > 0u) {
        const u64 vbytes = static_cast<u64>(header.vertices.count) * sizeof(render::raster::Vertex);
        if (header.vertices.offset > used_bytes ||
            vbytes > static_cast<u64>(used_bytes - header.vertices.offset)) {
            return false;
        }
        out.vertices.resize(header.vertices.count);
        std::memcpy(out.vertices.data(), blob.data + header.vertices.offset, vbytes);
    }
    // Indices: u32 records (face-local, parallel to the vertex slab).
    if (!copy_chunk(blob.data, used_bytes, header.indices, out.indices)) {
        out.vertices.clear();
        out.indices.clear();
        return false;
    }
    return true;
}

// --- Lightmap loader (W12-2) ----------------------------------------------
// Read the W12-2 lightmap chunks (BspFormat.h: `lightmaps` directory + the
// packed RGB16F `lightmap_pixels`) from the same blob and decode the half-float
// lumels into the RGBA8 pool the rasterizer's per-draw lightmap path wants. The
// chunk is OPTIONAL: a blob baked without lightmaps advertises lightmaps.count
// == 0, and we return success with an empty `lightmaps`/`lightmap_texels` and an
// all-kNoLightmap `face_lightmap` (so build_face_draws renders full-bright).
namespace {

// IEEE-754 binary16 -> binary32. Branch-light, deterministic. Mirrors the
// decode lm_bake uses for its .lmlight blob (bake.f16_to_f32) so the two tools
// agree bit-for-bit on the half-float interpretation.
f32 half_to_f32(u16 h) noexcept {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    const u32 mant = h & 0x3FFu;
    u32 bits;
    if (exp == 0u) {
        if (mant == 0u) {
            bits = sign;  // +/- zero
        } else {
            // Subnormal half -> normalised float.
            u32 e = 0u;
            u32 m = mant;
            while ((m & 0x400u) == 0u) {
                m <<= 1;
                ++e;
            }
            m &= 0x3FFu;
            const u32 fexp = 127u - 15u - e + 1u;
            bits = sign | (fexp << 23) | (m << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);  // inf / nan
    } else {
        const u32 fexp = exp - 15u + 127u;
        bits = sign | (fexp << 23) | (mant << 13);
    }
    f32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// Clamp + pack a linear RGB triple (lightmap irradiance, may be HDR) to an
// opaque RGBA8 lumel in the framebuffer's 0xAABBGGRR packing. A simple Reinhard
// tonemap keeps bright lumels in range without clipping to flat white; the
// rasterizer multiplies this by the vertex colour, so the absolute scale only
// needs to be perceptually monotone, not physically exact.
u32 pack_lumel_rgba8(f32 r, f32 g, f32 b) noexcept {
    auto tone = [](f32 x) -> u32 {
        if (!(x > 0.0f))
            x = 0.0f;
        const f32 mapped = x / (1.0f + x);  // Reinhard -> [0,1)
        i32 q = static_cast<i32>(mapped * 255.0f + 0.5f);
        if (q < 0)
            q = 0;
        if (q > 255)
            q = 255;
        return static_cast<u32>(q);
    };
    return tone(r) | (tone(g) << 8) | (tone(b) << 16) | (0xFFu << 24);
}

}  // namespace

bool load_lightmaps(std::string_view virtual_path, u32 face_count, BspGeometry& out) {
    out.lightmaps.clear();
    out.lightmap_texels.clear();
    out.face_lightmap.assign(face_count, BspGeometry::kNoLightmap);

    asset::Blob blob = asset::Vfs::Get().read(virtual_path);
    if (blob.data == nullptr || blob.bytes < sizeof(BspFileHeader)) {
        return false;
    }
    BspFileHeader header{};
    std::memcpy(&header, blob.data, sizeof(BspFileHeader));
    if (header.magic != kBspFileMagic || header.version != kBspFileVersion ||
        header.total_bytes > blob.bytes) {
        return false;
    }
    const u32 used_bytes = header.total_bytes;

    // Unlit blob: no lightmap directory -> success with everything full-bright.
    if (header.lightmaps.count == 0u) {
        return true;
    }

    // Bounds-check the directory chunk.
    std::vector<BspFileLightmap> dir;
    if (!copy_chunk(blob.data, used_bytes, header.lightmaps, dir)) {
        out.face_lightmap.assign(face_count, BspGeometry::kNoLightmap);
        return false;
    }

    // The pixel chunk is a flat byte blob of RGB16F lumels. `count` is the byte
    // count (see write_psybsp_engine). Validate its extent before we index it.
    const u32 pix_off = header.lightmap_pixels.offset;
    const u32 pix_bytes = header.lightmap_pixels.count;
    if (pix_bytes > 0u && (pix_off > used_bytes || pix_bytes > used_bytes - pix_off)) {
        out.face_lightmap.assign(face_count, BspGeometry::kNoLightmap);
        return false;
    }
    const u8* pix = blob.data + pix_off;

    out.lightmaps.reserve(dir.size());
    for (const BspFileLightmap& rec : dir) {
        const usize texel_count = static_cast<usize>(rec.width) * rec.height;
        if (texel_count == 0u)
            continue;
        const u64 need = static_cast<u64>(rec.pixel_offset) +
                         static_cast<u64>(texel_count) * kBspLightmapTexelBytes;
        if (rec.pixel_offset > pix_bytes || need > static_cast<u64>(pix_bytes)) {
            continue;  // skip a malformed row rather than fail the whole load
        }
        BspFaceLightmap fl{};
        fl.face = rec.face;
        fl.width = rec.width;
        fl.height = rec.height;
        fl.first_texel = static_cast<u32>(out.lightmap_texels.size());
        out.lightmap_texels.reserve(out.lightmap_texels.size() + texel_count);
        const u8* src = pix + rec.pixel_offset;
        for (usize t = 0; t < texel_count; ++t) {
            u16 hr, hg, hb;
            std::memcpy(&hr, src + t * kBspLightmapTexelBytes + 0, sizeof(u16));
            std::memcpy(&hg, src + t * kBspLightmapTexelBytes + 2, sizeof(u16));
            std::memcpy(&hb, src + t * kBspLightmapTexelBytes + 4, sizeof(u16));
            out.lightmap_texels.push_back(
                pack_lumel_rgba8(half_to_f32(hr), half_to_f32(hg), half_to_f32(hb)));
        }
        const u32 lm_index = static_cast<u32>(out.lightmaps.size());
        out.lightmaps.push_back(fl);
        if (rec.face < out.face_lightmap.size())
            out.face_lightmap[rec.face] = lm_index;
    }
    return true;
}

// `walk_portal_visible_leaves` + `build_portal_set` (Portal.h) live in
// PortalClip.cpp — Wave B replaced the Wave A PVS-only stub with a real
// portal-graph walk + frustum clip. See PortalClip.h for the helper surface.

}  // namespace psynder::world::bsp
