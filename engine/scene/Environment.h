// SPDX-License-Identifier: MIT
// Psynder — scene environment state shared by render systems.

#pragma once

#include "core/Types.h"

namespace psynder::scene {

struct EnvironmentSkySettings {
    u32 zenith_rgba8 = 0xFF101828u;
    u32 horizon_rgba8 = 0xFF24364Cu;
    f32 intensity = 1.0f;
};

struct EnvironmentCloudSettings {
    bool enabled = false;
    f32 coverage = 0.0f;
    f32 density = 1.0f;
    f32 height = 1200.0f;
    f32 wind_x = 0.0f;
    f32 wind_y = 0.0f;
};

struct EnvironmentSettings {
    bool clear_color = true;
    bool clear_depth = true;
    u32 clear_color_rgba8 = 0xFF000000u;
    EnvironmentSkySettings sky{};
    EnvironmentCloudSettings clouds{};

    [[nodiscard]] constexpr bool clear_enabled() const noexcept {
        return clear_color || clear_depth;
    }
};

class Environment {
   public:
    [[nodiscard]] EnvironmentSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const EnvironmentSettings& settings() const noexcept { return settings_; }

    void set_settings(const EnvironmentSettings& settings) noexcept { settings_ = settings; }

    void set_clear_color(u32 rgba8) noexcept {
        settings_.clear_color = true;
        settings_.clear_color_rgba8 = rgba8;
    }

    void set_clear_enabled(bool color, bool depth) noexcept {
        settings_.clear_color = color;
        settings_.clear_depth = depth;
    }

    void disable_clear() noexcept { set_clear_enabled(false, false); }

    [[nodiscard]] bool clear_enabled() const noexcept { return settings_.clear_enabled(); }

    [[nodiscard]] EnvironmentSkySettings& sky() noexcept { return settings_.sky; }
    [[nodiscard]] const EnvironmentSkySettings& sky() const noexcept { return settings_.sky; }
    [[nodiscard]] EnvironmentCloudSettings& clouds() noexcept { return settings_.clouds; }
    [[nodiscard]] const EnvironmentCloudSettings& clouds() const noexcept { return settings_.clouds; }

   private:
    EnvironmentSettings settings_{};
};

}  // namespace psynder::scene
