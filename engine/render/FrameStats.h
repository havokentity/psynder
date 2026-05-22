// SPDX-License-Identifier: MIT
// Psynder - render frame telemetry shared by renderers and app HUD.

#pragma once

#include "core/Types.h"

namespace psynder::render {

struct FrameStats {
    usize raster_draws = 0;
    usize raster_triangles = 0;
    u32 rt_tiles = 0;
    u32 rt_jobs = 0;
    bool raster_reported = false;
    bool rt_reported = false;

    [[nodiscard]] bool has_render_report() const noexcept {
        return raster_reported || rt_reported;
    }
};

namespace detail {
inline FrameStats& frame_stats_state() noexcept {
    static FrameStats stats{};
    return stats;
}
}  // namespace detail

inline void reset_frame_stats() noexcept {
    detail::frame_stats_state() = {};
}

inline void record_raster_work(usize draws, usize triangles) noexcept {
    FrameStats& stats = detail::frame_stats_state();
    stats.raster_draws += draws;
    stats.raster_triangles += triangles;
    stats.raster_reported = true;
}

inline void record_rt_work(u32 tiles, u32 jobs) noexcept {
    FrameStats& stats = detail::frame_stats_state();
    stats.rt_tiles += tiles;
    stats.rt_jobs += jobs;
    stats.rt_reported = true;
}

inline FrameStats frame_stats_snapshot() noexcept {
    return detail::frame_stats_state();
}

}  // namespace psynder::render
