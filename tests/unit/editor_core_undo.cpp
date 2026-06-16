// SPDX-License-Identifier: MIT
// Psynder — Lane 18 unit test: undo of a single entity move.
//
// The tests include the lane's internal Undo.h header (template-only) so
// the test binary doesn't need to link psynder_editor_core. The full,
// stateful version of the same scenario goes through the editor::ops
// dispatch and is covered by integration tests once lane 25 wires
// psynder_editor_core into tests/unit/CMakeLists.txt.

#include "editor/core/Undo.h"
#include "editor/core/CommandHistory.h"
#include "scene/SceneGraph.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::editor;

namespace {
// Minimal stand-in for the editor's entity table — just enough to test
// that undo of a single move restores the pre-move position. The real
// editor uses EditorState::EntityRec.
struct MiniEntity {
    u32 id = 0;
    math::Vec3 position{0, 0, 0};
};

void apply_move_delta(MiniEntity& e, const undo::Delta& d, bool reverse) noexcept {
    if (d.op != undo::Op::EntityMove)
        return;
    if (d.target_id != e.id)
        return;
    e.position = reverse ? d.before : d.after;
}
}  // namespace

TEST_CASE("undo: single move restores the pre-move position", "[editor][undo]") {
    MiniEntity e;
    e.id = 42;
    e.position = math::Vec3{1.0f, 2.0f, 3.0f};

    undo::Stack stack;
    const math::Vec3 before = e.position;
    const math::Vec3 after = math::Vec3{10.0f, 20.0f, 30.0f};

    // Perform the move + record the delta.
    e.position = after;
    stack.push(undo::make_move(e.id, before, after));

    REQUIRE(stack.size() == 1);
    REQUIRE(stack.redo_size() == 0);
    REQUIRE(e.position.x == 10.0f);
    REQUIRE(e.position.y == 20.0f);
    REQUIRE(e.position.z == 30.0f);

    // Undo: the stack pops the delta and we reverse-apply it.
    undo::Delta d;
    REQUIRE(stack.undo(d));
    apply_move_delta(e, d, /*reverse=*/true);

    REQUIRE(e.position.x == before.x);
    REQUIRE(e.position.y == before.y);
    REQUIRE(e.position.z == before.z);
    REQUIRE(stack.size() == 0);
    REQUIRE(stack.redo_size() == 1);
}

TEST_CASE("undo: redo replays the previously-undone move", "[editor][undo]") {
    MiniEntity e;
    e.id = 7;
    e.position = math::Vec3{0, 0, 0};

    undo::Stack stack;
    const math::Vec3 after = math::Vec3{5.0f, 0.0f, -5.0f};
    e.position = after;
    stack.push(undo::make_move(e.id, math::Vec3{0, 0, 0}, after));

    undo::Delta d;
    REQUIRE(stack.undo(d));
    apply_move_delta(e, d, true);
    REQUIRE(e.position.x == 0.0f);

    REQUIRE(stack.redo(d));
    apply_move_delta(e, d, false);
    REQUIRE(e.position.x == 5.0f);
    REQUIRE(e.position.z == -5.0f);
    REQUIRE(stack.size() == 1);
    REQUIRE(stack.redo_size() == 0);
}

TEST_CASE("undo: pushing a new delta clears the redo stack", "[editor][undo]") {
    undo::Stack stack;

    stack.push(undo::make_move(1, math::Vec3{0, 0, 0}, math::Vec3{1, 0, 0}));
    stack.push(undo::make_move(1, math::Vec3{1, 0, 0}, math::Vec3{2, 0, 0}));
    REQUIRE(stack.size() == 2);

    undo::Delta d;
    REQUIRE(stack.undo(d));
    REQUIRE(stack.size() == 1);
    REQUIRE(stack.redo_size() == 1);

    // New action discards the available redo.
    stack.push(undo::make_move(1, math::Vec3{1, 0, 0}, math::Vec3{9, 0, 0}));
    REQUIRE(stack.size() == 2);
    REQUIRE(stack.redo_size() == 0);

    // No more redos available.
    REQUIRE_FALSE(stack.redo(d));
}

TEST_CASE("undo: delta is exactly 32 bytes (O(1) push invariant)", "[editor][undo]") {
    STATIC_REQUIRE(sizeof(undo::Delta) == 32);
}

TEST_CASE("undo: transform callback restores and reapplies a gizmo move",
          "[editor][undo][gizmo]") {
    using editor::command_history::Command;
    using editor::command_history::History;

    scene::LocalTransform transform{};
    transform.translation = {0.0f, 1.0f, 2.0f};
    const scene::LocalTransform before = transform;
    scene::LocalTransform after = before;
    after.translation.x = 4.5f;
    after.translation.z = -3.25f;

    History history{8u};
    transform = after;
    history.push(Command::callback(
        "move entity",
        [&transform, before]() { transform = before; },
        [&transform, after]() { transform = after; }));

    Command command{};
    REQUIRE(history.undo(command));
    REQUIRE(transform.translation.x == before.translation.x);
    REQUIRE(transform.translation.y == before.translation.y);
    REQUIRE(transform.translation.z == before.translation.z);

    REQUIRE(history.redo(command));
    REQUIRE(transform.translation.x == after.translation.x);
    REQUIRE(transform.translation.y == after.translation.y);
    REQUIRE(transform.translation.z == after.translation.z);
}
