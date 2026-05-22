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

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "editor/core/SampleHook.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

int main(int argc, char** argv) {
    const app::AppArgs args = app::parse_common_args(argc, argv).args;
    const u32 smoke_frames = args.smoke_frames;

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 00 (clear)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    app::WindowApp app_host{args, desc};
    if (!app_host) {
        PSY_LOG_ERROR("failed to create window");
        return EXIT_FAILURE;
    }
    auto* window = &app_host.window();

    render::Framebuffer& fb = app_host.framebuffer();

    if (smoke_frames > 0) {
        PSY_LOG_INFO("Psynder sample 00 — smoke mode, {} frames", smoke_frames);
    } else {
        PSY_LOG_INFO("Psynder sample 00 running");
    }

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();

        // Drive the colour off frame index in smoke mode so the captured
        // frame is identical across hosts (golden-image determinism). Real
        // runs use wall-clock time so the animation looks smooth.
        const f64 t = smoke_frames > 0 ? static_cast<f64>(frame) * (1.0 / 60.0)
                                       : platform::Clock::seconds(platform::Clock::ticks_now() - t0);
        const u8 r = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.7));
        const u8 g = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.1 + 1.0));
        const u8 b = static_cast<u8>(127.0 + 127.0 * std::sin(t * 0.9 + 2.0));
        const u32 rgba = (static_cast<u32>(r)) | (static_cast<u32>(g) << 8) |
                         (static_cast<u32>(b) << 16) | (0xFFu << 24);

        render::raster::clear_framebuffer(fb, rgba);

        // Engine overlay suite: `~` console + F1 debug HUD + F2 badge.
        if (auto* in = platform::input()) {
            editor::frame_overlays(*in, fb);
        }

        window->present(fb);

        if (smoke_frames > 0 && ++frame >= smoke_frames) {
            PSY_LOG_INFO("sample_00: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    if (!app_host.write_capture_if_requested("sample_00"))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
