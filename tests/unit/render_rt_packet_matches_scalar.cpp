// SPDX-License-Identifier: MIT
// Psynder — Lane 08 unit test: packet-8 shadow trace matches scalar.
//
// Builds a small scene and feeds the same 8 rays through both
// `Tlas::occluded` (scalar) and `trace_shadow_packet` (8-wide AVX2 on
// x86-64; scalar fallback on NEON). The per-lane results must agree.
//
// This is the load-bearing correctness check for the AVX2 packet kernel.

#include <catch2/catch_test_macros.hpp>

#include "render/rt/Bvh.h"

#include <array>
#include <vector>

using namespace psynder;
using namespace psynder::render::rt;

namespace {

// Make an axis-aligned wall of triangles at z=10, spanning x,y ∈ [-2, 2].
std::vector<Triangle> make_wall_scene() {
    std::vector<Triangle> tris;
    // A 4x4 grid of 32 triangles forming a wall.
    for (i32 j = -2; j < 2; ++j) {
        for (i32 i = -2; i < 2; ++i) {
            const f32 x = static_cast<f32>(i);
            const f32 y = static_cast<f32>(j);
            tris.push_back(Triangle{
                math::Vec3{x, y, 10.0f},
                math::Vec3{x + 1.0f, y, 10.0f},
                math::Vec3{x + 1.0f, y + 1.0f, 10.0f},
            });
            tris.push_back(Triangle{
                math::Vec3{x, y, 10.0f},
                math::Vec3{x + 1.0f, y + 1.0f, 10.0f},
                math::Vec3{x, y + 1.0f, 10.0f},
            });
        }
    }
    return tris;
}

}  // namespace

TEST_CASE("Packet-8 occlusion matches scalar — mixed-hit packet", "[render_rt][packet]") {
    auto tris = make_wall_scene();
    Bvh8 blas;
    blas.build(tris.data(), static_cast<u32>(tris.size()));

    Tlas::InstanceDesc desc{};
    desc.blas = &blas;
    desc.transform = math::identity4();
    Tlas tlas;
    tlas.build(&desc, 1);

    // 8 rays: alternating hits/misses across the packet.
    ShadowPacket8 pkt{};
    const float xs[8] = {0.5f, -10.0f, 1.5f, -10.0f, -1.5f, -10.0f, -0.5f, -10.0f};
    const float ys[8] = {0.5f, 0.0f, 1.5f, 0.0f, -0.5f, 0.0f, -1.5f, 0.0f};
    for (u32 i = 0; i < 8; ++i) {
        pkt.rays[i].origin = {xs[i], ys[i], 0.0f};
        pkt.rays[i].direction = {0.0f, 0.0f, 1.0f};
        pkt.rays[i].t_min = 1e-4f;
        pkt.rays[i].t_max = 1e30f;
    }

    // Scalar reference.
    bool scalar_ref[8];
    for (u32 i = 0; i < 8; ++i) {
        scalar_ref[i] = tlas.occluded(pkt.rays[i]);
    }

    // Packet path.
    trace_shadow_packet(tlas, pkt);

    for (u32 i = 0; i < 8; ++i) {
        INFO("lane " << i);
        REQUIRE(pkt.occluded[i] == scalar_ref[i]);
    }
}

TEST_CASE("Packet-8 occlusion matches scalar — all-hit packet", "[render_rt][packet]") {
    auto tris = make_wall_scene();
    Bvh8 blas;
    blas.build(tris.data(), static_cast<u32>(tris.size()));
    Tlas::InstanceDesc desc{};
    desc.blas = &blas;
    desc.transform = math::identity4();
    Tlas tlas;
    tlas.build(&desc, 1);

    ShadowPacket8 pkt{};
    for (u32 i = 0; i < 8; ++i) {
        // 8 rays all inside the wall span.
        const f32 dx = (static_cast<f32>(i) - 3.5f) * 0.4f;
        pkt.rays[i].origin = {dx, 0.0f, 0.0f};
        pkt.rays[i].direction = {0.0f, 0.0f, 1.0f};
        pkt.rays[i].t_min = 1e-4f;
        pkt.rays[i].t_max = 1e30f;
    }
    bool scalar_ref[8];
    for (u32 i = 0; i < 8; ++i) {
        scalar_ref[i] = tlas.occluded(pkt.rays[i]);
    }
    trace_shadow_packet(tlas, pkt);
    for (u32 i = 0; i < 8; ++i) {
        INFO("lane " << i);
        REQUIRE(pkt.occluded[i] == scalar_ref[i]);
    }
}

TEST_CASE("Packet-8 occlusion matches scalar — all-miss packet", "[render_rt][packet]") {
    auto tris = make_wall_scene();
    Bvh8 blas;
    blas.build(tris.data(), static_cast<u32>(tris.size()));
    Tlas::InstanceDesc desc{};
    desc.blas = &blas;
    desc.transform = math::identity4();
    Tlas tlas;
    tlas.build(&desc, 1);

    ShadowPacket8 pkt{};
    for (u32 i = 0; i < 8; ++i) {
        // 8 rays aimed at (x=±50, y=±50, z=0) — far off the wall.
        const f32 dx = (i & 1) ? 50.0f : -50.0f;
        const f32 dy = (i & 2) ? 50.0f : -50.0f;
        pkt.rays[i].origin = {dx, dy, 0.0f};
        pkt.rays[i].direction = {0.0f, 0.0f, 1.0f};
        pkt.rays[i].t_min = 1e-4f;
        pkt.rays[i].t_max = 1e30f;
    }
    bool scalar_ref[8];
    for (u32 i = 0; i < 8; ++i) {
        scalar_ref[i] = tlas.occluded(pkt.rays[i]);
    }
    trace_shadow_packet(tlas, pkt);
    for (u32 i = 0; i < 8; ++i) {
        INFO("lane " << i);
        REQUIRE(scalar_ref[i] == false);
        REQUIRE(pkt.occluded[i] == false);
    }
}

TEST_CASE("Packet-8 against TLAS with multiple instances matches scalar",
          "[render_rt][packet][tlas]") {
    Triangle quad_tri[2] = {
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
    blas.build(quad_tri, 2);

    std::array<Tlas::InstanceDesc, 3> inst;
    inst[0] = {&blas, math::translate(math::Vec3{-2.0f, 0.0f, 5.0f})};
    inst[1] = {&blas, math::translate(math::Vec3{0.0f, 0.0f, 5.0f})};
    inst[2] = {&blas, math::translate(math::Vec3{2.0f, 0.0f, 5.0f})};
    Tlas tlas;
    tlas.build(inst.data(), 3);

    ShadowPacket8 pkt{};
    const f32 xs[8] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f, -3.0f, -2.4f};
    for (u32 i = 0; i < 8; ++i) {
        pkt.rays[i].origin = {xs[i], 0.0f, 0.0f};
        pkt.rays[i].direction = {0.0f, 0.0f, 1.0f};
        pkt.rays[i].t_min = 1e-4f;
        pkt.rays[i].t_max = 1e30f;
    }
    bool scalar_ref[8];
    for (u32 i = 0; i < 8; ++i) {
        scalar_ref[i] = tlas.occluded(pkt.rays[i]);
    }
    trace_shadow_packet(tlas, pkt);
    for (u32 i = 0; i < 8; ++i) {
        INFO("lane " << i << " origin x=" << xs[i]);
        REQUIRE(pkt.occluded[i] == scalar_ref[i]);
    }
}
