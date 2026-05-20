// SPDX-License-Identifier: MIT
// Psynder — X11 fallback window. Lane 22.
//
// Wayland is the default; this code runs when the user has no Wayland
// session (e.g. via XWayland on a server install) or when
// PSYNDER_PLATFORM=x11 is set for testing.
//
// Implementation:
//   - XShmCreateImage on the root visual gives us a CPU-side framebuffer
//     shared with the X server (System-V SHM segment). Per-frame work is
//     just a memcpy + XShmPutImage; the server / driver handle the
//     compositing.
//   - For the *scaled* blit (DESIGN.md §7.9: window resize must rescale
//     the present, not the framebuffer), we use the X RENDER extension —
//     XRenderComposite with FilteringMode::Bilinear or Nearest. RENDER
//     handles the up/downscale on the GPU through any modern X driver.
//
// X11 doesn't require an event-driven keyboard / pointer pump for our
// purposes; we drain XPending() in poll_events() and translate KeyPress /
// ButtonPress / MotionNotify into the shared input state. Keysyms are
// resolved with XLookupKeysym (or XKB if installed); we go through our
// shared keycode_from_xkb helper either way.

#ifdef PSYNDER_PLATFORM_LINUX

#include "platform/Platform.h"
#include "LinuxKeymap.h"
#include "LinuxPlatform_internal.h"

#include "core/Log.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrender.h>

#include <sys/ipc.h>
#include <sys/shm.h>

namespace psynder::platform::linux_impl {

namespace {

class X11Window final : public Window {
   public:
    X11Window() = default;
    ~X11Window() override { teardown(); }

    bool init(const WindowDesc& desc) noexcept;

    void poll_events() override;
    bool should_close() const override { return closed_; }
    void present(const render::Framebuffer& fb) override;
    void set_title(std::string_view t) override {
        title_ = std::string{t};
        if (dpy_ && win_) {
            XStoreName(dpy_, win_, title_.c_str());
        }
    }
    u32 window_width() const override { return window_w_; }
    u32 window_height() const override { return window_h_; }

   private:
    void teardown() noexcept;
    bool allocate_xshm(u32 w, u32 h) noexcept;
    void free_xshm() noexcept;
    void handle_event(const XEvent& ev) noexcept;
    void apply_blit(const render::Framebuffer& fb) noexcept;

    Display* dpy_ = nullptr;
    int screen_ = 0;
    ::Window win_ = 0;
    GC gc_ = nullptr;
    Atom wm_delete_ = None;

    // XShm framebuffer image.
    XShmSegmentInfo shm_info_{};
    XImage* ximage_ = nullptr;
    bool shm_attached_ = false;
    u32 shm_w_ = 0;
    u32 shm_h_ = 0;

    // XRender resources for the scaled present.
    Pixmap render_pix_ = 0;
    Picture src_pic_ = 0;
    Picture dst_pic_ = 0;
    XRenderPictFormat* pict_fmt_ = nullptr;

    int xrender_evt_base_ = 0;
    int xrender_err_base_ = 0;
    int xshm_evt_base_ = 0;
    int xshm_err_base_ = 0;

    std::string title_ = "Psynder";
    u32 render_w_ = 0;
    u32 render_h_ = 0;
    u32 window_w_ = 0;
    u32 window_h_ = 0;
    ScaleMode scale_mode_ = ScaleMode::Linear;
    AspectMode aspect_ = AspectMode::Letterbox;
    bool closed_ = false;
    float mouse_x_ = 0.f;
    float mouse_y_ = 0.f;
};

bool X11Window::init(const WindowDesc& desc) noexcept {
    title_ = desc.title;
    render_w_ = desc.render_width;
    render_h_ = desc.render_height;
    window_w_ = desc.window_width;
    window_h_ = desc.window_height;
    scale_mode_ = desc.scale_mode;
    aspect_ = desc.aspect_mode;

    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_)
        return false;
    screen_ = DefaultScreen(dpy_);

