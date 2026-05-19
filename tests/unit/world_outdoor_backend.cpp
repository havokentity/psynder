// SPDX-License-Identifier: MIT
// Psynder — Lane 11 unit tests for the per-map terrain backend config
// (DESIGN.md §9.2 + ADR-008: `terrain_backend = mesh | raymarch` fixed at
// map load).

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/Terrain.h"
#include "world/outdoor/TerrainConfig_internal.h"

namespace pwo  = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

TEST_CASE("parse_backend accepts 'mesh' (default for racing tracks)",
          "[world_outdoor][backend]") {
    pwo::TerrainBackend b{};
    REQUIRE(pwod::parse_backend("mesh", b));
    REQUIRE(b == pwo::TerrainBackend::PolygonCDLOD);
}

TEST_CASE("parse_backend accepts 'raymarch' (default for tactical FPS maps)",
          "[world_outdoor][backend]") {
    pwo::TerrainBackend b{};
    REQUIRE(pwod::parse_backend("raymarch", b));
    REQUIRE(b == pwo::TerrainBackend::HeightmapRaymarch);
}

TEST_CASE("parse_backend trims whitespace",
          "[world_outdoor][backend]") {
    pwo::TerrainBackend b{};
    REQUIRE(pwod::parse_backend("  mesh\n", b));
    REQUIRE(b == pwo::TerrainBackend::PolygonCDLOD);
    REQUIRE(pwod::parse_backend("\traymarch\t", b));
    REQUIRE(b == pwo::TerrainBackend::HeightmapRaymarch);
}

TEST_CASE("parse_backend rejects unknown values",
          "[world_outdoor][backend]") {
    pwo::TerrainBackend b = pwo::TerrainBackend::PolygonCDLOD;
    REQUIRE_FALSE(pwod::parse_backend("voxel", b));
    REQUIRE_FALSE(pwod::parse_backend("", b));
    REQUIRE_FALSE(pwod::parse_backend("MESH", b));   // case sensitive
    // The output is unchanged on a failed parse.
    REQUIRE(b == pwo::TerrainBackend::PolygonCDLOD);
}

TEST_CASE("backend_name round-trips through parse_backend",
          "[world_outdoor][backend]") {
    for (auto bin : { pwo::TerrainBackend::PolygonCDLOD,
                      pwo::TerrainBackend::HeightmapRaymarch }) {
        const char* name = pwod::backend_name(bin);
        pwo::TerrainBackend bout{};
        REQUIRE(pwod::parse_backend(name, bout));
        REQUIRE(bout == bin);
    }
}
