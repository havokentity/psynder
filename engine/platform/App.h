// SPDX-License-Identifier: MIT
// Psynder — shared executable app host helpers.

#pragma once

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/PngWriter.h"

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

}  // namespace psynder::app
