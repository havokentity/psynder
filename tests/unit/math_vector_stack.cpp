// SPDX-License-Identifier: MIT
// Psynder — VectorStack bulk math tests.

#include "math/Math.h"
#include "math/VectorStack.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>

using namespace psynder;
using namespace psynder::math;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

void require_vec3_close(Vec3 a, Vec3 b) {
    REQUIRE(approx_eq(a.x, b.x));
    REQUIRE(approx_eq(a.y, b.y));
    REQUIRE(approx_eq(a.z, b.z));
}

Mat4 sample_transform() {
    Quat q = quat_normalize(quat_from_axis_angle(Vec3{0.2f, 0.7f, -0.4f}, 0.8f));
    return mul(translate(Vec3{3.0f, -2.0f, 5.0f}), mul(rotate_quat(q), scale(Vec3{1.5f, 0.5f, 2.0f})));
}

}  // namespace

TEST_CASE("VectorStack transforms SoA points in flush order", "[math][vector_stack]") {
    const Mat4 m = sample_transform();
    std::array<f32, 9> x{};
    std::array<f32, 9> y{};
    std::array<f32, 9> z{};
    std::array<f32, 9> ox{};
    std::array<f32, 9> oy{};
    std::array<f32, 9> oz{};

    for (usize i = 0; i < x.size(); ++i) {
        x[i] = (static_cast<f32>(i) - 4.0f) * 0.25f;
        y[i] = (static_cast<f32>(i % 3) - 1.0f) * 0.5f;
        z[i] = (static_cast<f32>(i % 5) - 2.0f) * 0.75f;
    }

    VectorStack stack;
    stack.transform_points(m,
                           Vec3SoaView{x.data(), y.data(), z.data(), x.size()},
                           MutableVec3SoaView{ox.data(), oy.data(), oz.data(), ox.size()});
    stack.flush();

    for (usize i = 0; i < x.size(); ++i) {
        const Vec4 ref = mul(m, Vec4{x[i], y[i], z[i], 1.0f});
        require_vec3_close(Vec3{ox[i], oy[i], oz[i]}, Vec3{ref.x, ref.y, ref.z});
    }
    REQUIRE(stack.stats().ops_submitted == 1);
    REQUIRE(stack.stats().ops_flushed == 1);
    REQUIRE(stack.stats().soa_elements == x.size());
}

TEST_CASE("Vec3SoaBuffer stores component streams on cache-line boundaries",
          "[math][vector_stack]") {
    Vec3SoaBuffer buffer(32);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.x_data()) % kCacheLine == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.y_data()) % kCacheLine == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.z_data()) % kCacheLine == 0);
    REQUIRE(buffer.count() == 32);
}

TEST_CASE("VectorStack transforms SoA dirs without translation", "[math][vector_stack]") {
    const Mat4 m = sample_transform();
    std::array<f32, 7> x{{1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 4.0f, -2.0f}};
    std::array<f32, 7> y{{0.0f, 1.0f, 0.0f, 2.0f, 2.0f, -0.5f, -1.0f}};
    std::array<f32, 7> z{{0.0f, 0.0f, 1.0f, 3.0f, -3.0f, 0.25f, 0.75f}};
    std::array<f32, 7> ox{};
    std::array<f32, 7> oy{};
    std::array<f32, 7> oz{};

    VectorStack stack;
    stack.transform_dirs(m,
                         Vec3SoaView{x.data(), y.data(), z.data(), x.size()},
                         MutableVec3SoaView{ox.data(), oy.data(), oz.data(), ox.size()});
    stack.flush();

    for (usize i = 0; i < x.size(); ++i) {
        const Vec4 ref = mul(m, Vec4{x[i], y[i], z[i], 0.0f});
        require_vec3_close(Vec3{ox[i], oy[i], oz[i]}, Vec3{ref.x, ref.y, ref.z});
    }
}

