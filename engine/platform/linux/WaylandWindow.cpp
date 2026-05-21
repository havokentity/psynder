// SPDX-License-Identifier: MIT
// Psynder — Wayland xdg-shell window. Lane 22 primary present path.
//
// Implements:
//   - Connect to compositor (wl_display, wl_registry), bind xdg_wm_base
//     and wp_viewporter for the scaled present (DESIGN.md §11.2 / §7.9).
//   - Allocate a wl_shm pool sized to render_width × render_height ·
//     bytes_per_pixel and map it as our framebuffer staging area.
//   - Per-frame: memcpy the engine framebuffer into the SHM buffer,
//     attach it to the surface, set the viewport src/dst rects per the
//     window's current size + scale/aspect mode, damage, commit.
//   - dmabuf (zwp_linux_dmabuf_v1) is detected and exposed for a future
//     zero-copy upload path, but the Wave-A SHM path is the one wired
//     in. The TODO is documented inline.
//
// Why SHM is fine for now: a 640×360 RGBA framebuffer is ~900 KiB per
// frame; the compositor handles the actual GPU upload from shared memory.
// We sized this codepath for the worst case 4K internal — that's 32 MiB
// memcpy per frame, ~0.6 ms on a modern x86 — still within the present
// budget. dmabuf gets us to ~0 µs upload at the cost of two extra
// platform-specific buffer allocators; we'll switch the default in Wave B.

#ifdef PSYNDER_PLATFORM_LINUX

#include "platform/Platform.h"
#include "LinuxKeymap.h"
#include "LinuxPlatform_internal.h"

#include "core/Log.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

// Wayland — runtime symbols are pulled in by libwayland-client. The
// protocol-specific headers (xdg-shell, viewporter, linux-dmabuf) are
// produced by `wayland-scanner` at configure / build time from the
// upstream protocol XML files. The build system arranges for them to
// land on the include path; see engine/platform/linux/CMakeLists.txt.
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"

// DRM FOURCC code for XRGB8888 (32-bit, X channel ignored). Defined in
// <drm/drm_fourcc.h> on Linux but we don't want a hard libdrm dep. The
// constant has been stable since 2012.
#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258u  // 'XR24'
#endif

// DMA-BUF heap ioctl (Linux 5.6+). Allocates a linear-memory dmabuf fd
// suitable for zero-copy SHM-style uploads. We probe /dev/dma_heap/system
// at runtime; failure cleanly falls back to the wl_shm path.
#include <sys/ioctl.h>
#ifndef DMA_HEAP_IOCTL_ALLOC
struct psy_dma_heap_allocation_data {
    std::uint64_t len;
    std::uint32_t fd;
    std::uint32_t fd_flags;
    std::uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0, struct psy_dma_heap_allocation_data)
#endif

// xkbcommon for key translation. Optional — if a future build can't link
// xkbcommon we still want key_down(Tilde) to work via the evdev rawkey
// fallback in EvdevInput.cpp; here we provide the rich xkb mapping for
// users on a typical desktop.
#include <xkbcommon/xkbcommon.h>

