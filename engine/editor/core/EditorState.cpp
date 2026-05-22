// SPDX-License-Identifier: MIT
// Psynder — editor singleton state.

#include "EditorState.h"

namespace psynder::editor::detail {

State& get_state() noexcept {
    // Function-local static gives us the same lazy init as scene::EcsRegistry::Get
    // (lane 06) and physics::World::Get (lane 13). The editor is part of
    // the engine library; this singleton is fenced by PSYNDER_EDITOR at
    // the lane-CMakeLists level (the lane is not added in retail builds).
    static State s;
    return s;
}

EntityRec* find_entity(u32 id) noexcept {
    State& s = get_state();
    for (auto& e : s.entities) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

BodyRec* find_body(u32 id) noexcept {
    State& s = get_state();
    for (auto& b : s.bodies) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

brush::Brush* find_brush(u32 id) noexcept {
    State& s = get_state();
    for (auto& b : s.brushes) {
        if (b.id == id)
            return &b;
    }
    return nullptr;
}

void apply_delta(const undo::Delta& d, bool reverse) noexcept {
    using undo::Op;
    State& s = get_state();
    switch (d.op) {
        case Op::EntityMove: {
            if (auto* e = find_entity(d.target_id)) {
                e->position = reverse ? d.before : d.after;
            }
            break;
        }
        case Op::EntitySpawn: {
            if (auto* e = find_entity(d.target_id)) {
                e->alive = !reverse;
                e->position = d.after;
            }
            break;
        }
        case Op::EntityDelete: {
            if (auto* e = find_entity(d.target_id)) {
                // Reverse-of-delete = respawn at the before-position.
                e->alive = reverse;
                e->position = reverse ? d.before : e->position;
            }
            break;
        }
        case Op::BrushAdd:
        case Op::BrushSub: {
            // Add toggles the brush list membership; undo erases, redo re-adds.
            if (reverse) {
                for (auto it = s.brushes.begin(); it != s.brushes.end(); ++it) {
                    if (it->id == d.target_id) {
                        s.brushes.erase(it);
                        break;
                    }
                }
            } else {
                // Redo path: ensure brush exists. (For an undo-of-add we don't
                // try to reconstruct extents — Wave-A scope: deletion-only.)
                bool present = false;
                for (const auto& b : s.brushes) {
                    if (b.id == d.target_id) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    brush::Brush b;
                    b.id = d.target_id;
                    b.origin = d.after;
                    b.op = (d.op == Op::BrushSub) ? brush::Op::Subtract : brush::Op::Add;
                    s.brushes.push_back(b);
                }
            }
            break;
        }
        case Op::BrushDelete: {
            // Symmetric with BrushAdd: undo = re-insert, redo = remove.
            if (reverse) {
                bool present = false;
                for (const auto& b : s.brushes) {
                    if (b.id == d.target_id) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    brush::Brush b;
                    b.id = d.target_id;
                    b.origin = d.before;
                    s.brushes.push_back(b);
                }
            } else {
                for (auto it = s.brushes.begin(); it != s.brushes.end(); ++it) {
                    if (it->id == d.target_id) {
                        s.brushes.erase(it);
                        break;
                    }
                }
            }
            break;
        }
        case Op::SculptStroke:
        case Op::PhysgunGrab:
        case Op::PhysgunDrop:
        case Op::Constraint:
        case Op::None:
        default:
            // These ops record provenance but their state is reconstructed
            // from the heightfield / physgun / constraint subsystems on
            // load. No-op here keeps the undo stack consistent.
            break;
    }
}

}  // namespace psynder::editor::detail
