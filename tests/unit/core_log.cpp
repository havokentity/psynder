// SPDX-License-Identifier: MIT
// Unit tests for psynder::log.
//
// Covers the RCU-style sink dispatch:
//   - add_sink + emit fan-out.
//   - remove_all_sinks clears.
//   - emit is callable with no sinks registered (no crash).

#include "core/Log.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace lg = psynder::log;

namespace {
std::atomic<int> g_test_log_count{0};
std::string g_test_last_line;
lg::Level g_test_last_level = lg::Level::Info;

void test_sink(lg::Level lv, const std::string& line) {
    g_test_log_count.fetch_add(1, std::memory_order_relaxed);
    g_test_last_level = lv;
    g_test_last_line = line;
}
}  // namespace

TEST_CASE("emit with no sinks is safe", "[core][log]") {
    lg::remove_all_sinks();
    // Should not crash, even though there are zero sinks registered.
    PSY_LOG_INFO("hello {}", 1);
}

TEST_CASE("add_sink + emit fan-out", "[core][log]") {
    lg::remove_all_sinks();
    g_test_log_count.store(0, std::memory_order_relaxed);
    g_test_last_line.clear();

    lg::add_sink(&test_sink);

    PSY_LOG_WARN("seven={}", 7);
    PSY_LOG_ERROR("err={}", "boom");

    REQUIRE(g_test_log_count.load() == 2);
    REQUIRE(g_test_last_level == lg::Level::Error);
    REQUIRE(g_test_last_line.find("err=boom") != std::string::npos);

    lg::remove_all_sinks();
}

TEST_CASE("remove_all_sinks unwires every callback", "[core][log]") {
    lg::remove_all_sinks();
    lg::add_sink(&test_sink);
    g_test_log_count.store(0, std::memory_order_relaxed);
    PSY_LOG_INFO("first");

    REQUIRE(g_test_log_count.load() == 1);

    lg::remove_all_sinks();
    PSY_LOG_INFO("after");
    REQUIRE(g_test_log_count.load() == 1);  // no further calls
}
