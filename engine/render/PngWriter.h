// SPDX-License-Identifier: MIT
// Psynder — minimal deterministic PNG encoder for runtime framebuffer captures.

#pragma once

#include "core/Types.h"

#include <array>
#include <cstdio>
#include <vector>

namespace psynder::render {

namespace png_detail {

constexpr u32 crc32_table_entry(u32 n) noexcept {
    u32 c = n;
    for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

constexpr std::array<u32, 256> make_crc32_table() noexcept {
    std::array<u32, 256> table{};
    for (u32 n = 0; n < table.size(); ++n) {
        table[n] = crc32_table_entry(n);
    }
    return table;
}

inline constexpr std::array<u32, 256> kCrc32Table = make_crc32_table();

inline u32 crc32_byte(u32 crc, u8 b) noexcept {
    return kCrc32Table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
}

inline u32 crc32(const u8* data, usize n, u32 seed = 0xFFFFFFFFu) noexcept {
    u32 crc = seed;
    for (usize i = 0; i < n; ++i)
        crc = crc32_byte(crc, data[i]);
    return crc;
}

inline u32 adler32(const u8* data, usize n) noexcept {
    u32 a = 1;
    u32 b = 0;
    for (usize i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

inline void put_be32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>((v >> 24) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    out.push_back(static_cast<u8>(v & 0xFFu));
}

inline void emit_chunk(std::vector<u8>& out, const char tag[4], const u8* data, usize n) {
    put_be32(out, static_cast<u32>(n));
    const usize crc_start = out.size();
    out.push_back(static_cast<u8>(tag[0]));
    out.push_back(static_cast<u8>(tag[1]));
    out.push_back(static_cast<u8>(tag[2]));
    out.push_back(static_cast<u8>(tag[3]));
    for (usize i = 0; i < n; ++i)
        out.push_back(data[i]);
    const u32 crc = crc32(out.data() + crc_start, out.size() - crc_start) ^ 0xFFFFFFFFu;
    put_be32(out, crc);
}

inline std::vector<u8> zlib_store(const u8* data, usize n) {
    std::vector<u8> z;
    z.reserve(n + 64);
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
        for (u16 i = 0; i < blk; ++i)
            z.push_back(data[off + i]);
        off += blk;
        if (blk == 0)
            break;
    }
    const u32 adler = adler32(data, n);
    z.push_back(static_cast<u8>((adler >> 24) & 0xFFu));
    z.push_back(static_cast<u8>((adler >> 16) & 0xFFu));
    z.push_back(static_cast<u8>((adler >> 8) & 0xFFu));
    z.push_back(static_cast<u8>(adler & 0xFFu));
    return z;
}

}  // namespace png_detail

inline bool write_png_rgb8(const char* path, const u8* rgb, u32 w, u32 h) noexcept {
    try {
        if (!path || !rgb || w == 0 || h == 0)
            return false;

        const usize row_bytes = static_cast<usize>(w) * 3u;
        std::vector<u8> raw;
        raw.reserve((row_bytes + 1u) * h);
        for (u32 y = 0; y < h; ++y) {
            raw.push_back(0);
            const u8* row = rgb + static_cast<usize>(y) * row_bytes;
            for (usize i = 0; i < row_bytes; ++i)
                raw.push_back(row[i]);
        }
        const std::vector<u8> idat = png_detail::zlib_store(raw.data(), raw.size());

        std::vector<u8> out;
        out.reserve(idat.size() + 96);
        const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        for (u8 b : sig)
            out.push_back(b);

        std::vector<u8> ihdr;
        png_detail::put_be32(ihdr, w);
        png_detail::put_be32(ihdr, h);
        ihdr.push_back(8);
        ihdr.push_back(2);
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        png_detail::emit_chunk(out, "IHDR", ihdr.data(), ihdr.size());
        png_detail::emit_chunk(out, "IDAT", idat.data(), idat.size());
        png_detail::emit_chunk(out, "IEND", nullptr, 0);

        std::FILE* f = std::fopen(path, "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    } catch (...) {
        return false;
    }
}

inline bool write_png_rgba8_framebuffer(const char* path, const u32* pixels, u32 w, u32 h) noexcept {
    try {
        if (!path || !pixels || w == 0 || h == 0)
            return false;
        std::vector<u8> rgb(static_cast<usize>(w) * h * 3u);
        for (usize i = 0, n = static_cast<usize>(w) * h; i < n; ++i) {
            const u32 p = pixels[i];
            rgb[i * 3 + 0] = static_cast<u8>(p & 0xFFu);
            rgb[i * 3 + 1] = static_cast<u8>((p >> 8) & 0xFFu);
            rgb[i * 3 + 2] = static_cast<u8>((p >> 16) & 0xFFu);
        }
        return write_png_rgb8(path, rgb.data(), w, h);
    } catch (...) {
        return false;
    }
}

}  // namespace psynder::render
