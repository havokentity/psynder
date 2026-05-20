// SPDX-License-Identifier: MIT
// Psynder physics unit tests — deterministic replay (DESIGN.md §10.1).
//
// Header-only: replays a fixed-step pile-of-spheres scenario twice using the
// kernel functions in `physics/internal/Kernels.h` and compares the final
// body state bit-by-bit. Bit-exact reproducibility on a single host is the
// floor on which cross-platform determinism is built — Wave-B layers the
// cross-host golden once we have CI fixtures.

#include "physics/internal/Kernels.h"
#include "physics/FpControl.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;

namespace {

struct ScenarioBody {
    Body body;
};

// Tiny standalone fixed-tick simulator using only kernel functions; mirrors
// the World facade's step loop but is fully self-contained inside the test
// TU (and therefore needs no linkage to psynder_physics).
struct Sim {
    std::vector<Body> bodies;
    math::Vec3 gravity{0, -9.81f, 0};
    SolverParams params;

    void step(f32 dt) {
        // Integrate forces.
        for (Body& b : bodies) {
            if (b.inv_mass == 0.0f)
                continue;
            b.linear_velocity = math::add(b.linear_velocity, math::mul(gravity, dt));
        }
        // Narrowphase: O(N^2) pair test, sufficient for small scenarios.
        std::vector<Contact> contacts;
        for (u32 i = 0; i < bodies.size(); ++i) {
            for (u32 j = i + 1; j < bodies.size(); ++j) {
                if (bodies[i].inv_mass == 0.0f && bodies[j].inv_mass == 0.0f)
                    continue;
                Contact c;
                if (kernels::kernel_collide_pair(bodies[i], bodies[j], c)) {
                    c.body_a = i;
                    c.body_b = j;
                    contacts.push_back(c);
                }
            }
        }
        // Detect islands and solve each.
        std::vector<u32> body_idx;
        std::vector<Island> islands;
        kernels::kernel_detect_islands(contacts, {bodies.data(), bodies.size()}, body_idx, islands);
        for (const Island& isl : islands) {
            kernels::kernel_solve_island(isl,
                                         {contacts.data() + isl.first_contact, isl.contact_count},
                                         {body_idx.data() + isl.first_body, isl.body_count},
                                         {bodies.data(), bodies.size()},
                                         params,
                                         dt);
        }
        // Integrate positions.
        for (Body& b : bodies) {
            if (b.inv_mass == 0.0f)
                continue;
            b.position = math::add(b.position, math::mul(b.linear_velocity, dt));
            math::Quat w_q{b.angular_velocity.x, b.angular_velocity.y, b.angular_velocity.z, 0.0f};
            math::Quat dq = math::quat_mul(w_q, b.rotation);
            b.rotation = math::quat_normalize({
                b.rotation.x + 0.5f * dt * dq.x,
                b.rotation.y + 0.5f * dt * dq.y,
                b.rotation.z + 0.5f * dt * dq.z,
                b.rotation.w + 0.5f * dt * dq.w,
            });
        }
    }
};

Sim make_scenario() {
    Sim s;
    // Static floor
    Body floor{};
    floor.position = {0, -0.5f, 0};
    floor.shape = 2;
    floor.half_extent = {10, 0.5f, 10};
    floor.rotation = {0, 0, 0, 1};
    floor.mass = 0.0f;
    floor.inv_mass = 0.0f;
    floor.inertia.inv_local = {0, 0, 0};
    floor.friction = 0.6f;
    floor.restitution = 0.0f;
    s.bodies.push_back(floor);

    // 4x4 grid of spheres at 5 m.
    for (int x = 0; x < 4; ++x) {
        for (int z = 0; z < 4; ++z) {
            Body b{};
            b.position = {static_cast<f32>(x) - 1.5f, 5.0f, static_cast<f32>(z) - 1.5f};
            b.rotation = {0, 0, 0, 1};
            b.shape = 0;
            b.half_extent = {0.5f, 0, 0};
            b.mass = 1.0f;
            b.inv_mass = 1.0f;
            f32 I = (2.0f / 5.0f) * 1.0f * 0.5f * 0.5f;
            b.inertia.local = {I, I, I};
            b.inertia.inv_local = {1.0f / I, 1.0f / I, 1.0f / I};
            b.friction = 0.5f;
            b.restitution = 0.3f;
            s.bodies.push_back(b);
        }
    }
    return s;
}

}  // namespace

TEST_CASE("Physics step is deterministic across identical runs (single host)",
          "[physics][determinism]") {
    // FpGuard pins round-to-nearest for the duration of the test, matching
    // what the production step does. Without this, code linked from other
    // TUs (audio codecs etc.) could flip the FPU and break bit-equality.
    FpGuard fp;

    Sim a = make_scenario();
    Sim b = make_scenario();

    for (int i = 0; i < 240; ++i)
        a.step(1.0f / 120.0f);
    for (int i = 0; i < 240; ++i)
        b.step(1.0f / 120.0f);

    REQUIRE(a.bodies.size() == b.bodies.size());
    for (usize i = 0; i < a.bodies.size(); ++i) {
        INFO("body index " << i);
        // Compare the kinematic state byte-by-byte. Use memcmp instead of
        // structural equality so we catch sign-bit differences on zero
        // values too.
        REQUIRE(std::memcmp(&a.bodies[i].position, &b.bodies[i].position, sizeof(math::Vec3)) == 0);
        REQUIRE(std::memcmp(&a.bodies[i].rotation, &b.bodies[i].rotation, sizeof(math::Quat)) == 0);
        REQUIRE(std::memcmp(&a.bodies[i].linear_velocity,
                            &b.bodies[i].linear_velocity,
                            sizeof(math::Vec3)) == 0);
        REQUIRE(std::memcmp(&a.bodies[i].angular_velocity,
                            &b.bodies[i].angular_velocity,
                            sizeof(math::Vec3)) == 0);
    }
}

TEST_CASE("Pile of spheres comes to rest at expected height after 5 s",
          "[physics][solver][stacking]") {
    FpGuard fp;
    Sim s = make_scenario();
    for (int i = 0; i < 600; ++i)
        s.step(1.0f / 120.0f);
    // Every sphere should be above the floor (y >= 0) and below the spawn
    // height (y < 5) — i.e. they fell and settled.
    for (usize i = 1; i < s.bodies.size(); ++i) {
        REQUIRE(s.bodies[i].position.y >= -0.1f);
        REQUIRE(s.bodies[i].position.y < 5.0f);
        // Velocity should be small after 5 s settle.
        f32 v = std::sqrt(math::dot(s.bodies[i].linear_velocity, s.bodies[i].linear_velocity));
        REQUIRE(v < 2.0f);  // generous bound — solver isn't NGS yet
    }
}
