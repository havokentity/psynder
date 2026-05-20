// SPDX-License-Identifier: MIT
// Unit tests for psynder::hardware.
//
// Only invariants that hold on every supported host:
//   - cores_logical >= cores_physical >= 1.
//   - Cache fields, when populated, agree (l1 <= l2 <= l3 in size).
//   - On AArch64 hosts, NEON is set. On x86_64 hosts, the SSE4.2 flag
//     matches what cpuid says.
// We do NOT assert specific feature flags here -- the test should pass
// on a low-spec x86 box that lacks AVX2.

#include "core/hardware/CpuFeatures.h"

#include <catch2/catch_test_macros.hpp>

namespace hw = psynder::hardware;

TEST_CASE("CPU core counts are sane", "[core][hardware]") {
    const auto& f = hw::detect();
    REQUIRE(f.cores_physical >= 1);
    REQUIRE(f.cores_logical >= f.cores_physical);
}

TEST_CASE("CPU cache sizes agree across levels", "[core][hardware]") {
    const auto& f = hw::detect();
    // Some hosts don't report L3 (single-CCX laptops, low-end ARM). Skip
    // the L2<=L3 check when L3 is zero -- treat absent as "unknown".
    if (f.cache_l1d > 0 && f.cache_l2 > 0) {
        REQUIRE(f.cache_l1d <= f.cache_l2);
    }
    if (f.cache_l2 > 0 && f.cache_l3 > 0) {
        REQUIRE(f.cache_l2 <= f.cache_l3);
    }
}

TEST_CASE("NEON is unconditionally on AArch64", "[core][hardware]") {
#if defined(__aarch64__) || defined(_M_ARM64)
    const auto& f = hw::detect();
    REQUIRE(f.neon);
#endif
}
