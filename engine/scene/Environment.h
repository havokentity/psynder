// SPDX-License-Identifier: MIT
// Psynder — scene environment state shared by render systems.

#pragma once

#include "core/Types.h"
#include "scene/SceneRuntime.h"

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
    Environment() noexcept = default;

    explicit Environment(EnvironmentRuntimeSoA& runtime, u32 slot = 0u) noexcept {
        bind_runtime(runtime, slot);
    }

    void bind_runtime(EnvironmentRuntimeSoA& runtime, u32 slot = 0u) noexcept {
        runtime_ = &runtime;
        runtime_slot_ = slot;
        sync_runtime_from_settings();
    }

    [[nodiscard]] EnvironmentSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const EnvironmentSettings& settings() const noexcept { return settings_; }

    void set_settings(const EnvironmentSettings& settings) noexcept {
        settings_ = settings;
        sync_runtime_from_settings();
    }

    void set_clear_color(u32 rgba8) noexcept {
        settings_.clear_color = true;
        settings_.clear_color_rgba8 = rgba8;
        sync_runtime_clear();
    }

    void set_clear_enabled(bool color, bool depth) noexcept {
        settings_.clear_color = color;
        settings_.clear_depth = depth;
        sync_runtime_clear();
    }

    void disable_clear() noexcept { set_clear_enabled(false, false); }

    [[nodiscard]] bool clear_enabled() const noexcept { return settings_.clear_enabled(); }

    [[nodiscard]] EnvironmentSkySettings& sky() noexcept { return settings_.sky; }
    [[nodiscard]] const EnvironmentSkySettings& sky() const noexcept { return settings_.sky; }
    [[nodiscard]] EnvironmentCloudSettings& clouds() noexcept { return settings_.clouds; }
    [[nodiscard]] const EnvironmentCloudSettings& clouds() const noexcept { return settings_.clouds; }

    void set_sky(const EnvironmentSkySettings& sky) noexcept {
        settings_.sky = sky;
        sync_runtime_sky();
    }

    void set_clouds(const EnvironmentCloudSettings& clouds) noexcept {
        settings_.clouds = clouds;
        sync_runtime_clouds();
    }

    void sync_runtime_from_settings() noexcept {
        sync_runtime_clear();
        sync_runtime_sky();
        sync_runtime_clouds();
    }

    void sync_runtime_sky() noexcept {
        if (!runtime_)
            return;
        runtime_->sky_zenith_rgba8[runtime_slot_] = settings_.sky.zenith_rgba8;
        runtime_->sky_horizon_rgba8[runtime_slot_] = settings_.sky.horizon_rgba8;
        runtime_->sky_intensity[runtime_slot_] = settings_.sky.intensity;
    }

    void sync_runtime_clouds() noexcept {
        if (!runtime_)
            return;
        runtime_->cloud_enabled[runtime_slot_] = settings_.clouds.enabled ? 1u : 0u;
        runtime_->cloud_coverage[runtime_slot_] = settings_.clouds.coverage;
        runtime_->cloud_density[runtime_slot_] = settings_.clouds.density;
        runtime_->cloud_height[runtime_slot_] = settings_.clouds.height;
        runtime_->cloud_wind_x[runtime_slot_] = settings_.clouds.wind_x;
        runtime_->cloud_wind_y[runtime_slot_] = settings_.clouds.wind_y;
    }

   private:
    EnvironmentSettings settings_{};
    EnvironmentRuntimeSoA* runtime_ = nullptr;
    u32 runtime_slot_ = 0u;

    void sync_runtime_clear() noexcept {
        if (!runtime_)
            return;
        EnvironmentClearFlags flags = EnvironmentClearFlags::None;
        if (settings_.clear_color)
            flags |= EnvironmentClearFlags::Color;
        if (settings_.clear_depth)
            flags |= EnvironmentClearFlags::Depth;
        runtime_->clear_flags[runtime_slot_] = flags;
        runtime_->clear_color_rgba8[runtime_slot_] = settings_.clear_color_rgba8;
    }
};

}  // namespace psynder::scene
