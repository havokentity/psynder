// SPDX-License-Identifier: MIT
// Psynder — Sample 00 / M0 demo. Open a window, animate the clear color,
// blit each frame. The single demo that proves the platform + framebuffer +
// rasterizer-clear path on every supported OS.
//
// CLI flags:
//   --smoke-frames=N    Run N frames then exit. Used by CI as a headless
//                       liveness check; pairs with the AGENTS.md smoke lock
//                       that serializes platform invocations on shared Mac
//                       runners.

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <cstdlib>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// Parse --smoke-frames=N (or --smoke-frames N) from argv. Returns 0 when
// the flag is absent ("run until the user closes the window"). Returns a
// positive int when the caller wants a fixed-frame headless run. Malformed
// values fall back to 0 with a warning so CI still gets a clean process
// exit even on operator typos.
u32 parse_uint(std::string_view v) {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

u32 parse_smoke_frames(int argc, char** argv) {
    constexpr std::string_view kFlag = "--smoke-frames=";
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a.starts_with(kFlag)) {
            const u32 n = parse_uint(a.substr(kFlag.size()));
            if (n == 0 && a.size() > kFlag.size()) {
                PSY_LOG_WARN("sample_00: ignoring malformed --smoke-frames value");
            }
            return n;
        }
        if (a == "--smoke-frames" && i + 1 < argc) {
            return parse_uint(std::string_view{argv[i + 1]});
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const u32 smoke_frames = parse_smoke_frames(argc, argv);

    platform::WindowDesc desc{};
    desc.title         = "Psynder — sample 00 (clear)";
    desc.window_width  = 1280;
    desc.window_height = 720;
    desc.render_width  = 640;
    desc.render_height = 360;
    desc.scale_mode    = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("failed to create window");
        return EXIT_FAILURE;
    }

    // CPU-side framebuffer at internal render resolution
    std::vector<u32> pixels(static_cast<usize>(desc.render_width) * desc.render_height, 0);
    render::Framebuffer fb{};
    fb.width  = desc.render_width;
    fb.height = desc.render_height;
    fb.pitch  = desc.render_width * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(pixels.data());

    if (smoke_frames > 0) {
        PSY_LOG_INFO("Psynder sample 00 — smoke mode, {} frames", smoke_frames);
    } else {
        PSY_LOG_INFO("Psynder sample 00 running");
    }

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame    = 0;

    while (!window->should_close()) {
        window->poll_events();

        const f64 t  = platform::Clock::seconds(platform::Clock::ticks_now() - t0);
        const u8  r  = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.7));
        const u8  g  = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.1 + 1.0));
        const u8  b  = static_cast<u8>(127.0 + 127.0 * std::sin(t * 0.9 + 2.0));
        const u32 rgba = (static_cast<u32>(r))
                       | (static_cast<u32>(g) << 8)
                       | (static_cast<u32>(b) << 16)
                       | (0xFFu << 24);

        render::raster::clear_framebuffer(fb, rgba);
        window->present(fb);

        if (smoke_frames > 0 && ++frame >= smoke_frames) {
            PSY_LOG_INFO("sample_00: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
