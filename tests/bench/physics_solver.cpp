// SPDX-License-Identifier: MIT
// Psynder physics bench — per-island solver cost + ADR-013 colored-parallel
// speedup (Wave-A bar §3.4, DESIGN.md §16 ADR-013).
//
// Two workloads:
//   (1) Whole-step pile of spheres (the original regression gate): one growing
//       contact island stepped through World::step, mean/stdev us.
//   (2) A LARGE single island (hundreds–thousands of contacts) solved directly
//       through solve_island_colored with a SERIAL dispatcher vs a parallel_for
//       dispatcher, reporting the multicore speedup. The two dispatchers MUST
//       produce bit-identical body state (disjoint-body colouring => order-free
//       within a colour); the bench asserts that and prints the speedup.
//
// CI gate: regress workload (1) by > 2% and the PR must justify in description
// or fail (Wave-A bar). Workload (2) is informational (the headline number) and
// also a correctness check (serial == parallel bit-for-bit).

#include "physics/Physics.h"
#include "physics/WorldImpl.h"
#include "physics/Solver.h"
#include "physics/FpControl.h"
#include "physics/internal/Kernels.h"
#include "jobs/JobSystem.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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

// ── Workload (2): a large single island for the colored-parallel speedup ──

using physics::detail::Body;
using physics::detail::Contact;
using physics::detail::Island;
using physics::detail::SolverParams;
namespace kern = physics::detail::kernels;

struct BigIsland {
    std::vector<Body> bodies;     // [0] = static floor, rest dynamic boxes
    std::vector<Contact> contacts;
    std::vector<u32> body_idx;
    Island island;
};

// `grid x grid` dynamic boxes resting on one big static floor. Every dynamic
// box has one contact with the floor (shares only the STATIC body with its
// neighbours, so the colouring keeps it to a couple of colours -> highly
// parallel) plus contacts with its +x / +z neighbours (these DO share dynamic
// bodies, exercising the real colouring). One island, grid*grid+1 bodies.
BigIsland make_big_island(u32 grid) {
    BigIsland s;
    Body floor{};
    floor.position = {0, -0.5f, 0};
    floor.shape = 2;
    floor.half_extent = {1000, 0.5f, 1000};
    floor.rotation = {0, 0, 0, 1};
    floor.mass = 0.0f;
    floor.inv_mass = 0.0f;
    floor.inertia.inv_local = {0, 0, 0};
    floor.friction = 0.8f;
    floor.restitution = 0.0f;
    s.bodies.push_back(floor);

    auto box_at = [&](f32 x, f32 z) {
        Body b{};
        b.position = {x, 0.45f, z};  // slight overlap with floor top (y=0)
        b.rotation = {0, 0, 0, 1};
        b.shape = 2;
        b.half_extent = {0.5f, 0.5f, 0.5f};
        b.mass = 1.0f;
        b.inv_mass = 1.0f;
        f32 I = (1.0f / 6.0f) * 1.0f;  // unit cube, side 1
        b.inertia.local = {I, I, I};
        b.inertia.inv_local = {1.0f / I, 1.0f / I, 1.0f / I};
        b.friction = 0.6f;
        b.restitution = 0.0f;
        s.bodies.push_back(b);
        return static_cast<u32>(s.bodies.size() - 1);
    };

    std::vector<std::vector<u32>> id(grid, std::vector<u32>(grid));
    for (u32 ix = 0; ix < grid; ++ix)
        for (u32 iz = 0; iz < grid; ++iz)
            id[ix][iz] = box_at(static_cast<f32>(ix) * 0.98f, static_cast<f32>(iz) * 0.98f);

    auto add_contact = [&](u32 a, u32 b, math::Vec3 n, math::Vec3 p, f32 depth) {
        Contact c{};
        c.body_a = a;
        c.body_b = b;
        c.normal_world = n;
        c.point_world = p;
        c.depth = depth;
        c.speculative = false;
        s.contacts.push_back(c);
    };

    // Box-floor contacts (share only the static floor).
    for (u32 ix = 0; ix < grid; ++ix) {
        for (u32 iz = 0; iz < grid; ++iz) {
            u32 b = id[ix][iz];
            add_contact(0u, b, {0, 1, 0}, s.bodies[b].position, 0.05f);
        }
    }
    // Box-box neighbour contacts in +x and +z (share DYNAMIC bodies).
    for (u32 ix = 0; ix < grid; ++ix) {
        for (u32 iz = 0; iz < grid; ++iz) {
            if (ix + 1 < grid)
                add_contact(id[ix][iz], id[ix + 1][iz], {1, 0, 0},
                            s.bodies[id[ix][iz]].position, 0.02f);
            if (iz + 1 < grid)
                add_contact(id[ix][iz], id[ix][iz + 1], {0, 0, 1},
                            s.bodies[id[ix][iz]].position, 0.02f);
        }
    }

    s.body_idx.clear();
    for (u32 i = 0; i < s.bodies.size(); ++i)
        s.body_idx.push_back(i);
    s.island.first_contact = 0;
    s.island.contact_count = static_cast<u32>(s.contacts.size());
    s.island.first_body = 0;
    s.island.body_count = static_cast<u32>(s.body_idx.size());
    return s;
}

