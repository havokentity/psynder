// SPDX-License-Identifier: MIT
// Psynder — cooked-asset binary writers / readers. Lane 24 / tools.

#include "CookFormats.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psynder::tools::cook {

namespace {

template <class T>
void append_le(std::vector<u8>& out, T value) {
    static_assert(std::is_integral_v<T>, "append_le expects integral");
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}

void append_f32(std::vector<u8>& out, f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_le<u32>(out, bits);
}

template <class T>
bool read_le(std::span<const u8> bytes, usize off, T& out) {
    if (off + sizeof(T) > bytes.size())
        return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        u |= static_cast<U>(bytes[off + i]) << (8 * i);
    }
    out = static_cast<T>(u);
    return true;
}

bool read_f32(std::span<const u8> bytes, usize off, f32& out) {
    u32 bits = 0;
    if (!read_le<u32>(bytes, off, bits))
        return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

}  // namespace

// ─── .lmm ────────────────────────────────────────────────────────────────

void write_lmm(const LmmMesh& mesh, std::vector<u8>& out) {
    out.clear();
    out.reserve(64 + mesh.vertices.size() * sizeof(LmmVertex) + mesh.indices.size() * 4);

    // Material name pool, computed first so we can stamp offsets in the header.
    std::vector<u8> mat_pool;
    std::vector<u32> mat_offsets;
    mat_offsets.reserve(mesh.materials.size());
    for (const auto& name : mesh.materials) {
        mat_offsets.push_back(static_cast<u32>(mat_pool.size()));
        mat_pool.insert(mat_pool.end(), name.begin(), name.end());
        mat_pool.push_back(0);  // NUL terminator for forward-compat
    }

    // Header.
    append_le<u32>(out, kLmmMagic);
    append_le<u32>(out, kLmmVersion);
    append_le<u32>(out, static_cast<u32>(mesh.vertices.size()));
    append_le<u32>(out, static_cast<u32>(mesh.indices.size()));
    append_le<u32>(out, static_cast<u32>(mesh.submeshes.size()));
    // material_name_offset / bytes — patched after we know where the pool lands.
    usize mat_off_slot = out.size();
    append_le<u32>(out, 0);
    append_le<u32>(out, static_cast<u32>(mat_pool.size()));
    append_le<u32>(out, 0);  // flags

    // Vertex array.
    for (const auto& v : mesh.vertices) {
        append_f32(out, v.px);
        append_f32(out, v.py);
        append_f32(out, v.pz);
        append_f32(out, v.nx);
        append_f32(out, v.ny);
        append_f32(out, v.nz);
        append_f32(out, v.u);
        append_f32(out, v.v);
        append_f32(out, v.tx);
        append_f32(out, v.ty);
        append_f32(out, v.tz);
        append_f32(out, v.tw);
    }
    // Index array.
    for (u32 ix : mesh.indices) {
        append_le<u32>(out, ix);
    }
    // Submesh array.
    for (const auto& sm : mesh.submeshes) {
        append_le<u32>(out, sm.first_index);
        append_le<u32>(out, sm.index_count);
        append_le<u32>(out, sm.material_index);
        append_le<u32>(out, sm.reserved);
    }
    // Patch the material pool offset, then emit the pool.
    u32 mat_offset = static_cast<u32>(out.size());
    std::memcpy(out.data() + mat_off_slot, &mat_offset, sizeof(mat_offset));
    out.insert(out.end(), mat_pool.begin(), mat_pool.end());
}

