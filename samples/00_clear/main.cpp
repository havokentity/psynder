// SPDX-License-Identifier: MIT
// Psynder — Sample 00 / M0 demo. Open a window, animate the clear color,
// blit each frame. The single demo that proves the platform + framebuffer +
// rasterizer-clear path on every supported OS.

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <cstdlib>
#include <vector>

using namespace psynder;

int main(int /*argc*/, char** /*argv*/) {
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

    PSY_LOG_INFO("Psynder sample 00 running");

    u64 t0 = platform::Clock::ticks_now();

    while (!window->should_close()) {
        window->poll_events();

        f64 t  = platform::Clock::seconds(platform::Clock::ticks_now() - t0);
        u8  r  = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.7));
        u8  g  = static_cast<u8>(127.0 + 127.0 * std::sin(t * 1.1 + 1.0));
        u8  b  = static_cast<u8>(127.0 + 127.0 * std::sin(t * 0.9 + 2.0));
        u32 rgba = (static_cast<u32>(r))
                 | (static_cast<u32>(g) << 8)
                 | (static_cast<u32>(b) << 16)
                 | (0xFFu << 24);

        render::raster::clear_framebuffer(fb, rgba);
        window->present(fb);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