namespace psynder::platform::linux_impl {

namespace {

// ─── Anonymous shm helpers ───────────────────────────────────────────────
// memfd_create is the modern path; older kernels fall back to a tmpfs file
// under XDG_RUNTIME_DIR. We never link to libc's deprecated SHM helpers.
int create_anonymous_file(off_t size) noexcept {
#ifdef __NR_memfd_create
    {
        // Scope the memfd `fd` so the mkostemp fallback below can reuse the
        // name without "redefinition of 'fd'" on glibc. (Apple Clang's libc
        // doesn't have __NR_memfd_create at all so the branch is dead there
        // and the diagnostic never fires on Mac.)
        int fd = ::syscall(__NR_memfd_create, "psynder-wl-shm", 0u);
        if (fd >= 0) {
            if (::ftruncate(fd, size) < 0) {
                ::close(fd);
                return -1;
            }
            return fd;
        }
    }
#endif
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        return -1;
    char path[256];
    std::snprintf(path, sizeof(path), "%s/psynder-wl-XXXXXX", runtime);
    int fd = ::mkostemp(path, O_CLOEXEC);
    if (fd < 0)
        return -1;
    ::unlink(path);
    if (::ftruncate(fd, size) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ─── Compositor globals ──────────────────────────────────────────────────
// We bind exactly what we need from the wl_registry advertisement. Anything
// missing causes try_create_wayland_window to return null so the caller can
// fall back to X11.
struct WaylandGlobals {
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wp_viewporter* viewporter = nullptr;
    // zwp_linux_dmabuf_v1 bound when the compositor exposes the protocol.
    // Used by WaylandWindow::try_dmabuf_present() for zero-copy upload of
    // the framebuffer instead of the wl_shm memcpy path.
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    // wl_seat capabilities — kbd / ptr availability.
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
};

class WaylandWindow final : public Window {
   public:
    WaylandWindow() = default;
    ~WaylandWindow() override { teardown(); }

    bool init(const WindowDesc& desc) noexcept;

    // ─── Window interface ──────────────────────────────────────────────
    void poll_events() override;
    bool should_close() const override { return closed_; }
    void present(const render::Framebuffer& fb) override;
    void set_title(std::string_view t) override {
        title_ = std::string{t};
        if (xdg_toplevel_)
            xdg_toplevel_set_title(xdg_toplevel_, title_.c_str());
    }
    u32 window_width() const override { return window_w_; }
    u32 window_height() const override { return window_h_; }

    // ─── Static listeners — must be at file scope for C linkage ────────
    static void on_registry(void*, wl_registry*, uint32_t, const char*, uint32_t);
    static void on_registry_remove(void*, wl_registry*, uint32_t);
    static void on_xdg_wm_base_ping(void*, xdg_wm_base*, uint32_t);
    static void on_xdg_surface_configure(void*, xdg_surface*, uint32_t);
    static void on_toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*);
    static void on_toplevel_close(void*, xdg_toplevel*);
    static void on_toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t);
    static void on_toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*);
    static void on_seat_capabilities(void*, wl_seat*, uint32_t);
    static void on_seat_name(void*, wl_seat*, const char*);
    static void on_keyboard_keymap(void*, wl_keyboard*, uint32_t, int32_t, uint32_t);
    static void on_keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*);
    static void on_keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*);
    static void on_keyboard_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void on_keyboard_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    static void on_keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t);
    static void on_pointer_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t);
    static void on_pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*);
    static void on_pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t);
    static void on_pointer_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void on_pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t);
    static void on_pointer_frame(void*, wl_pointer*);
    static void on_pointer_axis_source(void*, wl_pointer*, uint32_t);
    static void on_pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t);
    static void on_pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t);

   private:
    void teardown() noexcept;
    bool allocate_shm_pool(u32 w, u32 h) noexcept;
    bool ensure_shm_for(u32 w, u32 h) noexcept;

    // Wave-B: attempt zero-copy upload via zwp_linux_dmabuf_v1. Returns
    // true if the framebuffer was attached + committed via dmabuf, false
    // if any required step failed (no protocol, no dma-heap, alloc fail).
    // Callers must fall back to the wl_shm path on false.
    bool try_dmabuf_present(const render::Framebuffer& fb) noexcept;
    bool ensure_dmabuf_for(u32 w, u32 h) noexcept;
    void release_dmabuf() noexcept;

    // ─── Wayland objects ───────────────────────────────────────────────
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    WaylandGlobals globals_{};
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surf_ = nullptr;
    xdg_toplevel* xdg_toplevel_ = nullptr;
    wp_viewport* viewport_ = nullptr;

    // ─── SHM pool / buffer ─────────────────────────────────────────────
    int shm_fd_ = -1;
    void* shm_data_ = nullptr;
    size_t shm_size_ = 0;
    wl_shm_pool* shm_pool_ = nullptr;
    wl_buffer* shm_buffer_ = nullptr;

    // ─── dmabuf path (Wave-B zero-copy) ────────────────────────────────
    // dma_heap_fd_ is the file descriptor to /dev/dma_heap/system (kernel
    // 5.6+). When the heap isn't accessible we never populate dmabuf_buf_
    // and present() permanently falls back to the shm path.
    int dma_heap_fd_ = -1;
    int dma_fd_ = -1;  // currently-bound dmabuf fd
    void* dma_map_ = nullptr;
    size_t dma_size_ = 0;
    wl_buffer* dma_buffer_ = nullptr;
    u32 dma_w_ = 0;
    u32 dma_h_ = 0;
    bool dma_disabled_ = false;  // sticky after first failure

    // ─── State ─────────────────────────────────────────────────────────
    std::string title_ = "Psynder";
    u32 render_w_ = 0;
    u32 render_h_ = 0;
    u32 window_w_ = 0;
    u32 window_h_ = 0;
    ScaleMode scale_mode_ = ScaleMode::Linear;
    AspectMode aspect_ = AspectMode::Letterbox;
    bool closed_ = false;
    bool configured_ = false;

    // ─── xkbcommon ─────────────────────────────────────────────────────
    xkb_context* xkb_ctx_ = nullptr;
    xkb_keymap* xkb_keymap_ = nullptr;
    xkb_state* xkb_state_ = nullptr;
};

