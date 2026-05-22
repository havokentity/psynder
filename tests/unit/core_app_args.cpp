// SPDX-License-Identifier: MIT
// Unit tests for shared app/runtime argument parsing.

#include "core/AppArgs.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace app = psynder::app;

TEST_CASE("common app args parse smoke frame forms", "[core][app_args]") {
    const char* argv[] = {"sample", "--smoke-frames=4", "--smoke-frames", "7"};

    const app::AppArgParseResult result = app::parse_common_args(4, argv);

    REQUIRE(result.args.smoke_frames == 7u);
    REQUIRE_FALSE(result.diagnostics.malformed_smoke_frames);
    REQUIRE_FALSE(result.diagnostics.missing_smoke_frames_value);
}

TEST_CASE("common app args parse capture forms", "[core][app_args]") {
    const char* argv[] = {"sample",
                          "--smoke-capture-out=first.png",
                          "--smoke-capture-out",
                          "last.png"};

    const app::AppArgParseResult result = app::parse_common_args(4, argv);

    REQUIRE(result.args.capture_out == "last.png");
    REQUIRE_FALSE(result.diagnostics.missing_capture_out_value);
}

TEST_CASE("common app args report malformed smoke values", "[core][app_args]") {
    const char* argv[] = {"sample", "--smoke-frames=abc"};

    const app::AppArgParseResult result = app::parse_common_args(2, argv);

    REQUIRE(result.args.smoke_frames == 0u);
    REQUIRE(result.diagnostics.malformed_smoke_frames);
}

TEST_CASE("common app args reject overflowing smoke values", "[core][app_args]") {
    const char* argv[] = {"sample", "--smoke-frames=42949672960"};

    const app::AppArgParseResult result = app::parse_common_args(2, argv);

    REQUIRE(result.args.smoke_frames == 0u);
    REQUIRE(result.diagnostics.malformed_smoke_frames);
}

TEST_CASE("common app args report missing spaced values", "[core][app_args]") {
    const char* smoke_argv[] = {"sample", "--smoke-frames"};
    const char* capture_argv[] = {"sample", "--smoke-capture-out"};

    const app::AppArgParseResult smoke = app::parse_common_args(2, smoke_argv);
    const app::AppArgParseResult capture = app::parse_common_args(2, capture_argv);

    REQUIRE(smoke.args.smoke_frames == 0u);
    REQUIRE(smoke.diagnostics.missing_smoke_frames_value);
    REQUIRE(capture.args.capture_out.empty());
    REQUIRE(capture.diagnostics.missing_capture_out_value);
}

TEST_CASE("common app args report missing equals-form values", "[core][app_args]") {
    const char* smoke_argv[] = {"sample", "--smoke-frames="};
    const char* capture_argv[] = {"sample", "--smoke-capture-out="};

    const app::AppArgParseResult smoke = app::parse_common_args(2, smoke_argv);
    const app::AppArgParseResult capture = app::parse_common_args(2, capture_argv);

    REQUIRE(smoke.args.smoke_frames == 0u);
    REQUIRE(smoke.diagnostics.missing_smoke_frames_value);
    REQUIRE_FALSE(smoke.diagnostics.malformed_smoke_frames);
    REQUIRE(capture.args.capture_out.empty());
    REQUIRE(capture.diagnostics.missing_capture_out_value);
}

TEST_CASE("common app arg consumer leaves custom args to caller", "[core][app_args]") {
    const char* argv[] = {"sample", "--custom", "--smoke-frames", "3"};
    app::AppArgs args{};
    app::AppArgDiagnostics diagnostics{};

    int custom_index = 1;
    REQUIRE_FALSE(app::consume_common_arg(4, argv, custom_index, args, &diagnostics));
    REQUIRE(custom_index == 1);

    int smoke_index = 2;
    REQUIRE(app::consume_common_arg(4, argv, smoke_index, args, &diagnostics));
    REQUIRE(smoke_index == 3);
    REQUIRE(args.smoke_frames == 3u);
}

TEST_CASE("u32 decimal parser is strict and writes only on success", "[core][app_args]") {
    psynder::u32 value = 99u;

    REQUIRE(app::parse_u32_decimal("0", value));
    REQUIRE(value == 0u);
    REQUIRE(app::parse_u32_decimal("4294967295", value));
    REQUIRE(value == std::numeric_limits<psynder::u32>::max());
    REQUIRE_FALSE(app::parse_u32_decimal("", value));
    REQUIRE_FALSE(app::parse_u32_decimal("12x", value));
    REQUIRE_FALSE(app::parse_u32_decimal("4294967296", value));
    REQUIRE(value == std::numeric_limits<psynder::u32>::max());
}
