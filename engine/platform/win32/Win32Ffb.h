// SPDX-License-Identifier: MIT
// Psynder — DirectInput8 force-feedback wheel support (Wave B).
//
// Wired by sample 04's NFS-style track demo. The wheel is enumerated via
// `IDirectInput8::EnumDevices(DI8DEVCLASS_GAMECTRL, ..., DIEDFL_FORCEFEEDBACK)`,
// acquired in exclusive mode, then two effects are constructed:
//   * **Constant force** — DC torque, used by the driving model to apply a
//     steady tug while skidding / hitting kerbs.
//   * **Spring centering** — auto-centering effect (DICONDITION), used so
//     the wheel returns to centre when the player lets go.
//
// On non-Windows hosts the whole lane is platform-guarded — `Win32Ffb` only
// exists when `PSYNDER_PLATFORM_WIN32` is defined. The unit test exercises
// the descriptor-builder helpers (which live in `Win32Ffb.cpp`) via a
// host-only Catch2 case; on Mac / Linux the test binary links against a
// sentinel symbol from the test TU itself.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Common.h"

// DirectInput8 — we use the modern v8 surface so this works on every
// supported Windows host. dxguid + dinput8 import libs are added in the
// lane's CMakeLists when PSYNDER_PLATFORM_WIN32 is on.
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

namespace psynder::platform::win32 {

// Per-effect parameters surfaced to the game thread. Magnitudes are in the
// DirectInput-native `LONG` range [-10000, +10000].
struct FfbEffectDesc {
    // Constant force magnitude. Positive = right torque on the wheel.
    LONG constant_magnitude = 0;
    // Spring centering "stiffness" (0 = off, 10000 = fully stiff).
    LONG spring_coefficient = 5000;
    // Direction (Polar). 0 = north / "centre".
    DWORD direction = 0;
};

// Build a DIEFFECT for a constant-force descriptor. Pure function — used
// by the unit test to verify the descriptor layout without needing a real
// DirectInput device.
//
// `axes` / `direction_buf` / `cf_buf` must point at storage with at least
// one element each; the DIEFFECT references them by pointer so they must
// outlive the call to `CreateEffect`. Returns the populated DIEFFECT.
DIEFFECT build_constant_force_effect(
    const FfbEffectDesc& desc,
    DWORD*               axes,
    LONG*                direction_buf,
    DICONSTANTFORCE*     cf_buf) noexcept;

// Build a DIEFFECT for a spring centering descriptor (DICONDITION). Same
// ownership contract as `build_constant_force_effect`.
DIEFFECT build_spring_effect(
    const FfbEffectDesc& desc,
    DWORD*               axes,
    LONG*                direction_buf,
    DICONDITION*         cond_buf) noexcept;

// Process-wide FFB controller. Created on demand by the sample apps; the
// platform layer itself doesn't poll for wheels (lane 09 / input owns that
// in a later wave). Calling `acquire_first_wheel()` enumerates devices and
// latches onto the first FF-capable device found. Returns true on success.
class Win32Ffb {
public:
    Win32Ffb()  = default;
    ~Win32Ffb() { shutdown(); }

    Win32Ffb(const Win32Ffb&)            = delete;
    Win32Ffb& operator=(const Win32Ffb&) = delete;

    // Create the DirectInput8 root + enumerate the first FF wheel. Window
    // handle may be null for headless boot — exclusive cooperative level
    // requires a foreground window, so without one we silently degrade to
    // non-exclusive (read-only state, no FF effects). Returns true if at
    // least one FF device was acquired.
    bool acquire_first_wheel(HWND hwnd = nullptr) noexcept;

    // Apply / update the constant force. If the effect hasn't been built
    // yet this lazily creates it; subsequent calls call SetParameters.
    bool set_constant_force(LONG magnitude, DWORD direction = 0) noexcept;

    // Engage / update the spring centering effect.
    bool set_spring(LONG coefficient) noexcept;

    // Stop both effects (used when leaving a vehicle / unpausing the menu).
    void stop_all() noexcept;

    // Tear everything down. Called by the destructor; idempotent.
    void shutdown() noexcept;

    bool has_wheel() const noexcept { return device_ != nullptr; }

private:
    ComPtr<IDirectInput8W>       dinput_;
    ComPtr<IDirectInputDevice8W> device_;
    ComPtr<IDirectInputEffect>   constant_;
    ComPtr<IDirectInputEffect>   spring_;
    LONG                         current_constant_ = 0;
    LONG                         current_spring_   = 0;
};

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
