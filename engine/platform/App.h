// SPDX-License-Identifier: MIT
// Psynder — shared executable app host helpers.

#pragma once

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/PngWriter.h"

#include <concepts>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace psynder::app {

struct WindowAppOptions {
    bool depth_buffer = false;
};

// Owns the standard app/sample process boilerplate: common CLI args, one
// platform window, a CPU RGBA8 framebuffer backing store, optional depth, PNG
// capture, and deterministic cleanup. Samples keep only their scene/update code.
class WindowApp {
   public:
    WindowApp(const AppArgs& args, const platform::WindowDesc& desc, WindowAppOptions options = {})
        : args_(args), width_(desc.render_width), height_(desc.render_height) {
        window_ = platform::create_window(desc);
        if (!window_)
            return;

        pixels_.resize(static_cast<usize>(width_) * height_, 0u);
        if (options.depth_buffer)
            depth_.resize(static_cast<usize>(width_) * height_, 0u);

        framebuffer_.width = width_;
        framebuffer_.height = height_;
        framebuffer_.pitch = width_ * 4u;
        framebuffer_.format = render::PixelFormat::RGBA8;
        framebuffer_.pixels = reinterpret_cast<u8*>(pixels_.data());
        framebuffer_.depth = depth_.empty() ? nullptr : depth_.data();
    }

    WindowApp(const WindowApp&) = delete;
    WindowApp& operator=(const WindowApp&) = delete;

    WindowApp(WindowApp&& other) noexcept { move_from(other); }
    WindowApp& operator=(WindowApp&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(other);
        }
        return *this;
    }

    ~WindowApp() { destroy(); }

    [[nodiscard]] bool valid() const noexcept { return window_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] const AppArgs& args() const noexcept { return args_; }
    [[nodiscard]] u32 smoke_frames() const noexcept { return args_.smoke_frames; }
    [[nodiscard]] bool capture_requested() const noexcept { return !args_.capture_out.empty(); }

    [[nodiscard]] platform::Window& window() noexcept { return *window_; }
    [[nodiscard]] const platform::Window& window() const noexcept { return *window_; }
    [[nodiscard]] render::Framebuffer& framebuffer() noexcept { return framebuffer_; }
    [[nodiscard]] const render::Framebuffer& framebuffer() const noexcept { return framebuffer_; }
    [[nodiscard]] std::vector<u32>& pixels() noexcept { return pixels_; }
    [[nodiscard]] const std::vector<u32>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::vector<u32>& depth() noexcept { return depth_; }
    [[nodiscard]] const std::vector<u32>& depth() const noexcept { return depth_; }

    void present() { window().present(framebuffer_); }

    [[nodiscard]] bool write_capture_if_requested(std::string_view log_name) const {
        if (args_.capture_out.empty())
            return true;
        const bool ok = render::write_png_rgba8_framebuffer(args_.capture_out.c_str(),
                                                            pixels_.data(),
                                                            framebuffer_.width,
                                                            framebuffer_.height);
        if (!ok) {
            PSY_LOG_ERROR("{}: failed to write capture to {}", log_name, args_.capture_out);
            return false;
        }
        PSY_LOG_INFO("{}: wrote capture to {}", log_name, args_.capture_out);
        return true;
    }

   private:
    void destroy() noexcept {
        if (window_) {
            platform::destroy_window(window_);
            window_ = nullptr;
        }
    }

    void move_from(WindowApp& other) noexcept {
        args_ = other.args_;
        window_ = other.window_;
        width_ = other.width_;
        height_ = other.height_;
        pixels_ = std::move(other.pixels_);
        depth_ = std::move(other.depth_);
        framebuffer_ = other.framebuffer_;
        framebuffer_.pixels = reinterpret_cast<u8*>(pixels_.data());
        framebuffer_.depth = depth_.empty() ? nullptr : depth_.data();
        other.window_ = nullptr;
        other.framebuffer_ = {};
    }

    AppArgs args_{};
    platform::Window* window_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    std::vector<u32> pixels_;
    std::vector<u32> depth_;
    render::Framebuffer framebuffer_{};
};