    int xshm_major = 0, xshm_minor = 0;
    Bool xshm_pixmap = False;
    int xshm_major_opcode = 0;
    Bool xshm_ok =
        XQueryExtension(dpy_, "MIT-SHM", &xshm_major_opcode, &xshm_evt_base_, &xshm_err_base_);
    if (!xshm_ok) {
        PSY_LOG_WARN("x11: MIT-SHM extension absent");
        teardown();
        return false;
    }
    (void)XShmQueryVersion(dpy_, &xshm_major, &xshm_minor, &xshm_pixmap);

    if (!XRenderQueryExtension(dpy_, &xrender_evt_base_, &xrender_err_base_)) {
        PSY_LOG_WARN("x11: RENDER extension absent");
        teardown();
        return false;
    }

    Visual* visual = DefaultVisual(dpy_, screen_);
    pict_fmt_ = XRenderFindVisualFormat(dpy_, visual);
    if (!pict_fmt_) {
        teardown();
        return false;
    }

    XSetWindowAttributes attrs{};
    attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                       ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask;
    attrs.background_pixel = BlackPixel(dpy_, screen_);
    attrs.border_pixel = BlackPixel(dpy_, screen_);

    win_ = XCreateWindow(dpy_,
                         RootWindow(dpy_, screen_),
                         0,
                         0,
                         window_w_,
                         window_h_,
                         0,
                         CopyFromParent,
                         InputOutput,
                         visual,
                         CWEventMask | CWBackPixel | CWBorderPixel,
                         &attrs);
    if (!win_) {
        teardown();
        return false;
    }

    XStoreName(dpy_, win_, title_.c_str());
    XSetIconName(dpy_, win_, title_.c_str());

    wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wm_delete_, 1);

    gc_ = XCreateGC(dpy_, win_, 0, nullptr);

    if (!allocate_xshm(render_w_, render_h_)) {
        teardown();
        return false;
    }

    XMapWindow(dpy_, win_);
    XFlush(dpy_);
    return true;
}

void X11Window::teardown() noexcept {
    free_xshm();
    if (gc_) {
        XFreeGC(dpy_, gc_);
        gc_ = nullptr;
    }
    if (win_) {
        XDestroyWindow(dpy_, win_);
        win_ = 0;
    }
    if (dpy_) {
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
    }
}

bool X11Window::allocate_xshm(u32 w, u32 h) noexcept {
    free_xshm();
    if (w == 0 || h == 0)
        return false;

    Visual* visual = DefaultVisual(dpy_, screen_);
    int depth = DefaultDepth(dpy_, screen_);

    ximage_ = XShmCreateImage(dpy_,
                              visual,
                              depth,
                              ZPixmap,
                              nullptr,
                              &shm_info_,
                              static_cast<unsigned>(w),
                              static_cast<unsigned>(h));
    if (!ximage_)
        return false;

    const size_t bytes = static_cast<size_t>(ximage_->bytes_per_line) * ximage_->height;
    shm_info_.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (shm_info_.shmid < 0) {
        free_xshm();
        return false;
    }

    shm_info_.shmaddr = static_cast<char*>(shmat(shm_info_.shmid, nullptr, 0));
    if (shm_info_.shmaddr == reinterpret_cast<char*>(-1)) {
        free_xshm();
        return false;
    }
    ximage_->data = shm_info_.shmaddr;
    shm_info_.readOnly = False;

    if (!XShmAttach(dpy_, &shm_info_)) {
        free_xshm();
        return false;
    }
    shm_attached_ = true;

    // Free the shm segment id eagerly — the segment persists while attached
    // by either us or the server.
    shmctl(shm_info_.shmid, IPC_RMID, nullptr);

    // Create the matching Pixmap + Picture for XRender scaling.
    render_pix_ = XCreatePixmap(dpy_, win_, w, h, depth);
    if (!render_pix_) {
        free_xshm();
        return false;
    }

    XRenderPictureAttributes pa{};
    src_pic_ = XRenderCreatePicture(dpy_, render_pix_, pict_fmt_, 0, &pa);
    dst_pic_ = XRenderCreatePicture(dpy_, win_, pict_fmt_, 0, &pa);
    if (!src_pic_ || !dst_pic_) {
        free_xshm();
        return false;
    }

    shm_w_ = w;
    shm_h_ = h;
    return true;
}

