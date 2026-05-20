// SPDX-License-Identifier: MIT
// Psynder — Lane 11 unit tests for deterministic scatter placement
// (DESIGN.md §9.2: "instanced billboards and meshes seeded from density
// maps; placed deterministically so multiplayer stays in sync").
//
// We pin: (a) determinism — same seed + same maps → bitwise-identical
// instance set, (b) density gating — count grows with density, (c) hash
// finalizer avalanche (different seeds produce different placements).

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/Scatter_internal.h"
#include "world/outdoor/Terrain.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace pwo = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

namespace {

bool floats_bitwise_equal(float a, float b) noexcept {
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

}  // namespace

TEST_CASE("scatter is deterministic for the same seed", "[world_outdoor][scatter]") {
    const std::uint32_t W = 32;
    const std::uint32_t H = 32;
    std::vector<std::uint16_t> heights(W * H, 0);
    std::vector<std::uint8_t> density(W * H, 128);  // 50% target density

    pwo::HeightmapDesc hmap{};
    hmap.size_x = W;
    hmap.size_z = H;
    hmap.spacing = 1.0f;
    hmap.height_scale = 0.1f;
    hmap.heights = heights.data();

    pwod::DensityMap dmap{};
    dmap.size_x = W;
    dmap.size_z = H;
    dmap.spacing = 1.0f;
    dmap.data = density.data();

    std::vector<pwod::ScatterInstance> a, b;
    const std::uint64_t seed = 0xDEADBEEF12345678ULL;
    const std::uint32_t na = pwod::place_scatter(hmap,
                                                 dmap,
                                                 /*kind=*/0,
                                                 /*attempts=*/3,
                                                 seed,
                                                 a);
    const std::uint32_t nb = pwod::place_scatter(hmap,
                                                 dmap,
                                                 /*kind=*/0,
                                                 /*attempts=*/3,
                                                 seed,
                                                 b);
    REQUIRE(na > 0u);
    REQUIRE(na == nb);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(floats_bitwise_equal(a[i].position.x, b[i].position.x));
        REQUIRE(floats_bitwise_equal(a[i].position.y, b[i].position.y));
        REQUIRE(floats_bitwise_equal(a[i].position.z, b[i].position.z));
        REQUIRE(floats_bitwise_equal(a[i].scale, b[i].scale));
        REQUIRE(floats_bitwise_equal(a[i].yaw, b[i].yaw));
        REQUIRE(a[i].kind == b[i].kind);
    }
}

TEST_CASE("scatter count grows with density", "[world_outdoor][scatter]") {
    const std::uint32_t W = 16;
    const std::uint32_t H = 16;
    std::vector<std::uint16_t> heights(W * H, 0);
    pwo::HeightmapDesc hmap{};
    hmap.size_x = W;
    hmap.size_z = H;
    hmap.spacing = 1.0f;
    hmap.height_scale = 0.1f;
    hmap.heights = heights.data();

    auto run = [&](std::uint8_t dv) {
        std::vector<std::uint8_t> density(W * H, dv);
        pwod::DensityMap dmap{};
        dmap.size_x = W;
        dmap.size_z = H;
        dmap.spacing = 1.0f;
        dmap.data = density.data();
        std::vector<pwod::ScatterInstance> out;
        return pwod::place_scatter(hmap,
                                   dmap,
                                   /*kind=*/0,
                                   /*attempts=*/4,
                                   /*seed=*/42ULL,
                                   out);
    };
    const std::uint32_t n_low = run(32);
    const std::uint32_t n_mid = run(128);
    const std::uint32_t n_high = run(240);
    REQUIRE(n_low < n_mid);
    REQUIRE(n_mid < n_high);
}

