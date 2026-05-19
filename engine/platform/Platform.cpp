// SPDX-License-Identifier: MIT
// Psynder — platform shared shim. The actual window / input / clock impl
// lives in the platform-specific lane (win32, linux, macos). Provides a
// dispatching create_window() the samples call into.

#include "Platform.h"

#include <chrono>

namespace psynder::platform {

// Each platform lane provides this factory.
Window* create_window_impl(const WindowDesc& desc);
void    destroy_window_impl(Window* w);

Window* create_window(const WindowDesc& desc)  { return create_window_impl(desc); }
void    destroy_window(Window* w)              { destroy_window_impl(w); }

u64 Clock::ticks_now() {
    using clock = std::chrono::steady_clock;
    return static_cast<u64>(clock::now().time_since_epoch().count());
}
u64 Clock::ticks_per_second() {
    using clock = std::chrono::steady_clock;
    return clock::period::den / clock::period::num;
}
f64 Clock::seconds(u64 ticks) {
    return static_cast<f64>(ticks) / static_cast<f64>(ticks_per_second());
}

}  // namespace psynder::platform