void X11Window::free_xshm() noexcept {
    if (!dpy_) {
        ximage_ = nullptr;
        return;
    }
    if (dst_pic_) {
        XRenderFreePicture(dpy_, dst_pic_);
        dst_pic_ = 0;
    }
    if (src_pic_) {
        XRenderFreePicture(dpy_, src_pic_);
        src_pic_ = 0;
    }
    if (render_pix_) {
        XFreePixmap(dpy_, render_pix_);
        render_pix_ = 0;
    }
    if (shm_attached_) {
        XShmDetach(dpy_, &shm_info_);
        shm_attached_ = false;
    }
    if (shm_info_.shmaddr && shm_info_.shmaddr != reinterpret_cast<char*>(-1)) {
        shmdt(shm_info_.shmaddr);
        shm_info_.shmaddr = nullptr;
    }
    if (ximage_) {
        XDestroyImage(ximage_);
        ximage_ = nullptr;
    }
    shm_w_ = shm_h_ = 0;
}

void X11Window::handle_event(const XEvent& ev) noexcept {
    switch (ev.type) {
        case Expose:
            // Compositors usually pump us; nothing to do here — the next
            // present() will redraw.
            break;
        case ConfigureNotify: {
            const auto& c = ev.xconfigure;
            if (c.width > 0)
                window_w_ = static_cast<u32>(c.width);
            if (c.height > 0)
                window_h_ = static_cast<u32>(c.height);
            break;
        }
        case ClientMessage:
            if (ev.xclient.format == 32 && static_cast<Atom>(ev.xclient.data.l[0]) == wm_delete_) {
                closed_ = true;
            }
            break;
        case KeyPress:
        case KeyRelease: {
            const KeySym ks = XLookupKeysym(const_cast<XKeyEvent*>(&ev.xkey), 0);
            KeyCode kc = keycode_from_xkb(static_cast<uint32_t>(ks));
            if (kc == KeyCode::Unknown) {
                // Try evdev-style mapping (X11 keycodes = evdev + 8).
                const uint32_t code = ev.xkey.keycode >= 8 ? ev.xkey.keycode - 8 : 0;
                kc = keycode_from_evdev(code);
            }
            input_push_key(kc, ev.type == KeyPress);
            break;
        }
        case ButtonPress:
        case ButtonRelease: {
            const bool down = (ev.type == ButtonPress);
            switch (ev.xbutton.button) {
                case Button1:
                    input_push_mouse_button(0, down);
                    break;
                case Button2:
                    input_push_mouse_button(2, down);
                    break;
                case Button3:
                    input_push_mouse_button(1, down);
                    break;
                case Button4:
                    if (down)
                        input_push_mouse_wheel(1.f);
                    break;
                case Button5:
                    if (down)
                        input_push_mouse_wheel(-1.f);
                    break;
                default:
                    break;
            }
            break;
        }
        case MotionNotify: {
            const float new_x = static_cast<float>(ev.xmotion.x);
            const float new_y = static_cast<float>(ev.xmotion.y);
            const float dx = new_x - mouse_x_;
            const float dy = new_y - mouse_y_;
            mouse_x_ = new_x;
            mouse_y_ = new_y;
            input_push_mouse_motion(dx, dy, new_x, new_y);
            break;
        }
        default:
            break;
    }
}

void X11Window::poll_events() {
    if (!dpy_)
        return;
    while (XPending(dpy_) > 0) {
        XEvent ev;
        XNextEvent(dpy_, &ev);
        handle_event(ev);
    }
    input_frame_advance();
}

