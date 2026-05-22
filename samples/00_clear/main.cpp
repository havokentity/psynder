// SPDX-License-Identifier: MIT
// Psynder — Sample 00 / M0 demo. Open a window, animate the clear color,
// blit each frame. The single demo that proves the platform + framebuffer +
// rasterizer-clear path on every supported OS.
//
// CLI flags:
//   --smoke-frames=N         Run N frames then exit. Used by CI as a headless
//                            liveness check; pairs with the AGENTS.md smoke
//                            lock that serializes platform invocations on
//                            shared Mac runners.
//   --smoke-frames N         Same, space-separated (matches the cmake helper
//                            invocation in cmake/Goldens.cmake).
//   --smoke-capture-out PATH Write the final rendered framebuffer to PATH
//                            as a valid 24-bit RGB PNG. Used by the
//                            psynder_add_golden_cell() ctest cells to grab
//                            the actual-image-this-run output for the
//                            clear-color golden.

#include "core/Log.h"
#include "core/Types.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/raster/Raster.h"

#include <cmath>
#include <string_view>

using namespace psynder;

namespace {

struct ClearSample {
    static constexpr std::string_view log_name() noexcept { return "sample_00"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 00"; }

    static platform::WindowDesc window_desc(const app::AppArgs&) noexcept {
        platform::WindowDesc desc{};
        desc.title = "Psynder — sample 00 (clear)";
        desc.window_width = 1280;
        desc.window_height = 720;
        desc.render_width = 640;
        desc.render_height = 360;
        desc.scale_mode = platform::ScaleMode::Integer;
        return desc;
    }

    void frame(app::WindowFrameContext& ctx) {
        // Drive the colour off frame index in smoke mode so the captured
        // frame is identical across hosts (golden-image determinism). Real
        // runs use elapsed wall-clock time so the animation looks smooth.
        const f64 t = ctx.seconds;
        const u8 r = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.7));
        const u8 g = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.1 + 1.0));
        const u8 b = static_cast<u8>(127.0 + 127.0 * std::sin(t * 0.9 + 2.0));
        const u32 rgba = (static_cast<u32>(r)) | (static_cast<u32>(g) << 8) |
                         (static_cast<u32>(b) << 16) | (0xFFu << 24);

        render::raster::clear_framebuffer(ctx.framebuffer, rgba);
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(ClearSample)
