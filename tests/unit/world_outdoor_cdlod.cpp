// SPDX-License-Identifier: MIT
// Psynder — Lane 11 unit tests for the CDLOD mesh (Backend A).
//
// The load-bearing §9.2 invariant we pin here: **CDLOD stitching produces
// a watertight mesh**. Two adjacent chunks share an edge of vertices, and
// the world positions of those vertices must be BITWISE IDENTICAL between
// the two chunks. If they aren't, the rasterizer sees micro-gaps along
// the seam and the player sees flicker (or worse, sky bleeding through).
//
// The watertight invariant comes from one design choice: per-chunk
// vertices are computed at INTEGER texel coordinates × heightmap spacing.
// No per-chunk float origin, no incremental accumulation along a chunk —
// so the f32 representation of `texel * spacing` is identical no matter
// which neighbor produced it.

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/CdlodMesh_internal.h"
#include "world/outdoor/Heightmap_internal.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pwo  = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

namespace {

// Build a deterministic heightmap of the requested size. Each texel's u16
// value is a simple hash of (x, z) so the surface has enough variation to
// distinguish good from bad math (a constant-Y heightmap would mask many
// bugs).
std::vector<std::uint16_t> make_heightmap(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (std::uint32_t z = 0; z < h; ++z) {
        for (std::uint32_t x = 0; x < w; ++x) {
            // 2D LCG-ish, deterministic.
            const std::uint32_t v = (x * 1664525u + z * 1013904223u + 12345u);
            out[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint16_t>(v & 0xFFFFu);
        }
    }
    return out;
}

bool floats_bitwise_equal(float a, float b) noexcept {
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

}  // namespace

TEST_CASE("CDLOD chunk grid covers the whole map", "[world_outdoor][cdlod]") {
    // 129×129 → exactly 2×2 chunks of 64×64 quads + 1 vertex border on
    // each side (the next-chunk's first vertex IS this chunk's last).
    const std::uint32_t W = 129;
    const std::uint32_t H = 129;
    auto raw = make_heightmap(W, H);

    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.01f;
    desc.heights      = raw.data();

    REQUIRE(pwod::chunk_count_x(desc) == 2u);
    REQUIRE(pwod::chunk_count_z(desc) == 2u);

    auto chunks = pwod::build_all_chunks(desc);
    REQUIRE(chunks.size() == 4u);

    // Every chunk must have at least one triangle and a bounded AABB.
    for (const auto& c : chunks) {
        REQUIRE_FALSE(c.vertices.empty());
        REQUIRE_FALSE(c.indices.empty());
        REQUIRE((c.indices.size() % 3u) == 0u);
        REQUIRE(c.bounds.min.x <= c.bounds.max.x);
        REQUIRE(c.bounds.min.z <= c.bounds.max.z);
    }
}

TEST_CASE("CDLOD stitching is watertight along shared edges",
          "[world_outdoor][cdlod][watertight]") {
    // A 65×65 map covers two columns of leaf chunks (chunk 0 = quads 0..63,
    // chunk 1 = quads 64..63 — which is empty given the clamp). Use a 128×64
    // map instead: that gives a clean 2×1 chunk grid with a shared seam at
    // x=64.
    const std::uint32_t W = 129;   // 2 chunks of 64 quads + 1
    const std::uint32_t H = 65;    // 1 chunk of 64 quads + 1
    auto raw = make_heightmap(W, H);

    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 0.5f;
    desc.height_scale = 0.05f;
    desc.heights      = raw.data();

    REQUIRE(pwod::chunk_count_x(desc) == 2u);
    REQUIRE(pwod::chunk_count_z(desc) == 1u);

    const auto left  = pwod::build_chunk(desc, 0, 0);
    const auto right = pwod::build_chunk(desc, 1, 0);

    REQUIRE_FALSE(left.vertices.empty());
    REQUIRE_FALSE(right.vertices.empty());

    // The shared edge is x = kChunkDim (in texel coords) = 64. In the left
    // chunk, those are the LAST column of verts (vx = kChunkDim). In the
    // right chunk, those are the FIRST column (vx = 0).
    const std::uint32_t L_verts_x = pwod::kChunkDim + 1u;     // 65
    const std::uint32_t R_verts_x =
        std::min<std::uint32_t>(pwod::kChunkDim + 1u, W - pwod::kChunkDim); // 65
    const std::uint32_t L_verts_z = static_cast<std::uint32_t>(left.vertices.size())  / L_verts_x;
    const std::uint32_t R_verts_z = static_cast<std::uint32_t>(right.vertices.size()) / R_verts_x;
    REQUIRE(L_verts_z == 65u);
    REQUIRE(R_verts_z == 65u);

    // Walk the shared seam: for every Z, left.last-column must equal
    // right.first-column, BITWISE.
    for (std::uint32_t vz = 0; vz < L_verts_z; ++vz) {
        const auto& vL = left.vertices [vz * L_verts_x + (L_verts_x - 1u)];
        const auto& vR = right.vertices[vz * R_verts_x + 0u];
        INFO("seam row vz=" << vz);
        REQUIRE(floats_bitwise_equal(vL.position.x, vR.position.x));
        REQUIRE(floats_bitwise_equal(vL.position.y, vR.position.y));
        REQUIRE(floats_bitwise_equal(vL.position.z, vR.position.z));
        // Normals are central-difference; they're the same texel sample on
        // both sides, so they must also match bitwise.
        REQUIRE(floats_bitwise_equal(vL.normal.x, vR.normal.x));
        REQUIRE(floats_bitwise_equal(vL.normal.y, vR.normal.y));
        REQUIRE(floats_bitwise_equal(vL.normal.z, vR.normal.z));
        // And the per-vertex splat color (4-weight pack) must agree —
        // otherwise the seam shows a material discontinuity.
        REQUIRE(vL.color == vR.color);
    }
}

TEST_CASE("CDLOD chunk vertex positions reproduce the heightmap exactly",
          "[world_outdoor][cdlod]") {
    // Each leaf vertex should sit at world = (texel*spacing, h*height_scale).
    // No per-chunk drift, no half-texel offset, no implicit centering.
    const std::uint32_t W = 33;
    const std::uint32_t H = 33;
    auto raw = make_heightmap(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 2.0f;
    desc.height_scale = 0.25f;
    desc.heights      = raw.data();

    REQUIRE(pwod::chunk_count_x(desc) == 1u);
    REQUIRE(pwod::chunk_count_z(desc) == 1u);

    const auto chunk = pwod::build_chunk(desc, 0, 0);
    REQUIRE(chunk.vertices.size() == static_cast<std::size_t>(W) * H);

    // Sample 5 reference positions and confirm they reproduce input.
    const std::pair<std::uint32_t, std::uint32_t> samples[] = {
        {0,0}, {17,9}, {0,32}, {32,0}, {32,32},
    };
    const std::uint32_t verts_x = W;
    for (auto [sx, sz] : samples) {
        const auto& v = chunk.vertices[sz * verts_x + sx];
        REQUIRE(floats_bitwise_equal(v.position.x, static_cast<float>(sx) * desc.spacing));
        REQUIRE(floats_bitwise_equal(v.position.z, static_cast<float>(sz) * desc.spacing));
        const std::uint16_t h = raw[static_cast<std::size_t>(sz) * W + sx];
        const float         expected_y = static_cast<float>(h) * desc.height_scale;
        REQUIRE(floats_bitwise_equal(v.position.y, expected_y));
    }
}

TEST_CASE("CDLOD splat weights normalize to 1", "[world_outdoor][cdlod][splat]") {
    const std::uint32_t W = 17;
    const std::uint32_t H = 17;
    auto raw = make_heightmap(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.1f;
    desc.heights      = raw.data();

    for (std::int32_t z = 0; z < static_cast<std::int32_t>(H); ++z) {
        for (std::int32_t x = 0; x < static_cast<std::int32_t>(W); ++x) {
            const auto s = pwod::splat_at_texel(desc, x, z);
            const float sum = s.w[0] + s.w[1] + s.w[2] + s.w[3];
            REQUIRE(sum > 0.99f);
            REQUIRE(sum < 1.01f);
            for (std::uint32_t i = 0; i < pwod::kSplatWeightCount; ++i) {
                REQUIRE(s.w[i] >= 0.0f);
                REQUIRE(s.w[i] <= 1.0f);
            }
        }
    }
}

TEST_CASE("CDLOD morph parameter clamps to [0,1]", "[world_outdoor][cdlod]") {
    const psynder::math::Vec3 eye{ 0.0f, 0.0f, 0.0f };
    // Inside the near band — morph should be 0.
    REQUIRE(pwod::cdlod_morph_t(psynder::math::Vec3{10.0f, 0.0f, 0.0f}, eye, 20.0f, 80.0f) == 0.0f);
    // Beyond the far band — morph should be 1.
    REQUIRE(pwod::cdlod_morph_t(psynder::math::Vec3{100.0f, 0.0f, 0.0f}, eye, 20.0f, 80.0f) == 1.0f);
    // Halfway through the band — morph should be 0.5.
    const float t = pwod::cdlod_morph_t(psynder::math::Vec3{50.0f, 0.0f, 0.0f}, eye, 20.0f, 80.0f);
    REQUIRE(t > 0.49f);
    REQUIRE(t < 0.51f);
}