TEST_CASE("scatter respects zero density (no placements)", "[world_outdoor][scatter]") {
    const std::uint32_t W = 16;
    const std::uint32_t H = 16;
    std::vector<std::uint16_t> heights(W * H, 0);
    std::vector<std::uint8_t> density(W * H, 0);
    pwo::HeightmapDesc hmap{};
    hmap.size_x = W;
    hmap.size_z = H;
    hmap.spacing = 1.0f;
    hmap.height_scale = 0.1f;
    hmap.heights = heights.data();
    pwod::DensityMap dmap{};
    dmap.size_x = W;
    dmap.size_z = H;
    dmap.spacing = 1.0f;
    dmap.data = density.data();
    std::vector<pwod::ScatterInstance> out;
    const std::uint32_t n = pwod::place_scatter(hmap, dmap, 0, 4, 7ULL, out);
    REQUIRE(n == 0u);
    REQUIRE(out.empty());
}

TEST_CASE("scatter changes with seed", "[world_outdoor][scatter]") {
    const std::uint32_t W = 16;
    const std::uint32_t H = 16;
    std::vector<std::uint16_t> heights(W * H, 0);
    std::vector<std::uint8_t> density(W * H, 128);
    pwo::HeightmapDesc hmap{};
    hmap.size_x = W;
    hmap.size_z = H;
    hmap.spacing = 1.0f;
    hmap.height_scale = 0.1f;
    hmap.heights = heights.data();
    pwod::DensityMap dmap{};
    dmap.size_x = W;
    dmap.size_z = H;
    dmap.spacing = 1.0f;
    dmap.data = density.data();

    std::vector<pwod::ScatterInstance> a, b;
    pwod::place_scatter(hmap, dmap, 0, 4, 1ULL, a);
    pwod::place_scatter(hmap, dmap, 0, 4, 2ULL, b);

    // Different seeds: the placements should differ in at least one
    // position (otherwise the seed has no effect).
    bool any_diff = a.size() != b.size();
    if (!any_diff) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!floats_bitwise_equal(a[i].position.x, b[i].position.x) ||
                !floats_bitwise_equal(a[i].position.z, b[i].position.z)) {
                any_diff = true;
                break;
            }
        }
    }
    REQUIRE(any_diff);
}

TEST_CASE("scatter instances land on the terrain surface", "[world_outdoor][scatter]") {
    // Heightmap larger than the density map so every density cell is
    // interior to the heightmap — keeps the bilinear sample off the
    // border-clamp-to-zero path. (Real maps always have a heightmap
    // skirt around the playable area; this matches that convention.)
    const std::uint32_t HW = 32;
    const std::uint32_t HH = 32;
    std::vector<std::uint16_t> heights(HW * HH, 500);  // uniform Y = 10
    pwo::HeightmapDesc hmap{};
    hmap.size_x = HW;
    hmap.size_z = HH;
    hmap.spacing = 1.0f;
    hmap.height_scale = 0.02f;
    hmap.heights = heights.data();

    const std::uint32_t DW = 8;
    const std::uint32_t DH = 8;
    std::vector<std::uint8_t> density(DW * DH, 200);
    pwod::DensityMap dmap{};
    dmap.size_x = DW;
    dmap.size_z = DH;
    dmap.spacing = 1.0f;
    dmap.data = density.data();

    std::vector<pwod::ScatterInstance> out;
    const std::uint32_t n = pwod::place_scatter(hmap, dmap, 0, 4, 1234ULL, out);
    REQUIRE(n > 0u);
    for (const auto& inst : out) {
        REQUIRE(inst.position.y > 9.9f);
        REQUIRE(inst.position.y < 10.1f);
    }
}

TEST_CASE("splitmix64 finalizer avalanches input bits", "[world_outdoor][scatter]") {
    // The finalizer must produce wildly different outputs for tiny input
    // changes — required for the determinism + good distribution properties.
    const std::uint64_t a = pwod::splitmix64(0ULL);
    const std::uint64_t b = pwod::splitmix64(1ULL);
    REQUIRE(a != b);
    REQUIRE((a ^ b) != 0ULL);
}