TEST_CASE("VectorStack bridges legacy AoS Vec3 spans through scratch SoA", "[math][vector_stack]") {
    const Mat4 m = sample_transform();
    const std::array<Vec3, 11> in{{
        {-3.0f, 0.0f, 1.0f},
        {-2.0f, 0.5f, 2.0f},
        {-1.0f, -0.25f, 0.0f},
        {0.0f, 1.0f, -1.0f},
        {1.0f, -1.5f, 3.0f},
        {2.0f, 2.0f, -2.0f},
        {3.0f, 0.25f, 0.5f},
        {4.0f, -0.75f, 1.5f},
        {5.0f, 1.25f, -3.0f},
        {-5.0f, 3.0f, 2.0f},
        {0.5f, -2.0f, 4.0f},
    }};
    std::array<Vec3, in.size()> out{};

    VectorStack stack(4);
    stack.transform_points(m, in.data(), out.data(), out.size());
    stack.flush();

    for (usize i = 0; i < in.size(); ++i) {
        const Vec4 ref = mul(m, Vec4{in[i].x, in[i].y, in[i].z, 1.0f});
        require_vec3_close(out[i], Vec3{ref.x, ref.y, ref.z});
    }
    REQUIRE(stack.stats().aos_elements == in.size());
    REQUIRE(stack.stats().scratch_capacity >= 4);
}

TEST_CASE("VectorStack integrates SoA positions in bulk", "[math][vector_stack]") {
    std::array<f32, 8> px{{0.0f, 1.0f, 2.0f, 3.0f, -1.0f, -2.0f, 4.0f, 8.0f}};
    std::array<f32, 8> py{{0.5f, 1.5f, 2.5f, 3.5f, -1.5f, -2.5f, 4.5f, 8.5f}};
    std::array<f32, 8> pz{{1.0f, 2.0f, 3.0f, 4.0f, -2.0f, -3.0f, 5.0f, 9.0f}};
    std::array<f32, 8> vx{{1.0f, 0.5f, -1.0f, 2.0f, 4.0f, -2.0f, 0.25f, -0.5f}};
    std::array<f32, 8> vy{{-1.0f, 1.0f, 0.5f, -0.5f, 2.0f, 3.0f, -4.0f, 1.5f}};
    std::array<f32, 8> vz{{0.0f, 2.0f, -2.0f, 1.0f, -1.0f, 0.5f, 3.0f, -3.0f}};
    const auto ref_x = px;
    const auto ref_y = py;
    const auto ref_z = pz;
    constexpr f32 dt = 0.125f;

    VectorStack stack;
    stack.integrate_positions(MutableVec3SoaView{px.data(), py.data(), pz.data(), px.size()},
                              Vec3SoaView{vx.data(), vy.data(), vz.data(), vx.size()},
                              dt);
    stack.flush();

    for (usize i = 0; i < px.size(); ++i) {
        REQUIRE(approx_eq(px[i], ref_x[i] + vx[i] * dt));
        REQUIRE(approx_eq(py[i], ref_y[i] + vy[i] * dt));
        REQUIRE(approx_eq(pz[i], ref_z[i] + vz[i] * dt));
    }
    REQUIRE(stack.stats().soa_elements == px.size());
}

TEST_CASE("VectorStack integrates legacy AoS positions", "[math][vector_stack]") {
    std::array<Vec3, 5> positions{{
        {0.0f, 0.5f, 1.0f},
        {1.0f, 1.5f, 2.0f},
        {2.0f, 2.5f, 3.0f},
        {-1.0f, -1.5f, -2.0f},
        {4.0f, 4.5f, 5.0f},
    }};
    const std::array<Vec3, 5> velocities{{
        {1.0f, -1.0f, 0.0f},
        {0.5f, 1.0f, 2.0f},
        {-1.0f, 0.5f, -2.0f},
        {4.0f, 2.0f, -1.0f},
        {0.25f, -4.0f, 3.0f},
    }};
    const auto ref = positions;
    constexpr f32 dt = 0.25f;

    VectorStack stack(4);
    stack.integrate_positions(positions.data(), velocities.data(), positions.size(), dt);
    stack.flush();

    for (usize i = 0; i < positions.size(); ++i) {
        require_vec3_close(positions[i], add(ref[i], mul(velocities[i], dt)));
    }
    REQUIRE(stack.stats().aos_elements == positions.size());
}

TEST_CASE("VectorStack ignores empty submissions", "[math][vector_stack]") {
    VectorStack stack;
    stack.transform_points(identity4(), nullptr, nullptr, 0);
    stack.flush();
    REQUIRE(stack.stats().ops_submitted == 0);
    REQUIRE(stack.stats().ops_flushed == 0);
}
