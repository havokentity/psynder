// SPDX-License-Identifier: MIT
// Psynder — Lane 02 math tests for batched transform helpers.

#include "math/Batch.h"
#include "math/Math.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using namespace psynder;
using namespace psynder::math;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-5f) {
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

TEST_CASE("batch transform_points matches Mat4 Vec4 transform", "[math][batch]") {
    const Mat4 m = sample_transform();
    const std::array<Vec3, 9> input{{
        {-3.0f, 0.0f, 1.0f},
        {-2.0f, 0.5f, 2.0f},
        {-1.0f, -0.25f, 0.0f},
        {0.0f, 1.0f, -1.0f},
        {1.0f, -1.5f, 3.0f},
        {2.0f, 2.0f, -2.0f},
        {3.0f, 0.25f, 0.5f},
        {4.0f, -0.75f, 1.5f},
        {5.0f, 1.25f, -3.0f},
    }};
    std::array<Vec3, input.size()> output{};

    transform_points(m, input.data(), output.data(), output.size());

    for (usize i = 0; i < input.size(); ++i) {
        const Vec4 ref = mul(m, Vec4{input[i].x, input[i].y, input[i].z, 1.0f});
        require_vec3_close(output[i], Vec3{ref.x, ref.y, ref.z});
    }
}

TEST_CASE("batch transform_dirs skips translation and handles scalar tail", "[math][batch]") {
    const Mat4 m = sample_transform();
    const std::array<Vec3, 7> input{{
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {1.0f, 2.0f, 3.0f},
        {-1.0f, 2.0f, -3.0f},
        {4.0f, -0.5f, 0.25f},
        {-2.0f, -1.0f, 0.75f},
    }};
    std::array<Vec3, input.size()> output{};

    transform_dirs(m, input.data(), output.data(), output.size());

    for (usize i = 0; i < input.size(); ++i) {
        const Vec4 ref = mul(m, Vec4{input[i].x, input[i].y, input[i].z, 0.0f});
        require_vec3_close(output[i], Vec3{ref.x, ref.y, ref.z});
    }
}

TEST_CASE("batch transform helpers accept empty spans", "[math][batch]") {
    const Mat4 m = sample_transform();
    Vec3 out{1.0f, 2.0f, 3.0f};

    transform_points(m, nullptr, &out, 0);
    require_vec3_close(out, Vec3{1.0f, 2.0f, 3.0f});

    transform_dirs(m, nullptr, &out, 0);
    require_vec3_close(out, Vec3{1.0f, 2.0f, 3.0f});
}
