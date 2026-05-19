// SPDX-License-Identifier: MIT
// Psynder — Win32 platform façade. Owns the factory functions the shim
// dispatches into, plus the file-system / executable-path helpers.
//
// Implementation lives in Win32Window / Win32Input / Win32Present /
// Win32Audio — this TU just wires them up.

#include "platform/Platform.h"

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Audio.h"
#include "Win32Common.h"
#include "Win32Input.h"
#include "Win32Window.h"
#include "core/Log.h"

#include <shlobj.h>

#include <string>

#pragma comment(lib, "shell32.lib")

namespace psynder::platform {

// ─────────────────────────────────────────────────────────────────────────
// Window factory — dispatched from the shared shim's create_window().
// ─────────────────────────────────────────────────────────────────────────

Window* create_window_impl(const WindowDesc& desc) {
    // COM must be alive before we touch DXGI / WASAPI. The scope sits on a
    // module-static so all subsequent windows share the same init/uninit.
    static win32::ComScope s_com_scope;

    // Boot the audio device on first window creation. Failures here are
    // logged but non-fatal — the user can still see the window.
    auto& audio = win32::audio_singleton();
    if (!audio.running()) {
        if (!audio.start()) {
            PSY_LOG_WARN("[win32] WASAPI start failed; running silent");
        }
    }

    return win32::create_window_native(desc);
}

void destroy_window_impl(Window* w) {
    delete w;
}

// ─────────────────────────────────────────────────────────────────────────
// File-system / process helpers.
// ─────────────────────────────────────────────────────────────────────────

std::string executable_path() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (n == 0) return {};
    return win32::from_wide(std::wstring_view{buf, n});
}

std::string user_config_dir() {
    PWSTR p = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) || !p) {
        if (p) ::CoTaskMemFree(p);
        return {};
    }
    std::string out = win32::from_wide(std::wstring_view{p});
    ::CoTaskMemFree(p);
    return out;
}

std::string current_working_directory() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ::GetCurrentDirectoryW(_countof(buf), buf);
    if (n == 0) return {};
    return win32::from_wide(std::wstring_view{buf, n});
}

bool file_exists(std::string_view path) {
    const std::wstring w = win32::to_wide(path);
    const DWORD attrs    = ::GetFileAttributesW(w.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace psynder::platform

#else  // !PSYNDER_PLATFORM_WIN32 — keep the TU empty on non-Windows

// Mac/Linux CI configures don't add this directory, so this fallback only
// fires if a future cross-tool ever pulls every .cpp through clang-tidy.
namespace psynder::platform::win32_stub { void anchor() {} }

#endif  // PSYNDER_PLATFORM_WIN32
