// SPDX-License-Identifier: MIT
// Psynder — Lane 15 unit tests. Covers:
//   1. Vm lifecycle (start / shutdown).
//   2. DOTS-shaped system registration via `world:register_system(...)`.
//   3. Running registered systems over a few entities — verifies the
//      callback receives component arrays, not per-entity objects.
//   4. REPL roundtrip (expression eval + statement) via `Vm::execute_repl`.
//   5. The DOTS contract negatively — no entity userdata is exposed.

#include "script/Script.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

class VmFixture {
   public:
    VmFixture() { REQUIRE(psynder::script::Vm::Get().start()); }
    ~VmFixture() { psynder::script::Vm::Get().shutdown(); }

    psynder::script::Vm& vm() { return psynder::script::Vm::Get(); }
};

}  // namespace

TEST_CASE("script: Vm starts and shuts down cleanly", "[script][vm]") {
    auto& vm = psynder::script::Vm::Get();
    REQUIRE(vm.start());
    // Idempotent.
    REQUIRE(vm.start());
    vm.shutdown();
    // Restart after shutdown must succeed.
    REQUIRE(vm.start());
    vm.shutdown();
}

TEST_CASE("script: REPL evaluates expressions and statements", "[script][repl]") {
    VmFixture fix;

    std::string out;

    REQUIRE(fix.vm().execute_repl("1 + 2", out));
    REQUIRE(out == "3");

    REQUIRE(fix.vm().execute_repl("'foo' .. 'bar'", out));
    REQUIRE(out == "foobar");

    // Multiple return values.
    REQUIRE(fix.vm().execute_repl("1, 2, 'three'", out));
    REQUIRE(out == "1\t2\tthree");

    // Statement form: assignment has no return value.
    REQUIRE(fix.vm().execute_repl("x = 42", out));
    REQUIRE(out.empty());

    // Subsequent expression sees the assignment.
    REQUIRE(fix.vm().execute_repl("x * 2", out));
    REQUIRE(out == "84");

    // Syntax error is reported, not silently swallowed.
    REQUIRE_FALSE(fix.vm().execute_repl("if then end", out));
    REQUIRE_FALSE(out.empty());

    // Runtime error is reported.
    REQUIRE_FALSE(fix.vm().execute_repl("error('boom')", out));
    REQUIRE(out.find("boom") != std::string::npos);
}

TEST_CASE("script: execute_string runs a chunk", "[script][exec]") {
    VmFixture fix;

    REQUIRE(fix.vm().execute_string("result = 7 * 6", "test-chunk"));

    std::string out;
    REQUIRE(fix.vm().execute_repl("result", out));
    REQUIRE(out == "42");
}

