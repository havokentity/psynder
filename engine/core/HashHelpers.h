// SPDX-License-Identifier: MIT
// Psynder - small deterministic hash helpers for procedural content.

#pragma once

#include "core/Types.h"

namespace psynder::hash_helpers {

[[nodiscard]] constexpr u32 hash2_u32(u32 x, u32 y, u32 seed = 0u) noexcept {
    u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    h ^= h >> 16u;
    return h;
}

[[nodiscard]] constexpr f32 unit24(u32 hash) noexcept {
    return static_cast<f32>(hash & 0x00FFFFFFu) / static_cast<f32>(0x01000000u);
}

[[nodiscard]] constexpr f32 unit32(u32 hash) noexcept {
    return static_cast<f32>(hash) * (1.0f / 4294967296.0f);
}

[[nodiscard]] constexpr f32 hash2_unit24(u32 x, u32 y, u32 seed = 0u) noexcept {
    return unit24(hash2_u32(x, y, seed));
}

[[nodiscard]] constexpr f32 hash2_unit32(u32 x, u32 y, u32 seed = 0u) noexcept {
    return unit32(hash2_u32(x, y, seed));
}

[[nodiscard]] constexpr u32 murmur_mix2_u32(u32 x, u32 y, u32 seed = 0u) noexcept {
    u32 h = x * 0x27d4eb2du ^ (y * 0x165667b1u + seed * 0x9e3779b9u);
    h ^= h >> 15u;
    h *= 0x85ebca6bu;
    h ^= h >> 13u;
    h *= 0xc2b2ae35u;
    h ^= h >> 16u;
    return h;
}

[[nodiscard]] constexpr f32 murmur_mix2_unit32(u32 x, u32 y, u32 seed = 0u) noexcept {
    return unit32(murmur_mix2_u32(x, y, seed));
}

}  // namespace psynder::hash_helpers
