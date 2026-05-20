// SPDX-License-Identifier: MIT
// Psynder — evdev input source. Lane 22.
//
// Wayland / X11 give us keyboard + mouse on the focused surface. evdev is
// the route to *gamepads* (and to keyboard/mouse if a future build needs
// it for headless workflows). For Wave A this TU's job is to discover
// gamepads under /dev/input/event* and tag them so the engine can poll
// axes / buttons.
//
// Scope:
//   - Enumerate /dev/input/event* at startup.
//   - For each device, query EVIOCGBIT(EV_KEY) / EVIOCGBIT(EV_ABS) to
//     classify joystick-likes (presence of BTN_GAMEPAD or BTN_SOUTH +
//     ABS_X axis).
//   - Open the device O_RDONLY | O_NONBLOCK and add it to a poll() set
//     drained from a background thread; events go into the shared input
//     state via input_push_*().
//
// Wave-A note: the public Input interface in Platform.h doesn't surface
// per-gamepad axes / button bitmasks — Wave B is where that lands once
// lane 16/17 wire UI controller support. So this TU detects + opens
// devices and prints them to the log; the actual axis stream is
// translated into raw key events for now (mappable later).
//
// We *intentionally* don't fight the Wayland/X11 keyboard for input
// focus — picking up /dev/input/event* keyboards while a Wayland surface
// also gets keys would duplicate events.  Gamepads are the only path
// that's purely evdev.

#ifdef PSYNDER_PLATFORM_LINUX

#include "LinuxPlatform_internal.h"

#include "core/Log.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace psynder::platform::linux_impl {

namespace {

constexpr int kMaxGamepads = 8;

struct EvdevDevice {
    int fd = -1;
    char name[64] = {};
    bool is_pad = false;
};

class EvdevPump {
   public:
    EvdevPump() = default;
    ~EvdevPump() { stop(); }

    void start() noexcept {
        if (running_.exchange(true, std::memory_order_acq_rel))
            return;
        scan();
        if (devices_.empty())
            return;
        worker_ = std::thread([this] { run(); });
    }

    void stop() noexcept {
        if (!running_.exchange(false, std::memory_order_acq_rel))
            return;
        if (worker_.joinable())
            worker_.join();
        for (auto& d : devices_) {
            if (d.fd >= 0)
                ::close(d.fd);
        }
        devices_.clear();
    }

   private:
    // ─── Bit-test helpers for EVIOCGBIT results ────────────────────────
    static bool test_bit(const std::uint8_t* mask, int bit) {
        return (mask[bit / 8] & (1u << (bit % 8))) != 0;
    }

    bool open_device(const char* path) noexcept {
        int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return false;
        char name[64] = {};
        if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            std::strncpy(name, "(unknown)", sizeof(name) - 1);
        }
        // Classify: a gamepad has BTN_GAMEPAD or BTN_SOUTH and ABS_X.
        std::uint8_t key_bits[(KEY_MAX / 8) + 1] = {};
        std::uint8_t abs_bits[(ABS_MAX / 8) + 1] = {};
        ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
        ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        const bool has_pad_btn = test_bit(key_bits, BTN_GAMEPAD) || test_bit(key_bits, BTN_SOUTH) ||
                                 test_bit(key_bits, BTN_A);
        const bool has_pad_axis = test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y);
        const bool is_pad = has_pad_btn && has_pad_axis;
        if (!is_pad) {
            ::close(fd);
            return false;
        }
        if (devices_.size() >= static_cast<size_t>(kMaxGamepads)) {
            ::close(fd);
            return false;
        }
        EvdevDevice d;
        d.fd = fd;
        d.is_pad = true;
        std::strncpy(d.name, name, sizeof(d.name) - 1);
        devices_.push_back(d);
        PSY_LOG_INFO("evdev: gamepad opened: {} ({})", d.name, path);
        return true;
    }

    void scan() noexcept {
        DIR* dir = ::opendir("/dev/input");
        if (!dir)
            return;
        while (dirent* ent = ::readdir(dir)) {
            if (std::strncmp(ent->d_name, "event", 5) != 0)
                continue;
            char path[64];
            std::snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            (void)open_device(path);
        }
        ::closedir(dir);
    }

    void run() noexcept {
        std::vector<pollfd> fds;
        fds.reserve(devices_.size());
        for (auto& d : devices_) {
            fds.push_back({d.fd, POLLIN, 0});
        }
        while (running_.load(std::memory_order_acquire)) {
            int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 50);
            if (n <= 0)
                continue;
            for (size_t i = 0; i < fds.size(); ++i) {
                if ((fds[i].revents & POLLIN) == 0)
                    continue;
                input_event evs[32];
                ssize_t got = ::read(fds[i].fd, evs, sizeof(evs));
                if (got <= 0)
                    continue;
                const size_t count = static_cast<size_t>(got) / sizeof(input_event);
                for (size_t j = 0; j < count; ++j) {
                    handle_event(evs[j]);
                }
            }
        }
    }

    void handle_event(const input_event& ev) noexcept {
        // Wave-A: map gamepad buttons to a couple of named keys so the
        // sample can prove the wire is alive.  Full axis stream will plug
        // into the engine input vocabulary once lane 16 lands.
        if (ev.type == EV_KEY) {
            const bool down = (ev.value != 0);
            KeyCode kc = KeyCode::Unknown;
            switch (ev.code) {
                case BTN_SOUTH:
                    kc = KeyCode::Enter;
                    break;  // A / Cross
                case BTN_EAST:
                    kc = KeyCode::Escape;
                    break;  // B / Circle
                case BTN_START:
                    kc = KeyCode::Tilde;
                    break;
                default:
                    break;
            }
            if (kc != KeyCode::Unknown) {
                input_push_key(kc, down);
            }
        } else if (ev.type == EV_ABS) {
            // Translate the left stick to mouse-delta-equivalent for now.
            // ABS_X / ABS_Y values are device-scaled; we'd normalize via
            // EVIOCGABS but keep this Wave-A-quick.
            if (ev.code == ABS_X || ev.code == ABS_Y) {
                constexpr float kDeadzone = 4000.f;
                const float v = static_cast<float>(ev.value);
                if (v > kDeadzone || v < -kDeadzone) {
                    const float dx = (ev.code == ABS_X) ? v / 32768.f : 0.f;
                    const float dy = (ev.code == ABS_Y) ? v / 32768.f : 0.f;
                    input_push_mouse_motion(dx, dy, 0.f, 0.f);
                }
            }
        }
    }

    std::vector<EvdevDevice> devices_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

EvdevPump g_evdev;

}  // namespace

// Called from the Wayland / X11 window backends after a successful create.
void evdev_start() noexcept {
    g_evdev.start();
}
void evdev_stop() noexcept {
    g_evdev.stop();
}

}  // namespace psynder::platform::linux_impl

#endif  // PSYNDER_PLATFORM_LINUX
