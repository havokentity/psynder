// SPDX-License-Identifier: MIT
// Psynder — Lane 15 Wave B unit tests. Covers:
//   1. `set_repl_backend` — installing a custom backend redirects
//      `dispatch_repl` calls (simulating lane 19's WS console path).
//   2. `world:spawn` — the new Lua binding allocates a real engine entity
//      and returns a non-zero integer handle that `scene::EcsRegistry::alive`
//      confirms is live.

#include "script/Script.h"
#include "script/internal/ReplHook.h"

#include "core/Types.h"
#include "scene/EcsRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

namespace {

class VmFixture {
   public:
    VmFixture() { REQUIRE(psynder::script::Vm::Get().start()); }
    ~VmFixture() {
        // Restore the default backend so cross-test state does not leak.
        psynder::script::set_repl_backend(nullptr);
        psynder::script::Vm::Get().shutdown();
    }
};

// Module-scope state poked by the test backend below.
std::atomic<int> g_fake_hits{0};
std::string g_fake_last_line;

bool fake_backend(std::string_view line, std::string& out) noexcept {
    g_fake_hits.fetch_add(1, std::memory_order_relaxed);
    g_fake_last_line.assign(line);
    out.assign("ok-from-fake");
    return true;
}

}  // namespace

TEST_CASE("script: set_repl_backend redirects dispatch_repl", "[script][hook][wave-b]") {
    VmFixture fix;

    // The default backend forwards to Vm::execute_repl. Sanity-check that
    // it works first so we know the test isn't a false positive from a
    // broken Vm.
    {
        std::string out;
        REQUIRE(psynder::script::dispatch_repl("return 2 + 3", out));
        REQUIRE(out == "5");
    }

    // Install the fake backend. Now `dispatch_repl` should bypass the Vm
    // entirely — this is the lane-19 WS handler path: a test or remote
    // console can swap in custom evaluators without rebuilding the engine.
    g_fake_hits = 0;
    g_fake_last_line.clear();
    psynder::script::set_repl_backend(&fake_backend);
    REQUIRE(psynder::script::repl_backend() == &fake_backend);

    std::string out;
    REQUIRE(psynder::script::dispatch_repl("print('hello')", out));
    REQUIRE(out == "ok-from-fake");
    REQUIRE(g_fake_hits.load() == 1);
    REQUIRE(g_fake_last_line == "print('hello')");

    // Fire twice to confirm the hook is sticky, not a one-shot.
    REQUIRE(psynder::script::dispatch_repl("anything", out));
    REQUIRE(g_fake_hits.load() == 2);

    // nullptr restores the default — confirm by evaluating something the
    // fake backend would have returned "ok-from-fake" for.
    psynder::script::set_repl_backend(nullptr);
    REQUIRE(psynder::script::dispatch_repl("return 10 * 4", out));
    REQUIRE(out == "40");
    // The fake backend was NOT called for the third dispatch.
    REQUIRE(g_fake_hits.load() == 2);
}

TEST_CASE("script: world:spawn returns a valid engine entity handle", "[script][spawn][wave-b]") {
    VmFixture fix;

    auto& vm = psynder::script::Vm::Get();

    // Run a tiny Lua chunk that spawns one entity and stashes the handle
    // in a global so the REPL can read it back out.
    REQUIRE(vm.execute_string("spawned = world:spawn('Prop', { Position = { x=1, y=2, z=3 } })",
                              "spawn-call"));

    std::string out;
    REQUIRE(vm.execute_repl("type(spawned)", out));
    REQUIRE(out == "number");

    REQUIRE(vm.execute_repl("spawned", out));
    REQUIRE_FALSE(out.empty());

    // Round-trip through `std::stoul` to recover the Entity::raw value;
    // confirm `scene::EcsRegistry::alive` agrees.
    const unsigned long raw_ul = std::stoul(out);
    psynder::Entity e;
    e.raw = static_cast<psynder::u32>(raw_ul);
    REQUIRE(e.valid());
    REQUIRE(psynder::scene::EcsRegistry::Get().alive(e));

    // Spawning a second entity returns a different raw handle — the shared
    // `scene::EcsRegistry` must hand out unique ids.
    REQUIRE(vm.execute_string("spawned2 = world:spawn('Prop', { Position = { x=4, y=5, z=6 } })",
                              "spawn-call-2"));
    REQUIRE(vm.execute_repl("spawned2", out));
    const unsigned long raw_ul2 = std::stoul(out);
    REQUIRE(raw_ul2 != raw_ul);

    // The DOTS storage table should now have a Position array with two
    // entries (one per spawn). Verifies the component bag plumbing works
    // end-to-end with `world:spawn`, not just `create_entity`.
    REQUIRE(
        vm.execute_repl("#debug.getregistry()['psynder.script.components']"
                        "  [world:component('Position')]",
                        out));
    REQUIRE(out == "2");
}
