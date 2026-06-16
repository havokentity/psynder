// SPDX-License-Identifier: MIT
// Psynder — MathLogicKernel tests.

#include "math/MathLogicKernel.h"
#include "math/VectorStack.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

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

}  // namespace

TEST_CASE("MathLogicKernel records ball motion once and executes SoA streams",
          "[math][logic_kernel]") {
    MathLogicKernelBuilder builder;
    builder.begin_record();

    auto position = builder.vec3_stream(0);
    auto movement = builder.vec3_stream(1);
    auto accel = builder.vec3_stream(2);
    const LogicScalar dt = builder.f32_uniform(0, 0.5f);
    const LogicScalar mass = builder.f32_uniform(1, 2.0f);

    movement = movement + accel * dt * mass;
    position = position + movement * dt;

    MathLogicKernel kernel = builder.end_record();

    Vec3SoaBuffer positions(5);
    Vec3SoaBuffer movements(5);
    Vec3SoaBuffer accels(5);

    for (usize i = 0; i < positions.count(); ++i) {
        positions.x_data()[i] = static_cast<f32>(i);
        positions.y_data()[i] = static_cast<f32>(i) + 10.0f;
        positions.z_data()[i] = static_cast<f32>(i) + 20.0f;
        movements.x_data()[i] = 1.0f;
        movements.y_data()[i] = 2.0f;
        movements.z_data()[i] = 3.0f;
        accels.x_data()[i] = 0.5f;
        accels.y_data()[i] = 1.0f;
        accels.z_data()[i] = -0.25f;
    }

    std::array streams{
        positions.mutable_view(),
        movements.mutable_view(),
        accels.mutable_view(),
    };
    REQUIRE(kernel.execute(streams) == positions.count());

    const Vec3 expected_movement{
        1.0f + 0.5f * 0.5f * 2.0f,
        2.0f + 1.0f * 0.5f * 2.0f,
        3.0f + -0.25f * 0.5f * 2.0f,
    };

    for (usize i = 0; i < positions.count(); ++i) {
        require_vec3_close(Vec3{movements.x_data()[i], movements.y_data()[i], movements.z_data()[i]},
                           expected_movement);
        require_vec3_close(Vec3{positions.x_data()[i], positions.y_data()[i], positions.z_data()[i]},
                           Vec3{
                               static_cast<f32>(i) + expected_movement.x * 0.5f,
                               static_cast<f32>(i) + 10.0f + expected_movement.y * 0.5f,
                               static_cast<f32>(i) + 20.0f + expected_movement.z * 0.5f,
                           });
    }

    REQUIRE(kernel.stats().stores == 2);
    REQUIRE(kernel.stats().fused_madd == 2);
    REQUIRE(kernel.stats().vec3_streams == 3);
}

TEST_CASE("MathLogicKernel uniforms can be updated without re-recording", "[math][logic_kernel]") {
    MathLogicKernelBuilder builder;
    builder.begin_record();

    auto position = builder.vec3_stream(0);
    auto velocity = builder.vec3_stream(1);
    const LogicScalar dt = builder.f32_uniform(0, 1.0f);

    position = position + velocity * dt;

    MathLogicKernel kernel = builder.end_record();
    REQUIRE(kernel.set_f32_uniform(0, 0.25f));

    Vec3SoaBuffer positions(2);
    Vec3SoaBuffer velocities(2);
    positions.x_data()[0] = 10.0f;
    positions.y_data()[0] = 20.0f;
    positions.z_data()[0] = 30.0f;
    velocities.x_data()[0] = 4.0f;
    velocities.y_data()[0] = -8.0f;
    velocities.z_data()[0] = 12.0f;

    std::array streams{
        positions.mutable_view(),
        velocities.mutable_view(),
    };
    REQUIRE(kernel.execute(streams) == positions.count());
    require_vec3_close(Vec3{positions.x_data()[0], positions.y_data()[0], positions.z_data()[0]},
                       Vec3{11.0f, 18.0f, 33.0f});
}

TEST_CASE("MathLogicKernel returns zero when required streams are missing", "[math][logic_kernel]") {
    MathLogicKernelBuilder builder;
    builder.begin_record();

    auto position = builder.vec3_stream(2);
    const auto offset = builder.vec3_uniform(0, Vec3{1.0f, 2.0f, 3.0f});
    position = position + offset;

    MathLogicKernel kernel = builder.end_record();
    std::array<MutableVec3SoaView, 1> streams{};
    REQUIRE(kernel.execute(streams) == 0);
}
