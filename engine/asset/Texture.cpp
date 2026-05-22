// SPDX-License-Identifier: MIT
// Psynder — async texture asset loading.

#include "Texture.h"

#include "Codecs.h"
#include "Vfs.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace psynder::asset {
namespace {

constexpr u32 kRgba8BytesPerPixel = 4;

bool checked_rgba_mip_size(u32 width, u32 height, usize& out_bytes) noexcept {
    if (width == 0 || height == 0)
        return false;
    const usize w = width;
    const usize h = height;
    constexpr usize kMax = std::numeric_limits<usize>::max();
    if (w > kMax / h)
        return false;
    const usize pixels = w * h;
    if (pixels > kMax / kRgba8BytesPerPixel)
        return false;
    out_bytes = pixels * kRgba8BytesPerPixel;
    return true;
}

bool append_rgba_mip(TextureData& out, u32 width, u32 height, std::span<const u8> rgba) {
    usize need = 0;
    if (!checked_rgba_mip_size(width, height, need) || rgba.size() != need)
        return false;
    TextureMip mip{};
    mip.width = width;
    mip.height = height;
    mip.offset = static_cast<u32>(out.pixels.size());
    mip.byte_size = static_cast<u32>(rgba.size());
    out.mips.push_back(mip);
    out.pixels.insert(out.pixels.end(), rgba.begin(), rgba.end());
    return true;
}

struct PpmCursor {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;
};

bool ppm_space(u8 c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool skip_ppm_space_and_comments(PpmCursor& c) noexcept {
    for (;;) {
        while (c.pos < c.size && ppm_space(c.data[c.pos]))
            ++c.pos;
        if (c.pos >= c.size)
            return false;
        if (c.data[c.pos] != '#')
            return true;
        while (c.pos < c.size && c.data[c.pos] != '\n' && c.data[c.pos] != '\r')
            ++c.pos;
    }
}

bool read_ppm_u32(PpmCursor& c, u32& out) noexcept {
    if (!skip_ppm_space_and_comments(c))
        return false;

    u64 value = 0;
    bool have_digit = false;
    while (c.pos < c.size) {
        const u8 ch = c.data[c.pos];
        if (ch < '0' || ch > '9')
            break;
        have_digit = true;
        value = value * 10u + static_cast<u32>(ch - '0');
        if (value > std::numeric_limits<u32>::max())
            return false;
        ++c.pos;
    }

    if (!have_digit)
        return false;
    out = static_cast<u32>(value);
    return true;
}

bool consume_ppm_raster_separator(PpmCursor& c) noexcept {
    if (c.pos >= c.size || !ppm_space(c.data[c.pos]))
        return false;
    ++c.pos;
    return true;
}

bool decode_ppm_p6(std::span<const u8> bytes, TextureData& out) {
    if (bytes.size() < 3)
        return false;
    PpmCursor c{bytes.data(), bytes.size(), 0};
    if (c.data[c.pos++] != 'P' || c.data[c.pos++] != '6')
        return false;

    u32 width = 0;
    u32 height = 0;
    u32 max_value = 0;
    if (!read_ppm_u32(c, width) || !read_ppm_u32(c, height) || !read_ppm_u32(c, max_value))
        return false;
    if (max_value != 255u || !consume_ppm_raster_separator(c))
        return false;

    usize rgba_bytes = 0;
    if (!checked_rgba_mip_size(width, height, rgba_bytes))
        return false;

    const usize pixel_count = rgba_bytes / kRgba8BytesPerPixel;
    const usize rgb_bytes = pixel_count * 3u;
    if (c.pos > bytes.size() || bytes.size() - c.pos < rgb_bytes)
        return false;

    TextureData tex{};
    tex.width = width;
    tex.height = height;
    tex.pixel_format = TexturePixelFormat::RGBA8;
    tex.color_space = TextureColorSpace::SRGB;
    tex.mips.push_back({width, height, 0u, static_cast<u32>(rgba_bytes)});
    tex.pixels.resize(rgba_bytes);

    const u8* rgb = bytes.data() + c.pos;
    for (usize i = 0; i < pixel_count; ++i) {
        tex.pixels[i * 4u + 0u] = rgb[i * 3u + 0u];
        tex.pixels[i * 4u + 1u] = rgb[i * 3u + 1u];
        tex.pixels[i * 4u + 2u] = rgb[i * 3u + 2u];
        tex.pixels[i * 4u + 3u] = 0xFFu;
    }

    out = std::move(tex);
    return true;
}

u8 expand_5_to_8(u32 v) noexcept {
    return static_cast<u8>((v << 3u) | (v >> 2u));
}

u8 expand_6_to_8(u32 v) noexcept {
    return static_cast<u8>((v << 2u) | (v >> 4u));
}

bool append_lmt_mip_as_rgba8(TextureData& out,
                             const lmt::Texture& source,
                             const lmt::Mip& mip) {
    const usize source_begin = mip.offset;
    const usize source_end = source_begin + mip.byte_size;
    if (source_begin > source.pixel_data.size() || source_end > source.pixel_data.size())
        return false;

    usize rgba_bytes = 0;
    if (!checked_rgba_mip_size(mip.width, mip.height, rgba_bytes))
        return false;
    const usize pixel_count = rgba_bytes / kRgba8BytesPerPixel;
    std::vector<u8> rgba(rgba_bytes);
    const u8* src = source.pixel_data.data() + source_begin;

    switch (source.pixel_fmt) {
        case formats::LmtPixelFmt::RGBA8: {
            if (mip.byte_size != rgba_bytes)
                return false;
            std::copy(src, src + mip.byte_size, rgba.begin());
            break;
        }
        case formats::LmtPixelFmt::P8: {
            if (source.palette.size() != 256u * 4u || mip.byte_size != pixel_count)
                return false;
            for (usize i = 0; i < pixel_count; ++i) {
                const u8 index = src[i];
                const u8* pal = source.palette.data() + static_cast<usize>(index) * 4u;
                rgba[i * 4u + 0u] = pal[0];
                rgba[i * 4u + 1u] = pal[1];
                rgba[i * 4u + 2u] = pal[2];
                rgba[i * 4u + 3u] = pal[3];
            }
            break;
        }
        case formats::LmtPixelFmt::RGB565: {
            if (mip.byte_size != pixel_count * 2u)
                return false;
            for (usize i = 0; i < pixel_count; ++i) {
                const u32 packed = static_cast<u32>(src[i * 2u + 0u]) |
                                   (static_cast<u32>(src[i * 2u + 1u]) << 8u);
                rgba[i * 4u + 0u] = expand_5_to_8((packed >> 11u) & 0x1Fu);
                rgba[i * 4u + 1u] = expand_6_to_8((packed >> 5u) & 0x3Fu);
                rgba[i * 4u + 2u] = expand_5_to_8(packed & 0x1Fu);
                rgba[i * 4u + 3u] = 0xFFu;
            }
            break;
        }
        case formats::LmtPixelFmt::RG88: {
            if (mip.byte_size != pixel_count * 2u)
                return false;
            for (usize i = 0; i < pixel_count; ++i) {
                rgba[i * 4u + 0u] = src[i * 2u + 0u];
                rgba[i * 4u + 1u] = src[i * 2u + 1u];
                rgba[i * 4u + 2u] = 0u;
                rgba[i * 4u + 3u] = 0xFFu;
            }
            break;
        }
    }

    return append_rgba_mip(out, mip.width, mip.height, rgba);
}

bool decode_lmt(std::span<const u8> bytes, TextureData& out) {
    lmt::Texture source{};
    if (!lmt::read_texture(bytes, source))
        return false;
    if (source.mips.empty())
        return false;

    TextureData tex{};
    tex.width = source.width;
    tex.height = source.height;
    tex.pixel_format = TexturePixelFormat::RGBA8;
    tex.color_space =
        (source.flags & formats::kLmtFlagSRGB) ? TextureColorSpace::SRGB : TextureColorSpace::Linear;
    tex.mips.reserve(source.mips.size());

    for (const lmt::Mip& mip : source.mips) {
        if (!append_lmt_mip_as_rgba8(tex, source, mip))
            return false;
    }

    out = std::move(tex);
    return true;
}

bool has_magic(std::span<const u8> bytes, u32 magic) noexcept {
    if (bytes.size() < sizeof(u32))
        return false;
    const u32 found = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8u) |
                      (static_cast<u32>(bytes[2]) << 16u) |
                      (static_cast<u32>(bytes[3]) << 24u);
    return found == magic;
}

bool decode_auto(std::span<const u8> bytes, TextureData& out) {
    if (bytes.size() >= 2 && bytes[0] == 'P' && bytes[1] == '6')
        return decode_ppm_p6(bytes, out);
    if (has_magic(bytes, formats::kLmtMagic))
        return decode_lmt(bytes, out);
    return false;
}

}  // namespace

