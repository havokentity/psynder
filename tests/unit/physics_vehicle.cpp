// SPDX-License-Identifier: MIT
// Psynder physics unit tests — Pacejka tires, drivetrain, aero (Wave B).
//
// Header-only: pulls algorithm kernels straight from
// `physics/internal/Kernels.h` (same pattern as the rest of the lane's
// tests). No psynder_physics linkage needed.
//
// DESIGN.md §10.1 — `psynder_phys::vehicle` Wave-B contract.

#include "physics/internal/Kernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

// ─── Pacejka tire model ──────────────────────────────────────────────────

TEST_CASE("Pacejka magic-formula peaks within the expected slip-ratio window",
          "[physics][vehicle][pacejka]") {
    // Sample the longitudinal channel across a sweep of slip ratios; the
    // peak should land at a small positive κ (Pacejka '94 passenger tire on
    // dry tarmac peaks around κ ≈ 0.10..0.15).
    kernels::PacejkaCoeffs c;
    f32 Fz = 4000.0f;     // 4 kN per wheel (~midsize car corner load)
    f32 mu = 1.0f;

    f32 best_slip = 0.0f;
    f32 best_fx   = 0.0f;
    for (f32 k = 0.01f; k < 1.0f; k += 0.01f) {
        auto t = kernels::kernel_pacejka_combined(k, 0.0f, Fz, mu, c);
        if (t.Fx > best_fx) { best_fx = t.Fx; best_slip = k; }
    }
    REQUIRE(best_slip > 0.02f);
    REQUIRE(best_slip < 0.30f);
    REQUIRE(best_fx <= Fz);       // never above mu·Fz
    REQUIRE(best_fx > 0.7f * Fz); // realistic peak
}

TEST_CASE("Pacejka longitudinal force is monotone in slip ratio below the peak",
          "[physics][vehicle][pacejka]") {
    // For the linear/saturation region (small κ), Fx must be monotonically
    // non-decreasing as κ grows from 0 to the peak — the classic tire-load
    // contract that prevents wheelslip causing acceleration loss.
    kernels::PacejkaCoeffs c;
    f32 Fz = 4000.0f;
    f32 mu = 1.0f;

    f32 prev = 0.0f;
    bool monotone = true;
    for (f32 k = 0.0f; k < 0.05f; k += 0.005f) {
        auto t = kernels::kernel_pacejka_combined(k, 0.0f, Fz, mu, c);
        if (t.Fx + 1e-3f < prev) { monotone = false; break; }
        prev = t.Fx;
    }
    REQUIRE(monotone);
}

TEST_CASE("Pacejka combined slip stays inside the friction circle (mu·Fz)",
          "[physics][vehicle][pacejka]") {
    // At high combined slip the magnitude must never exceed mu·Fz —
    // that's the friction-circle clipping the kernel must enforce.
    kernels::PacejkaCoeffs c;
    f32 Fz = 3000.0f;
    f32 mu = 0.9f;
    auto t = kernels::kernel_pacejka_combined(0.6f, 0.3f, Fz, mu, c);
    f32 mag = std::sqrt(t.Fx * t.Fx + t.Fy * t.Fy);
    REQUIRE(mag <= mu * Fz + 1e-3f);
    REQUIRE(mag > 0.5f * mu * Fz);
}

