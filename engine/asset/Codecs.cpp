// SPDX-License-Identifier: MIT
// Psynder — .lmm / .lmt / .lma codec implementation.
//
// All three formats follow the same shape:
//   1) 16-byte `FileHeader` (magic + version + flags + payload_size)
//   2) format-specific header (struct from `Formats.h`)
//   3) variable-size tables and payload sections
//
// We treat the structs in `Formats.h` as the wire layout: they're already
// `alignas(8)` and we built them to match disk byte-for-byte. The codec
// therefore reduces to `memcpy(buf, &hdr, sizeof(hdr))` and the reverse,
// followed by appending the variable-size tail.
//
// Endianness: little-endian on the host. The reference targets (Mac M-series,
// x86_64, Linux arm64) are all LE. If we ever ship to a BE target we'll
// add explicit swaps here; today we'd hit a static_assert in `Types.h`
// before getting this far.

#include "Codecs.h"

#include <cstring>

namespace psynder::asset {

// ─── Wire-layout assertions ──────────────────────────────────────────────
// The codec memcpy's `Formats.h` structs directly into the output buffer,
// so any drift in the compiler's chosen padding would silently corrupt
// the disk format. These asserts catch a layout regression at compile
// time so the cooker and the runtime can't disagree.
//
// Layouts (LE host, all reference compilers):
//   FileHeader      = 16 bytes  (4 + 2 + 2 + 8)
//   LmmSubmesh      = 16 bytes  (4 × 4)
//   LmmHeader       = 56 bytes  (file 16, vcount 4, icount 4, vfmt 2,
//                                smcount 2, stride 4, bbox 24)
//   LmtMip          = 16 bytes  (4 × 4)
//   LmtHeader       = 32 bytes  (file 16, w 2, h 2, mc 1, r0 1, fmt 2,
//                                pal_off 4, px_off 4)
//   LmaHeader       = 40 bytes  (file 16, srate 4, fcount 4, fmt 2, ch 2,
//                                lstart 4, lend 4)
static_assert(sizeof(formats::FileHeader) == 16,  "FileHeader is the v1 wire size");
static_assert(sizeof(formats::LmmSubmesh) == 16,  "LmmSubmesh is the v1 wire size");
static_assert(sizeof(formats::LmmHeader)  == 56,  "LmmHeader is the v1 wire size");
static_assert(sizeof(formats::LmtMip)     == 16,  "LmtMip is the v1 wire size");
static_assert(sizeof(formats::LmtHeader)  == 32,  "LmtHeader is the v1 wire size");
static_assert(sizeof(formats::LmaHeader)  == 40,  "LmaHeader is the v1 wire size");

namespace {

template <class T>
void append_bytes(std::vector<u8>& out, const T& value) {
    const u8* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

void append_raw(std::vector<u8>& out, const u8* data, usize bytes) {
    out.insert(out.end(), data, data + bytes);
}

// Span-slice helper: take `count` bytes starting at `off` and copy them
// into `dst`. Uses pointer arithmetic to avoid sign-conversion warnings
// from std::span<...>::iterator + usize.
void assign_slice(std::vector<u8>& dst, std::span<const u8> bytes, usize off, usize count) {
    const u8* base = bytes.data() + off;
    dst.assign(base, base + count);
}

// Read `sizeof(T)` bytes from `bytes` at `off` into `out`. Returns false
// if the read would overrun the span.
template <class T>
bool read_pod(std::span<const u8> bytes, usize off, T& out) {
    if (off + sizeof(T) > bytes.size()) return false;
    std::memcpy(&out, bytes.data() + off, sizeof(T));
    return true;
}

}  // namespace

// ─── .lmm ────────────────────────────────────────────────────────────────
namespace lmm {

bool write_mesh(std::vector<u8>& out, const Mesh& mesh) {
    // Sanity-check the struct so we don't write a buffer the reader can't
    // parse back.
    const u32 stride = mesh.vertex_stride;
    if (stride == 0) return false;
    if (mesh.vertex_data.size() != static_cast<usize>(mesh.vertex_count) * stride) {
        return false;
    }
    const u32 ix_bytes = index_byte_size(mesh.vertex_count);
    if (mesh.index_data.size() != static_cast<usize>(mesh.index_count) * ix_bytes) {
        return false;
    }
    // Submesh index ranges must lie inside the index buffer.
    for (const auto& sm : mesh.submeshes) {
        const u64 end = static_cast<u64>(sm.index_start) + sm.index_count;
        if (end > mesh.index_count) return false;
    }

    // Payload = submesh table + vertex bytes + index bytes (after the LmmHeader).
    const usize submesh_bytes = mesh.submeshes.size() * sizeof(formats::LmmSubmesh);
    const usize payload_after_header =
        submesh_bytes + mesh.vertex_data.size() + mesh.index_data.size();

    formats::LmmHeader hdr{};
    hdr.file.magic        = formats::kLmmMagic;
    hdr.file.version      = formats::kLmmVersion;
    hdr.file.flags        = 0;
    hdr.file.payload_size = payload_after_header;
    hdr.vertex_count      = mesh.vertex_count;
    hdr.index_count       = mesh.index_count;
    hdr.vertex_fmt        = mesh.vertex_fmt;
    hdr.submesh_count     = static_cast<u16>(mesh.submeshes.size());
    hdr.vertex_stride     = stride;
    std::memcpy(hdr.bbox_min, mesh.bbox_min, sizeof(hdr.bbox_min));
    std::memcpy(hdr.bbox_max, mesh.bbox_max, sizeof(hdr.bbox_max));

    out.reserve(out.size() + sizeof(hdr) + payload_after_header);
    append_bytes(out, hdr);

    for (const auto& sm : mesh.submeshes) {
        formats::LmmSubmesh wire{};
        wire.index_start   = sm.index_start;
        wire.index_count   = sm.index_count;
        wire.material_hash = sm.material_hash;
        wire.reserved      = sm.reserved;
        append_bytes(out, wire);
    }
    append_raw(out, mesh.vertex_data.data(), mesh.vertex_data.size());
    append_raw(out, mesh.index_data.data(), mesh.index_data.size());
    return true;
}

bool read_mesh(std::span<const u8> bytes, Mesh& mesh) {
    formats::LmmHeader hdr{};
    if (!read_pod(bytes, 0, hdr)) return false;
    if (hdr.file.magic != formats::kLmmMagic) return false;
    if (hdr.file.version != formats::kLmmVersion) return false;
    if (hdr.vertex_stride == 0) return false;

    const u32 ix_bytes = index_byte_size(hdr.vertex_count);
    const usize submesh_bytes = static_cast<usize>(hdr.submesh_count) * sizeof(formats::LmmSubmesh);
    const usize vertex_bytes  = static_cast<usize>(hdr.vertex_count)  * hdr.vertex_stride;
    const usize index_bytes   = static_cast<usize>(hdr.index_count)   * ix_bytes;
    const usize need = sizeof(hdr) + submesh_bytes + vertex_bytes + index_bytes;
    if (need > bytes.size()) return false;

    mesh.vertex_fmt    = hdr.vertex_fmt;
    mesh.vertex_stride = hdr.vertex_stride;
    mesh.vertex_count  = hdr.vertex_count;
    mesh.index_count   = hdr.index_count;
    std::memcpy(mesh.bbox_min, hdr.bbox_min, sizeof(mesh.bbox_min));
    std::memcpy(mesh.bbox_max, hdr.bbox_max, sizeof(mesh.bbox_max));

    mesh.submeshes.clear();
    mesh.submeshes.reserve(hdr.submesh_count);
    usize cursor = sizeof(hdr);
    for (u16 i = 0; i < hdr.submesh_count; ++i) {
        formats::LmmSubmesh wire{};
        if (!read_pod(bytes, cursor, wire)) return false;
        cursor += sizeof(wire);
        Submesh sm;
        sm.index_start   = wire.index_start;
        sm.index_count   = wire.index_count;
        sm.material_hash = wire.material_hash;
        sm.reserved      = wire.reserved;
        // Range-check the submesh against the index buffer.
        const u64 end = static_cast<u64>(sm.index_start) + sm.index_count;
        if (end > hdr.index_count) return false;
        mesh.submeshes.push_back(sm);
    }

    assign_slice(mesh.vertex_data, bytes, cursor, vertex_bytes);
    cursor += vertex_bytes;
    assign_slice(mesh.index_data, bytes, cursor, index_bytes);
    return true;
}

}  // namespace lmm

// ─── .lmt ────────────────────────────────────────────────────────────────
namespace lmt {

bool write_texture(std::vector<u8>& out, const Texture& texture) {
    // Mip table sanity: each mip's offset+byte_size lies inside pixel_data.
    for (const auto& mip : texture.mips) {
        const u64 end = static_cast<u64>(mip.offset) + mip.byte_size;
        if (end > texture.pixel_data.size()) return false;
    }
    if (texture.pixel_fmt == formats::LmtPixelFmt::P8) {
        if (texture.palette.size() != 256u * 4u) return false;
    }
    if (texture.mips.size() > 16) return false;     // matches LmtHeader::mip_count

    // The on-disk layout, in order after LmtHeader:
    //   - LmtMip[mip_count] table
    //   - palette (if P8)
    //   - pixel bytes (in mip order; offsets relative to LmtHeader start)
    //
    // We rewrite per-mip offsets so they're file-absolute (relative to
    // LmtHeader start), matching the LmtMip layout in Formats.h.
    const usize mip_table_bytes = texture.mips.size() * sizeof(formats::LmtMip);
    const usize palette_bytes   = texture.palette.size();
    const usize palette_off     = sizeof(formats::LmtHeader) + mip_table_bytes;
    const usize pixels_off      = palette_off + palette_bytes;
    const usize payload_after_header =
        mip_table_bytes + palette_bytes + texture.pixel_data.size();

    formats::LmtHeader hdr{};
    hdr.file.magic        = formats::kLmtMagic;
    hdr.file.version      = formats::kLmtVersion;
    hdr.file.flags        = texture.flags;
    hdr.file.payload_size = payload_after_header;
    hdr.width             = texture.width;
    hdr.height            = texture.height;
    hdr.mip_count         = static_cast<u8>(texture.mips.size());
    hdr.reserved0         = 0;
    hdr.pixel_fmt         = texture.pixel_fmt;
    hdr.palette_offset    = (palette_bytes > 0) ? static_cast<u32>(palette_off) : 0u;
    hdr.pixels_offset     = static_cast<u32>(pixels_off);

    out.reserve(out.size() + sizeof(hdr) + payload_after_header);
    append_bytes(out, hdr);

    for (const auto& mip : texture.mips) {
        formats::LmtMip wire{};
        wire.width     = mip.width;
        wire.height    = mip.height;
        // Translate from pixel_data-relative to LmtHeader-relative.
        wire.offset    = static_cast<u32>(pixels_off + mip.offset);
        wire.byte_size = mip.byte_size;
        append_bytes(out, wire);
    }
    if (!texture.palette.empty()) {
        append_raw(out, texture.palette.data(), texture.palette.size());
    }
    append_raw(out, texture.pixel_data.data(), texture.pixel_data.size());
    return true;
}

bool read_texture(std::span<const u8> bytes, Texture& texture) {
    formats::LmtHeader hdr{};
    if (!read_pod(bytes, 0, hdr)) return false;
    if (hdr.file.magic != formats::kLmtMagic) return false;
    if (hdr.file.version != formats::kLmtVersion) return false;
    if (hdr.mip_count == 0 || hdr.mip_count > 16) return false;

    const usize mip_table_off   = sizeof(hdr);
    const usize mip_table_bytes = static_cast<usize>(hdr.mip_count) * sizeof(formats::LmtMip);
    if (mip_table_off + mip_table_bytes > bytes.size()) return false;

    texture.width     = hdr.width;
    texture.height    = hdr.height;
    texture.pixel_fmt = hdr.pixel_fmt;
    texture.flags     = hdr.file.flags;
    texture.mips.clear();
    texture.mips.reserve(hdr.mip_count);

    // Compute pixel section so we can normalize each mip offset back to
    // pixel_data-relative.
    const u32 pixels_off = hdr.pixels_offset;
    if (pixels_off == 0 || pixels_off > bytes.size()) return false;

    for (u8 i = 0; i < hdr.mip_count; ++i) {
        formats::LmtMip wire{};
        if (!read_pod(bytes, mip_table_off + i * sizeof(wire), wire)) return false;
        if (wire.offset < pixels_off) return false;
        const u64 end = static_cast<u64>(wire.offset) + wire.byte_size;
        if (end > bytes.size()) return false;
        Mip m;
        m.width     = wire.width;
        m.height    = wire.height;
        m.offset    = wire.offset - pixels_off;
        m.byte_size = wire.byte_size;
        texture.mips.push_back(m);
    }

    // Palette is present when palette_offset is non-zero. The block size
    // is fixed at 256 RGBA8 entries.
    texture.palette.clear();
    if (hdr.palette_offset != 0) {
        const usize pal_off = hdr.palette_offset;
        const usize pal_bytes = 256u * 4u;
        if (pal_off + pal_bytes > bytes.size()) return false;
        assign_slice(texture.palette, bytes, pal_off, pal_bytes);
    }

    // Pixel section runs from pixels_off to (last mip end). Take the
    // span that covers every mip block.
    usize pixel_end = pixels_off;
    for (const auto& m : texture.mips) {
        pixel_end = std::max(pixel_end, static_cast<usize>(pixels_off) + m.offset + m.byte_size);
    }
    if (pixel_end > bytes.size()) return false;
    assign_slice(texture.pixel_data, bytes, pixels_off, pixel_end - pixels_off);
    return true;
}

}  // namespace lmt

// ─── .lma ────────────────────────────────────────────────────────────────
namespace lma {

bool write_audio(std::vector<u8>& out, const Audio& audio) {
    if (audio.channels == 0) return false;
    const u32 bps = bytes_per_sample(audio.sample_fmt);
    const usize expect_bytes =
        static_cast<usize>(audio.frame_count) * audio.channels * bps;
    if (audio.pcm_data.size() != expect_bytes) return false;
    // Loop-range sanity (only checked when the loop flag is set so unused
    // loop_start/loop_end fields don't fail the cooker).
    if ((audio.flags & formats::kLmaFlagLoop) != 0) {
        if (audio.loop_end > audio.frame_count) return false;
        if (audio.loop_start > audio.loop_end) return false;
    }

    formats::LmaHeader hdr{};
    hdr.file.magic        = formats::kLmaMagic;
    hdr.file.version      = formats::kLmaVersion;
    hdr.file.flags        = audio.flags;
    hdr.file.payload_size = audio.pcm_data.size();
    hdr.sample_rate       = audio.sample_rate;
    hdr.frame_count       = audio.frame_count;
    hdr.sample_fmt        = audio.sample_fmt;
    hdr.channels          = audio.channels;
    hdr.loop_start        = audio.loop_start;
    hdr.loop_end          = audio.loop_end;

    out.reserve(out.size() + sizeof(hdr) + audio.pcm_data.size());
    append_bytes(out, hdr);
    append_raw(out, audio.pcm_data.data(), audio.pcm_data.size());
    return true;
}

bool read_audio(std::span<const u8> bytes, Audio& audio) {
    formats::LmaHeader hdr{};
    if (!read_pod(bytes, 0, hdr)) return false;
    if (hdr.file.magic != formats::kLmaMagic) return false;
    if (hdr.file.version != formats::kLmaVersion) return false;
    if (hdr.channels == 0) return false;

    const u32 bps = bytes_per_sample(hdr.sample_fmt);
    const usize expect_bytes =
        static_cast<usize>(hdr.frame_count) * hdr.channels * bps;
    if (sizeof(hdr) + expect_bytes > bytes.size()) return false;

    audio.sample_rate = hdr.sample_rate;
    audio.frame_count = hdr.frame_count;
    audio.sample_fmt  = hdr.sample_fmt;
    audio.channels    = hdr.channels;
    audio.flags       = hdr.file.flags;
    audio.loop_start  = hdr.loop_start;
    audio.loop_end    = hdr.loop_end;
    assign_slice(audio.pcm_data, bytes, sizeof(hdr), expect_bytes);
    return true;
}

}  // namespace lma

}  // namespace psynder::asset
