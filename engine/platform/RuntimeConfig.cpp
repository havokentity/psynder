// SPDX-License-Identifier: MIT
// Psynder — runtime config source of truth.

#include "RuntimeConfig.h"

#include "core/console/Console.h"
#include "platform/Platform.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>

namespace psynder::platform::runtime_config {
namespace {

std::once_flag g_commands_once;
std::once_flag g_autosave_once;

std::string build_directory() {
    const std::string base = platform::user_config_dir();
    return base.empty() ? std::string{"."} : base;
}

std::string build_console_archive_path() {
    return directory() + "/psynder.cfg";
}

}  // namespace

// By value, no cached static — see RuntimeConfig.h: the atexit autosave
// handler must not depend on a static string that could already be destroyed
// at exit (static-destruction-order use-after-free).
std::string directory() {
    return build_directory();
}

std::string console_archive_path() {
    return build_console_archive_path();
}

bool ensure_directory() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{directory()}, ec);
    return !ec;
}

bool open_directory() {
    if (!ensure_directory())
        return false;
    return platform::open_external_url(directory());
}

bool load_console_archive() {
    std::error_code ec;
    if (!std::filesystem::exists(console_archive_path(), ec) || ec)
        return false;
    return console::Console::Get().LoadFromFile(console_archive_path()).ok;
}

int save_console_archive() {
    if (!ensure_directory())
        return -1;
    return console::Console::Get().SaveArchivedCvars(console_archive_path());
}

void register_console_archive_autosave() {
    std::call_once(g_autosave_once, [] {
        // Force the Console singleton to be constructed BEFORE we register the
        // atexit handler. [basic.start.term]: an atexit-registered function is
        // sequenced before the destructor of any static-storage object whose
        // construction completed before the std::atexit call. So touching
        // Console::Get() here guarantees the autosave handler runs while the
        // Console (and its internal mutex) is still alive at exit — otherwise
        // SaveArchivedCvars would lock a destroyed mutex (use-after-free /
        // "mutex lock failed: Invalid argument" abort under sanitizers).
        (void)console::Console::Get();
        std::atexit([] { (void)runtime_config::save_console_archive(); });
    });
}

void register_console_commands() {
    std::call_once(g_commands_once, [] {
        auto& con = console::Console::Get();
        con.RegisterCommand("config_path",
                            "Print the Psynder runtime settings directory and cvar archive path.",
                            [](std::span<const std::string_view>, console::Output& out) {
                                out.FormatLine("config dir: {}", runtime_config::directory());
                                out.FormatLine("cvars: {}", runtime_config::console_archive_path());
                            });
        con.RegisterCommand("config_open_dir",
                            "Open the Psynder runtime settings directory.",
                            [](std::span<const std::string_view>, console::Output& out) {
                                out.FormatLine("config dir: {}", runtime_config::directory());
                                if (runtime_config::open_directory())
                                    out.PrintLine("config_open_dir: opened");
                                else
                                    out.PrintLine("config_open_dir: failed");
                            });
        con.RegisterCommand("config_save",
                            "Save archived cvars to the runtime settings file now.",
                            [](std::span<const std::string_view>, console::Output& out) {
                                const int n = runtime_config::save_console_archive();
                                if (n < 0)
                                    out.FormatLine("config_save: failed writing {}",
                                                   runtime_config::console_archive_path());
                                else
                                    out.FormatLine("config_save: wrote {} cvars to {}",
                                                   n,
                                                   runtime_config::console_archive_path());
                            });
        con.RegisterCommand("config_reload",
                            "Reload archived cvars from the runtime settings file.",
                            [](std::span<const std::string_view>, console::Output& out) {
                                if (runtime_config::load_console_archive())
                                    out.FormatLine("config_reload: loaded {}",
                                                   runtime_config::console_archive_path());
                                else
                                    out.FormatLine("config_reload: no config at {}",
                                                   runtime_config::console_archive_path());
                            });
    });
}

}  // namespace psynder::platform::runtime_config
