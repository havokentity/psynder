// SPDX-License-Identifier: MIT
// Psynder — lane 23 / macOS filesystem + process helpers. Kept in plain
// C++ (no AppKit / Foundation) so the unit test can link against this TU
// without dragging the Cocoa frameworks into the test binary.

#include "MacPlatform_internal.h"
#include "platform/Platform.h"

#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::platform::macos {

std::string fs_executable_path() {
    // _NSGetExecutablePath returns the requested length on truncation. Loop
    // until the buffer is large enough; cap at 2 attempts (the OS path limit
    // is well under PATH_MAX = 1024 on Darwin).
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);   // queries required size
    std::vector<char> buf(size + 1);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return {};
    }
    // Canonicalise to absolute via realpath so symlinks (Homebrew etc) resolve.
    std::array<char, 1024> resolved{};
    if (realpath(buf.data(), resolved.data()) != nullptr) {
        return std::string{resolved.data()};
    }
    return std::string{buf.data()};
}

std::string fs_user_config_dir() {
    // ~/Library/Application Support/Psynder per Apple's File System Programming
    // Guide. Fall back to $HOME / /tmp when the home directory cannot be
    // resolved so callers always get a usable path.
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (passwd* pw = getpwuid(getuid())) {
            home = pw->pw_dir;
        }
    }
    std::string base = (home && *home) ? std::string{home} : std::string{"/tmp"};
    return base + "/Library/Application Support/Psynder";
}

std::string fs_current_working_directory() {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    if (ec) return {};
    return p.string();
}

bool fs_file_exists(std::string_view path) {
    if (path.empty()) return false;
    std::string p{path};
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

}  // namespace psynder::platform::macos

namespace psynder::platform {

// The public surface delegates to the internal helpers. Keeping these tiny
// wrappers here (rather than in the .mm) means filesystem queries do not
// pull AppKit into builds / tests.
std::string executable_path()             { return macos::fs_executable_path(); }
std::string user_config_dir()             { return macos::fs_user_config_dir(); }
std::string current_working_directory()   { return macos::fs_current_working_directory(); }
bool        file_exists(std::string_view p) { return macos::fs_file_exists(p); }

}  // namespace psynder::platform