// Reset per-step dynamic state so each timed solve starts from the same input
// (warm-start accumulators + velocities zeroed, a falling velocity seeded).
void reset_dynamics(BigIsland& s) {
    for (usize i = 1; i < s.bodies.size(); ++i) {
        s.bodies[i].linear_velocity = {0.0f, -1.0f, 0.0f};
        s.bodies[i].angular_velocity = {0, 0, 0};
    }
    for (Contact& c : s.contacts) {
        c.normal_impulse_acc = 0.0f;
        c.friction_impulse_acc1 = 0.0f;
        c.friction_impulse_acc2 = 0.0f;
    }
}

void solve_big(BigIsland& s, const SolverParams& params,
               kern::ColoredIslandScratch& scratch, bool parallel) {
    kern::ColorBatchDispatch disp;
    if (parallel) {
        // Mirror World.cpp::run_island_solve's production dispatcher exactly so
        // the reported speedup reflects the real path (large grain, tiny colours
        // inline).
        disp = [](usize count, const std::function<void(usize, usize)>& fn) {
            if (count < kern::kColoredColorMinParallel) {
                fn(0, count);
                return;
            }
            jobs::JobSystem::Get().parallel_for(0, count, kern::kColoredParallelGrain, fn);
        };
    } else {
        disp = kern::solver_serial_dispatch;
    }
    physics::detail::solve_island_colored(
        s.island,
        {s.contacts.data(), s.contacts.size()},
        {s.body_idx.data(), s.body_idx.size()},
        {s.bodies.data(), s.bodies.size()},
        params, 1.0f / 120.0f, scratch, disp);
}

struct Stats {
    f64 mean = 0, stdev = 0;
};
Stats summarize(const std::vector<f64>& xs) {
    f64 sum = 0;
    for (f64 x : xs)
        sum += x;
    f64 mean = sum / static_cast<f64>(xs.size());
    f64 var = 0;
    for (f64 x : xs)
        var += (x - mean) * (x - mean);
    var /= static_cast<f64>(xs.size());
    return {mean, std::sqrt(var)};
}

