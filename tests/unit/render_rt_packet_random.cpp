// SPDX-License-Identifier: MIT
// Psynder — Lane 08 unit test (Wave B):
// 1000-random-ray fuzz that the packet shadow trace matches the scalar
// reference. This is the load-bearing correctness gate for the real
// 8-wide AVX2 / 4-wide NEON packet kernels added in Wave B.
//
// We generate a moderately complex scene (a tilted wall of triangles)
// and 125 packets of 8 rays each, with random origins / directions.
// Every lane of every packet must agree with `Tlas::occluded` (the
// scalar reference path).

#include <catch2/catch_test_macros.hpp>

#include "render/rt/Bvh.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace psynder;
using namespace psynder::render::rt;

namespace {

// Tiny deterministic LCG so the test stays reproducible across hosts.
struct Lcg {
    u64 state;
    explicit Lcg(u64 s) noexcept : state(s ? s : 0x12345u) {}
    u32 next_u32() noexcept {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(state >> 32);
    }
    f32 next_f32() noexcept { return static_cast<f32>(next_u32()) * (1.0f / 4294967296.0f); }
    f32 next_signed(f32 a, f32 b) noexcept { return a + (b - a) * next_f32(); }
};

// Build a clump of triangles arranged as a perturbed wall in [-3, 3]² at
// approximately z=10. 128 triangles — enough to make the BVH non-trivial.
std::vector<Triangle> make_perturbed_wall(u32 dim, Lcg& rng) {
    std::vector<Triangle> tris;
    tris.reserve(dim * dim * 2);
    const f32 cell = 6.0f / static_cast<f32>(dim);
    for (u32 j = 0; j < dim; ++j) {
        for (u32 i = 0; i < dim; ++i) {
            const f32 x0 = -3.0f + cell * static_cast<f32>(i);
            const f32 y0 = -3.0f + cell * static_cast<f32>(j);
            const f32 jz = rng.next_signed(-0.2f, 0.2f);
            const f32 z = 10.0f + jz;
            tris.push_back(Triangle{
                math::Vec3{x0, y0, z},
                math::Vec3{x0 + cell, y0, z},
                math::Vec3{x0 + cell, y0 + cell, z},
            });
            tris.push_back(Triangle{
                math::Vec3{x0, y0, z},
                math::Vec3{x0 + cell, y0 + cell, z},
                math::Vec3{x0, y0 + cell, z},
            });
        }
    }
    return tris;
}

}  // namespace

TEST_CASE("Packet-8 matches scalar across 1000 random rays", "[render_rt][packet][fuzz]") {
    Lcg rng(0xC0FFEEull);
    auto tris = make_perturbed_wall(8, rng);  // 128 triangles
    Bvh8 blas;
    blas.build(tris.data(), static_cast<u32>(tris.size()));

    Tlas::InstanceDesc desc{};
    desc.blas = &blas;
    desc.transform = math::identity4();
    Tlas tlas;
    tlas.build(&desc, 1);

    constexpr u32 kPackets = 125;  // 125 * 8 = 1000 rays.
    u32 disagreements = 0;
    for (u32 p = 0; p < kPackets; ++p) {
        ShadowPacket8 pkt{};
        Ray rays[8];
        for (u32 i = 0; i < 8; ++i) {
            const f32 ox = rng.next_signed(-6.0f, 6.0f);
            const f32 oy = rng.next_signed(-6.0f, 6.0f);
            const f32 oz = rng.next_signed(-3.0f, 4.0f);
            // Random direction within forward cone toward +Z.
            const f32 dx = rng.next_signed(-1.0f, 1.0f);
            const f32 dy = rng.next_signed(-1.0f, 1.0f);
            const f32 dz = rng.next_signed(0.3f, 1.0f);
            const f32 inv_l = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
            rays[i].origin = {ox, oy, oz};
            rays[i].direction = {dx * inv_l, dy * inv_l, dz * inv_l};
            rays[i].t_min = 1e-4f;
            rays[i].t_max = 1e30f;
            pkt.rays[i] = rays[i];
        }
        bool scalar_ref[8];
        for (u32 i = 0; i < 8; ++i) {
            scalar_ref[i] = tlas.occluded(rays[i]);
        }
        trace_shadow_packet(tlas, pkt);
        for (u32 i = 0; i < 8; ++i) {
            if (pkt.occluded[i] != scalar_ref[i]) {
                ++disagreements;
                INFO("packet " << p << " lane " << i << " origin=(" << rays[i].origin.x << ","
                               << rays[i].origin.y << "," << rays[i].origin.z << ") dir=("
                               << rays[i].direction.x << "," << rays[i].direction.y << ","
                               << rays[i].direction.z << ")"
                               << " scalar=" << scalar_ref[i] << " packet=" << pkt.occluded[i]);
                CHECK(pkt.occluded[i] == scalar_ref[i]);
            }
        }
    }
    REQUIRE(disagreements == 0u);
}

TEST_CASE("Packet-8 matches scalar with multi-instance TLAS + random rays",
          "[render_rt][packet][fuzz][tlas]") {
    Lcg rng(0xDEADBEEFull);
    // Single quad in object space; instance it 4 times at scattered places.
    Triangle quad[2] = {
        Triangle{
            math::Vec3{-0.5f, -0.5f, 0.0f},
            math::Vec3{0.5f, -0.5f, 0.0f},
            math::Vec3{0.5f, 0.5f, 0.0f},
        },
        Triangle{
            math::Vec3{-0.5f, -0.5f, 0.0f},
            math::Vec3{0.5f, 0.5f, 0.0f},
            math::Vec3{-0.5f, 0.5f, 0.0f},
        },
    };
    Bvh8 blas;
    blas.build(quad, 2);

    Tlas::InstanceDesc inst[4];
    for (u32 i = 0; i < 4; ++i) {
        const f32 dx = (i & 1) ? 2.0f : -2.0f;
        const f32 dy = (i & 2) ? 2.0f : -2.0f;
        inst[i].blas = &blas;
        inst[i].transform = math::translate(math::Vec3{dx, dy, 5.0f});
    }
    Tlas tlas;
    tlas.build(inst, 4);

    constexpr u32 kPackets = 30;  // 240 rays — keep fast.
    for (u32 p = 0; p < kPackets; ++p) {
        ShadowPacket8 pkt{};
        Ray rays[8];
        for (u32 i = 0; i < 8; ++i) {
            const f32 ox = rng.next_signed(-3.5f, 3.5f);
            const f32 oy = rng.next_signed(-3.5f, 3.5f);
            const f32 oz = rng.next_signed(0.0f, 1.0f);
            const f32 dx = rng.next_signed(-0.4f, 0.4f);
            const f32 dy = rng.next_signed(-0.4f, 0.4f);
            const f32 dz = rng.next_signed(0.6f, 1.0f);
            const f32 inv_l = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
            rays[i].origin = {ox, oy, oz};
            rays[i].direction = {dx * inv_l, dy * inv_l, dz * inv_l};
            rays[i].t_min = 1e-4f;
            rays[i].t_max = 1e30f;
            pkt.rays[i] = rays[i];
        }
        bool scalar_ref[8];
        for (u32 i = 0; i < 8; ++i)
            scalar_ref[i] = tlas.occluded(rays[i]);
        trace_shadow_packet(tlas, pkt);
        for (u32 i = 0; i < 8; ++i) {
            INFO("packet " << p << " lane " << i);
            REQUIRE(pkt.occluded[i] == scalar_ref[i]);
        }
    }
}