// ─── Listener tables ─────────────────────────────────────────────────────
wl_registry_listener kRegistryListener = {
    /* global        */ &WaylandWindow::on_registry,
    /* global_remove */ &WaylandWindow::on_registry_remove,
};
xdg_wm_base_listener kWmBaseListener = {
    /* ping */ &WaylandWindow::on_xdg_wm_base_ping,
};
xdg_surface_listener kXdgSurfaceListener = {
    /* configure */ &WaylandWindow::on_xdg_surface_configure,
};
// xdg_toplevel listener: positional init, trailing fields zero-init.
// configure_bounds (v4) and wm_capabilities (v5) land as null on older
// headers — and on those same older systems the compositor won't even
// emit those events. Safe either way.
xdg_toplevel_listener kXdgToplevelListener = {
    /* configure        */ &WaylandWindow::on_toplevel_configure,
    /* close            */ &WaylandWindow::on_toplevel_close,
    /* configure_bounds */ &WaylandWindow::on_toplevel_configure_bounds,
    /* wm_capabilities  */ &WaylandWindow::on_toplevel_wm_capabilities,
};
wl_seat_listener kSeatListener = {
    /* capabilities */ &WaylandWindow::on_seat_capabilities,
    /* name         */ &WaylandWindow::on_seat_name,
};
wl_keyboard_listener kKeyboardListener = {
    /* keymap      */ &WaylandWindow::on_keyboard_keymap,
    /* enter       */ &WaylandWindow::on_keyboard_enter,
    /* leave       */ &WaylandWindow::on_keyboard_leave,
    /* key         */ &WaylandWindow::on_keyboard_key,
    /* modifiers   */ &WaylandWindow::on_keyboard_modifiers,
    /* repeat_info */ &WaylandWindow::on_keyboard_repeat_info,
};
// wl_pointer_listener: filled lazily inside init() so the binary stays
// portable across libwayland-client header versions. wl_pointer v8+
// (axis_value120) and v9+ (axis_relative_direction) callbacks don't
// exist as struct fields on older LTS distros; by filling only the
// guaranteed pre-v5 fields (enter / leave / motion / button / axis) plus
// optionally the v5 set (frame / axis_source / axis_stop / axis_discrete)
// we sidestep that ABI variance. The trailing fields are zero-initialized
// by the {} below — the compositor treats null callbacks as "ignored".
wl_pointer_listener kPointerListener = {
    /* enter         */ &WaylandWindow::on_pointer_enter,
    /* leave         */ &WaylandWindow::on_pointer_leave,
    /* motion        */ &WaylandWindow::on_pointer_motion,
    /* button        */ &WaylandWindow::on_pointer_button,
    /* axis          */ &WaylandWindow::on_pointer_axis,
    /* frame         */ &WaylandWindow::on_pointer_frame,
    /* axis_source   */ &WaylandWindow::on_pointer_axis_source,
    /* axis_stop     */ &WaylandWindow::on_pointer_axis_stop,
    /* axis_discrete */ &WaylandWindow::on_pointer_axis_discrete,
};

// ─── Init / teardown ─────────────────────────────────────────────────────
bool WaylandWindow::init(const WindowDesc& desc) noexcept {
    title_ = desc.title;
    render_w_ = desc.render_width;
    render_h_ = desc.render_height;
    window_w_ = desc.window_width;
    window_h_ = desc.window_height;
    scale_mode_ = desc.scale_mode;
    aspect_ = desc.aspect_mode;

    display_ = wl_display_connect(nullptr);
    if (!display_)
        return false;

    registry_ = wl_display_get_registry(display_);
    if (!registry_) {
        teardown();
        return false;
    }
    wl_registry_add_listener(registry_, &kRegistryListener, this);

    // Roundtrip 1: receive globals.
    if (wl_display_roundtrip(display_) < 0) {
        teardown();
        return false;
    }
    // Roundtrip 2: receive seat capabilities (kbd / pointer attach).
    if (wl_display_roundtrip(display_) < 0) {
        teardown();
        return false;
    }

    if (!globals_.compositor || !globals_.shm || !globals_.wm_base) {
        PSY_LOG_WARN("wayland: missing compositor / shm / xdg_wm_base; falling back");
        teardown();
        return false;
    }
    xdg_wm_base_add_listener(globals_.wm_base, &kWmBaseListener, this);

    surface_ = wl_compositor_create_surface(globals_.compositor);
    if (!surface_) {
        teardown();
        return false;
    }

    xdg_surf_ = xdg_wm_base_get_xdg_surface(globals_.wm_base, surface_);
    xdg_surface_add_listener(xdg_surf_, &kXdgSurfaceListener, this);
    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surf_);
    xdg_toplevel_add_listener(xdg_toplevel_, &kXdgToplevelListener, this);
    xdg_toplevel_set_title(xdg_toplevel_, title_.c_str());
    xdg_toplevel_set_app_id(xdg_toplevel_, "io.psynder.engine");

    // Viewporter is required for our scaled present.
    if (globals_.viewporter) {
        viewport_ = wp_viewporter_get_viewport(globals_.viewporter, surface_);
    }

    // SHM staging for the present.
    if (!ensure_shm_for(render_w_, render_h_)) {
        PSY_LOG_WARN("wayland: SHM pool alloc failed");
        teardown();
        return false;
    }

    // xkbcommon context.
    xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    // Commit so the compositor sends the initial configure.
    wl_surface_commit(surface_);
    while (!configured_) {
        if (wl_display_dispatch(display_) < 0) {
            teardown();
            return false;
        }
    }
    return true;
}

