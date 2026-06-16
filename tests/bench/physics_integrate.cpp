// SPDX-License-Identifier: MIT
// Psynder physics bench - per-body integrator throughput (Wave 8, Lane 4).
//
// Times the three embarrassingly-parallel per-body loops of the fixed-tick
// integrator over a LARGE body set (50k bodies, many steps):
//     integrate_forces  : v += g*dt + force*inv_mass*dt; angular via inertia
//     integrate_positions: x += v*dt; quaternion integrate + normalise
//     rebuild_aabbs      : world-AABB + speculative swept expansion
//
// The loop bodies here are a faithful copy of World.cpp's `detail::` statics
// (which are file-local and not exported), driven through the SAME kernel
// helpers World.cpp now uses (physics/internal/IntegrateKernels.h). The bench
// runs BOTH the scalar twin and the vectorised twin over identical body
// buffers and (a) reports mean us +/- stdev for each, and (b) memcmp's the two
// resulting buffers byte-for-byte - so the speed number and the bit-identity
// proof come from one binary. A drift makes the bench return non-zero.

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Body.h"
#include "physics/Shape.h"
#include "physics/FpControl.h"
#include "physics/internal/IntegrateKernels.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;

namespace {

constexpr f32 kSpeculativeMargin = 0.02f;  // mirrors kernels::kSpeculativeMargin (2 cm)
constexpr f32 kDt = 1.0f / 120.0f;

template <class T>
PSY_FORCEINLINE void do_not_optimize(const T& v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&v) : "memory");
#else
    const volatile T& sink = v;
    (void)sink;
#endif
}

// Deterministic body soup: a mix of moving dynamic boxes/spheres so the swept
// branch in rebuild_aabbs is exercised on a real fraction of the set, plus a
// sprinkling of static/sleeping holes so the skip predicate is non-trivial.
std::vector<Body> make_bodies(usize n) {
    std::vector<Body> v(n);
    for (usize i = 0; i < n; ++i) {
        Body& b = v[i];
        const f32 fi = static_cast<f32>(i);
        b.alive = 1;
        b.position = {std::sin(fi * 0.017f) * 20.0f,
                      5.0f + std::cos(fi * 0.013f) * 3.0f,
                      std::sin(fi * 0.011f) * 20.0f};
        b.rotation = math::quat_normalize({std::sin(fi * 0.03f), std::cos(fi * 0.02f),
                                           std::sin(fi * 0.05f), 1.0f});
        b.linear_velocity = {std::cos(fi * 0.019f) * 6.0f, std::sin(fi * 0.007f) * 2.0f,
                             std::cos(fi * 0.023f) * 6.0f};
        b.angular_velocity = {std::sin(fi * 0.04f) * 1.5f, std::cos(fi * 0.06f) * 1.5f,
                              std::sin(fi * 0.08f) * 1.5f};
        b.force = {std::cos(fi * 0.09f) * 4.0f, std::sin(fi * 0.10f) * 4.0f,
                   std::cos(fi * 0.11f) * 4.0f};
        b.torque = {std::sin(fi * 0.12f) * 2.0f, std::cos(fi * 0.13f) * 2.0f,
                    std::sin(fi * 0.14f) * 2.0f};
        b.mass = 1.0f;
        b.inv_mass = 1.0f;
        b.shape = static_cast<u8>(i % 3);  // 0 sphere, 1 capsule, 2 box
        b.half_extent = {0.5f, 0.4f, 0.6f};
        const f32 ix = 0.2f, iy = 0.25f, iz = 0.3f;
        b.inertia.local = {ix, iy, iz};
        b.inertia.inv_local = {1.0f / ix, 1.0f / iy, 1.0f / iz};
        // Every 17th body is a static hole; every 31st sleeps - non-trivial skip.
        if (i % 17 == 0) {
            b.inv_mass = 0.0f;
            b.mass = 0.0f;
        }
        if (i % 31 == 0)
            b.flags = BodyFlags::Sleeping;
    }
    return v;
}

const math::Vec3 kGravity{0, -9.81f, 0};

// --- Scalar twin (literal copy of World.cpp's pre-SIMD loop bodies) -------
void tick_scalar(std::vector<Body>& bodies, f32 dt) {
    for (Body& b : bodies) {
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
            continue;
        b.linear_velocity = integrate::forces_linear_scalar(b.linear_velocity, kGravity,
                                                            b.force, b.inv_mass, dt);
        math::Vec3 ang_accel =
            apply_inv_inertia_world(b.rotation, b.inertia.inv_local, b.torque);
        b.angular_velocity = math::add(b.angular_velocity, math::mul(ang_accel, dt));
        b.force = {0, 0, 0};
        b.torque = {0, 0, 0};
    }
    // rebuild_aabbs
    for (const Body& b : bodies) {
        if (b.alive == 0)
            continue;
        math::Aabb box = aabb_world(b.shape, b.half_extent, b.position, b.rotation);
        if (b.inv_mass > 0.0f) {
            const math::Vec3 disp = integrate::swept_disp_scalar(b.linear_velocity, dt);
            const f32 disp_len2 = math::dot(disp, disp);
            const f32 margin = kSpeculativeMargin;
            if (disp_len2 > margin * margin) {
                if (disp.x < 0.0f) box.min.x += disp.x; else box.max.x += disp.x;
                if (disp.y < 0.0f) box.min.y += disp.y; else box.max.y += disp.y;
                if (disp.z < 0.0f) box.min.z += disp.z; else box.max.z += disp.z;
            }
        }
        do_not_optimize(box);
    }
    for (Body& b : bodies) {
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
            continue;
        b.position = integrate::position_linear_scalar(b.position, b.linear_velocity, dt);
        math::Quat w_q{b.angular_velocity.x, b.angular_velocity.y, b.angular_velocity.z, 0.0f};
        math::Quat dq = math::quat_mul(w_q, b.rotation);
        b.rotation = math::quat_normalize(
            integrate::quat_step_pre_normalize_scalar(b.rotation, dq, dt));
    }
}

