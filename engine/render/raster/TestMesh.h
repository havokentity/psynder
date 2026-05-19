// SPDX-License-Identifier: MIT
// Psynder — test-mesh helpers for sample_01_triangle and the rasterizer
// unit / bench tests. Lane 25 wires these into the sample binaries; lane 07
// owns the helpers themselves so the test data lives next to the rasterizer
// that consumes it.
//
// All meshes here live in static storage — no per-frame new/delete, no
// shared_ptr, no virtual. The samples just take a pointer + count.

#pragma once

#include "Raster.h"

namespace psynder::render::raster::test_mesh {

// A small CCW triangle in NDC-ish coordinates, with vertex colors. The
// rotating triangle in sample_01 transforms this with a Mat4 per frame.
struct TriangleMesh {
    const Vertex* vertices;
    u32           vertex_count;
    const u32*    indices;
    u32           index_count;
};

// Single colored triangle, vertices at (-0.6, -0.5), (0.6, -0.5), (0.0, 0.6).
// Colors R / G / B at the three corners — used by the perspective-correct
// test (lerp the corner colors and compare).
TriangleMesh colored_triangle() noexcept;

// A simple 2-triangle fullscreen-ish quad, useful for the bin/coverage
// stress benchmarks (covers every tile in 640×360).
TriangleMesh fullscreen_quad() noexcept;

// Procedurally generated 8×8 checkerboard texture (RGBA8), used by
// sample_02_textured_quad and the perspective-correct attribute test.
const u32* checkerboard_texture(u32& w, u32& h) noexcept;

}  // namespace psynder::render::raster::test_mesh
