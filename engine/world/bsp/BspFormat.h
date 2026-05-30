// SPDX-License-Identifier: MIT
// Psynder — .psybsp on-disk file format. Lane 10 (world-bsp) owns; written
// by lane 24's `lm_qbsp` compiler, consumed by `Bsp::load`.
//
// The format is a flat, header-prefixed binary blob with a fixed root header
// followed by six chunks (nodes / leaves / faces / vertices / indices /
// pvs-bytes). Each chunk is identified in the header by a (offset, count)
// pair measured from the start of the file. All offsets are 4-byte aligned
// and the integers are stored little-endian (BSP data is shipped per-arch in
// .lmpak, so we don't bother with a portable byte-swap path — lm_pak owns
// any future endianness conversion).
//
// Format version is bumped any time the on-disk layout changes. The runtime
// rejects any blob whose version is not exactly kBspFileVersion (no implicit
// upgrade path — lm_qbsp re-emits when the format moves).
//
// The header lives at file offset 0 and is exactly 96 bytes (with the trailing
// reserved field). The data chunks follow in declaration order and may be
// padded for alignment. Total file size = `total_bytes` in the header.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  off  size  field                                                       │
// │  0    4     magic            'PBSP' (0x50534250 little-endian)          │
// │  4    4     version          kBspFileVersion                            │
// │  8    4     flags            bitmap (currently unused)                  │
// │  12   4     total_bytes      file size                                  │
// │  16   4     cluster_count    PVS clusters                               │
// │  20   4     pvs_row_bytes    bytes per cluster row (== ceil(C/8))       │
// │  24   8     nodes    (offset, count)                                    │
// │  32   8     leaves   (offset, count)                                    │
// │  40   8     faces    (offset, count)                                    │
// │  48   8     vertices (offset, count)                                    │
// │  56   8     indices  (offset, count)                                    │
// │  64   8     pvs      (offset, count = pvs_row_bytes * cluster_count)    │
// │  72   8     lightmaps       (offset, count = lit-face directory rows)   │  (W12-2)
// │  80   8     lightmap_pixels (offset, byte_count = packed RGB16F lumels) │  (W12-2)
// │  88   8     reserved                                                    │
// └─────────────────────────────────────────────────────────────────────────┘
//
// W12-2 lightmap chunks (ADDITIVE — repurposed the trailing reserved bytes; the
// header is still exactly 96 bytes and the loader's nodes/leaves/faces/vertices/
// indices/pvs parsing is byte-for-byte unchanged). When `lightmaps.count == 0`
// the level is unlit and every face keeps `lightmap == kBspNoLightmap` — nothing
// regresses. When non-zero, `lightmaps` is a directory of per-lit-face records
// (see BspFileLightmap) and `lightmap_pixels` holds the packed 16-bit half-float
// RGB lumels those records index into (DESIGN.md §8.1: RGB16F, per-surface res).

#pragma once

#include "core/Types.h"

