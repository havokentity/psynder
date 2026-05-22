// SPDX-License-Identifier: MIT
// Psynder — renderer-neutral packed colour helpers.

#pragma once

#include "core/Types.h"

namespace psynder::render {

// Packed RGBA8 with R in the low byte. On little-endian hosts the framebuffer
// bytes land as [R, G, B, A], matching PixelFormat::RGBA8.
inline constexpr u32 rgba8(u8 r, u8 g, u8 b, u8 a = 0xFFu) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8u) |
           (static_cast<u32>(b) << 16u) | (static_cast<u32>(a) << 24u);
}

}  // namespace psynder::render