void WaylandWindow::teardown() noexcept {
    if (xkb_state_) {
        xkb_state_unref(xkb_state_);
        xkb_state_ = nullptr;
    }
    if (xkb_keymap_) {
        xkb_keymap_unref(xkb_keymap_);
        xkb_keymap_ = nullptr;
    }
    if (xkb_ctx_) {
        xkb_context_unref(xkb_ctx_);
        xkb_ctx_ = nullptr;
    }

    release_dmabuf();
    if (dma_heap_fd_ >= 0) {
        ::close(dma_heap_fd_);
        dma_heap_fd_ = -1;
    }

    if (shm_buffer_) {
        wl_buffer_destroy(shm_buffer_);
        shm_buffer_ = nullptr;
    }
    if (shm_pool_) {
        wl_shm_pool_destroy(shm_pool_);
        shm_pool_ = nullptr;
    }
    if (shm_data_ && shm_size_) {
        ::munmap(shm_data_, shm_size_);
        shm_data_ = nullptr;
        shm_size_ = 0;
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }

    if (viewport_) {
        wp_viewport_destroy(viewport_);
        viewport_ = nullptr;
    }
    if (xdg_toplevel_) {
        xdg_toplevel_destroy(xdg_toplevel_);
        xdg_toplevel_ = nullptr;
    }
    if (xdg_surf_) {
        xdg_surface_destroy(xdg_surf_);
        xdg_surf_ = nullptr;
    }
    if (surface_) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (globals_.pointer) {
        wl_pointer_destroy(globals_.pointer);
        globals_.pointer = nullptr;
    }
    if (globals_.keyboard) {
        wl_keyboard_destroy(globals_.keyboard);
        globals_.keyboard = nullptr;
    }
    if (globals_.dmabuf) {
        zwp_linux_dmabuf_v1_destroy(globals_.dmabuf);
        globals_.dmabuf = nullptr;
    }
    if (globals_.viewporter) {
        wp_viewporter_destroy(globals_.viewporter);
        globals_.viewporter = nullptr;
    }
    if (globals_.wm_base) {
        xdg_wm_base_destroy(globals_.wm_base);
        globals_.wm_base = nullptr;
    }
    if (globals_.seat) {
        wl_seat_destroy(globals_.seat);
        globals_.seat = nullptr;
    }
    if (globals_.shm) {
        wl_shm_destroy(globals_.shm);
        globals_.shm = nullptr;
    }
    if (globals_.compositor) {
        wl_compositor_destroy(globals_.compositor);
        globals_.compositor = nullptr;
    }
    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

// ─── SHM pool sizing ────────────────────────────────────────────────────
bool WaylandWindow::ensure_shm_for(u32 w, u32 h) noexcept {
    constexpr u32 kBpp = 4;  // RGBA8
    const size_t needed = static_cast<size_t>(w) * h * kBpp;
    if (needed == 0)
        return false;
    if (needed <= shm_size_ && shm_data_ && shm_buffer_)
        return true;
    return allocate_shm_pool(w, h);
}

bool WaylandWindow::allocate_shm_pool(u32 w, u32 h) noexcept {
    // Tear down any previous buffer / mapping first.
    if (shm_buffer_) {
        wl_buffer_destroy(shm_buffer_);
        shm_buffer_ = nullptr;
    }
    if (shm_pool_) {
        wl_shm_pool_destroy(shm_pool_);
        shm_pool_ = nullptr;
    }
    if (shm_data_ && shm_size_) {
        ::munmap(shm_data_, shm_size_);
        shm_data_ = nullptr;
        shm_size_ = 0;
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }

    constexpr u32 kBpp = 4;
    const size_t size = static_cast<size_t>(w) * h * kBpp;
    if (size == 0)
        return false;

    int fd = create_anonymous_file(static_cast<off_t>(size));
    if (fd < 0)
        return false;

    void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    wl_shm_pool* pool = wl_shm_create_pool(globals_.shm, fd, static_cast<int32_t>(size));
    if (!pool) {
        ::munmap(map, size);
        ::close(fd);
        return false;
    }

    wl_buffer* buf = wl_shm_pool_create_buffer(pool,
                                               0,
                                               static_cast<int32_t>(w),
                                               static_cast<int32_t>(h),
                                               static_cast<int32_t>(w * kBpp),
                                               WL_SHM_FORMAT_XRGB8888);
    if (!buf) {
        wl_shm_pool_destroy(pool);
        ::munmap(map, size);
        ::close(fd);
        return false;
    }

    shm_fd_ = fd;
    shm_data_ = map;
    shm_size_ = size;
    shm_pool_ = pool;
    shm_buffer_ = buf;
    return true;
}

// ─── dmabuf zero-copy path (Wave B) ──────────────────────────────────────
//
// Strategy:
//   1. Skip if the compositor never bound zwp_linux_dmabuf_v1 (no
//      `globals_.dmabuf`), or if we already determined the path is
//      unusable on this host (no dma-heap, alloc failed, etc.).
//   2. Lazily open /dev/dma_heap/system on the first call.
//   3. Allocate a linear dmabuf sized to width*height*4 via the heap
//      ioctl; mmap it as the staging area; hand the fd to the compositor
//      through zwp_linux_buffer_params_v1::create_immed.
//   4. Per-frame: memcpy the framebuffer into the mapped dmabuf with the
//      same RGBA → XRGB byte swizzle as the shm path; attach the wl_buffer
//      to the surface; commit.
//
// On any failure we mark the path disabled and return false — the caller
// falls back to wl_shm for the rest of the program's lifetime, so we
// don't keep paying syscall overhead probing a missing /dev/dma_heap.
void WaylandWindow::release_dmabuf() noexcept {
    if (dma_buffer_) {
        wl_buffer_destroy(dma_buffer_);
        dma_buffer_ = nullptr;
    }
    if (dma_map_ && dma_size_) {
        ::munmap(dma_map_, dma_size_);
        dma_map_ = nullptr;
        dma_size_ = 0;
    }
    if (dma_fd_ >= 0) {
        ::close(dma_fd_);
        dma_fd_ = -1;
    }
    dma_w_ = dma_h_ = 0;
}

bool WaylandWindow::ensure_dmabuf_for(u32 w, u32 h) noexcept {
    if (dma_disabled_)
        return false;
    if (!globals_.dmabuf) {
        dma_disabled_ = true;
        return false;
    }
    if (dma_buffer_ && dma_w_ == w && dma_h_ == h)
        return true;

    // Drop the prior buffer; we'll allocate a freshly-sized one.
    release_dmabuf();

    if (dma_heap_fd_ < 0) {
        dma_heap_fd_ = ::open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
        if (dma_heap_fd_ < 0) {
            // Try the legacy CMA heap name as a fallback. Most modern
            // distros expose `system`; servers/embedded sometimes use
            // `linux,cma` instead.
            dma_heap_fd_ = ::open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
        }
        if (dma_heap_fd_ < 0) {
            dma_disabled_ = true;
            return false;
        }
    }

    constexpr u32 kBpp = 4;
    const std::uint64_t size = static_cast<std::uint64_t>(w) * h * kBpp;
    if (size == 0) {
        dma_disabled_ = true;
        return false;
    }

    psy_dma_heap_allocation_data req{};
    req.len = size;
    req.fd_flags = O_RDWR | O_CLOEXEC;
    if (::ioctl(dma_heap_fd_, DMA_HEAP_IOCTL_ALLOC, &req) < 0) {
        dma_disabled_ = true;
        return false;
    }

    void* map = ::mmap(nullptr,
                       static_cast<size_t>(size),
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED,
                       static_cast<int>(req.fd),
                       0);
    if (map == MAP_FAILED) {
        ::close(static_cast<int>(req.fd));
        dma_disabled_ = true;
        return false;
    }

    // Build the wl_buffer via zwp_linux_buffer_params_v1.
    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(globals_.dmabuf);
    if (!params) {
        ::munmap(map, static_cast<size_t>(size));
        ::close(static_cast<int>(req.fd));
        dma_disabled_ = true;
        return false;
    }
    // Plane 0 — single-plane XRGB8888 with linear modifier (0 hi, 0 lo).
    zwp_linux_buffer_params_v1_add(params,
                                   static_cast<int32_t>(req.fd),
                                   /*plane_idx*/ 0u,
                                   /*offset*/ 0u,
                                   /*stride*/ w * kBpp,
                                   /*modifier_hi*/ 0u,
                                   /*modifier_lo*/ 0u);
    wl_buffer* buf = zwp_linux_buffer_params_v1_create_immed(params,
                                                             static_cast<int32_t>(w),
                                                             static_cast<int32_t>(h),
                                                             DRM_FORMAT_XRGB8888,
                                                             /*flags*/ 0u);
    zwp_linux_buffer_params_v1_destroy(params);

    if (!buf) {
        ::munmap(map, static_cast<size_t>(size));
        ::close(static_cast<int>(req.fd));
        dma_disabled_ = true;
        return false;
    }

    dma_fd_ = static_cast<int>(req.fd);
    dma_map_ = map;
    dma_size_ = static_cast<size_t>(size);
    dma_buffer_ = buf;
    dma_w_ = w;
    dma_h_ = h;
    return true;
}

bool WaylandWindow::try_dmabuf_present(const render::Framebuffer& fb) noexcept {
    if (!ensure_dmabuf_for(fb.width, fb.height))
        return false;

    // Same RGBA → XRGB swizzle as the shm path. With dmabuf the
    // compositor reads the linear buffer directly — no extra GPU copy.
    const u32* src = reinterpret_cast<const u32*>(fb.pixels);
    u32* dst = reinterpret_cast<u32*>(dma_map_);
    const size_t pixels_n = static_cast<size_t>(dma_w_) * dma_h_;
    for (size_t i = 0; i < pixels_n; ++i) {
        const u32 v = src[i];
        const u32 r = (v) & 0xFFu;
        const u32 g = (v >> 8) & 0xFFu;
        const u32 b = (v >> 16) & 0xFFu;
        dst[i] = (b) | (g << 8) | (r << 16) | (0xFFu << 24);
    }

    // Viewport update — identical to the shm path. The scale / aspect
    // computation doesn't depend on the upload mechanism.
    if (viewport_) {
        wp_viewport_set_source(viewport_,
                               wl_fixed_from_int(0),
                               wl_fixed_from_int(0),
                               wl_fixed_from_int(static_cast<int>(dma_w_)),
                               wl_fixed_from_int(static_cast<int>(dma_h_)));
        const BlitRect r = compute_blit_rect(static_cast<int>(window_w_),
                                             static_cast<int>(window_h_),
                                             static_cast<int>(dma_w_),
                                             static_cast<int>(dma_h_),
                                             aspect_,
                                             scale_mode_);
        const int dst_w = r.w > 0 ? r.w : static_cast<int>(window_w_);
        const int dst_h = r.h > 0 ? r.h : static_cast<int>(window_h_);
        wp_viewport_set_destination(viewport_, dst_w, dst_h);
    }

    wl_surface_attach(surface_, dma_buffer_, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, static_cast<int32_t>(dma_w_), static_cast<int32_t>(dma_h_));
    wl_surface_commit(surface_);
    wl_display_flush(display_);
    return true;
}

// ─── Frame pump ──────────────────────────────────────────────────────────
void WaylandWindow::poll_events() {
    if (!display_)
        return;
    // Non-blocking dispatch: drain whatever events are already queued for
    // this client (keyboard / pointer / configure callbacks) without
    // sleeping waiting for the compositor. flush() pushes our outgoing
    // queue (the present commit from the previous frame's wl_surface
    // operations) so the compositor sees it before we paint again.
    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);
    // Edge-advance is per-frame: clear deltas and promote down→prev for
    // key_pressed().
    input_frame_advance();
}

