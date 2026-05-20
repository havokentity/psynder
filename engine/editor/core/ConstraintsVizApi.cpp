// SPDX-License-Identifier: MIT
// Psynder — wire ConstraintsViz.h into lane 16's ui::imm::line primitive.
// The header-only viz core builds caller-owned ScreenSegment vectors that
// tests can inspect; this TU is the production sink that hands those
// segments to the immediate-mode overlay.

#include "ConstraintsViz.h"
#include "EditorState.h"

#include "ui/imm/Imm.h"

#include <vector>

namespace psynder::editor::viz {

// Render every Wave-B constraint in the live editor graph as a screen-space
// glyph. The caller supplies the camera (the editor stores it in lane 19's
// IPC payload; for the in-process call path the renderer hands its current
// view to us). All edits go through `ui::imm::line` so the overlay is
// composited with the rest of the in-viewport debug UI.
void render_constraints(const Camera& cam) {
    auto& s = detail::get_state();
    if (s.constraint_graph.size() == 0)
        return;

    // Snapshot the body poses for the segment builder. Bodies are flat
    // and small (32-ish per Wave-B contraption); a one-frame copy is
    // perfectly fine and keeps the builder linker-clean.
    std::vector<WorldBodyPose> poses;
    poses.reserve(s.bodies.size());
    for (const auto& b : s.bodies) {
        if (!b.alive)
            continue;
        WorldBodyPose p;
        p.id = b.id;
        p.position = b.position;
        p.rotation = b.rotation;
        poses.push_back(p);
    }

    std::vector<ScreenSegment> lines;
    lines.reserve(s.constraint_graph.size() * 4);
    build_graph_lines(lines, cam, s.constraint_graph, poses);

    for (const ScreenSegment& seg : lines) {
        ui::imm::line(seg.a, seg.b, seg.rgba);
    }
}

}  // namespace psynder::editor::viz
