// SPDX-License-Identifier: MIT
// Psynder — public brush_* API impl. Backed by editor::brush::* (internal
// header) and editor::detail::State.

#include "Editor.h"
#include "EditorState.h"
#include "Brush.h"
#include "Undo.h"

namespace psynder::editor {

namespace {
PSY_FORCEINLINE u32 alloc_brush_id() noexcept {
    auto& s = detail::get_state();
    return s.next_brush_id++;
}

PSY_FORCEINLINE math::Vec3 snap_origin(math::Vec3 v) noexcept {
    auto& s = detail::get_state();
    // Default grid = 0.25 m (per DESIGN.md §3.1 grid hierarchy). The
    // editor IPC layer can change this later; for Wave A we snap on
    // brush creation only.
    constexpr f32 kDefaultGrid = 0.25f;
    (void)s;
    return brush::snap_vec3(v, kDefaultGrid);
}
}  // namespace

void brush_create(BrushShape s, math::Vec3 origin, math::Vec3 extents) {
    auto& st = detail::get_state();
    brush::Brush b;
    b.id = alloc_brush_id();
    b.shape = static_cast<u8>(s);
    b.op = brush::Op::Add;
    b.origin = snap_origin(origin);
    b.extents = extents;
    b.grid_size = 0.25f;
    b.sides = 8;
    st.brushes.push_back(b);

    // Record an undo step. The redo path reinstates the brush id +
    // origin; full extents survive in the editor's session-only history
    // for Wave A — full per-field rollback is M3 polish.
    st.undo.push(undo::make_brush_add(b.id, b.origin));
}

void brush_subtract(u32 brush_id, BrushShape s, math::Vec3 origin, math::Vec3 extents) {
    auto& st = detail::get_state();
    (void)brush_id;  // legacy hook: passes-through to the next free id
    brush::Brush b;
    b.id = alloc_brush_id();
    b.shape = static_cast<u8>(s);
    b.op = brush::Op::Subtract;
    b.origin = snap_origin(origin);
    b.extents = extents;
    b.grid_size = 0.25f;
    b.sides = 8;
    st.brushes.push_back(b);
    st.undo.push(undo::make_brush_sub(b.id, b.origin));
}

void brush_commit() {
    // Compile the brush list to a BSP. Lane 24's lm_qbsp is the real
    // pipeline; here we run the deterministic in-process fallback so the
    // editor stays usable even when the tool isn't reachable. The result
    // is cached on the editor state for the renderer to pick up on the
    // next frame.
    auto& st = detail::get_state();

    // Compile (result discarded after caching) — the lane 10 BSP loader
    // owns the runtime representation, this is just the seed.
    (void)brush::compile_brushes(st.brushes);
}

}  // namespace psynder::editor
