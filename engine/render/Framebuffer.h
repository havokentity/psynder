// SPDX-License-Identifier: MIT
// Psynder — framebuffer / render target. Used by the rasterizer (lane 07),
// the platform layer (21/22/23), the resolve/post pass (lane 09), and the
// editor in-viewport overlay (lane 16).
//
// The framebuffer lives in CPU memory at the fixed internal resolution
// (DESIGN.md §7.9). The platform present blits this texture to the window
// with the chosen scale mode.

#pragma once

#include "core/Types.h"

#include <algorithm>

namespace psynder::render {

enum class PixelFormat : u32 {
    RGBA8,      // 32-bit, default
    BGRA8,      // 32-bit, alternative byte order (some platforms prefer)
    RGB565,     // 16-bit retro
    Paletted8,  // 8-bit + 256 palette
};

struct Framebuffer {
    u32 width = 0;
    u32 height = 0;
    PixelFormat format = PixelFormat::RGBA8;
    u32 pitch = 0;  // bytes per row
    u8* pixels = nullptr;

    // 24-bit float Z + 8-bit stencil interleaved; width*height entries
    u32* depth = nullptr;

    constexpr usize byte_size() const noexcept { return static_cast<usize>(pitch) * height; }
};

// Clear CPU framebuffer storage without taking a dependency on a concrete
// renderer. Depth clears to far (1.0) with stencil 0 when present.
inline void clear_framebuffer_color(Framebuffer& fb, u32 rgba) noexcept {
    if (!fb.pixels || fb.width == 0 || fb.height == 0)
        return;
    if (fb.format == PixelFormat::RGBA8 || fb.format == PixelFormat::BGRA8) {
        for (u32 y = 0; y < fb.height; ++y) {
            auto* row = reinterpret_cast<u32*>(fb.pixels + static_cast<usize>(fb.pitch) * y);
            std::fill_n(row, fb.width, rgba);
        }
    }
}

inline void clear_framebuffer_depth(Framebuffer& fb) noexcept {
    if (fb.depth) {
        constexpr u32 kPackedDepthFarStencilZero = 0x3F800000u;
        const usize dcount = static_cast<usize>(fb.width) * fb.height;
        std::fill_n(fb.depth, dcount, kPackedDepthFarStencilZero);
    }
}

inline void clear_framebuffer(Framebuffer& fb, u32 rgba) noexcept {
    clear_framebuffer_color(fb, rgba);
    clear_framebuffer_depth(fb);
}

// Lane 07/09 fills these; the platform layer reads them.
struct PresentRequest {
    const Framebuffer* fb = nullptr;
    u32 window_w = 0;
    u32 window_h = 0;
    bool vsync = true;
};

}  // namespace psynder::render