void WaylandWindow::present(const render::Framebuffer& fb) {
    if (!surface_ || !configured_)
        return;
    if (fb.width == 0 || fb.height == 0 || fb.pixels == nullptr)
        return;

    // ─── Wave-B: zero-copy dmabuf upload when available ──────────────
    // try_dmabuf_present commits the surface internally on success and
    // returns true; on false we fall through to the shm memcpy path.
    if (try_dmabuf_present(fb)) {
        return;
    }

    if (!shm_data_)
        return;

    // Resize SHM if the engine changed render resolution between frames.
    if (fb.width != render_w_ || fb.height != render_h_) {
        render_w_ = fb.width;
        render_h_ = fb.height;
        if (!allocate_shm_pool(render_w_, render_h_))
            return;
    }

    // Copy framebuffer into the SHM pool. RGBA → XRGB byte order: source
    // bytes are R,G,B,A (little-endian as u32 = 0xAABBGGRR). The
    // WL_SHM_FORMAT_XRGB8888 layout is little-endian XRGB — host-byte
    // order on x86 places X at byte 3. We swizzle on the way in.
    // The rasterizer in lane 07 packs as
    //   rgba = R | G<<8 | B<<16 | A<<24
    // i.e. byte 0 = R, byte 1 = G, byte 2 = B, byte 3 = A. The WL XRGB
    // layout (little endian) wants byte 0 = B, byte 1 = G, byte 2 = R,
    // byte 3 = X. So we BSwap channels per pixel.
    const u32* src = reinterpret_cast<const u32*>(fb.pixels);
    u32* dst = reinterpret_cast<u32*>(shm_data_);
    const size_t pixels_n = static_cast<size_t>(render_w_) * render_h_;
    for (size_t i = 0; i < pixels_n; ++i) {
        const u32 v = src[i];
        const u32 r = (v) & 0xFFu;
        const u32 g = (v >> 8) & 0xFFu;
        const u32 b = (v >> 16) & 0xFFu;
        dst[i] = (b) | (g << 8) | (r << 16) | (0xFFu << 24);
    }

    // Update the viewport per current window size + scale/aspect mode.
    if (viewport_) {
        // Source rect is the whole framebuffer.
        wp_viewport_set_source(viewport_,
                               wl_fixed_from_int(0),
                               wl_fixed_from_int(0),
                               wl_fixed_from_int(static_cast<int>(render_w_)),
                               wl_fixed_from_int(static_cast<int>(render_h_)));
        const BlitRect r = compute_blit_rect(static_cast<int>(window_w_),
                                             static_cast<int>(window_h_),
                                             static_cast<int>(render_w_),
                                             static_cast<int>(render_h_),
                                             aspect_,
                                             scale_mode_);
        const int dst_w = r.w > 0 ? r.w : static_cast<int>(window_w_);
        const int dst_h = r.h > 0 ? r.h : static_cast<int>(window_h_);
        wp_viewport_set_destination(viewport_, dst_w, dst_h);
    }

    wl_surface_attach(surface_, shm_buffer_, 0, 0);
    wl_surface_damage_buffer(surface_,
                             0,
                             0,
                             static_cast<int32_t>(render_w_),
                             static_cast<int32_t>(render_h_));
    wl_surface_commit(surface_);
    wl_display_flush(display_);
}

