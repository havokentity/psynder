// SPDX-License-Identifier: MIT
// Psynder bench — MathLogicKernel recorded ball movement.

#include "core/Types.h"
#include "math/MathLogicKernel.h"
#include "math/VectorStack.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace psynder;

namespace {

template <class T>
PSY_FORCEINLINE void do_not_optimize(const T& v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&v) : "memory");
#else
    const volatile T& sink = v;
    (void)sink;
#endif
}

struct CaseConfig {
    usize balls = 0;
    u32 iters = 0;
};

struct BallSoa {
    math::Vec3SoaBuffer position;
    math::Vec3SoaBuffer movement;
    math::Vec3SoaBuffer accel;
};

struct BallAos {
    math::Vec3 position{};
    math::Vec3 movement{};
    math::Vec3 accel{};
};

std::vector<BallAos> make_balls_aos(usize count) {
    std::vector<BallAos> balls(count);
    for (usize i = 0; i < count; ++i) {
        const f32 fi = static_cast<f32>(i);
        balls[i].position = math::Vec3{
            std::sin(fi * 0.013f) * 10.0f,
            std::cos(fi * 0.017f) * 4.0f,
            std::sin(fi * 0.019f) * 10.0f,
        };
        balls[i].movement = math::Vec3{
            std::cos(fi * 0.011f) * 0.25f,
            std::sin(fi * 0.007f) * 0.2f,
            std::cos(fi * 0.005f) * 0.25f,
        };
        balls[i].accel = math::Vec3{
            0.15f + std::sin(fi * 0.003f) * 0.05f,
            -9.8f,
            std::cos(fi * 0.004f) * 0.05f,
        };
    }
    return balls;
}

BallSoa make_balls(usize count) {
    BallSoa balls{
        math::Vec3SoaBuffer(count),
        math::Vec3SoaBuffer(count),
        math::Vec3SoaBuffer(count),
    };

    for (usize i = 0; i < count; ++i) {
        const f32 fi = static_cast<f32>(i);
        balls.position.x_data()[i] = std::sin(fi * 0.013f) * 10.0f;
        balls.position.y_data()[i] = std::cos(fi * 0.017f) * 4.0f;
        balls.position.z_data()[i] = std::sin(fi * 0.019f) * 10.0f;
        balls.movement.x_data()[i] = std::cos(fi * 0.011f) * 0.25f;
        balls.movement.y_data()[i] = std::sin(fi * 0.007f) * 0.2f;
        balls.movement.z_data()[i] = std::cos(fi * 0.005f) * 0.25f;
        balls.accel.x_data()[i] = 0.15f + std::sin(fi * 0.003f) * 0.05f;
        balls.accel.y_data()[i] = -9.8f;
        balls.accel.z_data()[i] = std::cos(fi * 0.004f) * 0.05f;
    }

    return balls;
}

void step_aos(std::vector<BallAos>& balls, f32 dt, f32 mass) {
    const f32 accel_step = dt * mass;
    for (BallAos& ball : balls) {
        ball.movement = math::add(ball.movement, math::mul(ball.accel, accel_step));
        ball.position = math::add(ball.position, math::mul(ball.movement, dt));
    }
}

void step_hand(BallSoa& balls, f32 dt, f32 mass) {
    f32* px = balls.position.x_data();
    f32* py = balls.position.y_data();
    f32* pz = balls.position.z_data();
    f32* mx = balls.movement.x_data();
    f32* my = balls.movement.y_data();
    f32* mz = balls.movement.z_data();
    const f32* ax = balls.accel.x_data();
    const f32* ay = balls.accel.y_data();
    const f32* az = balls.accel.z_data();
    const usize count = balls.position.count();
    const f32 accel_step = dt * mass;

    for (usize i = 0; i < count; ++i) {
        mx[i] += ax[i] * accel_step;
        my[i] += ay[i] * accel_step;
        mz[i] += az[i] * accel_step;
        px[i] += mx[i] * dt;
        py[i] += my[i] * dt;
        pz[i] += mz[i] * dt;
    }
}

