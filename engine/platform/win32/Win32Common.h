// SPDX-License-Identifier: MIT
// Psynder — Win32 platform shared bits (header-only).
//
// All Win32 sources are guarded by PSYNDER_PLATFORM_WIN32. The lane's
// CMakeLists only adds this directory when building on Windows, but we
// guard at the source level too so an out-of-band Mac/Linux compile (e.g.
// running clang-tidy across the tree) doesn't choke on windows.h.
//
// This header centralizes the Win32 + COM + DXGI includes and a few tiny
// helpers shared across Win32Window/Input/Present/Audio.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// COM + DXGI + D3D11 + WASAPI + XInput
#include <combaseapi.h>
#include <objbase.h>
#include <wrl/client.h>

#include <dxgi1_4.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <xinput.h>

#include "core/Log.h"
#include "core/Types.h"

#include <string>
#include <string_view>

namespace psynder::platform::win32 {

// Microsoft-style ComPtr alias, kept short so call sites stay readable.
template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// hresult logging helper. Returns true on success, logs and returns false
// on failure. Use as `if (!psy_hr_ok(hr, "CreateSwapChain")) return false;`.
inline bool psy_hr_ok(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr))
        return true;
    PSY_LOG_ERROR("[win32] {} failed: hr=0x{:08x}", what, static_cast<u32>(hr));
    return false;
}

// Convert a UTF-8 std::string_view to a wide string for the Win32 W APIs.
// Used by window-title sets and FS helpers.
inline std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out;
    out.resize(static_cast<usize>(needed));
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

// Inverse — wide buffer to UTF-8. Used by FS helpers to return std::string.
inline std::string from_wide(std::wstring_view wide) {
    if (wide.empty())
        return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8,
                                             0,
                                             wide.data(),
                                             static_cast<int>(wide.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (needed <= 0)
        return {};
    std::string out;
    out.resize(static_cast<usize>(needed));
    ::WideCharToMultiByte(CP_UTF8,
                          0,
                          wide.data(),
                          static_cast<int>(wide.size()),
                          out.data(),
                          needed,
                          nullptr,
                          nullptr);
    return out;
}

// One-shot COM bootstrap — multiple Init calls are fine (refcounted by Windows).
struct ComScope {
    ComScope() {
        // COINIT_MULTITHREADED matches WASAPI + DXGI threading expectations.
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // Some host already chose apartment-threaded; live with it.
            initialized_ = false;
        } else {
            initialized_ = SUCCEEDED(hr);
        }
    }
    ~ComScope() {
        if (initialized_)
            ::CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

   private:
    bool initialized_ = false;
};

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