enum class FrameAction {
    Continue,
    Exit,
};

struct WindowFrameContext {
    WindowApp& app;
    platform::Window& window;
    render::Framebuffer& framebuffer;
    const AppArgs& args;
    u32 frame_index = 0;
    f64 seconds = 0.0;
};

namespace detail {

template <class Sample>
AppArgs parse_sample_args(int argc, char** argv) {
    if constexpr (requires { Sample::parse_args(argc, argv); }) {
        return Sample::parse_args(argc, argv);
    } else {
        return parse_common_args(argc, argv).args;
    }
}

template <class Sample>
std::string_view sample_log_name(const Sample& sample) noexcept {
    if constexpr (requires { sample.log_name(); }) {
        return sample.log_name();
    } else if constexpr (requires { Sample::log_name(); }) {
        return Sample::log_name();
    } else {
        return "app";
    }
}

template <class Sample>
std::string_view sample_display_name(const Sample& sample) noexcept {
    if constexpr (requires { sample.display_name(); }) {
        return sample.display_name();
    } else if constexpr (requires { Sample::display_name(); }) {
        return Sample::display_name();
    } else {
        return sample_log_name(sample);
    }
}

template <class Sample>
WindowAppOptions sample_window_options(const Sample& sample, const AppArgs& args) noexcept {
    if constexpr (requires { sample.window_options(args); }) {
        return sample.window_options(args);
    } else if constexpr (requires { Sample::window_options(args); }) {
        return Sample::window_options(args);
    } else {
        return {};
    }
}

template <class Sample>
void sample_started(Sample& sample, WindowApp& app) {
    if constexpr (requires { sample.started(app); }) {
        sample.started(app);
    }
}

template <class Sample>
void sample_stopped(Sample& sample, WindowApp& app) {
    if constexpr (requires { sample.stopped(app); }) {
        sample.stopped(app);
    }
}

template <class Sample>
FrameAction run_sample_frame(Sample& sample, WindowFrameContext& ctx) {
    if constexpr (requires { sample.frame(ctx); }) {
        if constexpr (requires {
                          { sample.frame(ctx) } -> std::same_as<FrameAction>;
                      }) {
            return sample.frame(ctx);
        } else {
            sample.frame(ctx);
            return FrameAction::Continue;
        }
    } else {
        return FrameAction::Continue;
    }
}

}  // namespace detail

template <class Sample>
int run_window_sample(int argc, char** argv) {
    const AppArgs args = detail::parse_sample_args<Sample>(argc, argv);
    Sample sample{};
    const std::string_view log_name = detail::sample_log_name(sample);
    const std::string_view display_name = detail::sample_display_name(sample);

    WindowApp app{args, sample.window_desc(args), detail::sample_window_options(sample, args)};
    if (!app) {
        PSY_LOG_ERROR("{}: failed to create window", log_name);
        return EXIT_FAILURE;
    }

    detail::sample_started(sample, app);

    if (args.smoke_frames > 0) {
        PSY_LOG_INFO("{} — smoke mode, {} frames", display_name, args.smoke_frames);
    } else {
        PSY_LOG_INFO("{} running", display_name);
    }

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;
    while (!app.window().should_close()) {
        app.window().poll_events();

        WindowFrameContext ctx{
            app,
            app.window(),
            app.framebuffer(),
            args,
            frame,
            args.smoke_frames > 0 ? static_cast<f64>(frame) * (1.0 / 60.0)
                                  : platform::Clock::seconds(platform::Clock::ticks_now() - t0),
        };

        const FrameAction action = detail::run_sample_frame(sample, ctx);
        app.present();

        ++frame;
        if (action == FrameAction::Exit) {
            break;
        }
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("{}: smoke target reached ({}); exiting", log_name, args.smoke_frames);
            break;
        }
    }

    const bool capture_ok = app.write_capture_if_requested(log_name);
    detail::sample_stopped(sample, app);
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace psynder::app

#define PSYNDER_WINDOW_SAMPLE_MAIN(SampleType)                            \
    int main(int argc, char** argv) {                                     \
        return ::psynder::app::run_window_sample<SampleType>(argc, argv); \
    }
