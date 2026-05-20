// SPDX-License-Identifier: MIT
// Psynder — internal header. Undo / redo stack with delta-encoded steps,
// O(1) per push, O(1) per undo/redo (DESIGN.md §10.8 "Undo / redo").
//
// Header-only so tests/unit/editor_core_undo.cpp can include it without
// linking psynder_editor_core (see PR body for the lane-25 caveat).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace psynder::editor::undo {

// ─── Delta encoding ──────────────────────────────────────────────────────
// Each delta is one of a fixed set of editor operations; storage is a
// 32-byte fixed-size record so push/pop are O(1) and the stack never
// touches the heap once it grows once.
enum class Op : u8 {
    None = 0,
    EntityMove = 1,    // before / after Vec3 position for an entity
    EntitySpawn = 2,   // spawn an entity (undo = despawn)
    EntityDelete = 3,  // delete an entity (undo = respawn at last known state)
    BrushAdd = 4,      // additive brush created
    BrushSub = 5,      // subtractive brush created
    BrushDelete = 6,   // brush removed
    SculptStroke = 7,  // chunk of heightmap sculpt
    PhysgunGrab = 8,   // body pickup (records prior transform)
    PhysgunDrop = 9,   // body drop (records new transform)
    Constraint = 10,   // constraint added / removed
};

struct Delta {
    Op op = Op::None;
    u8 _pad[3]{};
    u32 target_id = 0;           // entity / body / brush id depending on op
    math::Vec3 before{0, 0, 0};  // pre-op state (interpretation depends on op)
    math::Vec3 after{0, 0, 0};   // post-op state
};

static_assert(sizeof(Delta) == 32, "Undo delta must stay packed at 32 bytes for O(1) push");

// ─── Stack ────────────────────────────────────────────────────────────────
// Two ring-style buffers in lockstep: `done` is the undo stack, `redo` is
// the redo stack. Push clears the redo stack. Undo pops from `done` into
// `redo`. Redo pops from `redo` back into `done`. All operations on these
// vectors are amortized O(1) (back-only), which is what DESIGN.md asks for.
class Stack {
   public:
    Stack() = default;

    PSY_FORCEINLINE void push(const Delta& d) {
        done_.push_back(d);
        redo_.clear();
    }

    // Pop one delta from `done` into `redo`; returns it for the caller to
    // re-apply in reverse. False if the undo stack is empty.
    PSY_FORCEINLINE bool undo(Delta& out) {
        if (done_.empty())
            return false;
        out = done_.back();
        done_.pop_back();
        redo_.push_back(out);
        return true;
    }

    // Mirror of undo: pop from `redo` back onto `done`.
    PSY_FORCEINLINE bool redo(Delta& out) {
        if (redo_.empty())
            return false;
        out = redo_.back();
        redo_.pop_back();
        done_.push_back(out);
        return true;
    }

    PSY_FORCEINLINE void clear() noexcept {
        done_.clear();
        redo_.clear();
    }
    PSY_FORCEINLINE usize size() const noexcept { return done_.size(); }
    PSY_FORCEINLINE usize redo_size() const noexcept { return redo_.size(); }

    // Read-only access for tests / inspector.
    const Delta& at(usize i) const { return done_[i]; }

   private:
    std::vector<Delta> done_;
    std::vector<Delta> redo_;
};

// ─── Helpers to construct common deltas ──────────────────────────────────
PSY_FORCEINLINE Delta make_move(u32 entity, math::Vec3 before, math::Vec3 after) noexcept {
    Delta d;
    d.op = Op::EntityMove;
    d.target_id = entity;
    d.before = before;
    d.after = after;
    return d;
}

PSY_FORCEINLINE Delta make_spawn(u32 entity, math::Vec3 position) noexcept {
    Delta d;
    d.op = Op::EntitySpawn;
    d.target_id = entity;
    d.after = position;
    return d;
}

PSY_FORCEINLINE Delta make_delete(u32 entity, math::Vec3 last_position) noexcept {
    Delta d;
    d.op = Op::EntityDelete;
    d.target_id = entity;
    d.before = last_position;
    return d;
}

PSY_FORCEINLINE Delta make_brush_add(u32 brush_id, math::Vec3 origin) noexcept {
    Delta d;
    d.op = Op::BrushAdd;
    d.target_id = brush_id;
    d.after = origin;
    return d;
}

PSY_FORCEINLINE Delta make_brush_sub(u32 brush_id, math::Vec3 origin) noexcept {
    Delta d;
    d.op = Op::BrushSub;
    d.target_id = brush_id;
    d.after = origin;
    return d;
}

}  // namespace psynder::editor::undo
