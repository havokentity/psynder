// SPDX-License-Identifier: MIT
// Psynder — platform abstraction. The CPU does everything except present.
// Lanes 21 / 22 / 23 each implement this for win32 / linux / macos.
//
// The platform provides: window, framebuffer present (scaled blit), input,
// timing, audio device, file-system queries. Nothing else.

#pragma once

#include "core/Types.h"
#include "render/Framebuffer.h"

#include <span>
#include <string>
#include <string_view>

namespace psynder::platform {

// ─── Window / surface ────────────────────────────────────────────────────
enum class ScaleMode : u8 {
    Nearest,
    Linear,
    Integer,  // snap to integer multiples for pixel-perfect upscale
};

enum class AspectMode : u8 {
    Letterbox,  // default
    Stretch,
    Crop,
};

struct WindowDesc {
    std::string title = "Psynder";
    u32 window_width = 1280;
    u32 window_height = 720;
    u32 render_width = 1280;
    u32 render_height = 720;
    ScaleMode scale_mode = ScaleMode::Linear;
    AspectMode aspect_mode = AspectMode::Letterbox;
    bool fullscreen = false;
    bool resizable = true;
    bool vsync = true;
};

class Window {
   public:
    virtual ~Window() = default;

    virtual void poll_events() = 0;
    virtual bool should_close() const = 0;

    // Present a CPU framebuffer. The platform handles scaled blit per the
    // window's current size + ScaleMode + AspectMode.
    virtual void present(const render::Framebuffer& fb) = 0;

    // Mutable parts
    virtual void set_title(std::string_view title) = 0;
    virtual u32 window_width() const = 0;
    virtual u32 window_height() const = 0;

    // ── Runtime display control ──────────────────────────────────────────
    // Optional; the default is a no-op so a backend opts in when it can. The
    // CPU framebuffer is NOT touched by these — present() simply stretches it
    // to fill the new window / screen size (the existing scaled blit).
    //   set_fullscreen(true)  → borderless full-screen (frame stretched to fit)
    //   set_window_size(w,h)  → windowed content size in pixels
    virtual void set_fullscreen(bool on) { (void)on; }
    virtual bool is_fullscreen() const { return false; }
    virtual void set_window_size(u32 width, u32 height) {
        (void)width;
        (void)height;
    }
};

Window* create_window(const WindowDesc& desc);
void destroy_window(Window* w);

// ─── Display control on the active window ────────────────────────────────
// Single window per process (DESIGN §11), so these target the live window
// without the caller holding a Window* — the software console drives them.
// Must be called on the main thread (where poll_events() / present() run);
// no-op if no window is open or the backend doesn't implement the mode.
void request_fullscreen(bool on);
void toggle_fullscreen();
bool is_fullscreen();
void request_window_size(u32 width, u32 height);

// When true, the platform suppresses its default Escape-closes-the-window
// behaviour because something is capturing text (the software console is
// open). The console sets this each frame; otherwise Escape behaves as the
// host wires it. Lets the console own Escape (e.g. clear the prompt) without
// the window quitting underneath it.
void set_text_input_capturing(bool capturing);
bool text_input_capturing();

// ─── Input ───────────────────────────────────────────────────────────────
enum class KeyCode : u16 {
    Unknown = 0,
    Escape,
    Enter,
    Space,
    Tab,
    Backspace,
    Delete,  // forward delete (Del / fn+Delete) — removes the char AT the caret
    Left,
    Right,
    Up,
    Down,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Tilde,  // ~ — toggles editor mode per DESIGN.md §10.8
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    Count,
};

struct MouseState {
    f32 x = 0, y = 0;
    f32 dx = 0, dy = 0;
    f32 wheel = 0;
    bool left = false, right = false, middle = false;
};

class Input {
   public:
    virtual ~Input() = default;
    virtual bool key_down(KeyCode k) const = 0;
    virtual bool key_pressed(KeyCode k) const = 0;
    virtual const MouseState& mouse() const = 0;

    // Unicode codepoints typed this frame, in input order. Empty unless the
    // platform captures character events (text entry for the software console
    // overlay). Refreshed every poll_events(); the returned span stays valid
    // until the next poll. Non-pure with an empty default so backends that
    // don't capture text — and test doubles — need not implement it.
    //
    // Codepoints are already layout- and modifier-mapped by the OS (so Shift+2
    // yields '@' on a US layout). Control characters (Enter, Backspace, Tab,
    // Escape, arrows) are NOT delivered here — query those via key_pressed so
    // the console can bind them to editing actions rather than literal text.
    virtual std::span<const u32> text_input() const { return {}; }
};

Input* input();

// ─── Timing ──────────────────────────────────────────────────────────────
struct Clock {
    static u64 ticks_now();
    static u64 ticks_per_second();
    static f64 seconds(u64 ticks);
};

// ─── Process / FS helpers ────────────────────────────────────────────────
std::string executable_path();
std::string user_config_dir();
std::string current_working_directory();
bool file_exists(std::string_view path);

}  // namespace psynder::platform