void X11Window::apply_blit(const render::Framebuffer& /*fb*/) noexcept {
    if (!ximage_ || !src_pic_ || !dst_pic_)
        return;
    // Compute destination rect per the engine's scale/aspect mode.
    const BlitRect r = compute_blit_rect(static_cast<int>(window_w_),
                                         static_cast<int>(window_h_),
                                         static_cast<int>(render_w_),
                                         static_cast<int>(render_h_),
                                         aspect_,
                                         scale_mode_);
    if (r.w <= 0 || r.h <= 0)
        return;

    // Clear borders (letterbox bars) — fill the full window with black
    // first, then blit the framebuffer rectangle into it.
    XSetForeground(dpy_, gc_, BlackPixel(dpy_, screen_));
    XFillRectangle(dpy_, win_, gc_, 0, 0, window_w_, window_h_);

    // Push the framebuffer pixels into the XShm pixmap.
    XShmPutImage(dpy_, render_pix_, gc_, ximage_, 0, 0, 0, 0, render_w_, render_h_, False);

    // Compute the affine for XRender: src extent / dst extent gives the
    // scale factor; we encode as XTransform fixed-point (16.16).
    auto fix = [](double v) -> XFixed { return static_cast<XFixed>(v * 65536.0); };
    const double sx = static_cast<double>(render_w_) / static_cast<double>(r.w);
    const double sy = static_cast<double>(render_h_) / static_cast<double>(r.h);
    XTransform xform{{
        {fix(sx), 0, 0},
        {0, fix(sy), 0},
        {0, 0, fix(1.0)},
    }};
    XRenderSetPictureTransform(dpy_, src_pic_, &xform);

    // Pick the filter from ScaleMode.  XRender filter names are strings.
    const char* filter = "bilinear";
    if (scale_mode_ == ScaleMode::Nearest || scale_mode_ == ScaleMode::Integer) {
        filter = "nearest";
    }
    XRenderSetPictureFilter(dpy_, src_pic_, filter, nullptr, 0);

    XRenderComposite(dpy_, PictOpSrc, src_pic_, None, dst_pic_, 0, 0, 0, 0, r.x, r.y, r.w, r.h);
    XFlush(dpy_);
}

void X11Window::present(const render::Framebuffer& fb) {
    if (!dpy_ || !win_)
        return;
    if (fb.width == 0 || fb.height == 0 || fb.pixels == nullptr)
        return;

    if (fb.width != shm_w_ || fb.height != shm_h_) {
        render_w_ = fb.width;
        render_h_ = fb.height;
        if (!allocate_xshm(render_w_, render_h_))
            return;
    }

    // Copy framebuffer into the XShm image. X11 uses BGR0/RGB0 depending
    // on the visual; for the common 24-bit TrueColor with red_mask 0x00FF0000
    // we want byte order B,G,R,_.  We follow the same swizzle as the
    // Wayland path.
    const u32* src = reinterpret_cast<const u32*>(fb.pixels);
    u32* dst = reinterpret_cast<u32*>(ximage_->data);
    const size_t pixels_n = static_cast<size_t>(render_w_) * render_h_;
    for (size_t i = 0; i < pixels_n; ++i) {
        const u32 v = src[i];
        const u32 r = (v) & 0xFFu;
        const u32 g = (v >> 8) & 0xFFu;
        const u32 b = (v >> 16) & 0xFFu;
        dst[i] = (b) | (g << 8) | (r << 16);
    }

    apply_blit(fb);
}

}  // namespace

Window* try_create_x11_window(const WindowDesc& desc) noexcept {
    auto* w = new (std::nothrow) X11Window();
    if (!w)
        return nullptr;
    if (!w->init(desc)) {
        delete w;
        return nullptr;
    }
    return w;
}

}  // namespace psynder::platform::linux_impl

#endif  // PSYNDER_PLATFORM_LINUX
