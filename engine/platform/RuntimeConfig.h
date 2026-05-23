// SPDX-License-Identifier: MIT
// Psynder — platform-backed runtime config paths and console archive plumbing.

#pragma once

#include <string>

namespace psynder::platform::runtime_config {

[[nodiscard]] const std::string& directory();
[[nodiscard]] const std::string& console_archive_path();

bool ensure_directory();
bool open_directory();

void register_console_commands();
void register_console_archive_autosave();
bool load_console_archive();
int save_console_archive();

}  // namespace psynder::platform::runtime_config
