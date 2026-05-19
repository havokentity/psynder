// SPDX-License-Identifier: MIT
// Psynder — Lua 5.4 binding. Scripts run on a dedicated game thread;
// engine workers never enter the VM. Lane 15 owns.

#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>

namespace psynder::script {

class Vm {
public:
    static Vm& Get();

    bool start();
    void shutdown();

    bool execute_string(std::string_view source, std::string_view name = "");
    bool execute_file(std::string_view virtual_path);

    // REPL — called by the editor IPC layer.
    bool execute_repl(std::string_view line, std::string& out);
};

}  // namespace psynder::script