namespace texture_detail {

struct TextureLoadState {
    std::atomic<bool> done{false};
    TextureLoadStatus status = TextureLoadStatus::Pending;
    TextureData texture;
    std::atomic<bool> consumed{false};
};

struct TextureLoadPayload {
    std::shared_ptr<TextureLoadState> state;
    TextureSourceFormat source_format = TextureSourceFormat::Auto;
    TextureLoadCallback callback = nullptr;
    void* user = nullptr;
};

void on_blob_loaded(Blob blob, void* user) noexcept {
    std::unique_ptr<TextureLoadPayload> payload(static_cast<TextureLoadPayload*>(user));

    TextureData decoded{};
    const bool ok = blob.data && blob.bytes != 0 &&
                    decode_texture_rgba8({blob.data, blob.bytes}, payload->source_format, decoded);
    payload->state->texture = ok ? std::move(decoded) : TextureData{};
    payload->state->status = ok ? TextureLoadStatus::Ready : TextureLoadStatus::Failed;
    payload->state->done.store(true, std::memory_order_release);

    if (payload->callback) {
        payload->callback(payload->state->status, payload->state->texture, payload->user);
    }
}

}  // namespace texture_detail

bool TextureLoad::ready() const noexcept {
    return state_ && state_->done.load(std::memory_order_acquire);
}

