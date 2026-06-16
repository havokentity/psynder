// SPDX-License-Identifier: MIT
// Psynder physics bench — VectorStack particle/gravity workload.
//
// This is a measuring stick for the coming physics refactor: gameplay-style
// kinematic work should live in SoA streams and submit bulk math to
// VectorStack. The current rigid-body World is still AoS; this bench lets us
// quantify the migration path before cutting into that system.

#include "core/Types.h"
#include "math/Math.h"
#include "math/VectorStack.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace psynder;

namespace {

struct BenchOpts {
    bool smoke = false;
    usize particles = 1u << 16;
    u32 steps = 120;
    u32 planets = 4;
};

struct Planet {
    math::Vec3 position{};
    f32 gm = 1.0f;
};

struct Particle {
    math::Vec3 position{};
    math::Vec3 velocity{};
};

template <class T>
PSY_FORCEINLINE void do_not_optimize(const T& v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&v) : "memory");
#else
    const volatile T& sink = v;
    (void)sink;
#endif
}

BenchOpts parse_opts(int argc, char** argv) {
    BenchOpts o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            o.smoke = true;
            o.particles = 4096;
            o.steps = 8;
            o.planets = 3;
        }
    }
    return o;
}

std::vector<Planet> make_planets(u32 count) {
    std::vector<Planet> planets;
    planets.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const f32 t = static_cast<f32>(i) * 1.6180339f;
        planets.push_back(Planet{
            math::Vec3{
                std::cos(t) * 42.0f,
                std::sin(t * 0.7f) * 9.0f,
                std::sin(t) * 42.0f,
            },
            85.0f + static_cast<f32>(i) * 19.0f,
        });
    }
    return planets;
}

std::vector<Particle> make_particles_aos(usize count) {
    std::vector<Particle> particles(count);
    for (usize i = 0; i < count; ++i) {
        const f32 fi = static_cast<f32>(i);
        particles[i].position = math::Vec3{
            (static_cast<f32>(i % 257) - 128.0f) * 0.22f,
            (static_cast<f32>((i / 257) % 97) - 48.0f) * 0.18f,
            (static_cast<f32>((i / 73) % 251) - 125.0f) * 0.22f,
        };
        particles[i].velocity = math::Vec3{
            std::sin(fi * 0.017f) * 0.35f,
            std::cos(fi * 0.013f) * 0.20f,
            std::sin(fi * 0.011f) * 0.35f,
        };
    }
    return particles;
}

void make_particles_soa(usize count, math::Vec3SoaBuffer& positions, math::Vec3SoaBuffer& velocities) {
    positions.resize(count);
    velocities.resize(count);
    f32* px = positions.x_data();
    f32* py = positions.y_data();
    f32* pz = positions.z_data();
    f32* vx = velocities.x_data();
    f32* vy = velocities.y_data();
    f32* vz = velocities.z_data();
    for (usize i = 0; i < count; ++i) {
        const f32 fi = static_cast<f32>(i);
        px[i] = (static_cast<f32>(i % 257) - 128.0f) * 0.22f;
        py[i] = (static_cast<f32>((i / 257) % 97) - 48.0f) * 0.18f;
        pz[i] = (static_cast<f32>((i / 73) % 251) - 125.0f) * 0.22f;
        vx[i] = std::sin(fi * 0.017f) * 0.35f;
        vy[i] = std::cos(fi * 0.013f) * 0.20f;
        vz[i] = std::sin(fi * 0.011f) * 0.35f;
    }
}

void step_aos(std::vector<Particle>& particles, const std::vector<Planet>& planets, f32 dt) {
    constexpr f32 kSoftening = 9.0f;
    for (Particle& p : particles) {
        math::Vec3 accel{0.0f, 0.0f, 0.0f};
        for (const Planet& planet : planets) {
            const math::Vec3 d = math::sub(planet.position, p.position);
            const f32 r2 = math::dot(d, d) + kSoftening;
            const f32 inv_r = 1.0f / std::sqrt(r2);
            const f32 s = planet.gm * inv_r * inv_r * inv_r;
            accel = math::add(accel, math::mul(d, s));
        }
        p.velocity = math::add(p.velocity, math::mul(accel, dt));
        p.position = math::add(p.position, math::mul(p.velocity, dt));
    }
}

