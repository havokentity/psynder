// SPDX-License-Identifier: MIT
// Psynder physics bench — per-vehicle solver cost (Wave-B bar §10.1).
//
// Tight, reproducible micro-bench: one full vehicle (chassis + 4 wheels) at
// a high-grip cornering condition, stepped N times through the algorithm
// kernels (Pacejka combined slip × 4 wheels, drivetrain step, aero). The
// numbers are printed as mean/stdev microseconds and the program returns
// non-zero on NaN/inf so CI fails loudly if a future change breaks the
// determinism contract.
//
// We deliberately avoid touching the World singleton: the bench is meant to
// measure the per-vehicle math, not the broadphase. Adding it on top of the
// existing physics_solver bench is what M4's full integration test will do
// (DESIGN.md milestone M4 — drivetrain stress).

#include "physics/internal/Kernels.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;

namespace {

// Synthetic vehicle config — keep the numbers stable across runs so the bench
// signal isn't drowned by configuration drift.
struct Wheel {
    f32 omega = 0.0f;
    f32 steer = 0.0f;
    f32 load_n = 4500.0f;  // ~kN per corner on a midsize car
    f32 radius = 0.32f;
    bool drive = false;
};

struct Vehicle {
    Wheel wheels[4];
    kernels::PacejkaCoeffs tire;
    kernels::DrivetrainParams drive;
    f32 engine_rpm = 3500.0f;
    f32 throttle = 0.6f;
    f32 brake = 0.0f;
    f32 clutch = 1.0f;
    i32 gear = 3;
    f32 forward_v = 25.0f;  // m/s
};

Vehicle make_vehicle() {
    Vehicle v;
    v.wheels[0].steer = 0.10f;  // FL
    v.wheels[1].steer = 0.10f;  // FR
    v.wheels[2].drive = true;   // RL
    v.wheels[3].drive = true;   // RR
    v.drive.curve_count = 4;
    v.drive.curve[0] = {1000.0f, 200.0f};
    v.drive.curve[1] = {3500.0f, 400.0f};
    v.drive.curve[2] = {5000.0f, 380.0f};
    v.drive.curve[3] = {7000.0f, 280.0f};
    return v;
}

void bench_step(Vehicle& v, f32 dt) {
    // Drivetrain on the rear axle.
    f32 om_l = v.wheels[2].omega;
    f32 om_r = v.wheels[3].omega;
    auto out =
        kernels::kernel_drivetrain_step(v.drive, v.throttle, v.brake, v.clutch, v.gear, om_l, om_r, v.engine_rpm);
    v.engine_rpm = out.engine_rpm;
    v.wheels[2].omega += (out.wheel_torque_l / 1.5f) * dt;
    v.wheels[3].omega += (out.wheel_torque_r / 1.5f) * dt;

    // Per-wheel Pacejka.
    f32 total_fx = 0.0f, total_fy = 0.0f;
    for (Wheel& w : v.wheels) {
        f32 wheel_speed = w.omega * w.radius;
        f32 denom = std::max(std::fabs(v.forward_v), 1.0f);
        f32 slip_long = (wheel_speed - v.forward_v) / denom;
        f32 slip_lat = std::atan2(0.5f, std::max(std::fabs(v.forward_v), 1.0f)) - w.steer;
        auto t = kernels::kernel_pacejka_combined(slip_long, slip_lat, w.load_n, 1.0f, v.tire);
        total_fx += t.Fx;
        total_fy += t.Fy;
        // Couple Fx back to wheel omega: F·r torque opposes wheel.
        w.omega -= (t.Fx * w.radius / 1.5f) * dt;
    }

    // Aero on the chassis.
    auto aero = kernels::kernel_aero_force(math::Vec3{v.forward_v, 0, 0},
                                           math::Vec3{0, -1, 0},
                                           0.30f,
                                           2.2f,
                                           1.5f,
                                           1.0f);

    // Couple back into v.forward_v so the loop converges instead of running
    // away (we're benching the inner math, not chassis dynamics).
    v.forward_v += (total_fx + aero.x) / 1500.0f * dt;
    if (v.forward_v < 5.0f)
        v.forward_v = 5.0f;
    if (v.forward_v > 80.0f)
        v.forward_v = 80.0f;
    (void)total_fy;
}

f64 bench_one_step(Vehicle& v) {
    auto t0 = std::chrono::steady_clock::now();
    bench_step(v, 1.0f / 120.0f);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<f64, std::micro>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0)
            smoke = true;
    }

    const u32 kWarm = smoke ? 5u : 60u;
    const u32 kSteps = smoke ? 20u : 480u;  // 4 s of sim @ 120 Hz

    Vehicle v = make_vehicle();
    for (u32 i = 0; i < kWarm; ++i)
        bench_step(v, 1.0f / 120.0f);

    std::vector<f64> samples;
    samples.reserve(kSteps);
    for (u32 i = 0; i < kSteps; ++i)
        samples.push_back(bench_one_step(v));

    f64 sum = 0;
    for (f64 s : samples)
        sum += s;
    f64 mean = sum / static_cast<f64>(samples.size());
    f64 var = 0;
    for (f64 s : samples)
        var += (s - mean) * (s - mean);
    var /= static_cast<f64>(samples.size());
    f64 stdev = std::sqrt(var);

    std::printf("[vehicle-bench] steps=%u mean=%.2fus stdev=%.2fus engine_rpm=%.0f speed=%.1fm/s\n",
                kSteps,
                mean,
                stdev,
                static_cast<f64>(v.engine_rpm),
                static_cast<f64>(v.forward_v));

    if (!std::isfinite(mean) || mean <= 0.0)
        return 1;
    if (!std::isfinite(v.engine_rpm) || !std::isfinite(v.forward_v))
        return 1;
    return 0;
}
