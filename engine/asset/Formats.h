// SPDX-License-Identifier: MIT
// Psynder — cooker output formats (header structs only).
//
// The actual cookers live under tools/lm_cook/ (lane 24). This header
// fixes the on-disk layouts so the runtime loader and the offline
// cooker agree on a v1 contract.
//
// All three formats start with a 16-byte fixed header that names the
// magic + version. Mismatches abort the load; backward-compat is via
// version bump.

#pragma once

#include "core/Types.h"

namespace psynder::asset::formats {

// ─── Common 16-byte preamble ─────────────────────────────────────────────
struct alignas(8) FileHeader {
    u32 magic;         // format-specific FOURCC
    u16 version;       // bumped on incompatible layout changes
    u16 flags;         // format-specific bits
    u64 payload_size;  // bytes following this header (sanity check)
};
static_assert(sizeof(FileHeader) == 16, "FileHeader is the v1 wire size");

// ─── .lmm — cooked mesh ─────────────────────────────────────────────────
// Magic 'L','M','M','1'. One mesh = static interleaved vertex stream + an
// indexed triangle list. Submeshes split the index range by material.

inline constexpr u32 kLmmMagic = 0x314D4D4Cu;  // 'L','M','M','1' LE
inline constexpr u16 kLmmVersion = 1u;

enum class LmmVertexFmt : u16 {
    Pos3N3UV2 = 0,    // 32 bytes/vertex: float3 pos, float3 normal, float2 uv
    Pos3N3T4UV2 = 1,  // 48 bytes/vertex: + float4 tangent
};

struct alignas(8) LmmHeader {
    FileHeader file;  // magic=kLmmMagic, version=kLmmVersion
    u32 vertex_count;
    u32 index_count;  // 16-bit indices if vertex_count<=65535
    LmmVertexFmt vertex_fmt;
    u16 submesh_count;
    u32 vertex_stride;  // bytes per vertex (matches vertex_fmt)
    f32 bbox_min[3];
    f32 bbox_max[3];
    // Following the header in file order:
    //   - LmmSubmesh[submesh_count]
    //   - vertex_count * vertex_stride bytes
    //   - index_count * (vertex_count<=65535 ? 2 : 4) bytes
};

struct alignas(8) LmmSubmesh {
    u32 index_start;
    u32 index_count;
    u32 material_hash;  // FNV-1a of the material name; binds to a .lmt
    u32 reserved;
};

// ─── .lmt — cooked texture (paletted / 16-bit / 32-bit + mipchain) ──────
// Magic 'L','M','T','1'. The texture stores a mipmap pyramid; mip 0 is
// the largest, last mip is 1x1. Paletted textures embed a 256-entry
// 32bpp palette right after the mip table.

inline constexpr u32 kLmtMagic = 0x31544D4Cu;  // 'L','M','T','1' LE
inline constexpr u16 kLmtVersion = 1u;

enum class LmtPixelFmt : u16 {
    P8 = 0,      // 8-bit palette index + 256-entry RGBA8 palette
    RGB565 = 1,  // 16-bit, little-endian R5G6B5
    RGBA8 = 2,   // 32-bit, sRGB
    RG88 = 3,    // 16-bit, two-channel (normal-map tangent space)
};

inline constexpr u16 kLmtFlagSRGB = 1u << 0;
inline constexpr u16 kLmtFlagNormalMap = 1u << 1;
inline constexpr u16 kLmtFlagWrapClamp = 1u << 2;

struct alignas(8) LmtHeader {
    FileHeader file;
    u16 width;     // mip 0 width
    u16 height;    // mip 0 height
    u8 mip_count;  // 1..16
    u8 reserved0;
    LmtPixelFmt pixel_fmt;
    u32 palette_offset;  // byte offset from header (0 if no palette)
    u32 pixels_offset;   // byte offset of mip 0 pixels from header
    // Following the header in file order:
    //   - if P8: 256 * 4 bytes palette at palette_offset
    //   - LmtMip[mip_count] table (mips_offset is right after the header)
    //   - mip 0 pixels, mip 1 pixels, ...
};

struct alignas(8) LmtMip {
    u32 width;
    u32 height;
    u32 offset;  // relative to LmtHeader start
    u32 byte_size;
};

// ─── .lma — cooked audio ────────────────────────────────────────────────
// Magic 'L','M','A','1'. PCM int16 mono/stereo, optional zstd-compressed
// payload. Streamed assets (music) live as multiple .lma chunks.

inline constexpr u32 kLmaMagic = 0x31414D4Cu;  // 'L','M','A','1' LE
inline constexpr u16 kLmaVersion = 1u;

enum class LmaSampleFmt : u16 {
    PCM_S16 = 0,  // signed 16-bit little-endian
    PCM_F32 = 1,  // 32-bit float, [-1, +1]
};

inline constexpr u16 kLmaFlagLoop = 1u << 0;
inline constexpr u16 kLmaFlagStreamed = 1u << 1;
inline constexpr u16 kLmaFlagZstd = 1u << 2;

struct alignas(8) LmaHeader {
    FileHeader file;
    u32 sample_rate;  // Hz (44100, 48000, ...)
    u32 frame_count;  // number of multi-channel frames
    LmaSampleFmt sample_fmt;
    u16 channels;    // 1=mono, 2=stereo
    u32 loop_start;  // frame index, valid when kLmaFlagLoop
    u32 loop_end;    // exclusive frame index
    // Following the header in file order:
    //   - frame_count * channels * bytes_per_sample bytes of PCM
};

}  // namespace psynder::asset::formats