// ─── Listener bodies ─────────────────────────────────────────────────────
void WaylandWindow::on_registry(
    void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    auto* w = static_cast<WaylandWindow*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        w->globals_.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, std::min<uint32_t>(version, 4u)));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        w->globals_.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        w->globals_.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, std::min<uint32_t>(version, 5u)));
    } else if (std::strcmp(iface, wp_viewporter_interface.name) == 0) {
        w->globals_.viewporter =
            static_cast<wp_viewporter*>(wl_registry_bind(reg, name, &wp_viewporter_interface, 1));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        w->globals_.seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, std::min<uint32_t>(version, 7u)));
        wl_seat_add_listener(w->globals_.seat, &kSeatListener, w);
    } else if (std::strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        // Bind at v3 (immediate-buffer creation + format/modifier events).
        // Older compositors only advertise v1/v2 — clamp to what they
        // expose so we don't trip a protocol error.
        w->globals_.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, std::min<uint32_t>(version, 3u)));
    }
}

void WaylandWindow::on_registry_remove(void*, wl_registry*, uint32_t) {}

void WaylandWindow::on_xdg_wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

void WaylandWindow::on_xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial) {
    auto* w = static_cast<WaylandWindow*>(data);
    xdg_surface_ack_configure(surf, serial);
    w->configured_ = true;
}