// Returns 0 on success (serial==parallel bit-for-bit), 1 on mismatch.
int bench_big_island(bool smoke) {
    physics::detail::FpGuard fp;
    // grid x grid dynamic boxes on one static floor -> ~3*grid^2 contacts in a
    // single island. 100 -> ~29800 contacts, comfortably past the release
    // parallel break-even; 16 -> ~736 for a quick smoke.
    const u32 grid = smoke ? 16u : 100u;
    const u32 iters = smoke ? 30u : 120u;

    BigIsland tmpl = make_big_island(grid);
    const u32 contacts = tmpl.island.contact_count;
    SolverParams params;

    kern::ColoredIslandScratch scratch_s, scratch_p;

    // Warm both scratch pools + caches.
    {
        BigIsland s = tmpl;
        reset_dynamics(s);
        solve_big(s, params, scratch_s, false);
        BigIsland p = tmpl;
        reset_dynamics(p);
        solve_big(p, params, scratch_p, true);
    }

    // Correctness: serial vs parallel must agree to the last bit.
    {
        BigIsland s = tmpl;
        reset_dynamics(s);
        solve_big(s, params, scratch_s, false);
        BigIsland p = tmpl;
        reset_dynamics(p);
        solve_big(p, params, scratch_p, true);
        for (usize i = 0; i < s.bodies.size(); ++i) {
            if (std::memcmp(&s.bodies[i].linear_velocity, &p.bodies[i].linear_velocity,
                            sizeof(math::Vec3)) != 0 ||
                std::memcmp(&s.bodies[i].angular_velocity, &p.bodies[i].angular_velocity,
                            sizeof(math::Vec3)) != 0 ||
                std::memcmp(&s.bodies[i].position, &p.bodies[i].position, sizeof(math::Vec3)) != 0) {
                std::printf("[physics-bench] FAIL serial!=parallel at body %zu\n", i);
                return 1;
            }
        }
    }

    // Time all SERIAL iterations, then all PARALLEL iterations (no
    // interleaving, single reusable working copy reset each iter), so neither
    // path pollutes the other's caches / scheduler state. Report both mean and
    // min (min = the achievable best, least contaminated by OS scheduling).
    auto time_path = [&](kern::ColoredIslandScratch& sc, bool par, std::vector<f64>& out) {
        BigIsland w = tmpl;  // one copy reused across iters
        for (u32 it = 0; it < iters; ++it) {
            reset_dynamics(w);
            // Restore positions too (solve writes them) so every iter is equal.
            for (usize i = 0; i < w.bodies.size(); ++i)
                w.bodies[i].position = tmpl.bodies[i].position;
            auto t0 = std::chrono::steady_clock::now();
            solve_big(w, params, sc, par);
            auto t1 = std::chrono::steady_clock::now();
            out.push_back(std::chrono::duration<f64, std::micro>(t1 - t0).count());
        }
    };
    std::vector<f64> serial_us, parallel_us;
    serial_us.reserve(iters);
    parallel_us.reserve(iters);
    time_path(scratch_s, false, serial_us);
    time_path(scratch_p, true, parallel_us);

    auto minv = [](const std::vector<f64>& xs) {
        f64 m = xs[0];
        for (f64 x : xs)
            if (x < m)
                m = x;
        return m;
    };
    Stats ss = summarize(serial_us);
    Stats sp = summarize(parallel_us);
    f64 smin = minv(serial_us), pmin = minv(parallel_us);
    f64 speedup_mean = (sp.mean > 0.0) ? ss.mean / sp.mean : 0.0;
    f64 speedup_min = (pmin > 0.0) ? smin / pmin : 0.0;
    std::printf(
        "[physics-bench] big-island grid=%u contacts=%u bodies=%zu workers=%u\n",
        grid, contacts, tmpl.bodies.size(), jobs::JobSystem::Get().worker_count());
    std::printf("[physics-bench]   serial-colored   mean=%.2fus stdev=%.2fus min=%.2fus\n",
                ss.mean, ss.stdev, smin);
    std::printf("[physics-bench]   parallel-colored mean=%.2fus stdev=%.2fus min=%.2fus\n",
                sp.mean, sp.stdev, pmin);
    std::printf("[physics-bench]   speedup mean=%.2fx min=%.2fx (serial==parallel bit-for-bit OK)\n",
                speedup_mean, speedup_min);
    return 0;
}

}  // anonymous namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0)
            smoke = true;
    }

    // Start the job pool so the parallel dispatcher actually spreads colours
    // across cores. parallel_for falls back to synchronous if this is skipped,
    // so the bench still RUNS without a pool — it just reports ~1x speedup.
    jobs::JobSystem::Get().start();

    // ── Workload (1): whole-step pile (regression gate) ──
    const u32 kBodies = smoke ? 16u : 64u;
    const u32 kWarmSteps = smoke ? 5u : 60u;
    const u32 kBenchSteps = smoke ? 10u : 240u;

    reset_world();
    build_pile(kBodies);
    for (u32 i = 0; i < kWarmSteps; ++i)
        physics::World::Get().step(1.0f / 120.0f);

    std::vector<f64> samples;
    samples.reserve(kBenchSteps);
    for (u32 i = 0; i < kBenchSteps; ++i)
        samples.push_back(bench_one_step());

    Stats st = summarize(samples);
    std::printf("[physics-bench] pile=%u steps=%u mean=%.2fus stdev=%.2fus\n",
                kBodies, kBenchSteps, st.mean, st.stdev);

    // ── Workload (2): colored-parallel large island ──
    int big_rc = bench_big_island(smoke);

    jobs::JobSystem::Get().stop();

    if (!std::isfinite(st.mean) || st.mean <= 0.0)
        return 1;
    return big_rc;
}
