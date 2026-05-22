// SPDX-License-Identifier: MIT
// Psynder — Sample 00 / M0 demo. Open a window, animate the clear color,
// and let the engine host own overlays, presentation, smoke, and capture.

#include "platform/App.h"
#include "render/Color.h"

using namespace psynder;

namespace {

u32 animated_clear_color(double seconds) noexcept {
    return render::rgba8(render::sine_channel8(seconds, 1.7),
                         render::sine_channel8(seconds, 1.1, 1.0),
                         render::sine_channel8(seconds, 0.9, 2.0));
}

struct ClearSample {
    static constexpr const char* log_name() noexcept { return "sample_00"; }
    static constexpr const char* display_name() noexcept { return "Psynder sample 00 (clear)"; }

    static app::FrameClear frame_clear(const app::WindowFrameContext& ctx) noexcept {
        // Drive the colour off frame index in smoke mode so the captured
        // frame is identical across hosts (golden-image determinism). Real
        // runs use elapsed wall-clock time so the animation looks smooth.
        return app::FrameClear::color_only(animated_clear_color(ctx.seconds));
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(ClearSample)