namespace psynder::world::bsp {

inline constexpr u32 kBspFileMagic = 0x50534250u;  // 'P','B','S','P'
inline constexpr u32 kBspFileVersion = 1u;

struct BspFileChunk {
    u32 offset;
    u32 count;
};

struct BspFileHeader {
    u32 magic;
    u32 version;
    u32 flags;
    u32 total_bytes;
    u32 cluster_count;
    u32 pvs_row_bytes;
    BspFileChunk nodes;
    BspFileChunk leaves;
    BspFileChunk faces;
    BspFileChunk vertices;
    BspFileChunk indices;
    BspFileChunk pvs;
    // W12-2 lightmap chunks (ADDITIVE — carved out of the old `reserved[6]`, so
    // the header is still exactly 96 bytes). `lightmaps` = per-lit-face directory
    // (BspFileLightmap rows); `lightmap_pixels` = packed RGB16F lumel bytes the
    // rows index into. Both are 0/0 for an unlit blob (no regression).
    BspFileChunk lightmaps;
    BspFileChunk lightmap_pixels;
    u32 reserved[2];
};
static_assert(sizeof(BspFileHeader) == 96, "BspFileHeader must be exactly 96 bytes");

// ─── On-disk records ─────────────────────────────────────────────────────
// These mirror the in-memory structs in Bsp.h but use explicit-width fields
// so the on-disk layout is independent of any future host-struct changes.
struct BspFileNode {
    f32 nx, ny, nz;   // plane normal
    f32 d;            // plane distance (n.dot(p) = d)
    i32 front_child;  // child >= 0 → node, child < 0 → leaf index (~child)
    i32 back_child;
};
static_assert(sizeof(BspFileNode) == 24, "BspFileNode layout drift");

struct BspFileLeaf {
    i32 cluster;
    u32 first_face;
    u32 face_count;
    f32 bbox_min_x, bbox_min_y, bbox_min_z;
    f32 bbox_max_x, bbox_max_y, bbox_max_z;
};
static_assert(sizeof(BspFileLeaf) == 36, "BspFileLeaf layout drift");

struct BspFileFace {
    u32 first_vertex;
    u32 vertex_count;
    u32 material;
    u32 lightmap;  // index into the lightmap directory, or kBspFaceNoLightmap.
};
static_assert(sizeof(BspFileFace) == 16, "BspFileFace layout drift");

// W12-2 per-lit-face lightmap directory record. `lm_qbsp` bakes one of these
// for every face it shades; `BspFace::lightmap` indexes this directory (NOT a
// raw page id). `pixel_offset` is a BYTE offset into the `lightmap_pixels`
// chunk; the lumel block at that offset is `width * height` RGB16F texels
// (3 * 2 bytes each), row-major, addressed by the face's base UV (0..1). The
// lumels are 16-bit IEEE-754 half-floats (DESIGN.md §8.1) so the bake can carry
// HDR irradiance; the runtime decodes them to LDR RGBA8 for the rasterizer's
// per-draw lightmap chunk (BspDraw.h).
struct BspFileLightmap {
    u32 face;          // BspFace index this lumel block belongs to.
    u32 width;         // lumels across (== face base-U span).
    u32 height;        // lumels down  (== face base-V span).
    u32 pixel_offset;  // BYTE offset into the lightmap_pixels chunk.
};
static_assert(sizeof(BspFileLightmap) == 16, "BspFileLightmap layout drift");

// Sentinel stored in `BspFileFace::lightmap` when a face carries no baked
// lightmap (full-bright). Mirrors lm_qbsp's kBspNoLightmap so an unlit blob
// round-trips a face's flat-shaded state through the loader untouched.
inline constexpr u32 kBspFaceNoLightmap = 0xFFFFFFFFu;

// RGB16F: 3 half-float channels per lumel = 6 bytes. Matches the LightmapAtlas
// page texel stride (LightmapAtlas.h) and DESIGN.md §8.1's "16-bit half-float
// RGB lightmaps".
inline constexpr u32 kBspLightmapTexelBytes = 6u;

// Vertices in the .psybsp blob are a direct subset of the rasterizer Vertex
// (position / normal / uv / lightmap_uv / packed RGBA8). We don't re-declare
// the struct here — we just bulk-memcpy bytes — but the size is asserted so
// any change to the rasterizer Vertex layout requires a format version bump.
inline constexpr u32 kBspFileVertexBytes = 48;
inline constexpr u32 kBspFileIndexBytes = 4;

// Sentinel cluster used when a leaf is "solid" (inside-wall) and has no PVS
// row. `locate` may return such a leaf when the point lies inside geometry.
inline constexpr i32 kBspSolidCluster = -1;

// ─── Helpers — index <-> leaf-child encoding ─────────────────────────────
// A node's front_child / back_child is either a non-negative node index OR
// a "leaf reference" encoded as ~leaf_index (so leaf 0 → -1, leaf 1 → -2, …).
constexpr bool bsp_is_leaf(i32 child) noexcept {
    return child < 0;
}
constexpr i32 bsp_leaf_index(i32 child) noexcept {
    return ~child;
}
constexpr i32 bsp_encode_leaf(i32 leaf_index) noexcept {
    return ~leaf_index;
}

}  // namespace psynder::world::bsp