void WaylandWindow::on_toplevel_configure(void* data, xdg_toplevel*, int32_t w_, int32_t h_, wl_array*) {
    auto* w = static_cast<WaylandWindow*>(data);
    if (w_ > 0)
        w->window_w_ = static_cast<u32>(w_);
    if (h_ > 0)
        w->window_h_ = static_cast<u32>(h_);
}

void WaylandWindow::on_toplevel_close(void* data, xdg_toplevel*) {
    static_cast<WaylandWindow*>(data)->closed_ = true;
}
void WaylandWindow::on_toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void WaylandWindow::on_toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

void WaylandWindow::on_seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* w = static_cast<WaylandWindow*>(data);
    const bool has_kbd = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_kbd && !w->globals_.keyboard) {
        w->globals_.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->globals_.keyboard, &kKeyboardListener, w);
    } else if (!has_kbd && w->globals_.keyboard) {
        wl_keyboard_destroy(w->globals_.keyboard);
        w->globals_.keyboard = nullptr;
    }
    if (has_ptr && !w->globals_.pointer) {
        w->globals_.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->globals_.pointer, &kPointerListener, w);
    } else if (!has_ptr && w->globals_.pointer) {
        wl_pointer_destroy(w->globals_.pointer);
        w->globals_.pointer = nullptr;
    }
}
void WaylandWindow::on_seat_name(void*, wl_seat*, const char*) {}

