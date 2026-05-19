// SPDX-License-Identifier: MIT
// Psynder — source-asset parsers for lm_cook.
//
// Wave-A scope (per Issue #24):
//   - OBJ: hand-rolled ASCII triangle-mesh parser (v / vn / vt / f / usemtl).
//   - glTF: minimal JSON-form .gltf with embedded base64 data-URI buffers.
//     Covers single-mesh round-tripping for tests; full glTF coverage
//     belongs in a Wave-B `cgltf` vendor pass.
//   - PNG: built-in encoder/decoder pair for "uncompressed-stored deflate"
//     (DEFLATE BTYPE=0) 8-bit RGBA, which is all we need to round-trip the
//     synthesized test inputs. stb_image vendoring is Wave-B.
//   - WAV: canonical RIFF / WAVE / fmt / data, PCM 8-/16-/24-/32-bit + float.
//
// The "minimal parsers ship today / wider parsers in Wave-B" trade keeps
// the tool buildable on a clean dev box without fetching extra deps.

#pragma once

#include "core/Types.h"
#include "CookFormats.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::cook {

// ─── OBJ ─────────────────────────────────────────────────────────────────
//
// Returned mesh has tangents synthesised from per-face geometry; submeshes
// are emitted on every `usemtl` change.
bool parse_obj(std::string_view text, LmmMesh& out, std::string* err = nullptr);

// ─── glTF (minimal JSON form) ────────────────────────────────────────────
//
// Accepts a .gltf where every buffer has either:
//   - `uri: "data:application/octet-stream;base64,..."`, or
//   - the buffer is referenced by index 0 and supplied as `external_buffer`.
//
// Only the first mesh's first primitive is read (POSITION + NORMAL + TEXCOORD_0
// + indices), which is enough for the Wave-A smoke test. Returns false with
// a descriptive error if the file uses anything more elaborate.
bool parse_gltf(std::string_view json,
                std::span<const u8> external_buffer,   // empty if none
                LmmMesh& out,
                std::string* err = nullptr);

// ─── PNG (uncompressed-stored DEFLATE only) ──────────────────────────────
//
// Decoder: validates PNG signature + chunks, reconstructs RGBA8 pixels from
// IDAT chunks whose payloads are uncompressed (BTYPE=0) deflate blocks with
// filter byte 0 per row. Producers using zlib's `Z_NO_COMPRESSION` produce
// exactly this layout. Returns false on anything else.
bool decode_png_stored(std::span<const u8> bytes,
                       std::vector<u8>& out_rgba,
                       u32& width,
                       u32& height,
                       std::string* err = nullptr);

// Encoder: produces a valid 8-bit RGBA PNG using only stored deflate blocks.
// The encoded file is round-trip readable by decode_png_stored above as well
// as by any standards-compliant PNG decoder (stb_image, libpng, browsers).
void encode_png_stored(const u8* rgba, u32 width, u32 height,
                       std::vector<u8>& out_bytes);

// ─── WAV ─────────────────────────────────────────────────────────────────
//
// Accepts canonical RIFF WAVE with one `fmt ` chunk and one `data` chunk.
// Supports PCM (8/16/24/32) and IEEE float (32). Resampling / channel-mix
// is out of scope; samples are returned as-is in the LmaAudio's `samples`
// blob (little-endian, interleaved).
bool parse_wav(std::span<const u8> bytes, LmaAudio& out, std::string* err = nullptr);

// Encoder used by tests (writes 16-bit PCM mono/stereo WAVs we can then cook).
void encode_wav_pcm16(const i16* samples, u32 sample_count, u32 channels, u32 sample_rate,
                      std::vector<u8>& out_bytes);

}  // namespace psynder::tools::cook