bool read_lmm(std::span<const u8> bytes, LmmMesh& out, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };

    if (bytes.size() < 8 * 4)
        return fail("lmm header truncated");
    u32 magic = 0;
    read_le<u32>(bytes, 0, magic);
    if (magic != kLmmMagic)
        return fail("lmm bad magic");
    u32 version = 0;
    read_le<u32>(bytes, 4, version);
    if (version != kLmmVersion)
        return fail("lmm unsupported version");

    u32 vcount = 0, icount = 0, scount = 0, mat_off = 0, mat_bytes = 0, flags = 0;
    read_le<u32>(bytes, 8, vcount);
    read_le<u32>(bytes, 12, icount);
    read_le<u32>(bytes, 16, scount);
    read_le<u32>(bytes, 20, mat_off);
    read_le<u32>(bytes, 24, mat_bytes);
    read_le<u32>(bytes, 28, flags);
    (void)flags;

    usize cursor = 32;
    out.vertices.clear();
    out.vertices.reserve(vcount);
    constexpr usize kVtxBytes = 12 * 4;
    if (cursor + vcount * kVtxBytes > bytes.size())
        return fail("lmm vertices truncated");
    for (u32 i = 0; i < vcount; ++i) {
        LmmVertex v;
        read_f32(bytes, cursor + 0, v.px);
        read_f32(bytes, cursor + 4, v.py);
        read_f32(bytes, cursor + 8, v.pz);
        read_f32(bytes, cursor + 12, v.nx);
        read_f32(bytes, cursor + 16, v.ny);
        read_f32(bytes, cursor + 20, v.nz);
        read_f32(bytes, cursor + 24, v.u);
        read_f32(bytes, cursor + 28, v.v);
        read_f32(bytes, cursor + 32, v.tx);
        read_f32(bytes, cursor + 36, v.ty);
        read_f32(bytes, cursor + 40, v.tz);
        read_f32(bytes, cursor + 44, v.tw);
        cursor += kVtxBytes;
        out.vertices.push_back(v);
    }
    out.indices.clear();
    out.indices.reserve(icount);
    if (cursor + icount * 4 > bytes.size())
        return fail("lmm indices truncated");
    for (u32 i = 0; i < icount; ++i) {
        u32 ix = 0;
        read_le<u32>(bytes, cursor, ix);
        out.indices.push_back(ix);
        cursor += 4;
    }
    out.submeshes.clear();
    out.submeshes.reserve(scount);
    constexpr usize kSmBytes = 4 * 4;
    if (cursor + scount * kSmBytes > bytes.size())
        return fail("lmm submeshes truncated");
    for (u32 i = 0; i < scount; ++i) {
        LmmSubmesh sm{};
        read_le<u32>(bytes, cursor + 0, sm.first_index);
        read_le<u32>(bytes, cursor + 4, sm.index_count);
        read_le<u32>(bytes, cursor + 8, sm.material_index);
        read_le<u32>(bytes, cursor + 12, sm.reserved);
        cursor += kSmBytes;
        out.submeshes.push_back(sm);
    }
    out.materials.clear();
    if (mat_bytes > 0) {
        if (static_cast<usize>(mat_off) + mat_bytes > bytes.size())
            return fail("lmm material pool truncated");
        usize pos = mat_off;
        usize end = mat_off + mat_bytes;
        std::string current;
        while (pos < end) {
            char c = static_cast<char>(bytes[pos++]);
            if (c == 0) {
                out.materials.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty())
            out.materials.push_back(current);
    }
    return true;
}

// ─── .lmt ────────────────────────────────────────────────────────────────

LmtTexture build_mipchain_rgba8(const u8* base, u32 width, u32 height) {
    LmtTexture tex;
    tex.format = CookTexFormat::kRgba8;
    tex.width = width;
    tex.height = height;
    if (width == 0 || height == 0)
        return tex;

    u32 mip_w = width;
    u32 mip_h = height;
    std::vector<u8> current(base, base + static_cast<usize>(width) * height * 4);

    while (true) {
        LmtMipDesc md;
        md.width = mip_w;
        md.height = mip_h;
        md.offset = static_cast<u32>(tex.pixels.size());
        md.bytes = static_cast<u32>(current.size());
        tex.mips.push_back(md);
        tex.pixels.insert(tex.pixels.end(), current.begin(), current.end());

        if (mip_w == 1 && mip_h == 1)
            break;
        u32 nw = std::max(1u, mip_w / 2u);
        u32 nh = std::max(1u, mip_h / 2u);
        std::vector<u8> next(static_cast<usize>(nw) * nh * 4u);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                u32 sx = x * 2u, sy = y * 2u;
                // 2x2 box filter; clamp at edges if mip is asymmetric.
                u32 x1 = std::min(sx + 1u, mip_w - 1u);
                u32 y1 = std::min(sy + 1u, mip_h - 1u);
                auto fetch = [&](u32 px, u32 py, int ch) {
                    return current[(py * mip_w + px) * 4u + ch];
                };
                for (int ch = 0; ch < 4; ++ch) {
                    u32 sum =
                        static_cast<u32>(fetch(sx, sy, ch)) + static_cast<u32>(fetch(x1, sy, ch)) +
                        static_cast<u32>(fetch(sx, y1, ch)) + static_cast<u32>(fetch(x1, y1, ch));
                    next[(y * nw + x) * 4u + ch] = static_cast<u8>((sum + 2u) / 4u);
                }
            }
        }
        current.swap(next);
        mip_w = nw;
        mip_h = nh;
    }
    return tex;
}