// --- Vectorised twin (literal copy of World.cpp's CURRENT loop bodies) ----
void tick_simd(std::vector<Body>& bodies, f32 dt) {
    for (Body& b : bodies) {
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
            continue;
        b.linear_velocity = integrate::forces_linear_simd(b.linear_velocity, kGravity,
                                                          b.force, b.inv_mass, dt);
        math::Vec3 ang_accel =
            apply_inv_inertia_world(b.rotation, b.inertia.inv_local, b.torque);
        b.angular_velocity = math::add(b.angular_velocity, math::mul(ang_accel, dt));
        b.force = {0, 0, 0};
        b.torque = {0, 0, 0};
    }
    for (const Body& b : bodies) {
        if (b.alive == 0)
            continue;
        math::Aabb box = aabb_world(b.shape, b.half_extent, b.position, b.rotation);
        if (b.inv_mass > 0.0f) {
            const math::Vec3 disp = integrate::swept_disp_simd(b.linear_velocity, dt);
            const f32 disp_len2 = math::dot(disp, disp);
            const f32 margin = kSpeculativeMargin;
            if (disp_len2 > margin * margin) {
                if (disp.x < 0.0f) box.min.x += disp.x; else box.max.x += disp.x;
                if (disp.y < 0.0f) box.min.y += disp.y; else box.max.y += disp.y;
                if (disp.z < 0.0f) box.min.z += disp.z; else box.max.z += disp.z;
            }
        }
        do_not_optimize(box);
    }
    for (Body& b : bodies) {
        if (b.alive == 0 || b.inv_mass == 0.0f || (b.flags & BodyFlags::Sleeping) != 0u)
            continue;
        b.position = integrate::position_linear_simd(b.position, b.linear_velocity, dt);
        // Quaternion step stays scalar in BOTH configs (FMA-contraction -
        // see IntegrateKernels.h). Only forces/position/disp are vectorised.
        math::Quat w_q{b.angular_velocity.x, b.angular_velocity.y, b.angular_velocity.z, 0.0f};
        math::Quat dq = math::quat_mul(w_q, b.rotation);
        b.rotation = math::quat_normalize(
            integrate::quat_step_pre_normalize_scalar(b.rotation, dq, dt));
    }
}

template <class Fn>
void run(const char* label, Fn&& tick, std::vector<Body> bodies, u32 steps,
         std::vector<Body>& out_final) {
    std::vector<f64> samples;
    samples.reserve(steps);
    for (u32 s = 0; s < steps; ++s) {
        const auto t0 = std::chrono::steady_clock::now();
        tick(bodies, kDt);
        const auto t1 = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<f64, std::micro>(t1 - t0).count());
        do_not_optimize(bodies[0].position);
    }
    f64 sum = 0;
    for (f64 v : samples)
        sum += v;
    const f64 mean = sum / static_cast<f64>(samples.size());
    f64 var = 0;
    for (f64 v : samples)
        var += (v - mean) * (v - mean);
    var /= static_cast<f64>(samples.size());
    const f64 stdev = std::sqrt(var);
    std::printf("[physics-integrate] %-7s mean=%.2fus stdev=%.2fus\n", label, mean, stdev);
    out_final = std::move(bodies);
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0)
            smoke = true;
    }

    // Pin round-to-nearest so the scalar/SIMD memcmp is a fair fixed-FPU test,
    // exactly as production step() does (FpGuard).
    physics::detail::FpGuard fp_guard;

    const usize kBodies = smoke ? 4096u : 50000u;
    const u32 kSteps = smoke ? 8u : 240u;

    const std::vector<Body> seed = make_bodies(kBodies);

    std::vector<Body> final_scalar, final_simd;
    run("scalar", tick_scalar, seed, kSteps, final_scalar);
    run("simd", tick_simd, seed, kSteps, final_simd);

    std::printf("[physics-integrate] bodies=%zu steps=%u\n",
                static_cast<size_t>(kBodies), kSteps);

    // Bit-identity proof: after the same number of steps from the same seed,
    // the SIMD path must reproduce the scalar path BYTE-for-byte.
    if (final_scalar.size() != final_simd.size()) {
        std::printf("[physics-integrate] FAIL size mismatch\n");
        return 1;
    }
    usize mismatches = 0;
    for (usize i = 0; i < final_scalar.size(); ++i) {
        if (std::memcmp(&final_scalar[i].position, &final_simd[i].position,
                        sizeof(math::Vec3)) != 0 ||
            std::memcmp(&final_scalar[i].rotation, &final_simd[i].rotation,
                        sizeof(math::Quat)) != 0 ||
            std::memcmp(&final_scalar[i].linear_velocity, &final_simd[i].linear_velocity,
                        sizeof(math::Vec3)) != 0 ||
            std::memcmp(&final_scalar[i].angular_velocity, &final_simd[i].angular_velocity,
                        sizeof(math::Vec3)) != 0)
            ++mismatches;
    }
    std::printf("[physics-integrate] bit-identity scalar-vs-simd: %s (%zu mismatches)\n",
                mismatches == 0 ? "PASS" : "FAIL", static_cast<size_t>(mismatches));

    return mismatches == 0 ? 0 : 1;
}
