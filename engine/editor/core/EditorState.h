// SPDX-License-Identifier: MIT
// Psynder — internal header. Editor singleton state.
//
// The editor lives inside the running engine (per DESIGN.md §10.8) and
// owns a single state record: current mode, brush list, sculpt grid,
// physgun, constraints, entity selection set, undo stack. Other lanes
// reach into this through the public Editor.h facade only.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include "editor/core/Brush.h"
#include "editor/core/Constraints.h"
#include "editor/core/Physgun.h"
#include "editor/core/Sculpt.h"
#include "editor/core/Undo.h"

#include <atomic>
#include <vector>

namespace psynder::editor::detail {

// Entity record kept editor-side. Mirrors what gets serialised to .psylevel;
// the engine-side ECS handle (lane 06) is rebuilt on load. We carry one
// per spawned entity so undo can restore them after delete.
struct EntityRec {
    u32 id = 0;  // editor-stable id (separate from ECS handle)
    math::Vec3 position{0, 0, 0};
    math::Quat rotation{0, 0, 0, 1};
    math::Vec3 scale{1, 1, 1};
    u32 prefab_id = 0;  // 0 = empty entity
    bool alive = true;
};

// Body record kept editor-side. Mirrors what physgun sees, what
// contraption serialisation writes, and what constraints reference.
struct BodyRec {
    u32 id = 0;
    math::Vec3 position{0, 0, 0};
    math::Quat rotation{0, 0, 0, 1};
    math::Vec3 scale{1, 1, 1};
    bool frozen = false;
    bool alive = true;
};

struct State {
    // Mode toggle
    std::atomic<u8> mode{0};  // 0=Play, 1=Edit

    // Brush authoring
    std::vector<brush::Brush> brushes;
    u32 next_brush_id = 1;

    // Entities + bodies (editor-stable ids; these are *separate* from
    // ECS / physics handles, which only exist while the engine is live)
    std::vector<EntityRec> entities;
    u32 next_entity_id = 1;
    std::vector<BodyRec> bodies;
    u32 next_body_id = 1;

    // Sculpt
    sculpt::Heightfield heightfield;
    sculpt::SplatGrid splat;

    // Physgun
    physgun::State gun;

    // Constraint graph
    constraints::Graph constraint_graph;

    // Selection (entity ids)
    std::vector<u32> selection;

    // Undo / redo
    undo::Stack undo;
};

// Singleton access (defined in EditorState.cpp).
State& get_state() noexcept;

// Look up / create / mutate helpers ─────────────────────────────────────
EntityRec* find_entity(u32 id) noexcept;
BodyRec* find_body(u32 id) noexcept;
brush::Brush* find_brush(u32 id) noexcept;

// Apply / revert delta (used by undo / redo dispatch).
void apply_delta(const undo::Delta& d, bool reverse) noexcept;

}  // namespace psynder::editor::detail
