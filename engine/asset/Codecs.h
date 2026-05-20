// SPDX-License-Identifier: MIT
// Psynder — cooked-asset codec helpers: .lmm / .lmt / .lma read & write.
//
// `engine/asset/Formats.h` declares the v1 on-disk struct layouts; this
// header is the **operational** surface that produces and consumes those
// bytes. Wave A landed the typed header structs; Wave B (this file) adds
// the writer path so the cooker (or in-process tests) can synthesize
// byte-exact buffers and the reader path round-trips them.
//
// Design notes:
//   - Buffers are plain `std::vector<u8>`. The writer appends to a buffer
//     supplied by the caller so it can be embedded directly into an
//     .lmpak entry without an extra copy.
//   - Structures-of-arrays match the runtime consumers: the renderer
//     expects an interleaved vertex stream + a single index buffer, the
//     texture lane expects a packed mip pyramid + optional palette, the
//     audio mixer expects interleaved PCM frames.
//   - Mesh / texture / audio structs declared here are NOT part of the
//     public ABI (they're constructed afresh per-load). The public ABI
//     stays the byte layout in `Formats.h`.
//
// This header lives alongside `Formats.h` because both halves are
// engine-internal — the public Vfs.h surface returns raw `Blob`s, and
// it's the consuming lane (render/audio) that calls into these codecs.

#pragma once

#include "Formats.h"
#include "core/Types.h"

#include <span>
#include <string>
#include <vector>

namespace psynder::asset {

// ─── .lmm — mesh ─────────────────────────────────────────────────────────
namespace lmm {

// Submesh entry in the in-memory mesh. Maps 1:1 to formats::LmmSubmesh.
struct Submesh {
    u32 index_start = 0;
    u32 index_count = 0;
    u32 material_hash = 0;  // FNV-1a of the material name; binds to a .lmt
    u32 reserved = 0;
};

// In-memory mesh. Vertex bytes are kept opaque so callers can vary the
// stride per `vertex_fmt`; the codec writes/reads them verbatim.
struct Mesh {
    formats::LmmVertexFmt vertex_fmt = formats::LmmVertexFmt::Pos3N3UV2;
    u32 vertex_stride = 32;  // bytes per vertex
    u32 vertex_count = 0;
    u32 index_count = 0;
    f32 bbox_min[3] = {0, 0, 0};
    f32 bbox_max[3] = {0, 0, 0};
    std::vector<Submesh> submeshes;
    std::vector<u8> vertex_data;  // vertex_count * vertex_stride
    std::vector<u8> index_data;   // index_count * (16-bit ? 2 : 4)
};

// Return the size in bytes of one index for this vertex count. The runtime
// uses u16 indices when the mesh has 65535 or fewer vertices so the
// index stream halves in memory.
inline u32 index_byte_size(u32 vertex_count) noexcept {
    return vertex_count <= 0xFFFFu ? 2u : 4u;
}

// Compute the stride implied by a vertex format.
inline u32 stride_for(formats::LmmVertexFmt fmt) noexcept {
    switch (fmt) {
        case formats::LmmVertexFmt::Pos3N3UV2:
            return 32;  // 8 floats
        case formats::LmmVertexFmt::Pos3N3T4UV2:
            return 48;  // 12 floats
    }
    return 32;
}

// Serialize `mesh` to the end of `out`. Existing bytes in `out` are
// preserved so the caller can chain writes (e.g. into an .lmpak entry).
// Returns false only when the mesh struct is internally inconsistent
// (size mismatches) so callers can early-out instead of producing a
// corrupt blob.
bool write_mesh(std::vector<u8>& out, const Mesh& mesh);

// Parse `bytes` into `mesh`. The span must start at the LmmHeader; trailing
// bytes after the mesh payload are ignored (so an .lmpak entry that has
// more data after the mesh still parses cleanly).
bool read_mesh(std::span<const u8> bytes, Mesh& mesh);

}  // namespace lmm

// ─── .lmt — texture ──────────────────────────────────────────────────────
namespace lmt {

struct Mip {
    u32 width = 0;
    u32 height = 0;
    u32 offset = 0;  // offset within `pixel_data`
    u32 byte_size = 0;
};

struct Texture {
    u16 width = 0;
    u16 height = 0;
    formats::LmtPixelFmt pixel_fmt = formats::LmtPixelFmt::RGBA8;
    u16 flags = 0;
    std::vector<Mip> mips;       // mip 0 is largest
    std::vector<u8> palette;     // 256*4 bytes when P8, else empty
    std::vector<u8> pixel_data;  // concatenated mip pixels
};

// Bytes per pixel for the runtime side of LmtPixelFmt. P8 maps to 1 byte
// of index data per pixel (the palette is separate).
inline u32 bytes_per_pixel(formats::LmtPixelFmt fmt) noexcept {
    switch (fmt) {
        case formats::LmtPixelFmt::P8:
            return 1;
        case formats::LmtPixelFmt::RGB565:
            return 2;
        case formats::LmtPixelFmt::RGBA8:
            return 4;
        case formats::LmtPixelFmt::RG88:
            return 2;
    }
    return 4;
}

bool write_texture(std::vector<u8>& out, const Texture& texture);
bool read_texture(std::span<const u8> bytes, Texture& texture);

}  // namespace lmt

// ─── .lma — audio ────────────────────────────────────────────────────────
namespace lma {

struct Audio {
    u32 sample_rate = 48000;
    u32 frame_count = 0;
    formats::LmaSampleFmt sample_fmt = formats::LmaSampleFmt::PCM_S16;
    u16 channels = 1;
    u16 flags = 0;
    u32 loop_start = 0;  // valid only when kLmaFlagLoop is set
    u32 loop_end = 0;
    std::vector<u8> pcm_data;  // frame_count * channels * bytes_per_sample
};

// Bytes per sample for a given format.
inline u32 bytes_per_sample(formats::LmaSampleFmt fmt) noexcept {
    switch (fmt) {
        case formats::LmaSampleFmt::PCM_S16:
            return 2;
        case formats::LmaSampleFmt::PCM_F32:
            return 4;
    }
    return 2;
}

bool write_audio(std::vector<u8>& out, const Audio& audio);
bool read_audio(std::span<const u8> bytes, Audio& audio);

}  // namespace lma

}  // namespace psynder::asset
