// SPDX-License-Identifier: MIT
// Psynder — Lane 18 unit test: save/load roundtrip for `.psylevel` (full
// scene including brushes, entities, bodies, constraints, heightfield,
// splat grid) and `.psyc` (contraption-only: bodies + constraints).
//
// The serial namespace is header-only (Serialization.h), so the test goes
// through `serial::encode_state` + `serial::decode_state` directly without
// linking psynder_editor_core. This is the same pattern used by the
// brush-CSG, sculpt, and undo tests in this directory.

#include "editor/core/Brush.h"
#include "editor/core/Constraints.h"
#include "editor/core/EditorState.h"
#include "editor/core/Sculpt.h"
#include "editor/core/Serialization.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::editor;

namespace {

constexpr f32 kEps = 1e-5f;

PSY_FORCEINLINE bool approx_eq(f32 a, f32 b) noexcept {
    const f32 d = a - b;
    return d > -kEps && d < kEps;
}

// Populate a fresh editor::detail::State with a representative Wave-B
// scene: two brushes (one Add box, one Subtract box), two entities, three
// bodies, one Weld + one Axis + one Rope constraint, plus a small
// heightfield and splat grid.
//
// `State` carries a std::atomic<u8> for the mode field, so we fill an
// out-parameter rather than returning by value.
void build_sample_scene(detail::State& s) {
    // Brushes ---------------------------------------------------------
    {
        brush::Brush a;
        a.id        = 1;
        a.shape     = 0;        // Box
        a.op        = brush::Op::Add;
        a.origin    = math::Vec3{-2.0f, 0.0f, 0.0f};
        a.extents   = math::Vec3{1.0f, 1.0f, 1.0f};
        a.grid_size = 0.25f;
        a.sides     = 8;
        s.brushes.push_back(a);

        brush::Brush b;
        b.id        = 2;
        b.shape     = 2;        // Cylinder
        b.op        = brush::Op::Subtract;
        b.origin    = math::Vec3{ 4.0f, 1.5f, 2.0f};
        b.extents   = math::Vec3{0.5f, 2.0f, 0.5f};
        b.grid_size = 0.125f;
        b.sides     = 12;
        s.brushes.push_back(b);
        s.next_brush_id = 3;
    }

    // Entities --------------------------------------------------------
    {
        detail::EntityRec e1;
        e1.id        = 1;
        e1.prefab_id = 42;
        e1.position  = math::Vec3{ 1.0f,  0.5f, -1.0f};
        e1.rotation  = math::Quat{0.0f, 0.7071f, 0.0f, 0.7071f};
        e1.scale     = math::Vec3{1.0f, 1.0f, 1.0f};
        e1.alive     = true;
        s.entities.push_back(e1);

        detail::EntityRec e2;
        e2.id        = 2;
        e2.prefab_id = 99;
        e2.position  = math::Vec3{-3.0f,  2.0f,  5.0f};
        e2.rotation  = math::Quat{0,0,0,1};
        e2.scale     = math::Vec3{2.0f, 2.0f, 2.0f};
        e2.alive     = false;       // exercise the alive-flag flip
        s.entities.push_back(e2);
        s.next_entity_id = 3;
    }

    // Bodies ----------------------------------------------------------
    {
        detail::BodyRec b1;
        b1.id       = 1;
        b1.position = math::Vec3{ 0.0f, 1.0f, 0.0f };
        b1.rotation = math::Quat{ 0,0,0,1 };
        b1.scale    = math::Vec3{ 1,1,1 };
        b1.frozen   = false;
        b1.alive    = true;
        s.bodies.push_back(b1);

        detail::BodyRec b2;
        b2.id       = 2;
        b2.position = math::Vec3{ 1.0f, 1.0f, 0.0f };
        b2.rotation = math::Quat{ 0.0f, 0.0f, 0.3826834f, 0.9238795f };  // 45° about Z
        b2.scale    = math::Vec3{ 1,1,1 };
        b2.frozen   = true;
        b2.alive    = true;
        s.bodies.push_back(b2);

        detail::BodyRec b3;
        b3.id       = 3;
        b3.position = math::Vec3{ 2.5f, 0.5f, 0.0f };
        b3.rotation = math::Quat{ 0,0,0,1 };
        b3.scale    = math::Vec3{ 1,1,1 };
        b3.frozen   = false;
        b3.alive    = true;
        s.bodies.push_back(b3);
        s.next_body_id = 4;
    }

    // Constraints -----------------------------------------------------
    {
        auto& g = s.constraint_graph;
        g.add(constraints::make_weld(1, 2, math::Vec3{0.5f, 1.0f, 0.0f}));
        g.add(constraints::make_axis(2, 3, math::Vec3{1.75f, 0.75f, 0.0f},
                                     math::Vec3{0.0f, 0.0f, 1.0f},
                                     -math::kPi, math::kPi));
        g.add(constraints::make_rope(1, 3,
                                     math::Vec3{0,0,0},
                                     math::Vec3{0,0,0},
                                     /*max_length=*/3.5f));
    }

    // Heightfield + splat --------------------------------------------
    s.heightfield.allocate(8, 6, 0.5f);
    s.heightfield.origin = math::Vec3{ -4.0f, 0.0f, -3.0f };
    // Paint a stair-stepped surface so the roundtrip notices any byte
    // ordering mistakes.
    for (u32 iz = 0; iz < s.heightfield.size_z; ++iz) {
        for (u32 ix = 0; ix < s.heightfield.size_x; ++ix) {
            s.heightfield.store(ix, iz, static_cast<f32>(ix) * 0.1f
                                       + static_cast<f32>(iz) * 0.01f);
        }
    }
    s.splat.allocate(s.heightfield.size_x, s.heightfield.size_z);
    s.splat.at(4, 3) = { 0.5f, 0.3f, 0.15f, 0.05f };
}

}  // namespace