math::MathLogicKernel make_kernel(f32 dt, f32 mass) {
    math::MathLogicKernelBuilder builder;
    builder.begin_record();

    auto position = builder.vec3_stream(0);
    auto movement = builder.vec3_stream(1);
    auto accel = builder.vec3_stream(2);
    const auto logic_dt = builder.f32_uniform(0, dt);
    const auto logic_mass = builder.f32_uniform(1, mass);

    movement = movement + accel * logic_dt * logic_mass;
    position = position + movement * logic_dt;

    return builder.end_record();
}

void step_kernel(math::MathLogicKernel& kernel, BallSoa& balls) {
    std::array streams{
        balls.position.mutable_view(),
        balls.movement.mutable_view(),
        balls.accel.mutable_view(),
    };
    do_not_optimize(kernel.execute(streams));
}

template <class Fn>
f64 time_iters(Fn&& fn, u32 iters) {
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 i = 0; i < iters; ++i)
        fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<f64, std::nano>(t1 - t0).count();
}

f32 checksum(const BallSoa& balls) {
    f32 sum = 0.0f;
    for (usize i = 0; i < balls.position.count(); i += 17) {
        sum += balls.position.x_data()[i] + balls.position.y_data()[i] * 0.5f +
               balls.position.z_data()[i] * 0.25f;
    }
    return sum;
}

f32 checksum(const std::vector<BallAos>& balls) {
    f32 sum = 0.0f;
    for (usize i = 0; i < balls.size(); i += 17) {
        sum += balls[i].position.x + balls[i].position.y * 0.5f + balls[i].position.z * 0.25f;
    }
    return sum;
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0)
            smoke = true;
    }

    const std::array cases = smoke ? std::array<CaseConfig, 5>{{
                                         {1, 1000},
                                         {10, 1000},
                                         {100, 1000},
                                         {1000, 400},
                                         {10000, 80},
                                     }}
                                   : std::array<CaseConfig, 5>{{
                                         {1, 400000},
                                         {10, 200000},
                                         {100, 100000},
                                         {1000, 20000},
                                         {10000, 3000},
                                     }};

    constexpr f32 kDt = 1.0f / 60.0f;
    constexpr f32 kMass = 1.25f;
    std::printf(
        "# math_logic_kernel "
        "balls,iters,aos_ns_ball,soa_ns_ball,kernel_ns_ball,kernel_vs_aos,kernel_vs_soa,"
        "checksum_delta\n");

    for (const CaseConfig& cfg : cases) {
        std::vector<BallAos> aos = make_balls_aos(cfg.balls);
        BallSoa hand = make_balls(cfg.balls);
        BallSoa recorded = make_balls(cfg.balls);
        math::MathLogicKernel kernel = make_kernel(kDt, kMass);
        kernel.set_f32_uniform(0, kDt);
        kernel.set_f32_uniform(1, kMass);

        step_aos(aos, kDt, kMass);
        step_hand(hand, kDt, kMass);
        step_kernel(kernel, recorded);

        aos = make_balls_aos(cfg.balls);
        hand = make_balls(cfg.balls);
        recorded = make_balls(cfg.balls);

        const f64 aos_ns = time_iters([&]() { step_aos(aos, kDt, kMass); }, cfg.iters);
        const f64 hand_ns = time_iters([&]() { step_hand(hand, kDt, kMass); }, cfg.iters);
        const f64 kernel_ns = time_iters([&]() { step_kernel(kernel, recorded); }, cfg.iters);
        const f64 denom = static_cast<f64>(cfg.balls) * static_cast<f64>(cfg.iters);
        const f32 aos_sum = checksum(aos);
        const f32 hand_sum = checksum(hand);
        const f32 kernel_sum = checksum(recorded);
        do_not_optimize(aos_sum);
        do_not_optimize(hand_sum);
        do_not_optimize(kernel_sum);

        std::printf("math_logic_kernel,%zu,%u,%.3f,%.3f,%.3f,%.2f,%.2f,%.6f\n",
                    static_cast<size_t>(cfg.balls),
                    cfg.iters,
                    aos_ns / denom,
                    hand_ns / denom,
                    kernel_ns / denom,
                    aos_ns / kernel_ns,
                    hand_ns / kernel_ns,
                    static_cast<f64>(std::fabs(aos_sum - kernel_sum) +
                                     std::fabs(hand_sum - kernel_sum)));
    }

    return 0;
}
