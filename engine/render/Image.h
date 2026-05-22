// SPDX-License-Identifier: MIT
// Psynder — small CPU image helpers for runtime/sample tools.

#pragma once

#include "core/Types.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace psynder::render {

struct Rgba8Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;  // byte layout in memory: R, G, B, A
};

namespace image_detail {

inline bool is_ppm_space(int c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool skip_ppm_space_and_comments(std::FILE* f) noexcept {
    for (;;) {
        int c = std::fgetc(f);
        while (is_ppm_space(c))
            c = std::fgetc(f);

        if (c == '#') {
            do {
                c = std::fgetc(f);
            } while (c != '\n' && c != '\r' && c != EOF);
            continue;
        }

        if (c == EOF)
            return false;
        return std::ungetc(c, f) != EOF;
    }
}

inline bool read_ppm_u32(std::FILE* f, u32& out) noexcept {
    if (!skip_ppm_space_and_comments(f))
        return false;

    u64 value = 0;
    bool have_digit = false;
    for (;;) {
        const int c = std::fgetc(f);
        if (c < '0' || c > '9') {
            if (c != EOF && std::ungetc(c, f) == EOF)
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

inline bool consume_ppm_raster_separator(std::FILE* f) noexcept {
    const int c = std::fgetc(f);
    return is_ppm_space(c);
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

// Worker-side file loader. This intentionally lives in detail: runtime callers
// should use async asset requests from Texture.h instead of blocking on I/O.
inline bool load_ppm_rgba8_from_file_blocking(const char* path, Rgba8Image& out) noexcept {
    try {
        if (!path)
            return false;

        using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;
        FilePtr f(std::fopen(path, "rb"), &std::fclose);
        if (!f)
            return false;

        const int p = std::fgetc(f.get());
        const int six = std::fgetc(f.get());
        if (p != 'P' || six != '6')
            return false;

        u32 width = 0;
        u32 height = 0;
        u32 max_value = 0;
        if (!read_ppm_u32(f.get(), width) || !read_ppm_u32(f.get(), height) ||
            !read_ppm_u32(f.get(), max_value) || max_value != 255u ||
            !consume_ppm_raster_separator(f.get())) {
            return false;
        }

        usize pixel_count = 0;
        usize rgb_bytes = 0;
        if (!checked_ppm_sizes(width, height, pixel_count, rgb_bytes))
            return false;

        std::vector<u8> rgb(rgb_bytes);
        if (std::fread(rgb.data(), 1, rgb.size(), f.get()) != rgb.size())
            return false;

        Rgba8Image image{};
        image.width = width;
        image.height = height;
        image.pixels.resize(pixel_count);
        for (usize i = 0; i < pixel_count; ++i) {
            const u32 r = rgb[i * 3u + 0u];
            const u32 g = rgb[i * 3u + 1u];
            const u32 b = rgb[i * 3u + 2u];
            image.pixels[i] = r | (g << 8) | (b << 16) | (0xFFu << 24);
        }
        out = std::move(image);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace image_detail

}  // namespace psynder::render
