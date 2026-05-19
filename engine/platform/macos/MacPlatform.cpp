// SPDX-License-Identifier: MIT
// Psynder — macOS platform stub. Lane 23 implements the AppKit / CAMetalLayer
// scanout. Phase-0 stub provides a non-crashing factory so sample binaries link.

#include "platform/Platform.h"

namespace psynder::platform {

namespace {
class StubWindow final : public Window {
public:
    explicit StubWindow(const WindowDesc& d) : desc_(d) {}

    void poll_events() override                       {}
    bool should_close() const override                { return ++frames_ > 1; }
    void present(const render::Framebuffer&) override {}
    void set_title(std::string_view t) override       { desc_.title = std::string{t}; }
    u32  window_width()  const override               { return desc_.window_width; }
    u32  window_height() const override               { return desc_.window_height; }
private:
    mutable int frames_ = 0;
    WindowDesc  desc_;
};
}  // namespace

Window* create_window_impl(const WindowDesc& desc) { return new StubWindow(desc); }
void    destroy_window_impl(Window* w)             { delete w; }

namespace {
class StubInput final : public Input {
public:
    bool key_down(KeyCode) const override    { return false; }
    bool key_pressed(KeyCode) const override { return false; }
    const MouseState& mouse() const override { return mouse_; }
private:
    MouseState mouse_{};
};
StubInput g_input;
}  // namespace
Input* input() { return &g_input; }

std::string executable_path()        { return {}; }
std::string user_config_dir()        { return {}; }
std::string current_working_directory() { return {}; }
bool        file_exists(std::string_view) { return false; }

}  // namespace psynder::platform