TextureLoadStatus TextureLoad::status() const noexcept {
    if (!state_)
        return TextureLoadStatus::Failed;
    if (!state_->done.load(std::memory_order_acquire))
        return TextureLoadStatus::Pending;
    return state_->status;
}

const TextureData* TextureLoad::texture() const noexcept {
    return ok() ? &state_->texture : nullptr;
}

bool TextureLoad::take_if_ready(TextureData& out) noexcept {
    if (!state_ || !state_->done.load(std::memory_order_acquire) ||
        state_->status != TextureLoadStatus::Ready) {
        return false;
    }
    bool expected = false;
    if (!state_->consumed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    out = std::move(state_->texture);
    return true;
}

TextureLoad load_texture_async(TextureLoadDesc desc, TextureLoadCallback callback, void* user) {
    auto state = std::make_shared<texture_detail::TextureLoadState>();
    auto* payload =
        new texture_detail::TextureLoadPayload{state, desc.source_format, callback, user};
    Vfs::Get().read_async(desc.virtual_path, &texture_detail::on_blob_loaded, payload);
    return TextureLoad{std::move(state)};
}

bool decode_ppm_rgba8(std::span<const u8> bytes, TextureData& out) noexcept {
    try {
        return decode_ppm_p6(bytes, out);
    } catch (...) {
        return false;
    }
}

bool decode_lmt_rgba8(std::span<const u8> bytes, TextureData& out) noexcept {
    try {
        return decode_lmt(bytes, out);
    } catch (...) {
        return false;
    }
}

bool decode_texture_rgba8(std::span<const u8> bytes,
                          TextureSourceFormat source_format,
                          TextureData& out) noexcept {
    try {
        switch (source_format) {
            case TextureSourceFormat::Auto:
                return decode_auto(bytes, out);
            case TextureSourceFormat::PpmP6:
                return decode_ppm_p6(bytes, out);
            case TextureSourceFormat::Lmt:
                return decode_lmt(bytes, out);
        }
        return false;
    } catch (...) {
        return false;
    }
}

}  // namespace psynder::asset
