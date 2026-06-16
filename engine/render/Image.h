// SPDX-License-Identifier: MIT
// Psynder — small CPU image helpers for runtime/sample tools.

#pragma once

#include "core/Types.h"

#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace psynder::render {

struct Rgba8Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;  // byte layout in memory: R, G, B, A
};

namespace image_detail {

constexpr int kPpmEof = -1;

inline bool is_ppm_space(int c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool checked_ppm_sizes(u32 width, u32 height, usize& pixel_count, usize& rgb_bytes) noexcept {
    if (width == 0 || height == 0)
        return false;

    constexpr usize kMax = std::numeric_limits<usize>::max();
    const usize w = width;
    const usize h = height;
    if (w > kMax / h)
        return false;
    pixel_count = w * h;
    if (pixel_count > kMax / 3u)
        return false;
    rgb_bytes = pixel_count * 3u;
    if (pixel_count > kMax / sizeof(u32))
        return false;
    return true;
}

struct PpmMemoryReader {
    const u8* data = nullptr;
    usize bytes = 0;
    usize cursor = 0;

    int get() noexcept {
        if (cursor >= bytes)
            return kPpmEof;
        return data[cursor++];
    }

    bool unget() noexcept {
        if (cursor == 0)
            return false;
        --cursor;
        return true;
    }
};

inline bool skip_ppm_space_and_comments(PpmMemoryReader& r) noexcept {
    for (;;) {
        int c = r.get();
        while (is_ppm_space(c))
            c = r.get();

        if (c == '#') {
            do {
                c = r.get();
            } while (c != '\n' && c != '\r' && c != kPpmEof);
            continue;
        }

        if (c == kPpmEof)
            return false;
        return r.unget();
    }
}

inline bool read_ppm_u32(PpmMemoryReader& r, u32& out) noexcept {
    if (!skip_ppm_space_and_comments(r))
        return false;

    u64 value = 0;
    bool have_digit = false;
    for (;;) {
        const int c = r.get();
        if (c < '0' || c > '9') {
            if (c != kPpmEof && !r.unget())
                return false;
            break;
        }
        have_digit = true;
        value = value * 10u + static_cast<u32>(c - '0');
        if (value > std::numeric_limits<u32>::max())
            return false;
    }

    if (!have_digit)
        return false;
    out = static_cast<u32>(value);
    return true;
}

inline bool consume_ppm_raster_separator(PpmMemoryReader& r) noexcept {
    return is_ppm_space(r.get());
}

inline bool decode_ppm_rgba8(std::span<const u8> bytes, Rgba8Image& out) noexcept {
    try {
        PpmMemoryReader r{bytes.data(), bytes.size(), 0};
        if (r.get() != 'P' || r.get() != '6')
            return false;

        u32 width = 0;
        u32 height = 0;
        u32 max_value = 0;
        if (!read_ppm_u32(r, width) || !read_ppm_u32(r, height) ||
            !read_ppm_u32(r, max_value) || max_value != 255u ||
            !consume_ppm_raster_separator(r)) {
            return false;
        }

        usize pixel_count = 0;
        usize rgb_bytes = 0;
        if (!checked_ppm_sizes(width, height, pixel_count, rgb_bytes))
            return false;
        if (r.cursor > bytes.size() || bytes.size() - r.cursor < rgb_bytes)
            return false;

        const u8* rgb = bytes.data() + r.cursor;
        Rgba8Image image{};
        image.width = width;
        image.height = height;
        image.pixels.resize(pixel_count);
        for (usize i = 0; i < pixel_count; ++i) {
            const u32 rr = rgb[i * 3u + 0u];
            const u32 gg = rgb[i * 3u + 1u];
            const u32 bb = rgb[i * 3u + 2u];
            image.pixels[i] = rr | (gg << 8) | (bb << 16) | (0xFFu << 24);
        }
        out = std::move(image);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace image_detail

}  // namespace psynder::render
