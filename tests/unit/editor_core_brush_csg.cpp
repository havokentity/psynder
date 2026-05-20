// SPDX-License-Identifier: MIT
// Psynder — Lane 18 unit test: brush CSG of two boxes produces the
// expected BSP. Header-only inclusion of editor::brush::* avoids the
// need to link psynder_editor_core (see editor_core_undo.cpp for the
// same rationale).

#include "editor/core/Brush.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::editor;

namespace {
constexpr f32 kEps = 1e-4f;

PSY_FORCEINLINE bool approx_eq(f32 a, f32 b) noexcept {
    const f32 d = a - b;
    return d > -kEps && d < kEps;
}
}  // namespace

TEST_CASE("brush: snap_vec3 with zero step is the identity", "[editor][brush]") {
    const math::Vec3 v{1.123f, -2.456f, 3.789f};
    const math::Vec3 r = brush::snap_vec3(v, 0.0f);
    REQUIRE(r.x == v.x);
    REQUIRE(r.y == v.y);
    REQUIRE(r.z == v.z);
}

TEST_CASE("brush: snap_vec3 rounds to the nearest grid step", "[editor][brush]") {
    const math::Vec3 v{1.13f, 2.62f, -3.49f};
    const math::Vec3 r = brush::snap_vec3(v, 0.25f);
    REQUIRE(approx_eq(r.x, 1.25f));
    REQUIRE(approx_eq(r.y, 2.50f));
    REQUIRE(approx_eq(r.z, -3.50f));
}

TEST_CASE("brush: build_box emits 6 outward-facing planes", "[editor][brush]") {
    const auto box = brush::build_box(math::Vec3{0, 0, 0}, math::Vec3{1, 1, 1});
    REQUIRE(box.planes.size() == 6);
    REQUIRE(approx_eq(box.bounds.min.x, -1.0f));
    REQUIRE(approx_eq(box.bounds.max.x, 1.0f));

    // Each plane's normal must agree with one of the six axis-aligned
    // unit vectors and the plane offset must place the box face exactly
    // at +/-1 m along that axis.
    int seen[6] = {0, 0, 0, 0, 0, 0};
    for (const auto& p : box.planes) {
        if (approx_eq(p.n.x, 1.0f)) {
            ++seen[0];
            REQUIRE(approx_eq(p.d, 1.0f));
        }
        if (approx_eq(p.n.x, -1.0f)) {
            ++seen[1];
            REQUIRE(approx_eq(p.d, 1.0f));
        }
        if (approx_eq(p.n.y, 1.0f)) {
            ++seen[2];
            REQUIRE(approx_eq(p.d, 1.0f));
        }
        if (approx_eq(p.n.y, -1.0f)) {
            ++seen[3];
            REQUIRE(approx_eq(p.d, 1.0f));
        }
        if (approx_eq(p.n.z, 1.0f)) {
            ++seen[4];
            REQUIRE(approx_eq(p.d, 1.0f));
        }
        if (approx_eq(p.n.z, -1.0f)) {
            ++seen[5];
            REQUIRE(approx_eq(p.d, 1.0f));
        }
    }
    for (int i = 0; i < 6; ++i)
        REQUIRE(seen[i] == 1);
}

TEST_CASE("brush: build_wedge emits 5 planes inside the same AABB", "[editor][brush]") {
    const auto wedge = brush::build_wedge(math::Vec3{0, 0, 0}, math::Vec3{1, 1, 1});
    REQUIRE(wedge.planes.size() == 5);
    REQUIRE(approx_eq(wedge.bounds.min.y, -1.0f));
    REQUIRE(approx_eq(wedge.bounds.max.y, 1.0f));
}

TEST_CASE("brush: build_cylinder emits 2 + N planes", "[editor][brush]") {
    const auto cyl = brush::build_cylinder(math::Vec3{0, 0, 0}, math::Vec3{1, 1, 1}, /*sides=*/8);
    REQUIRE(cyl.planes.size() == 10);
}

TEST_CASE("brush CSG: two disjoint boxes -> two solid leaves + one void", "[editor][brush][csg]") {
    std::vector<brush::Brush> brushes;
    {
        brush::Brush a;
        a.id = 1;
        a.shape = 0;  // Box
        a.op = brush::Op::Add;
        a.origin = math::Vec3{-5, 0, 0};
        a.extents = math::Vec3{1, 1, 1};
        brushes.push_back(a);
    }
    {
        brush::Brush b;
        b.id = 2;
        b.shape = 0;  // Box
        b.op = brush::Op::Add;
        b.origin = math::Vec3{5, 0, 0};
        b.extents = math::Vec3{1, 1, 1};
        brushes.push_back(b);
    }

    const auto bsp = brush::compile_brushes(brushes);

    // 2 solid leaves + 1 outside leaf.
    REQUIRE(bsp.leaves.size() == 3);
    REQUIRE(bsp.leaves[0].solid);
    REQUIRE(bsp.leaves[1].solid);
    REQUIRE_FALSE(bsp.leaves[2].solid);

    // Solid leaves carry the original brush AABBs.
    REQUIRE(approx_eq(bsp.leaves[0].bounds.min.x, -6.0f));
    REQUIRE(approx_eq(bsp.leaves[0].bounds.max.x, -4.0f));
    REQUIRE(approx_eq(bsp.leaves[1].bounds.min.x, 4.0f));
    REQUIRE(approx_eq(bsp.leaves[1].bounds.max.x, 6.0f));

    // World bounds (outside leaf) span both inputs.
    REQUIRE(approx_eq(bsp.leaves[2].bounds.min.x, -6.0f));
    REQUIRE(approx_eq(bsp.leaves[2].bounds.max.x, 6.0f));

    // Two distinct mid-X splits, X-axis-aligned planes.
    REQUIRE(bsp.nodes.size() == 2);
    REQUIRE(approx_eq(bsp.nodes[0].plane.d, -5.0f));
    REQUIRE(approx_eq(bsp.nodes[1].plane.d, 5.0f));
    REQUIRE(approx_eq(bsp.nodes[0].plane.n.x, 1.0f));
    REQUIRE(approx_eq(bsp.nodes[1].plane.n.x, 1.0f));
}

TEST_CASE("brush CSG: a subtractive box that fully encloses an additive cancels it",
          "[editor][brush][csg]") {
    std::vector<brush::Brush> brushes;
    {
        brush::Brush a;
        a.id = 1;
        a.shape = 0;
        a.op = brush::Op::Add;
        a.origin = math::Vec3{0, 0, 0};
        a.extents = math::Vec3{1, 1, 1};
        brushes.push_back(a);
    }
    {
        brush::Brush s;
        s.id = 2;
        s.shape = 0;
        s.op = brush::Op::Subtract;
        s.origin = math::Vec3{0, 0, 0};
        s.extents = math::Vec3{2, 2, 2};  // strictly contains the additive
        brushes.push_back(s);
    }

    const auto bsp = brush::compile_brushes(brushes);

    // No solid leaves remain — only the outside leaf.
    REQUIRE(bsp.leaves.size() == 1);
    REQUIRE_FALSE(bsp.leaves[0].solid);
    REQUIRE(bsp.nodes.empty());
}

TEST_CASE("brush CSG: empty input is a single empty leaf", "[editor][brush][csg]") {
    const auto bsp = brush::compile_brushes({});
    REQUIRE(bsp.leaves.size() == 1);
    REQUIRE_FALSE(bsp.leaves[0].solid);
    REQUIRE(bsp.nodes.empty());
}