TEST_CASE("Pacejka returns zero force on a wheel that is off the ground",
          "[physics][vehicle][pacejka]") {
    kernels::PacejkaCoeffs c;
    auto t = kernels::kernel_pacejka_combined(0.5f, 0.2f, 0.0f, 1.0f, c);
    REQUIRE(t.Fx == Approx(0.0f).margin(1e-6f));
    REQUIRE(t.Fy == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("Pacejka force scales linearly with vertical load near peak slip",
          "[physics][vehicle][pacejka]") {
    // Double the load → roughly double the peak force (the classic
    // dry-tarmac assumption). Real tires lose a few percent due to load
    // sensitivity, but the kernel uses an identical mu so the test is
    // tight.
    kernels::PacejkaCoeffs c;
    auto a = kernels::kernel_pacejka_combined(0.10f, 0.0f, 2000.0f, 1.0f, c);
    auto b = kernels::kernel_pacejka_combined(0.10f, 0.0f, 4000.0f, 1.0f, c);
    REQUIRE(b.Fx == Approx(2.0f * a.Fx).margin(0.05f * a.Fx + 1.0f));
}

// ─── Drivetrain ──────────────────────────────────────────────────────────

namespace {
kernels::DrivetrainParams make_default_drivetrain(f32 peak = 350.0f) {
    kernels::DrivetrainParams d;
    d.curve_count = 4;
    d.curve[0] = { 1000.0f, 0.5f * peak };
    d.curve[1] = { 3000.0f, 0.9f * peak };
    d.curve[2] = { 4500.0f, peak };
    d.curve[3] = { 7000.0f, 0.6f * peak };
    d.idle_rpm = 800.0f;
    d.redline_rpm = 7000.0f;
    d.gears = { -3.4f, 3.6f, 2.2f, 1.5f, 1.15f, 0.9f, 0.75f };
    d.final_drive = 3.42f;
    return d;
}
}  // namespace

TEST_CASE("Engine torque curve interpolates linearly between control points",
          "[physics][vehicle][drivetrain]") {
    auto d = make_default_drivetrain(400.0f);
    // Halfway between 1000 (200) and 3000 (360) is at 2000 rpm → 280 N·m.
    f32 t = kernels::kernel_engine_torque_at(d, 2000.0f);
    REQUIRE(t == Approx(280.0f).margin(1.0f));
}

TEST_CASE("Engine torque curve clamps below idle and above redline",
          "[physics][vehicle][drivetrain]") {
    auto d = make_default_drivetrain(400.0f);
    f32 t_low  = kernels::kernel_engine_torque_at(d, 500.0f);
    f32 t_high = kernels::kernel_engine_torque_at(d, 9000.0f);
    REQUIRE(t_low  == Approx(200.0f).margin(1.0f));
    REQUIRE(t_high == Approx(240.0f).margin(1.0f));
}

TEST_CASE("Drivetrain delivers correct wheel torque for known throttle + gear",
          "[physics][vehicle][drivetrain]") {
    // Full throttle in 1st at 4500 rpm: engine torque = 400 N·m, gear
    // ratio = 3.6, final drive = 3.42. The differential splits 50/50, so
    // each wheel should see ≈ 0.5 · 400 · 3.6 · 3.42 = 2462 N·m, less the
    // tiny RPM update. Clutch is fully engaged.
    auto d = make_default_drivetrain(400.0f);
    auto out = kernels::kernel_drivetrain_step(
        d, /*throttle*/1.0f, /*brake*/0.0f, /*clutch*/1.0f, /*gear*/1,
        /*omega_l*/0.0f, /*omega_r*/0.0f, /*rpm_in*/4500.0f);
    REQUIRE(out.wheel_torque_l == Approx(0.5f * 400.0f * 3.6f * 3.42f)
                                     .margin(5.0f));
    REQUIRE(out.wheel_torque_l == Approx(out.wheel_torque_r).margin(1e-3f));
}

TEST_CASE("Drivetrain produces zero wheel torque in neutral",
          "[physics][vehicle][drivetrain]") {
    auto d = make_default_drivetrain(400.0f);
    auto out = kernels::kernel_drivetrain_step(
        d, 1.0f, 0.0f, 1.0f, /*neutral*/0, 0.0f, 0.0f, 3000.0f);
    REQUIRE(out.wheel_torque_l == Approx(0.0f).margin(1e-3f));
    REQUIRE(out.wheel_torque_r == Approx(0.0f).margin(1e-3f));
    // Neutral throttle still revs the engine.
    REQUIRE(out.engine_rpm > 3000.0f);
}

TEST_CASE("Reverse gear produces negative wheel torque under positive throttle",
          "[physics][vehicle][drivetrain]") {
    auto d = make_default_drivetrain(400.0f);
    auto out = kernels::kernel_drivetrain_step(
        d, 1.0f, 0.0f, 1.0f, /*reverse*/-1, 0.0f, 0.0f, 3000.0f);
    REQUIRE(out.wheel_torque_l < 0.0f);
    REQUIRE(out.wheel_torque_r < 0.0f);
}

TEST_CASE("Brake torque opposes wheel rotation in both directions",
          "[physics][vehicle][drivetrain]") {
    auto d = make_default_drivetrain(400.0f);
    // Forward-rolling wheels, brakes on: brake torque must be negative.
    auto fwd = kernels::kernel_drivetrain_step(
        d, 0.0f, 1.0f, 1.0f, /*neutral*/0, 10.0f, 10.0f, 1000.0f);
    REQUIRE(fwd.wheel_torque_l < 0.0f);
    REQUIRE(fwd.wheel_torque_r < 0.0f);
    // Reverse-rolling: brake torque flips sign.
    auto rev = kernels::kernel_drivetrain_step(
        d, 0.0f, 1.0f, 1.0f, /*neutral*/0, -10.0f, -10.0f, 1000.0f);
    REQUIRE(rev.wheel_torque_l > 0.0f);
    REQUIRE(rev.wheel_torque_r > 0.0f);
}

// ─── Aero ────────────────────────────────────────────────────────────────

TEST_CASE("Aero drag scales with v^2 — doubling speed quadruples drag",
          "[physics][vehicle][aero]") {
    // Drag force = ½ρv²·Cd·A. With the same Cd·A, doubling the speed should
    // multiply the magnitude by exactly 4 (within float noise).
    math::Vec3 down{0, -1, 0};
    math::Vec3 v1{10.0f, 0, 0};
    math::Vec3 v2{20.0f, 0, 0};
    auto f1 = kernels::kernel_aero_force(v1, down, 0.30f, 2.2f, 0.0f, 0.0f);
    auto f2 = kernels::kernel_aero_force(v2, down, 0.30f, 2.2f, 0.0f, 0.0f);
    f32 m1 = std::sqrt(math::dot(f1, f1));
    f32 m2 = std::sqrt(math::dot(f2, f2));
    REQUIRE(m2 == Approx(4.0f * m1).margin(1e-3f));
}

TEST_CASE("Aero drag opposes velocity direction", "[physics][vehicle][aero]") {
    math::Vec3 down{0, -1, 0};
    math::Vec3 v{30.0f, 0, 0};
    auto f = kernels::kernel_aero_force(v, down, 0.30f, 2.2f, 0.0f, 0.0f);
    REQUIRE(f.x < 0.0f);     // points opposite to +x velocity
    REQUIRE(std::fabs(f.y) < 1e-3f);
    REQUIRE(std::fabs(f.z) < 1e-3f);
}

TEST_CASE("Downforce points along world-down when configured",
          "[physics][vehicle][aero]") {
    math::Vec3 down{0, -1, 0};
    math::Vec3 v{40.0f, 0, 0};      // 40 m/s ≈ 144 km/h
    auto f = kernels::kernel_aero_force(v, down, 0.30f, 2.2f, 3.0f, 1.4f);
    // Y-component must be negative (downforce) and substantial (~kilonewton-ish
    // at this speed — order-of-magnitude check).
    REQUIRE(f.y < -100.0f);
}

TEST_CASE("Aero produces zero force at standstill", "[physics][vehicle][aero]") {
    auto f = kernels::kernel_aero_force({0, 0, 0}, {0, -1, 0},
                                        0.30f, 2.2f, 1.0f, 1.0f);
    REQUIRE(f.x == Approx(0.0f).margin(1e-6f));
    REQUIRE(f.y == Approx(0.0f).margin(1e-6f));
    REQUIRE(f.z == Approx(0.0f).margin(1e-6f));
}
