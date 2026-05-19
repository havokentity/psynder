// SPDX-License-Identifier: MIT
// Psynder — DirectInput8 force-feedback wheel implementation.

#include "Win32Ffb.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "core/Log.h"

#include <cstring>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace psynder::platform::win32 {

namespace {

// Clamp a LONG into [-10000, +10000] — DirectInput's documented full-scale.
LONG clamp_di(LONG v) noexcept {
    if (v >  10000) return  10000;
    if (v < -10000) return -10000;
    return v;
}

// Enumeration trampoline for the first FF-capable device. The context
// pointer is the DIDEVICEINSTANCE storage we want to fill.
BOOL CALLBACK enum_first_wheel(LPCDIDEVICEINSTANCEW didi, LPVOID ctx) {
    auto* out = static_cast<DIDEVICEINSTANCEW*>(ctx);
    *out = *didi;
    return DIENUM_STOP;  // first match wins
}

}  // namespace

// ── Descriptor builders ─────────────────────────────────────────────────
DIEFFECT build_constant_force_effect(
    const FfbEffectDesc& desc,
    DWORD*               axes,
    LONG*                direction_buf,
    DICONSTANTFORCE*     cf_buf) noexcept
{
    axes[0]         = DIJOFS_X;
    direction_buf[0] = static_cast<LONG>(desc.direction);
    cf_buf->lMagnitude = clamp_di(desc.constant_magnitude);

    DIEFFECT eff{};
    eff.dwSize                  = sizeof(DIEFFECT);
    eff.dwFlags                 = DIEFF_POLAR | DIEFF_OBJECTOFFSETS;
    eff.dwDuration              = INFINITE;
    eff.dwSamplePeriod          = 0;
    eff.dwGain                  = DI_FFNOMINALMAX;
    eff.dwTriggerButton         = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes                   = 1;
    eff.rgdwAxes                = axes;
    eff.rglDirection            = direction_buf;
    eff.lpEnvelope              = nullptr;
    eff.cbTypeSpecificParams    = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams   = cf_buf;
    eff.dwStartDelay            = 0;
    return eff;
}

DIEFFECT build_spring_effect(
    const FfbEffectDesc& desc,
    DWORD*               axes,
    LONG*                direction_buf,
    DICONDITION*         cond_buf) noexcept
{
    axes[0]         = DIJOFS_X;
    direction_buf[0] = 0;  // centring is symmetric — direction unused

    cond_buf->lOffset              = 0;
    cond_buf->lPositiveCoefficient = clamp_di(desc.spring_coefficient);
    cond_buf->lNegativeCoefficient = clamp_di(desc.spring_coefficient);
    cond_buf->dwPositiveSaturation = DI_FFNOMINALMAX;
    cond_buf->dwNegativeSaturation = DI_FFNOMINALMAX;
    cond_buf->lDeadBand            = 0;

    DIEFFECT eff{};
    eff.dwSize                  = sizeof(DIEFFECT);
    eff.dwFlags                 = DIEFF_POLAR | DIEFF_OBJECTOFFSETS;
    eff.dwDuration              = INFINITE;
    eff.dwSamplePeriod          = 0;
    eff.dwGain                  = DI_FFNOMINALMAX;
    eff.dwTriggerButton         = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes                   = 1;
    eff.rgdwAxes                = axes;
    eff.rglDirection            = direction_buf;
    eff.lpEnvelope              = nullptr;
    eff.cbTypeSpecificParams    = sizeof(DICONDITION);
    eff.lpvTypeSpecificParams   = cond_buf;
    eff.dwStartDelay            = 0;
    return eff;
}

