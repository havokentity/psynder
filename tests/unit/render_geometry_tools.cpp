// SPDX-License-Identifier: MIT
// Psynder — geometry tool descriptor contract tests.

#include <catch2/catch_test_macros.hpp>

#include "render/GeometryTools.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace psynder;

namespace {

using MeshFactory = render::MeshDesc (*)(const render::TextureAsset*) noexcept;

void require_mesh_desc(const render::MeshDesc& mesh) {
    REQUIRE(mesh.vertices != nullptr);
    REQUIRE(mesh.vertex_count > 0u);
    REQUIRE(mesh.indices != nullptr);
    REQUIRE(mesh.index_count > 0u);
    REQUIRE((mesh.index_count % 3u) == 0u);
    REQUIRE(mesh.local_bounds.max.x >= mesh.local_bounds.min.x);
    REQUIRE(mesh.local_bounds.max.y >= mesh.local_bounds.min.y);
    REQUIRE(mesh.local_bounds.max.z >= mesh.local_bounds.min.z);

    for (u32 i = 0; i < mesh.index_count; ++i)
        REQUIRE(mesh.indices[i] < mesh.vertex_count);

    for (u32 i = 0; i < mesh.vertex_count; ++i) {
        const auto& v = mesh.vertices[i];
        REQUIRE(std::isfinite(v.position.x));
        REQUIRE(std::isfinite(v.position.y));
        REQUIRE(std::isfinite(v.position.z));
        REQUIRE(std::isfinite(v.normal.x));
        REQUIRE(std::isfinite(v.normal.y));
        REQUIRE(std::isfinite(v.normal.z));
        REQUIRE(std::isfinite(v.uv.x));
        REQUIRE(std::isfinite(v.uv.y));
        REQUIRE(v.uv.x >= -0.001f);
        REQUIRE(v.uv.x <= 1.001f);
        REQUIRE(v.uv.y >= -0.001f);
        REQUIRE(v.uv.y <= 1.001f);
    }
}

void require_height_variation(const render::MeshDesc& mesh) {
    f32 min_y = mesh.vertices[0].position.y;
    f32 max_y = mesh.vertices[0].position.y;
    for (u32 i = 1; i < mesh.vertex_count; ++i) {
        min_y = std::min(min_y, mesh.vertices[i].position.y);
        max_y = std::max(max_y, mesh.vertices[i].position.y);
    }
    REQUIRE((max_y - min_y) > 0.05f);
}

}  // namespace

TEST_CASE("geometry tools expose valid primitive mesh descriptors", "[render][geometry_tools]") {
    constexpr std::array<MeshFactory, 7> factories{{
        render::geometry_tools::textured_triangle,
        render::geometry_tools::pyramid,
        render::geometry_tools::cone,
        render::geometry_tools::uv_sphere,
        render::geometry_tools::geodesic_sphere,
        render::geometry_tools::sierpinski_tetrahedron,
        render::geometry_tools::sierpinski_carpet,
    }};

    for (const MeshFactory factory : factories)
        require_mesh_desc(factory(nullptr));
}

TEST_CASE("geometry tools expose varied terrain mesh descriptors", "[render][geometry_tools]") {
    constexpr std::array<MeshFactory, 5> factories{{
        render::geometry_tools::island_terrain,
        render::geometry_tools::mountainous_terrain,
        render::geometry_tools::desert_terrain,
        render::geometry_tools::rolling_hills_terrain,
        render::geometry_tools::canyon_terrain,
    }};

    for (const MeshFactory factory : factories) {
        const render::MeshDesc mesh = factory(nullptr);
        require_mesh_desc(mesh);
        require_height_variation(mesh);
    }
}