TEST_CASE("save/load: .psylevel roundtrip preserves every section",
          "[editor][serial][roundtrip]") {
    detail::State src;
    build_sample_scene(src);

    // Encode with terrain.
    std::vector<u8> blob;
    serial::encode_state(blob, src, /*with_terrain=*/true);
    REQUIRE_FALSE(blob.empty());

    // Decode into a fresh state.
    detail::State dst;
    REQUIRE(serial::decode_state(blob.data(), blob.size(), dst, /*with_terrain=*/true));

    // Brushes ---------------------------------------------------------
    REQUIRE(dst.brushes.size() == src.brushes.size());
    for (usize i = 0; i < src.brushes.size(); ++i) {
        REQUIRE(dst.brushes[i].id        == src.brushes[i].id);
        REQUIRE(dst.brushes[i].shape     == src.brushes[i].shape);
        REQUIRE(dst.brushes[i].op        == src.brushes[i].op);
        REQUIRE(dst.brushes[i].sides     == src.brushes[i].sides);
        REQUIRE(approx_eq(dst.brushes[i].origin.x,    src.brushes[i].origin.x));
        REQUIRE(approx_eq(dst.brushes[i].origin.y,    src.brushes[i].origin.y));
        REQUIRE(approx_eq(dst.brushes[i].origin.z,    src.brushes[i].origin.z));
        REQUIRE(approx_eq(dst.brushes[i].extents.x,   src.brushes[i].extents.x));
        REQUIRE(approx_eq(dst.brushes[i].extents.y,   src.brushes[i].extents.y));
        REQUIRE(approx_eq(dst.brushes[i].extents.z,   src.brushes[i].extents.z));
        REQUIRE(approx_eq(dst.brushes[i].grid_size,   src.brushes[i].grid_size));
    }

    // Entities --------------------------------------------------------
    REQUIRE(dst.entities.size() == src.entities.size());
    for (usize i = 0; i < src.entities.size(); ++i) {
        REQUIRE(dst.entities[i].id        == src.entities[i].id);
        REQUIRE(dst.entities[i].prefab_id == src.entities[i].prefab_id);
        REQUIRE(dst.entities[i].alive     == src.entities[i].alive);
        REQUIRE(approx_eq(dst.entities[i].position.x, src.entities[i].position.x));
        REQUIRE(approx_eq(dst.entities[i].position.y, src.entities[i].position.y));
        REQUIRE(approx_eq(dst.entities[i].position.z, src.entities[i].position.z));
        REQUIRE(approx_eq(dst.entities[i].rotation.x, src.entities[i].rotation.x));
        REQUIRE(approx_eq(dst.entities[i].rotation.w, src.entities[i].rotation.w));
        REQUIRE(approx_eq(dst.entities[i].scale.x,    src.entities[i].scale.x));
    }

    // Bodies ----------------------------------------------------------
    REQUIRE(dst.bodies.size() == src.bodies.size());
    for (usize i = 0; i < src.bodies.size(); ++i) {
        REQUIRE(dst.bodies[i].id     == src.bodies[i].id);
        REQUIRE(dst.bodies[i].frozen == src.bodies[i].frozen);
        REQUIRE(dst.bodies[i].alive  == src.bodies[i].alive);
        REQUIRE(approx_eq(dst.bodies[i].position.x, src.bodies[i].position.x));
        REQUIRE(approx_eq(dst.bodies[i].position.y, src.bodies[i].position.y));
        REQUIRE(approx_eq(dst.bodies[i].position.z, src.bodies[i].position.z));
        REQUIRE(approx_eq(dst.bodies[i].rotation.z, src.bodies[i].rotation.z));
        REQUIRE(approx_eq(dst.bodies[i].rotation.w, src.bodies[i].rotation.w));
    }

    // Constraints -----------------------------------------------------
    REQUIRE(dst.constraint_graph.size() == src.constraint_graph.size());
    for (usize i = 0; i < src.constraint_graph.size(); ++i) {
        const auto& a = src.constraint_graph.at(i);
        const auto& b = dst.constraint_graph.at(i);
        REQUIRE(b.id     == a.id);
        REQUIRE(b.kind   == a.kind);
        REQUIRE(b.body_a == a.body_a);
        REQUIRE(b.body_b == a.body_b);
        REQUIRE(approx_eq(b.anchor_a.x, a.anchor_a.x));
        REQUIRE(approx_eq(b.anchor_b.y, a.anchor_b.y));
        REQUIRE(approx_eq(b.rest_length, a.rest_length));
        REQUIRE(approx_eq(b.min_limit,   a.min_limit));
        REQUIRE(approx_eq(b.max_limit,   a.max_limit));
    }

    // Heightfield -----------------------------------------------------
    REQUIRE(dst.heightfield.size_x  == src.heightfield.size_x);
    REQUIRE(dst.heightfield.size_z  == src.heightfield.size_z);
    REQUIRE(approx_eq(dst.heightfield.spacing,   src.heightfield.spacing));
    REQUIRE(approx_eq(dst.heightfield.origin.x,  src.heightfield.origin.x));
    REQUIRE(approx_eq(dst.heightfield.origin.z,  src.heightfield.origin.z));
    REQUIRE(dst.heightfield.heights.size() == src.heightfield.heights.size());
    for (usize i = 0; i < src.heightfield.heights.size(); ++i) {
        REQUIRE(approx_eq(dst.heightfield.heights[i], src.heightfield.heights[i]));
    }

    // Splat -----------------------------------------------------------
    REQUIRE(dst.splat.size_x == src.splat.size_x);
    REQUIRE(dst.splat.size_z == src.splat.size_z);
    REQUIRE(dst.splat.weights.size() == src.splat.weights.size());
    // SplatGrid::at is non-const; read via the underlying weights vector.
    const usize idx = src.splat.index(4, 3);
    for (usize ch = 0; ch < 4; ++ch) {
        REQUIRE(approx_eq(dst.splat.weights[idx][ch],
                          src.splat.weights[idx][ch]));
    }
}

