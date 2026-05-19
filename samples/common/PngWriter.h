// SPDX-License-Identifier: MIT
// Psynder — minimal PNG encoder used by sample binaries to write captured
// frames in --smoke-capture-out mode. Header-only, zero deps, deterministic.
//
// The encoder uses uncompressed (stored) deflate blocks inside the zlib
// stream that PNG wraps around IDAT. Output is a fully valid PNG that any
// viewer can open; the file is a few percent larger than a compressed
// equivalent because the size of our captures (640×360 at most) is tiny
// for the rasterizer-test use case the encoder serves. The golden-image
// regression harness (cmake/Goldens.cmake) compares actual vs reference
// at the pixel level, so a non-compressed payload makes no difference to
// the gate beyond on-disk bytes.
//
// Owned by lane 25 (samples-tests). Lives under samples/common/ so all
// three sample binaries we capture from (00_clear, 01_triangle,
// 02_textured_quad) share the same encoder; the cmake helper invokes
// `--smoke-capture-out <path>` on the sample to produce the actual PNG.

#pragma once

#include "core/Types.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace psynder::samples {

namespace png_detail {

// CRC32 (poly 0xEDB88320, reflected) — table on first use.
inline u32 crc32_byte(u32 crc, u8 b) noexcept {
    static u32 table[256];
    static bool inited = false;
    if (!inited) {
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        inited = true;
    }
    return table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
}

inline u32 crc32(const u8* data, usize n, u32 seed = 0xFFFFFFFFu) noexcept {
    u32 crc = seed;
    for (usize i = 0; i < n; ++i) crc = crc32_byte(crc, data[i]);
    return crc;
}

inline u32 adler32(const u8* data, usize n) noexcept {
    u32 a = 1, b = 0;
    for (usize i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a)       % 65521u;
    }
    return (b << 16) | a;
}

inline void put_be32(std::vector<u8>& out, u32 v) noexcept {
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >>  8) & 0xFFu));
    out.push_back(static_cast<u8>( v        & 0xFFu));
}

inline void emit_chunk(std::vector<u8>& out, const char tag[4],
                       const u8* data, usize n) noexcept {
    put_be32(out, static_cast<u32>(n));
    const usize crc_start = out.size();
    out.push_back(static_cast<u8>(tag[0]));
    out.push_back(static_cast<u8>(tag[1]));
    out.push_back(static_cast<u8>(tag[2]));
    out.push_back(static_cast<u8>(tag[3]));
    for (usize i = 0; i < n; ++i) out.push_back(data[i]);
    const u32 crc = crc32(out.data() + crc_start,
                          out.size() - crc_start) ^ 0xFFFFFFFFu;
    put_be32(out, crc);
}

// Wrap raw filtered scanlines in a zlib stream using stored (uncompressed)
// deflate blocks. Each block holds ≤ 65535 bytes.
inline std::vector<u8> zlib_store(const u8* data, usize n) noexcept {
    std::vector<u8> z;
    z.reserve(n + 64);
    // CMF + FLG. Default compression level, no dict; FCHECK chosen so
    // (CMF * 256 + FLG) % 31 == 0. We pick CMF=0x78, FLG=0x01 → 0x7801.
    z.push_back(0x78);
    z.push_back(0x01);

    usize off = 0;
    while (off < n || n == 0) {
        const usize remain = n - off;
        const u16 blk = static_cast<u16>(remain > 65535 ? 65535 : remain);
        const bool last = (static_cast<usize>(blk) == remain);
        z.push_back(last ? 0x01 : 0x00);
        z.push_back(static_cast<u8>(blk & 0xFFu));
        z.push_back(static_cast<u8>((blk >> 8) & 0xFFu));
        const u16 nlen = static_cast<u16>(~blk);
        z.push_back(static_cast<u8>(nlen & 0xFFu));
        z.push_back(static_cast<u8>((nlen >> 8) & 0xFFu));
        for (u16 i = 0; i < blk; ++i) z.push_back(data[off + i]);
        off += blk;
        if (blk == 0) break;  // n == 0 edge case
    }
    const u32 adler = adler32(data, n);
    z.push_back(static_cast<u8>((adler >> 24) & 0xFFu));
    z.push_back(static_cast<u8>((adler >> 16) & 0xFFu));
    z.push_back(static_cast<u8>((adler >>  8) & 0xFFu));
    z.push_back(static_cast<u8>( adler        & 0xFFu));
    return z;
}

}  // namespace png_detail

// Write an RGB8 buffer (w*h*3 bytes, row-major top-to-bottom) as a fully
// valid PNG. Returns false on any I/O error.
inline bool write_png_rgb8(const char* path, const u8* rgb,
                           u32 w, u32 h) noexcept {
    using namespace png_detail;
    if (!path || !rgb || w == 0 || h == 0) return false;

    // Build IDAT payload: one filter byte (0 = None) per scanline + raw RGB.
    const usize row_bytes = static_cast<usize>(w) * 3u;
    std::vector<u8> raw;
    raw.reserve((row_bytes + 1) * h);
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);  // None
        const u8* row = rgb + static_cast<usize>(y) * row_bytes;
        for (usize i = 0; i < row_bytes; ++i) raw.push_back(row[i]);
    }
    const std::vector<u8> idat = zlib_store(raw.data(), raw.size());

    std::vector<u8> out;
    out.reserve(idat.size() + 96);
    // Signature
    const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    for (u8 b : sig) out.push_back(b);

    // IHDR — 13 bytes: width, height, bit_depth=8, color_type=2 (RGB),
    //                   compression=0, filter=0, interlace=0.
    std::vector<u8> ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // color type RGB
    ihdr.push_back(0);  // compression
    ihdr.push_back(0);  // filter
    ihdr.push_back(0);  // interlace
    emit_chunk(out, "IHDR", ihdr.data(), ihdr.size());

    emit_chunk(out, "IDAT", idat.data(), idat.size());
    emit_chunk(out, "IEND", nullptr, 0);

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// Convenience: write an RGBA8 framebuffer (packed in u32, byte order R,G,B,A
// in little-endian — i.e. low 8 bits = R) as RGB PNG (alpha dropped).
inline bool write_png_rgba8_framebuffer(const char* path,
                                        const u32* pixels,
                                        u32 w, u32 h) noexcept {
    if (!path || !pixels || w == 0 || h == 0) return false;
    std::vector<u8> rgb(static_cast<usize>(w) * h * 3u);
    for (usize i = 0, n = static_cast<usize>(w) * h; i < n; ++i) {
        const u32 p = pixels[i];
        rgb[i * 3 + 0] = static_cast<u8>( p        & 0xFFu);
        rgb[i * 3 + 1] = static_cast<u8>((p >>  8) & 0xFFu);
        rgb[i * 3 + 2] = static_cast<u8>((p >> 16) & 0xFFu);
    }
    return write_png_rgb8(path, rgb.data(), w, h);
}

}  // namespace psynder::samples
