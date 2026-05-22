// SPDX-License-Identifier: MIT
// Psynder — scene-owned hot runtime packets consumed by render/app systems.

#pragma once

#include "core/Types.h"

#include <array>

namespace psynder::scene {

enum class EnvironmentClearFlags : u8 {
    None = 0u,
    Color = 1u << 0,
    Depth = 1u << 1,
};

[[nodiscard]] constexpr u8 environment_clear_flags_bits(EnvironmentClearFlags flags) noexcept {
    return static_cast<u8>(flags);
}

[[nodiscard]] constexpr EnvironmentClearFlags operator|(EnvironmentClearFlags a,
                                                        EnvironmentClearFlags b) noexcept {
    return static_cast<EnvironmentClearFlags>(environment_clear_flags_bits(a) |
                                              environment_clear_flags_bits(b));
}

[[nodiscard]] constexpr u8 operator&(EnvironmentClearFlags a,
                                     EnvironmentClearFlags b) noexcept {
    return environment_clear_flags_bits(a) & environment_clear_flags_bits(b);
}

constexpr EnvironmentClearFlags& operator|=(EnvironmentClearFlags& a,
                                            EnvironmentClearFlags b) noexcept {
    a = a | b;
    return a;
}

struct PSY_CACHELINE_ALIGN EnvironmentRuntimeSoA {
    static constexpr u32 kMaxEnvironments = 1u;

    std::array<EnvironmentClearFlags, kMaxEnvironments> clear_flags{
        EnvironmentClearFlags::Color | EnvironmentClearFlags::Depth};
    std::array<u32, kMaxEnvironments> clear_color_rgba8{0xFF000000u};

    std::array<u32, kMaxEnvironments> sky_zenith_rgba8{0xFF101828u};
    std::array<u32, kMaxEnvironments> sky_horizon_rgba8{0xFF24364Cu};
    std::array<f32, kMaxEnvironments> sky_intensity{1.0f};

    std::array<u8, kMaxEnvironments> cloud_enabled{0u};
    std::array<f32, kMaxEnvironments> cloud_coverage{0.0f};
    std::array<f32, kMaxEnvironments> cloud_density{1.0f};
    std::array<f32, kMaxEnvironments> cloud_height{1200.0f};
    std::array<f32, kMaxEnvironments> cloud_wind_x{0.0f};
    std::array<f32, kMaxEnvironments> cloud_wind_y{0.0f};

    [[nodiscard]] bool clear_enabled(u32 slot = 0u) const noexcept {
        return clear_flags[slot] != EnvironmentClearFlags::None;
    }
};

struct PSY_CACHELINE_ALIGN SceneRuntime {
    EnvironmentRuntimeSoA environment{};
};

static_assert(alignof(EnvironmentRuntimeSoA) == kCacheLine);
static_assert(alignof(SceneRuntime) == kCacheLine);

}  // namespace psynder::scene
