// SPDX-License-Identifier: MIT
// Psynder — Sample 00 / M0 demo. Open a window, animate the clear color,
// and let the engine host own overlays, presentation, smoke, and capture.

#include "core/Types.h"
#include "platform/App.h"
#include <cmath>
#include <string_view>

using namespace psynder;

namespace {

struct ClearSample {
    static constexpr std::string_view log_name() noexcept { return "sample_00"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 00 (clear)"; }

    static app::FrameClear frame_clear(const app::WindowFrameContext& ctx) noexcept {
        // Drive the colour off frame index in smoke mode so the captured
        // frame is identical across hosts (golden-image determinism). Real
        // runs use elapsed wall-clock time so the animation looks smooth.
        const f64 t = ctx.seconds;
        const u8 r = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.7));
        const u8 g = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.1 + 1.0));
        const u8 b = static_cast<u8>(127.0 + 127.0 * std::sin(t * 0.9 + 2.0));
        const u32 rgba = (static_cast<u32>(r)) | (static_cast<u32>(g) << 8) |
                         (static_cast<u32>(b) << 16) | (0xFFu << 24);
        return app::FrameClear::color_only(rgba);
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(ClearSample)