TEST_CASE("script: DOTS — register_system over component arrays", "[script][dots]") {
    VmFixture fix;

    // The system callback receives whole component arrays as positional
    // arguments — exactly the §3.3 contract. Notice there is NO per-entity
    // userdata anywhere in this script. The function loops over the array
    // and pokes the `data` table of each entry. Engine-side the schedule
    // is built from the declared reads/writes set.
    const char* lua = R"(
        world:component('Position')
        world:component('Velocity')

        world:create_entity({
            Position = { x = 0.0, y = 0.0, z = 0.0 },
            Velocity = { x = 1.0, y = 0.0, z = 0.0 },
        })
        world:create_entity({
            Position = { x = 10.0, y = 5.0, z = 0.0 },
            Velocity = { x = 0.0, y = 2.0, z = 0.0 },
        })
        world:create_entity({
            Position = { x = -3.0, y = 0.0, z = 7.0 },
            Velocity = { x = 0.5, y = 0.5, z = 0.5 },
        })

        integrate_count = 0

        world:register_system(
            { reads = {'Velocity'}, writes = {'Position'},
              name = 'integrate_motion' },
            function(velocities, positions, dt)
                integrate_count = integrate_count + 1
                -- The two arrays line up by index (entity slot). This is
                -- the DOTS hot loop: indexable, branch-free, no virtual.
                for i = 1, #positions do
                    positions[i].data.x = positions[i].data.x
                                        + velocities[i].data.x * dt
                    positions[i].data.y = positions[i].data.y
                                        + velocities[i].data.y * dt
                    positions[i].data.z = positions[i].data.z
                                        + velocities[i].data.z * dt
                end
            end)

        -- Tick twice with dt = 0.5
        world:run_systems(0.5)
        world:run_systems(0.5)
    )";

    REQUIRE(fix.vm().execute_string(lua, "dots-system"));

    std::string out;
    REQUIRE(fix.vm().execute_repl("world:system_count()", out));
    REQUIRE(out == "1");

    REQUIRE(fix.vm().execute_repl("integrate_count", out));
    REQUIRE(out == "2");  // called twice

    // Entity 0: x = 0 + 1.0 * 0.5 * 2 = 1.0
    // Fetch entity 0's Position.x via the per-VM storage table.
    REQUIRE(fix.vm().execute_repl(
        "string.format('%.4f', "
        "  (function() "
        "    for _, e in ipairs(debug.getregistry()['psynder.script.components']"
        "                       [world:component('Position')]) do "
        "      if e.entity == 1 then return e.data.x end "
        "    end "
        "    return -999 "
        "  end)())",
        out));
    REQUIRE(out == "1.0000");

    REQUIRE(fix.vm().execute_repl(
        "string.format('%.4f', "
        "  (function() "
        "    for _, e in ipairs(debug.getregistry()['psynder.script.components']"
        "                       [world:component('Position')]) do "
        "      if e.entity == 2 then return e.data.y end "
        "    end "
        "    return -999 "
        "  end)())",
        out));
    // Entity 1: y starts at 5, gains 2 * 0.5 * 2 = 2  ->  7.0000
    REQUIRE(out == "7.0000");
}

TEST_CASE("script: DOTS — no per-entity OOP escape hatch exists", "[script][dots]") {
    VmFixture fix;

    std::string out;

    // There is no `Entity` global.
    REQUIRE(fix.vm().execute_repl("type(Entity)", out));
    REQUIRE(out == "nil");

    // `world` is a plain table of methods — registering systems and
    // creating entities. It has no `:tick`, `:update`, or `:object`.
    REQUIRE(fix.vm().execute_repl("type(world.tick)", out));
    REQUIRE(out == "nil");
    REQUIRE(fix.vm().execute_repl("type(world.update)", out));
    REQUIRE(out == "nil");

    // `world:create_entity` returns a plain integer handle, not userdata
    // with methods. Users cannot reach into it to register a behaviour.
    REQUIRE(
        fix.vm().execute_string("world:component('A')\n"
                                "local e = world:create_entity({A = {v=1}})\n"
                                "stored_handle_type = type(e)\n",
                                "no-oop"));
    REQUIRE(fix.vm().execute_repl("stored_handle_type", out));
    REQUIRE(out == "number");

    // The DOTS contract makes it physically impossible to invoke a method
    // on an entity — the handle is not a userdata, so `:methodcall` syntax
    // raises immediately.
    REQUIRE_FALSE(fix.vm().execute_repl("local e = world:create_entity({A={v=2}}) e:tick()", out));
    REQUIRE_FALSE(out.empty());
}

TEST_CASE("script: register_system validates input shapes", "[script][dots][validation]") {
    VmFixture fix;

    std::string out;

    // Missing function arg: must error.
    REQUIRE_FALSE(fix.vm().execute_repl("world:register_system({reads={'P'}}, nil)", out));

    // Reads must be a table of strings.
    REQUIRE_FALSE(fix.vm().execute_repl("world:register_system({reads='nope'}, function() end)", out));

    // After failures, the registry must still be sane: a valid call
    // succeeds.
    REQUIRE(
        fix.vm().execute_repl("world:register_system({reads={'P'},writes={'V'}}, "
                              "  function() end)",
                              out));
}
