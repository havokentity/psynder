// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "platform/Platform.h"
#include "platform/RuntimeConfig.h"

TEST_CASE("platform: runtime config paths share one source of truth", "[platform][config]") {
    const std::string& dir = psynder::platform::runtime_config::directory();
    const std::string& archive = psynder::platform::runtime_config::console_archive_path();

    REQUIRE_FALSE(dir.empty());
    REQUIRE(dir == psynder::platform::user_config_dir());
    REQUIRE(archive.find(dir) == 0);
    REQUIRE(archive.ends_with("psynder.cfg"));
}
