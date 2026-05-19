// SPDX-License-Identifier: MIT
// Psynder — Lane 11 Wave-B unit tests for inter-LOD vertex morph.
//
// The load-bearing properties:
// 1. Watertight invariant — two same-LOD chunks sharing an edge produce
//    bitwise-identical Y values along that edge, even under non-zero morph.
//    This extends the Wave-A property (which only pinned LOD=0, leaf only)
//    to all LOD levels and morph factors.
// 2. Morph endpoints — morph=0 reproduces the leaf-level surface (no change
//    from Wave A); morph=1 reproduces the coarse-grid bilinear surface.
// 3. Per-chunk LOD selection — `compute_chunk_morph` picks the right LOD
//    band and computes morph monotonically inside that band.

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

std::vector<std::uint16_t> make_heightmap_lod(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (std::uint32_t z = 0; z < h; ++z) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t v = (x * 2246822519u + z * 3266489917u + 31u);
            out[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint16_t>(v & 0xFFFFu);
        }
    }
    return out;
}

bool floats_bitwise_equal_morph(float a, float b) noexcept {
    std::uint32_t ua, ub;
    std::memcpy(&ua, &a, sizeof(ua));
    std::memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

}  // namespace

TEST_CASE("morph_height = lerp(fine, coarse, t)", "[world_outdoor][cdlod][morph]") {
    REQUIRE(pwod::morph_height(10.0f, 20.0f, 0.0f) == 10.0f);   // pure fine
    REQUIRE(pwod::morph_height(10.0f, 20.0f, 1.0f) == 20.0f);   // pure coarse
    REQUIRE(pwod::morph_height(10.0f, 20.0f, 0.5f) == 15.0f);   // mid
}

TEST_CASE("coarse_height_at_lod identity at level 0", "[world_outdoor][cdlod][morph]") {
    const std::uint32_t W = 16, H = 16;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 1.0f; desc.height_scale = 0.1f;
    desc.heights = raw.data();

    for (std::uint32_t z = 0; z < H; ++z) {
        for (std::uint32_t x = 0; x < W; ++x) {
            const float coarse = pwod::coarse_height_at_lod(desc,
                                    static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(z), 0);
            const float fine   = pwod::height_at_texel(desc,
                                    static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(z));
            REQUIRE(floats_bitwise_equal_morph(coarse, fine));
        }
    }
}

TEST_CASE("coarse_height_at_lod returns the corner value at stride-aligned texels",
          "[world_outdoor][cdlod][morph]") {
    const std::uint32_t W = 17, H = 17;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 1.0f; desc.height_scale = 0.05f;
    desc.heights = raw.data();

    // At stride-aligned coords (multiples of 4 for LOD 2), coarse should
    // equal fine — those vertices are "kept" by the coarsening.
    for (std::uint32_t z = 0; z <= 16u; z += 4u) {
        for (std::uint32_t x = 0; x <= 16u; x += 4u) {
            const float coarse = pwod::coarse_height_at_lod(desc,
                                    static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(z), 2);
            const float fine   = pwod::height_at_texel(desc,
                                    static_cast<std::int32_t>(x),
                                    static_cast<std::int32_t>(z));
            REQUIRE(floats_bitwise_equal_morph(coarse, fine));
        }
    }
}

TEST_CASE("coarse_height_at_lod interpolates between corners off-grid",
          "[world_outdoor][cdlod][morph]") {
    const std::uint32_t W = 17, H = 17;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 1.0f; desc.height_scale = 0.05f;
    desc.heights = raw.data();

    // At texel (2, 0) under LOD 2 (stride 4), the coarse height is the
    // half-blend of texels (0, 0) and (4, 0).
    const float a = pwod::height_at_texel(desc, 0, 0);
    const float b = pwod::height_at_texel(desc, 4, 0);
    const float expected = a + (b - a) * 0.5f;
    const float coarse   = pwod::coarse_height_at_lod(desc, 2, 0, 2);
    REQUIRE(std::fabs(coarse - expected) < 1e-3f);
}