// ── Device acquire / effect lifetime ────────────────────────────────────
bool Win32Ffb::acquire_first_wheel(HWND hwnd) noexcept {
    if (device_) return true;

    HRESULT hr = ::DirectInput8Create(
        ::GetModuleHandleW(nullptr),
        DIRECTINPUT_VERSION,
        IID_IDirectInput8W,
        reinterpret_cast<void**>(dinput_.GetAddressOf()),
        nullptr);
    if (!psy_hr_ok(hr, "DirectInput8Create")) return false;

    DIDEVICEINSTANCEW didi{};
    didi.dwSize = sizeof(DIDEVICEINSTANCEW);
    hr = dinput_->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        &enum_first_wheel,
        &didi,
        DIEDFL_FORCEFEEDBACK | DIEDFL_ATTACHEDONLY);
    if (FAILED(hr) || didi.guidInstance == GUID_NULL) {
        PSY_LOG_INFO("[win32-ffb] no force-feedback wheel attached");
        dinput_.Reset();
        return false;
    }

    if (!psy_hr_ok(dinput_->CreateDevice(
            didi.guidInstance, device_.GetAddressOf(), nullptr),
            "IDirectInput8::CreateDevice")) {
        dinput_.Reset();
        return false;
    }

    if (!psy_hr_ok(device_->SetDataFormat(&c_dfDIJoystick2),
                   "IDirectInputDevice8::SetDataFormat")) {
        shutdown();
        return false;
    }

    // Exclusive cooperative level is required for FF; only achievable when
    // we own a foreground window. Without one, accept non-exclusive and
    // disable FF effects.
    DWORD coop = DISCL_BACKGROUND | DISCL_NONEXCLUSIVE;
    if (hwnd) coop = DISCL_FOREGROUND | DISCL_EXCLUSIVE;
    if (!psy_hr_ok(device_->SetCooperativeLevel(hwnd, coop),
                   "IDirectInputDevice8::SetCooperativeLevel")) {
        shutdown();
        return false;
    }

    if (!psy_hr_ok(device_->Acquire(), "IDirectInputDevice8::Acquire")) {
        // Acquire can fail transiently when the window is not yet
        // foreground — keep the device handle and let the next call retry.
        return false;
    }
    return true;
}

bool Win32Ffb::set_constant_force(LONG magnitude, DWORD direction) noexcept {
    if (!device_) return false;
    current_constant_ = clamp_di(magnitude);

    DWORD           axes[1]{};
    LONG            dir [1]{};
    DICONSTANTFORCE cf{};
    FfbEffectDesc   desc{current_constant_, current_spring_, direction};
    DIEFFECT        eff = build_constant_force_effect(desc, axes, dir, &cf);

    if (!constant_) {
        HRESULT hr = device_->CreateEffect(
            GUID_ConstantForce, &eff, constant_.GetAddressOf(), nullptr);
        if (!psy_hr_ok(hr, "CreateEffect(ConstantForce)")) return false;
        return psy_hr_ok(constant_->Start(1, 0), "Start(ConstantForce)");
    }
    // Existing effect — patch magnitude + direction in-place. We don't need
    // to restart; the loop is INFINITE.
    return psy_hr_ok(constant_->SetParameters(
        &eff,
        DIEP_TYPESPECIFICPARAMS | DIEP_DIRECTION | DIEP_START),
        "SetParameters(ConstantForce)");
}

bool Win32Ffb::set_spring(LONG coefficient) noexcept {
    if (!device_) return false;
    current_spring_ = clamp_di(coefficient);

    DWORD        axes[1]{};
    LONG         dir [1]{};
    DICONDITION  cond{};
    FfbEffectDesc desc{current_constant_, current_spring_, 0};
    DIEFFECT     eff = build_spring_effect(desc, axes, dir, &cond);

    if (!spring_) {
        HRESULT hr = device_->CreateEffect(
            GUID_Spring, &eff, spring_.GetAddressOf(), nullptr);
        if (!psy_hr_ok(hr, "CreateEffect(Spring)")) return false;
        return psy_hr_ok(spring_->Start(1, 0), "Start(Spring)");
    }
    return psy_hr_ok(spring_->SetParameters(
        &eff,
        DIEP_TYPESPECIFICPARAMS | DIEP_START),
        "SetParameters(Spring)");
}

void Win32Ffb::stop_all() noexcept {
    if (constant_) constant_->Stop();
    if (spring_)   spring_->Stop();
}

void Win32Ffb::shutdown() noexcept {
    if (constant_) { constant_->Stop(); constant_.Reset(); }
    if (spring_)   { spring_->Stop();   spring_.Reset(); }
    if (device_)   { device_->Unacquire(); device_.Reset(); }
    dinput_.Reset();
    current_constant_ = 0;
    current_spring_   = 0;
}

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
