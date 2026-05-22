// SPDX-License-Identifier: MIT
// Psynder — Sample 00 / M0 demo. Open a window, animate the clear color,
// and let the engine host own overlays, presentation, smoke, and capture.

#include "core/Types.h"
#include "platform/App.h"
#include "render/Color.h"

#include <algorithm>
#include <cmath>
#include <string_view>

using namespace psynder;

namespace {

u8 sine_channel(f64 seconds, f64 frequency, f64 phase = 0.0) noexcept {
    const f64 value = 127.5 + 127.5 * std::sin(seconds * frequency + phase);
    return static_cast<u8>(std::clamp(value, 0.0, 255.0));
}

u32 animated_clear_color(f64 seconds) noexcept {
    return render::rgba8(sine_channel(seconds, 1.7),
                         sine_channel(seconds, 1.1, 1.0),
                         sine_channel(seconds, 0.9, 2.0));
}

struct ClearSample {
    static constexpr std::string_view log_name() noexcept { return "sample_00"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 00 (clear)"; }

    static app::FrameClear frame_clear(const app::WindowFrameContext& ctx) noexcept {
        // Drive the colour off frame index in smoke mode so the captured
        // frame is identical across hosts (golden-image determinism). Real
        // runs use elapsed wall-clock time so the animation looks smooth.
        return app::FrameClear::color_only(animated_clear_color(ctx.seconds));
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(ClearSample)