TEST_CASE("apply_vertex_morph is leaf identity at morph_factor=0",
          "[world_outdoor][cdlod][morph]") {
    const std::uint32_t W = 17, H = 17;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 1.0f; desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwod::CdlodMorph morph{};
    morph.lod_level    = 2;
    morph.morph_factor = 0.0f;

    for (std::int32_t z = 0; z < 17; ++z) {
        for (std::int32_t x = 0; x < 17; ++x) {
            const float morphed = pwod::apply_vertex_morph(desc, x, z, morph);
            const float fine    = pwod::height_at_texel(desc, x, z);
            REQUIRE(floats_bitwise_equal_morph(morphed, fine));
        }
    }
}

TEST_CASE("apply_vertex_morph at morph_factor=1 is the coarse-grid surface",
          "[world_outdoor][cdlod][morph]") {
    const std::uint32_t W = 17, H = 17;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 1.0f; desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwod::CdlodMorph morph{};
    morph.lod_level    = 2;
    morph.morph_factor = 1.0f;

    for (std::int32_t z = 0; z < 17; ++z) {
        for (std::int32_t x = 0; x < 17; ++x) {
            const float morphed = pwod::apply_vertex_morph(desc, x, z, morph);
            const float coarse  = pwod::coarse_height_at_lod(desc, x, z, 2);
            REQUIRE(floats_bitwise_equal_morph(morphed, coarse));
        }
    }
}

TEST_CASE("CDLOD stitching stays watertight under same-LOD morph",
          "[world_outdoor][cdlod][morph][watertight]") {
    // Same map shape as the Wave-A watertight test, but with a non-trivial
    // morph applied to both chunks. The seam Y values must still match
    // bitwise — the §9.2 invariant cannot regress.
    const std::uint32_t W = 129;
    const std::uint32_t H = 65;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 0.5f; desc.height_scale = 0.05f;
    desc.heights = raw.data();

    pwod::CdlodMorph morph{};
    morph.lod_level    = 1;
    morph.morph_factor = 0.5f;

    const auto left  = pwod::build_chunk_with_morph(desc, 0, 0, morph);
    const auto right = pwod::build_chunk_with_morph(desc, 1, 0, morph);

    REQUIRE_FALSE(left.vertices.empty());
    REQUIRE_FALSE(right.vertices.empty());

    const std::uint32_t L_verts_x = pwod::kChunkDim + 1u;
    const std::uint32_t R_verts_x =
        std::min<std::uint32_t>(pwod::kChunkDim + 1u, W - pwod::kChunkDim);
    const std::uint32_t L_verts_z = static_cast<std::uint32_t>(left.vertices.size())  / L_verts_x;
    REQUIRE(L_verts_z == 65u);

    for (std::uint32_t vz = 0; vz < L_verts_z; ++vz) {
        const auto& vL = left.vertices [vz * L_verts_x + (L_verts_x - 1u)];
        const auto& vR = right.vertices[vz * R_verts_x + 0u];
        INFO("seam row vz=" << vz << " under LOD=1 morph=0.5");
        REQUIRE(floats_bitwise_equal_morph(vL.position.x, vR.position.x));
        REQUIRE(floats_bitwise_equal_morph(vL.position.y, vR.position.y));
        REQUIRE(floats_bitwise_equal_morph(vL.position.z, vR.position.z));
    }
}

TEST_CASE("CDLOD stitching stays watertight at morph=1 (fully coarse)",
          "[world_outdoor][cdlod][morph][watertight]") {
    const std::uint32_t W = 129;
    const std::uint32_t H = 65;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 0.5f; desc.height_scale = 0.05f;
    desc.heights = raw.data();

    pwod::CdlodMorph morph{};
    morph.lod_level    = 2;
    morph.morph_factor = 1.0f;

    const auto left  = pwod::build_chunk_with_morph(desc, 0, 0, morph);
    const auto right = pwod::build_chunk_with_morph(desc, 1, 0, morph);

    const std::uint32_t L_verts_x = pwod::kChunkDim + 1u;
    const std::uint32_t R_verts_x =
        std::min<std::uint32_t>(pwod::kChunkDim + 1u, W - pwod::kChunkDim);
    const std::uint32_t L_verts_z = static_cast<std::uint32_t>(left.vertices.size())  / L_verts_x;

    for (std::uint32_t vz = 0; vz < L_verts_z; ++vz) {
        const auto& vL = left.vertices [vz * L_verts_x + (L_verts_x - 1u)];
        const auto& vR = right.vertices[vz * R_verts_x + 0u];
        REQUIRE(floats_bitwise_equal_morph(vL.position.y, vR.position.y));
    }
}

