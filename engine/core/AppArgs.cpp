// SPDX-License-Identifier: MIT
// Psynder — shared app/runtime command-line argument parsing.

#include "core/AppArgs.h"

#include <limits>

namespace psynder::app {

namespace {

constexpr std::string_view kSmokeEq = "--smoke-frames=";
constexpr std::string_view kSmokeSp = "--smoke-frames";
constexpr std::string_view kCaptureEq = "--smoke-capture-out=";
constexpr std::string_view kCaptureSp = "--smoke-capture-out";

template <class Argv>
std::string_view arg_at(Argv argv, int index) noexcept {
    if (argv == nullptr || argv[index] == nullptr)
        return {};
    return std::string_view{argv[index]};
}

template <class Argv>
bool consume_common_arg_impl(int argc, Argv argv, int& index, AppArgs& args, AppArgDiagnostics* diagnostics) {
    if (index < 0 || index >= argc)
        return false;
    const std::string_view s = arg_at(argv, index);
    if (s.starts_with(kSmokeEq)) {
        const std::string_view value = s.substr(kSmokeEq.size());
        u32 frames = 0;
        if (value.empty()) {
            if (diagnostics)
                diagnostics->missing_smoke_frames_value = true;
        } else if (!parse_u32_decimal(value, frames)) {
            if (diagnostics)
                diagnostics->malformed_smoke_frames = true;
            frames = 0;
        }
        args.smoke_frames = frames;
        return true;
    }
    if (s == kSmokeSp) {
        if (index + 1 >= argc) {
            if (diagnostics)
                diagnostics->missing_smoke_frames_value = true;
            args.smoke_frames = 0;
            return true;
        }
        u32 frames = 0;
        ++index;
        if (!parse_u32_decimal(arg_at(argv, index), frames)) {
            if (diagnostics)
                diagnostics->malformed_smoke_frames = true;
            frames = 0;
        }
        args.smoke_frames = frames;
        return true;
    }
    if (s.starts_with(kCaptureEq)) {
        const std::string_view value = s.substr(kCaptureEq.size());
        if (value.empty()) {
            if (diagnostics)
                diagnostics->missing_capture_out_value = true;
            args.capture_out.clear();
            return true;
        }
        args.capture_out = std::string{value};
        return true;
    }
    if (s == kCaptureSp) {
        if (index + 1 >= argc) {
            if (diagnostics)
                diagnostics->missing_capture_out_value = true;
            args.capture_out.clear();
            return true;
        }
        ++index;
        args.capture_out = std::string{arg_at(argv, index)};
        return true;
    }
    return false;
}

template <class Argv>
AppArgParseResult parse_common_args_impl(int argc, Argv argv) {
    AppArgParseResult result{};
    for (int i = 1; i < argc; ++i)
        (void)consume_common_arg_impl(argc, argv, i, result.args, &result.diagnostics);
    return result;
}

}  // namespace

bool parse_u32_decimal(std::string_view text, u32& out) noexcept {
    if (text.empty())
        return false;

    u32 value = 0;
    for (char c : text) {
        if (c < '0' || c > '9')
            return false;
        const u32 digit = static_cast<u32>(c - '0');
        if (value > (std::numeric_limits<u32>::max() - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

bool consume_common_arg(int argc, char** argv, int& index, AppArgs& args, AppArgDiagnostics* diagnostics) {
    return consume_common_arg_impl(argc, argv, index, args, diagnostics);
}

bool consume_common_arg(
    int argc, const char* const* argv, int& index, AppArgs& args, AppArgDiagnostics* diagnostics) {
    return consume_common_arg_impl(argc, argv, index, args, diagnostics);
}

AppArgParseResult parse_common_args(int argc, char** argv) {
    return parse_common_args_impl(argc, argv);
}

AppArgParseResult parse_common_args(int argc, const char* const* argv) {
    return parse_common_args_impl(argc, argv);
}

}  // namespace psynder::app
