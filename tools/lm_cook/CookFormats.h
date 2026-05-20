// SPDX-License-Identifier: MIT
// Psynder — cooked asset binary formats (.lmm / .lmt / .lma).
//
// Lane 24 owns the writers (offline). Lane 05 will own the readers when it
// fleshes out streaming asset loading. Both sides agree on the structures
// declared here.
//
// All multi-byte integers are little-endian; floats are IEEE-754 binary32.
// The reference machines are LE; if we ever ship to BE we add a swap.

#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::cook {

// ─── .lmm — meshes ───────────────────────────────────────────────────────
//
//   char  magic[4]   = "LMM1"
//   u32   version    = 1
//   u32   vertex_count
//   u32   index_count
//   u32   submesh_count
//   u32   material_name_offset    (bytes from header start)
//   u32   material_name_bytes
//   u32   flags                   (reserved)
//   ── vertex array (vertex_count × Vertex) ──
//   ── index  array (index_count  × u32)    ──
//   ── submesh array (submesh_count × Submesh) ──
//   ── material-name pool (UTF-8) ──

inline constexpr u32 kLmmMagic = 0x314D4D4Cu;  // 'LMM1'
inline constexpr u32 kLmmVersion = 1u;

struct LmmVertex {
    f32 px, py, pz;      // position
    f32 nx, ny, nz;      // normal
    f32 u, v;            // texcoord 0
    f32 tx, ty, tz, tw;  // tangent (xyz + bitangent sign)
};

struct LmmSubmesh {
    u32 first_index;
    u32 index_count;
    u32 material_index;  // index into the per-mesh material name table
    u32 reserved;
};

struct LmmMesh {
    std::vector<LmmVertex> vertices;
    std::vector<u32> indices;
    std::vector<LmmSubmesh> submeshes;
    std::vector<std::string> materials;
};

void write_lmm(const LmmMesh& mesh, std::vector<u8>& out);
bool read_lmm(std::span<const u8> bytes, LmmMesh& out, std::string* err = nullptr);

// ─── .lmt — textures with mipchain ───────────────────────────────────────
//
//   char  magic[4]    = "LMT1"
//   u32   version     = 1
//   u32   format      (CookTexFormat enum)
//   u32   width
//   u32   height
//   u32   mip_count
//   u32   flags       (reserved)
//   ── per-mip table (mip_count × MipDesc) ──
//   ── pixel payload (each mip's bytes packed back-to-back) ──

inline constexpr u32 kLmtMagic = 0x31544D4Cu;  // 'LMT1'
inline constexpr u32 kLmtVersion = 1u;

enum class CookTexFormat : u32 {
    kRgba8 = 1,
    kRgb8 = 2,
    kR8 = 3,
};

struct LmtMipDesc {
    u32 width;
    u32 height;
    u32 offset;  // bytes from start of pixel payload
    u32 bytes;
};

struct LmtTexture {
    CookTexFormat format = CookTexFormat::kRgba8;
    u32 width = 0;
    u32 height = 0;
    std::vector<LmtMipDesc> mips;
    std::vector<u8> pixels;  // contiguous, indexed by MipDesc.offset
};

// Build mipchain (box-filter downsample) from a base-level RGBA8 image.
LmtTexture build_mipchain_rgba8(const u8* base_pixels, u32 width, u32 height);
void write_lmt(const LmtTexture& tex, std::vector<u8>& out);
bool read_lmt(std::span<const u8> bytes, LmtTexture& out, std::string* err = nullptr);

// ─── .lma — audio ────────────────────────────────────────────────────────
//
//   char  magic[4]    = "LMA1"
//   u32   version     = 1
//   u32   channels
//   u32   sample_rate (Hz)
//   u32   sample_count_per_channel
//   u32   bits_per_sample (8 / 16 / 24 / 32)
//   u32   flags       (bit0 = float samples)
//   ── interleaved samples ──

inline constexpr u32 kLmaMagic = 0x31414D4Cu;  // 'LMA1'
inline constexpr u32 kLmaVersion = 1u;

struct LmaAudio {
    u32 channels = 1;
    u32 sample_rate = 48000;
    u32 sample_count = 0;
    u32 bits_per_sample = 16;
    bool is_float = false;
    std::vector<u8> samples;  // interleaved, channel-major within one frame
};

void write_lma(const LmaAudio& audio, std::vector<u8>& out);
bool read_lma(std::span<const u8> bytes, LmaAudio& out, std::string* err = nullptr);

}  // namespace psynder::tools::cook