TEST_CASE("compute_chunk_morph picks the right LOD band by distance",
          "[world_outdoor][cdlod][morph]") {
    pwod::CdlodChunk chunk{};
    chunk.bounds.min = psynder::math::Vec3{ 0.0f, 0.0f, 0.0f };
    chunk.bounds.max = psynder::math::Vec3{ 4.0f, 0.0f, 4.0f };   // centre (2,0,2)

    const float distances[3] = { 50.0f, 200.0f, 800.0f };

    // Eye at 20m → distance ~ sqrt(18²+0²+2²) ≈ 18.1 → inside LOD 0 band.
    auto m0 = pwod::compute_chunk_morph(chunk, psynder::math::Vec3{20.0f, 0.0f, 2.0f},
                                         distances, 3);
    REQUIRE(m0.lod_level == 0u);
    REQUIRE(m0.morph_factor == 0.0f);     // well inside the inner range

    // Eye at 150m → distance ~ 148 → inside LOD 1 band, near the far edge.
    auto m1 = pwod::compute_chunk_morph(chunk, psynder::math::Vec3{150.0f, 0.0f, 2.0f},
                                         distances, 3);
    REQUIRE(m1.lod_level == 1u);

    // Eye at 1000m → past LOD 2 → clamps to coarsest, morph=1.
    auto mFar = pwod::compute_chunk_morph(chunk, psynder::math::Vec3{1000.0f, 0.0f, 2.0f},
                                          distances, 3);
    REQUIRE(mFar.lod_level == 2u);
    REQUIRE(mFar.morph_factor == 1.0f);
}

TEST_CASE("compute_chunk_morph morph factor is monotone in distance within a band",
          "[world_outdoor][cdlod][morph]") {
    pwod::CdlodChunk chunk{};
    chunk.bounds.min = psynder::math::Vec3{ 0.0f, 0.0f, 0.0f };
    chunk.bounds.max = psynder::math::Vec3{ 0.0f, 0.0f, 0.0f };
    const float distances[1] = { 100.0f };
    auto a = pwod::compute_chunk_morph(chunk, psynder::math::Vec3{70.0f, 0.0f, 0.0f},
                                        distances, 1, 0.667f);
    auto b = pwod::compute_chunk_morph(chunk, psynder::math::Vec3{85.0f, 0.0f, 0.0f},
                                        distances, 1, 0.667f);
    auto c = pwod::compute_chunk_morph(chunk, psynder::math::Vec3{95.0f, 0.0f, 0.0f},
                                        distances, 1, 0.667f);
    REQUIRE(a.lod_level == 0u);
    REQUIRE(b.lod_level == 0u);
    REQUIRE(c.lod_level == 0u);
    REQUIRE(a.morph_factor <= b.morph_factor);
    REQUIRE(b.morph_factor <= c.morph_factor);
    REQUIRE(c.morph_factor > 0.0f);
}

TEST_CASE("build_chunk_with_morph is identical to build_chunk when morph_factor=0",
          "[world_outdoor][cdlod][morph]") {
    const std::uint32_t W = 33;
    const std::uint32_t H = 33;
    auto raw = make_heightmap_lod(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W; desc.size_z = H;
    desc.spacing = 1.0f; desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwod::CdlodMorph morph{};
    morph.lod_level    = 0;
    morph.morph_factor = 0.0f;

    const auto leaf = pwod::build_chunk(desc, 0, 0);
    const auto with_morph = pwod::build_chunk_with_morph(desc, 0, 0, morph);
    REQUIRE(leaf.vertices.size() == with_morph.vertices.size());
    for (std::size_t i = 0; i < leaf.vertices.size(); ++i) {
        REQUIRE(floats_bitwise_equal_morph(leaf.vertices[i].position.x,
                                            with_morph.vertices[i].position.x));
        REQUIRE(floats_bitwise_equal_morph(leaf.vertices[i].position.y,
                                            with_morph.vertices[i].position.y));
        REQUIRE(floats_bitwise_equal_morph(leaf.vertices[i].position.z,
                                            with_morph.vertices[i].position.z));
    }
}
