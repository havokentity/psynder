// SPDX-License-Identifier: MIT
// Psynder — RmlUi RenderInterface adapter. Lane 17 owns.
//
// Wave-A: feeds layout boxes into the rasterizer (lane 07) as triangle
// pairs.  Lane 07's `submit` is a stub today; the moment it lights up,
// these triangles compose into the same framebuffer as the game.
//
// When PSYNDER_VENDOR_RMLUI=ON is flipped, this file holds the real
// `Rml::RenderInterface` override that batches RmlUi compiled geometries
// into the same Rasterizer queue.  The interface stays identical so
// designer HUDs swap without a code change.

#include "Rml_internal.h"

#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <vector>

namespace psynder::ui::rml::detail {

namespace {

// Convert a screen-space box into two triangles in clip-space-passthrough
// coordinates.  Lane 07's view path will transform by `model * view * proj`;
// we use an orthographic identity model so positions remain in pixels.
void emit_box(const LayoutBox& box,
              math::Vec2 viewport,
              std::vector<render::raster::Vertex>& verts,
              std::vector<u32>& indices) {
    const u32 base = static_cast<u32>(verts.size());

    // Map pixel coordinates into [-1, 1] clip space (top-left origin).
    auto to_clip = [&](math::Vec2 px) -> math::Vec3 {
        const f32 cx = (px.x / std::max(1.f, viewport.x)) * 2.f - 1.f;
        const f32 cy = 1.f - (px.y / std::max(1.f, viewport.y)) * 2.f;
        return {cx, cy, 0.f};
    };

    const math::Vec2 tl{box.origin.x, box.origin.y};
    const math::Vec2 tr{box.origin.x + box.size.x, box.origin.y};
    const math::Vec2 br{box.origin.x + box.size.x, box.origin.y + box.size.y};
    const math::Vec2 bl{box.origin.x, box.origin.y + box.size.y};

    render::raster::Vertex v{};
    v.normal = math::Vec3{0.f, 0.f, 1.f};
    v.color = box.rgba;
    v.uv = math::Vec2{0.f, 0.f};

    v.position = to_clip(tl);
    verts.push_back(v);
    v.position = to_clip(tr);
    verts.push_back(v);
    v.position = to_clip(br);
    verts.push_back(v);
    v.position = to_clip(bl);
    verts.push_back(v);

    // Two triangles, CCW
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}

}  // namespace

void submit_boxes_to_rasterizer(const std::vector<LayoutBox>& boxes, render::Framebuffer& target) {
    if (boxes.empty())
        return;

    // Build a single DrawItem with all boxes.  Per-frame heap usage is
    // bounded — vectors persist across frames in the caller.
    thread_local std::vector<render::raster::Vertex> verts;
    thread_local std::vector<u32> indices;
    verts.clear();
    indices.clear();

    const math::Vec2 viewport{static_cast<f32>(target.width), static_cast<f32>(target.height)};

    for (const auto& box : boxes)
        emit_box(box, viewport, verts, indices);

    if (verts.empty())
        return;

    render::raster::DrawItem item{};
    item.vertices = verts.data();
    item.vertex_count = static_cast<u32>(verts.size());
    item.indices = indices.data();
    item.index_count = static_cast<u32>(indices.size());

    // Identity model — boxes are already in clip space.
    item.model.m[0] = 1.f;
    item.model.m[5] = 1.f;
    item.model.m[10] = 1.f;
    item.model.m[15] = 1.f;

    // No material id — lane 07 falls back to vertex-colored flat.
    auto& rast = render::raster::Rasterizer::Get();
    rast.submit(item);
}

}  // namespace psynder::ui::rml::detail
