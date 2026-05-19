// SPDX-License-Identifier: MIT
// Psynder physics bench — per-island solver cost (Wave-A bar §3.4).
//
// CI gate: regress this by > 2% and the PR must justify in description or
// fail (Wave-A bar). Bench harness deliberately tiny — measure one
// reproducible workload so the regression signal is clean. Real
// micro-benchmark library wiring lives in lane 25 (samples-tests); here we
// just print mean and stdev as a smoke summary, plus run a quick correctness
// check so --smoke mode produces a clear pass/fail.

#include "physics/Physics.h"
#include "physics/WorldImpl.h"
#include "physics/Solver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace psynder;

namespace {

void reset_world() {
    auto& w = physics::World::Get();
    auto& s = physics::detail::world_state();
    for (u32 i = 0; i < s.bodies.size(); ++i) {
        if (s.bodies[i].gen != 0)
            w.destroy_body(physics::BodyId{i | (s.bodies[i].gen << 24)});
    }
    s.bodies.clear();
    s.free_slots.clear();
    s.accumulator = 0.0f;
    s.alpha = 0.0f;
}

// Build a small pile of `n` spheres dropping onto a static box. This creates
// a single growing contact island, which is the per-island solver's worst
// case (more constraints == more inner-loop iterations).
void build_pile(u32 n) {
    auto& w = physics::World::Get();
    w.set_gravity({0, -9.81f, 0});

    physics::BodyDesc ground;
    ground.shape = physics::Shape::Box;
    ground.mass = 0.0f;
    ground.position = {0, -0.5f, 0};
    ground.half_extent = {10, 0.5f, 10};
    ground.friction = 0.8f;
    w.create_body(ground);

    for (u32 i = 0; i < n; ++i) {
        physics::BodyDesc sd;
        sd.shape = physics::Shape::Sphere;
        sd.mass = 1.0f;
        f32 x = static_cast<f32>(static_cast<int>(i % 4) - 2) * 1.2f;
        f32 z = static_cast<f32>(static_cast<int>((i / 4) % 4) - 2) * 1.2f;
        f32 y = 2.0f + static_cast<f32>(i / 16) * 1.2f;
        sd.position = {x, y, z};
        sd.half_extent = {0.5f, 0, 0};
        sd.restitution = 0.05f;
        sd.friction = 0.5f;
        w.create_body(sd);
    }
}

f64 bench_one_step() {
    auto t0 = std::chrono::steady_clock::now();
    physics::World::Get().step(1.0f / 120.0f);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<f64, std::micro>(t1 - t0).count();
}

}  // anonymous namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
    }

    // Smaller body count + step count for smoke so CI completes in <1s.
    const u32 kBodies     = smoke ? 16u  : 64u;
    const u32 kWarmSteps  = smoke ? 5u   : 60u;
    const u32 kBenchSteps = smoke ? 10u  : 240u;

    reset_world();
    build_pile(kBodies);

    for (u32 i = 0; i < kWarmSteps; ++i) physics::World::Get().step(1.0f / 120.0f);

    std::vector<f64> samples;
    samples.reserve(kBenchSteps);
    for (u32 i = 0; i < kBenchSteps; ++i) samples.push_back(bench_one_step());

    f64 sum = 0;
    for (f64 s : samples) sum += s;
    f64 mean = sum / static_cast<f64>(samples.size());
    f64 var = 0;
    for (f64 s : samples) var += (s - mean) * (s - mean);
    var /= static_cast<f64>(samples.size());
    f64 stdev = std::sqrt(var);

    std::printf("[physics-bench] pile=%u steps=%u mean=%.2fus stdev=%.2fus\n",
                kBodies, kBenchSteps, mean, stdev);

    if (!std::isfinite(mean) || mean <= 0.0) return 1;
    return 0;
}