// ─── Keyboard ─────────────────────────────────────────────────────────────
void WaylandWindow::on_keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto* w = static_cast<WaylandWindow*>(data);
    if (!w->xkb_ctx_ || format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ::close(fd);
        return;
    }
    char* km_str = static_cast<char*>(::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    ::close(fd);
    if (km_str == MAP_FAILED)
        return;
    xkb_keymap* km = xkb_keymap_new_from_string(w->xkb_ctx_,
                                                km_str,
                                                XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
    ::munmap(km_str, size);
    if (!km)
        return;
    if (w->xkb_keymap_)
        xkb_keymap_unref(w->xkb_keymap_);
    if (w->xkb_state_)
        xkb_state_unref(w->xkb_state_);
    w->xkb_keymap_ = km;
    w->xkb_state_ = xkb_state_new(km);
}
void WaylandWindow::on_keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void WaylandWindow::on_keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}

void WaylandWindow::on_keyboard_key(
    void* data, wl_keyboard*, uint32_t /*serial*/, uint32_t /*time*/, uint32_t key, uint32_t state) {
    auto* w = static_cast<WaylandWindow*>(data);
    KeyCode kc = keycode_from_evdev(key);
    if (kc == KeyCode::Unknown && w->xkb_state_) {
        // Wayland keys are evdev codes offset by 8 (X11 historical reasons).
        const xkb_keycode_t xkc = key + 8;
        const xkb_keysym_t sym = xkb_state_key_get_one_sym(w->xkb_state_, xkc);
        kc = keycode_from_xkb(static_cast<uint32_t>(sym));
    }
    input_push_key(kc, state == WL_KEYBOARD_KEY_STATE_PRESSED);
    // Text entry for the software console. xkb_state_key_get_utf32 maps the
    // key through the active layout + modifier state to a Unicode scalar
    // (0 when the key yields no character); input_push_text drops controls.
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && w->xkb_state_) {
        const xkb_keycode_t xkc = key + 8;
        const uint32_t cp = xkb_state_key_get_utf32(w->xkb_state_, xkc);
        if (cp != 0)
            input_push_text(cp);
    }
}
void WaylandWindow::on_keyboard_modifiers(void* data,
                                          wl_keyboard*,
                                          uint32_t,
                                          uint32_t mods_depressed,
                                          uint32_t mods_latched,
                                          uint32_t mods_locked,
                                          uint32_t group) {
    auto* w = static_cast<WaylandWindow*>(data);
    if (w->xkb_state_) {
        xkb_state_update_mask(w->xkb_state_, mods_depressed, mods_latched, mods_locked, 0, 0, group);
    }
}
void WaylandWindow::on_keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

// ─── Pointer ──────────────────────────────────────────────────────────────
void WaylandWindow::on_pointer_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
}
void WaylandWindow::on_pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}

void WaylandWindow::on_pointer_motion(void* /*data*/, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    const float ax = static_cast<float>(wl_fixed_to_double(sx));
    const float ay = static_cast<float>(wl_fixed_to_double(sy));
    // wl_pointer.motion is absolute within the surface; deltas computed
    // here would race against the frame boundary. The relative-pointer
    // protocol is the right place for fps mouselook; for Wave A we keep
    // the absolute path only.
    input_push_mouse_motion(0.f, 0.f, ax, ay);
}

void WaylandWindow::on_pointer_button(
    void*, wl_pointer*, uint32_t /*serial*/, uint32_t /*time*/, uint32_t button, uint32_t state) {
    int b = -1;
    switch (button) {
        case evdev::BTN_LEFT:
            b = 0;
            break;
        case evdev::BTN_RIGHT:
            b = 1;
            break;
        case evdev::BTN_MIDDLE:
            b = 2;
            break;
        default:
            return;
    }
    input_push_mouse_button(b, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void WaylandWindow::on_pointer_axis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        input_push_mouse_wheel(static_cast<float>(-wl_fixed_to_double(value)));
    }
}
void WaylandWindow::on_pointer_frame(void*, wl_pointer*) {}
void WaylandWindow::on_pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void WaylandWindow::on_pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void WaylandWindow::on_pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

}  // namespace

Window* try_create_wayland_window(const WindowDesc& desc) noexcept {
    auto* w = new (std::nothrow) WaylandWindow();
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