void step_soa(math::Vec3SoaBuffer& positions,
              math::Vec3SoaBuffer& velocities,
              const std::vector<Planet>& planets,
              math::VectorStack& stack,
              f32 dt) {
    constexpr f32 kSoftening = 9.0f;
    f32* px = positions.x_data();
    f32* py = positions.y_data();
    f32* pz = positions.z_data();
    f32* vx = velocities.x_data();
    f32* vy = velocities.y_data();
    f32* vz = velocities.z_data();
    const usize count = positions.count();

    for (usize i = 0; i < count; ++i) {
        f32 ax = 0.0f;
        f32 ay = 0.0f;
        f32 az = 0.0f;
        const f32 x = px[i];
        const f32 y = py[i];
        const f32 z = pz[i];
        for (const Planet& planet : planets) {
            const f32 dx = planet.position.x - x;
            const f32 dy = planet.position.y - y;
            const f32 dz = planet.position.z - z;
            const f32 r2 = dx * dx + dy * dy + dz * dz + kSoftening;
            const f32 inv_r = 1.0f / std::sqrt(r2);
            const f32 s = planet.gm * inv_r * inv_r * inv_r;
            ax += dx * s;
            ay += dy * s;
            az += dz * s;
        }
        vx[i] += ax * dt;
        vy[i] += ay * dt;
        vz[i] += az * dt;
    }

    stack.clear();
    stack.integrate_positions(positions.mutable_view(), velocities.view(), dt);
    stack.flush();
}

template <class Fn>
f64 time_steps(Fn&& fn, u32 steps) {
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 step = 0; step < steps; ++step)
        fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<f64, std::nano>(t1 - t0).count();
}

f32 checksum_aos(const std::vector<Particle>& particles) {
    f32 sum = 0.0f;
    for (usize i = 0; i < particles.size(); i += 97)
        sum += particles[i].position.x + particles[i].position.y * 0.5f +
               particles[i].position.z * 0.25f;
    return sum;
}

f32 checksum_soa(const math::Vec3SoaBuffer& positions) {
    const f32* px = positions.x_data();
    const f32* py = positions.y_data();
    const f32* pz = positions.z_data();
    f32 sum = 0.0f;
    for (usize i = 0; i < positions.count(); i += 97)
        sum += px[i] + py[i] * 0.5f + pz[i] * 0.25f;
    return sum;
}

}  // namespace

int main(int argc, char** argv) {
    const BenchOpts opts = parse_opts(argc, argv);
    const std::vector<Planet> planets = make_planets(opts.planets);
    constexpr f32 kDt = 1.0f / 120.0f;

    std::vector<Particle> aos = make_particles_aos(opts.particles);
    const f64 aos_ns = time_steps([&]() { step_aos(aos, planets, kDt); }, opts.steps);
    const f32 aos_sum = checksum_aos(aos);
    do_not_optimize(aos_sum);

    math::Vec3SoaBuffer positions;
    math::Vec3SoaBuffer velocities;
    make_particles_soa(opts.particles, positions, velocities);
    math::VectorStack stack(opts.particles);
    stack.reserve_ops(1);
    const f64 soa_ns =
        time_steps([&]() { step_soa(positions, velocities, planets, stack, kDt); }, opts.steps);
    const f32 soa_sum = checksum_soa(positions);
    do_not_optimize(soa_sum);

    const f64 elems = static_cast<f64>(opts.particles) * static_cast<f64>(opts.steps);
    const f64 aos_ns_per = aos_ns / elems;
    const f64 soa_ns_per = soa_ns / elems;
    std::printf("[physics-particles] particles=%zu planets=%u steps=%u backend=%s\n",
                static_cast<size_t>(opts.particles),
                opts.planets,
                opts.steps,
                math::vector_stack_backend());
    std::printf(
        "[physics-particles] aos_scalar=%.3fns/particle soa_vectorstack=%.3fns/particle "
        "speedup=%.2fx\n",
        aos_ns_per,
        soa_ns_per,
        aos_ns_per / soa_ns_per);
    std::printf("[physics-particles] checksum_aos=%.6f checksum_soa=%.6f\n",
                static_cast<f64>(aos_sum),
                static_cast<f64>(soa_sum));

    if (!std::isfinite(aos_ns_per) || !std::isfinite(soa_ns_per) || soa_ns_per <= 0.0)
        return 1;
    if (!std::isfinite(aos_sum) || !std::isfinite(soa_sum))
        return 1;
    return 0;
}