void write_lmt(const LmtTexture& tex, std::vector<u8>& out) {
    out.clear();
    out.reserve(48 + tex.pixels.size());
    append_le<u32>(out, kLmtMagic);
    append_le<u32>(out, kLmtVersion);
    append_le<u32>(out, static_cast<u32>(tex.format));
    append_le<u32>(out, tex.width);
    append_le<u32>(out, tex.height);
    append_le<u32>(out, static_cast<u32>(tex.mips.size()));
    append_le<u32>(out, 0);  // flags

    for (const auto& md : tex.mips) {
        append_le<u32>(out, md.width);
        append_le<u32>(out, md.height);
        append_le<u32>(out, md.offset);
        append_le<u32>(out, md.bytes);
    }
    out.insert(out.end(), tex.pixels.begin(), tex.pixels.end());
}

bool read_lmt(std::span<const u8> bytes, LmtTexture& out, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (bytes.size() < 28)
        return fail("lmt header truncated");
    u32 magic = 0;
    read_le<u32>(bytes, 0, magic);
    if (magic != kLmtMagic)
        return fail("lmt bad magic");
    u32 version = 0;
    read_le<u32>(bytes, 4, version);
    if (version != kLmtVersion)
        return fail("lmt unsupported version");
    u32 fmt = 0;
    read_le<u32>(bytes, 8, fmt);
    out.format = static_cast<CookTexFormat>(fmt);
    read_le<u32>(bytes, 12, out.width);
    read_le<u32>(bytes, 16, out.height);
    u32 mip_count = 0;
    read_le<u32>(bytes, 20, mip_count);
    // flags @ 24

    usize cursor = 28;
    out.mips.clear();
    out.mips.reserve(mip_count);
    if (cursor + mip_count * 16 > bytes.size())
        return fail("lmt mip table truncated");
    for (u32 i = 0; i < mip_count; ++i) {
        LmtMipDesc md;
        read_le<u32>(bytes, cursor + 0, md.width);
        read_le<u32>(bytes, cursor + 4, md.height);
        read_le<u32>(bytes, cursor + 8, md.offset);
        read_le<u32>(bytes, cursor + 12, md.bytes);
        out.mips.push_back(md);
        cursor += 16;
    }
    if (cursor > bytes.size())
        return fail("lmt payload truncated");
    auto payload = bytes.subspan(cursor);
    out.pixels.assign(payload.begin(), payload.end());
    return true;
}

// ─── .lma ────────────────────────────────────────────────────────────────

void write_lma(const LmaAudio& audio, std::vector<u8>& out) {
    out.clear();
    out.reserve(32 + audio.samples.size());
    append_le<u32>(out, kLmaMagic);
    append_le<u32>(out, kLmaVersion);
    append_le<u32>(out, audio.channels);
    append_le<u32>(out, audio.sample_rate);
    append_le<u32>(out, audio.sample_count);
    append_le<u32>(out, audio.bits_per_sample);
    append_le<u32>(out, audio.is_float ? 1u : 0u);
    append_le<u32>(out, 0);  // reserved
    out.insert(out.end(), audio.samples.begin(), audio.samples.end());
}

bool read_lma(std::span<const u8> bytes, LmaAudio& out, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (bytes.size() < 32)
        return fail("lma header truncated");
    u32 magic = 0;
    read_le<u32>(bytes, 0, magic);
    if (magic != kLmaMagic)
        return fail("lma bad magic");
    u32 version = 0;
    read_le<u32>(bytes, 4, version);
    if (version != kLmaVersion)
        return fail("lma unsupported version");
    read_le<u32>(bytes, 8, out.channels);
    read_le<u32>(bytes, 12, out.sample_rate);
    read_le<u32>(bytes, 16, out.sample_count);
    read_le<u32>(bytes, 20, out.bits_per_sample);
    u32 flags = 0;
    read_le<u32>(bytes, 24, flags);
    out.is_float = (flags & 1u) != 0;
    auto payload = bytes.subspan(32);
    out.samples.assign(payload.begin(), payload.end());
    return true;
}

}  // namespace psynder::tools::cook