TEST_CASE("save/load: .psyc contraption roundtrip omits terrain",
          "[editor][serial][roundtrip]") {
    detail::State src;
    build_sample_scene(src);

    // Encode WITHOUT terrain (contraption flow).
    std::vector<u8> blob;
    serial::encode_state(blob, src, /*with_terrain=*/false);
    REQUIRE_FALSE(blob.empty());

    detail::State dst;
    // Pre-seed dst with non-empty terrain that should NOT change after the
    // contraption load (terrain section is intentionally not in the blob).
    dst.heightfield.allocate(4, 4, 1.0f);
    dst.heightfield.store(2, 2, 7.5f);
    const f32 expected_height_at_2_2 = 7.5f;

    REQUIRE(serial::decode_state(blob.data(), blob.size(), dst, /*with_terrain=*/false));

    // Bodies + constraints came through.
    REQUIRE(dst.bodies.size()           == src.bodies.size());
    REQUIRE(dst.constraint_graph.size() == src.constraint_graph.size());
    REQUIRE(dst.entities.size()         == src.entities.size());
    REQUIRE(dst.brushes.size()          == src.brushes.size());

    // Terrain preserved (decoder didn't touch it because with_terrain=false).
    REQUIRE(dst.heightfield.size_x == 4);
    REQUIRE(dst.heightfield.size_z == 4);
    REQUIRE(approx_eq(dst.heightfield.sample(2, 2), expected_height_at_2_2));
}

TEST_CASE("save/load: empty scene roundtrip", "[editor][serial][roundtrip]") {
    detail::State src;       // no brushes, no entities, no constraints
    std::vector<u8> blob;
    serial::encode_state(blob, src, /*with_terrain=*/true);
    REQUIRE_FALSE(blob.empty());

    detail::State dst;
    REQUIRE(serial::decode_state(blob.data(), blob.size(), dst, /*with_terrain=*/true));
    REQUIRE(dst.brushes.empty());
    REQUIRE(dst.entities.empty());
    REQUIRE(dst.bodies.empty());
    REQUIRE(dst.constraint_graph.size() == 0);
    REQUIRE(dst.heightfield.size_x == 0);
    REQUIRE(dst.heightfield.heights.empty());
    REQUIRE(dst.splat.size_x == 0);
}

TEST_CASE("save/load: decoder rejects truncated payload",
          "[editor][serial][roundtrip]") {
    detail::State src;
    build_sample_scene(src);
    std::vector<u8> blob;
    serial::encode_state(blob, src, /*with_terrain=*/true);

    // Hack off the back half.
    blob.resize(blob.size() / 2);

    detail::State dst;
    const bool ok = serial::decode_state(blob.data(), blob.size(), dst, /*with_terrain=*/true);
    REQUIRE_FALSE(ok);
}
