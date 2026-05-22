// SPDX-License-Identifier: MIT
// Psynder — shared app/runtime command-line argument parsing.

#pragma once

#include "core/Types.h"

#include <string>
#include <string_view>

namespace psynder::app {

// Arguments understood by every runnable Psynder app/sample. Keep this layer
// independent from platform/window creation so tools and tests can reuse it.
struct AppArgs {
    u32 smoke_frames = 0;     // 0 = interactive/unbounded run.
    std::string capture_out;  // Empty = no framebuffer capture requested.
};

struct AppArgDiagnostics {
    bool malformed_smoke_frames = false;
    bool missing_smoke_frames_value = false;
    bool missing_capture_out_value = false;
};

struct AppArgParseResult {
    AppArgs args;
    AppArgDiagnostics diagnostics;
};

bool parse_u32_decimal(std::string_view text, u32& out) noexcept;

// Consumes one common argument at argv[index], including its following value
// for space-separated forms. Returns true when the argument belongs to the
// common app/runtime set and updates index when a following value was consumed.
bool consume_common_arg(
    int argc, char** argv, int& index, AppArgs& args, AppArgDiagnostics* diagnostics = nullptr);
bool consume_common_arg(int argc,
                        const char* const* argv,
                        int& index,
                        AppArgs& args,
                        AppArgDiagnostics* diagnostics = nullptr);

AppArgParseResult parse_common_args(int argc, char** argv);
AppArgParseResult parse_common_args(int argc, const char* const* argv);

}  // namespace psynder::app
