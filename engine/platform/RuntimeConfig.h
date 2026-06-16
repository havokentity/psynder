// SPDX-License-Identifier: MIT
// Psynder — platform-backed runtime config paths and console archive plumbing.

#pragma once

#include <string>

namespace psynder::platform::runtime_config {

// Returned BY VALUE (not a cached static reference) on purpose: the
// console-archive autosave runs from a std::atexit handler, and a cached
// static std::string here could be destroyed before that handler fires —
// a static-destruction-order use-after-free (caught by ASan at exit). These
// are rare config-path queries, so recomputing is cheap and lifetime-safe.
[[nodiscard]] std::string directory();
[[nodiscard]] std::string console_archive_path();

bool ensure_directory();
bool open_directory();

void register_console_commands();
void register_console_archive_autosave();
bool load_console_archive();
int save_console_archive();

}  // namespace psynder::platform::runtime_config
