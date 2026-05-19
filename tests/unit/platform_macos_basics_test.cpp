// SPDX-License-Identifier: MIT
// Psynder — lane 23 / platform-macos unit tests.
//
// Two invariants checked here (the lane-23 issue body):
//   1. Clock::ticks_per_second() is sane (nanosecond-scale steady clock).
//   2. executable_path() returns an absolute path to a real file.
//
// Anything Cocoa / Metal / CoreAudio is deliberately NOT exercised in the
// unit test — those need an interactive runloop and a logged-in graphical
// session. The smoke binary `sample_00_clear` is the integration check.

#if defined(PSYNDER_PLATFORM_MACOS)

#include "platform/Platform.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using psynder::platform::Clock;

TEST_CASE("platform_macos: Clock::ticks_per_second is sane", "[platform_macos]") {
    const auto tps = Clock::ticks_per_second();
    // steady_clock on Darwin reports nanoseconds, so we expect 1e9, but we
    // accept any clock running at >= 1 MHz tick and <= 10 GHz. That bracket
    // catches any future libc++ migration without false-flagging.
    REQUIRE(tps >= 1'000'000ull);
    REQUIRE(tps <= 10'000'000'000ull);
}

TEST_CASE("platform_macos: Clock::seconds is monotonic and roughly real-time",
          "[platform_macos]") {
    const auto t0 = Clock::ticks_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t1 = Clock::ticks_now();
    REQUIRE(t1 > t0);
    const auto dt = Clock::seconds(t1 - t0);
    // Loose bounds — CI macs under load are slow, but we expect at least
    // half the sleep target and at most a thirty-fold overshoot.
    REQUIRE(dt > 0.005);
    REQUIRE(dt < 0.6);
}

TEST_CASE("platform_macos: executable_path returns absolute path to a file",
          "[platform_macos]") {
    const auto p = psynder::platform::executable_path();
    REQUIRE_FALSE(p.empty());
    REQUIRE(p.front() == '/');             // absolute on POSIX
    std::error_code ec;
    REQUIRE(std::filesystem::exists(p, ec));
    REQUIRE_FALSE(ec);
}

TEST_CASE("platform_macos: user_config_dir is absolute", "[platform_macos]") {
    const auto p = psynder::platform::user_config_dir();
    REQUIRE_FALSE(p.empty());
    REQUIRE(p.front() == '/');
    // Path may not yet exist (the engine creates it on first save) but the
    // string must look sensible — contain the canonical Mac dir suffix.
    REQUIRE(p.find("Application Support") != std::string::npos);
}

TEST_CASE("platform_macos: current_working_directory matches std::filesystem",
          "[platform_macos]") {
    const auto p = psynder::platform::current_working_directory();
    REQUIRE_FALSE(p.empty());
    REQUIRE(p.front() == '/');
    std::error_code ec;
    auto expected = std::filesystem::current_path(ec).string();
    REQUIRE_FALSE(ec);
    REQUIRE(p == expected);
}

TEST_CASE("platform_macos: file_exists round-trips the executable path",
          "[platform_macos]") {
    const auto exe = psynder::platform::executable_path();
    REQUIRE(psynder::platform::file_exists(exe));
    REQUIRE_FALSE(psynder::platform::file_exists("/no/such/path/__psynder__"));
    REQUIRE_FALSE(psynder::platform::file_exists(""));
}

#endif  // PSYNDER_PLATFORM_MACOS
