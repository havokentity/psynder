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
    constexpr std::array<MeshFactory, 8> factories{{
        render::geometry_tools::textured_triangle,
        render::geometry_tools::unit_cube,
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
    constexpr std::array<MeshFactory, 7> factories{{
        render::geometry_tools::island_terrain,
        render::geometry_tools::mountainous_terrain,
        render::geometry_tools::desert_terrain,
        render::geometry_tools::rolling_hills_terrain,
        render::geometry_tools::canyon_terrain,
        render::geometry_tools::volcanic_terrain,
        render::geometry_tools::arctic_terrain,
    }};

    for (const MeshFactory factory : factories) {
        const render::MeshDesc mesh = factory(nullptr);
        require_mesh_desc(mesh);
        require_height_variation(mesh);
    }
}

TEST_CASE("geometry tools terrain presets dispatch by enum", "[render][geometry_tools]") {
    using render::geometry_tools::TerrainPreset;
    constexpr std::array<TerrainPreset, 7> presets{{
        TerrainPreset::Island,
        TerrainPreset::Mountainous,
        TerrainPreset::Desert,
        TerrainPreset::RollingHills,
        TerrainPreset::Canyon,
        TerrainPreset::Volcanic,
        TerrainPreset::Arctic,
    }};

    for (const TerrainPreset preset : presets) {
        REQUIRE(render::geometry_tools::terrain_preset_name(preset)[0] != '\0');
        const render::MeshDesc mesh = render::geometry_tools::terrain(preset);
        require_mesh_desc(mesh);
        require_height_variation(mesh);
    }
}

TEST_CASE("geometry tools build parameterized primitive meshes", "[render][geometry_tools]") {
    render::geometry_tools::BoxDesc box{};
    box.half_extent = {2.0f, 0.25f, 1.0f};
    box.color = 0xFF3366AAu;
    const render::geometry_tools::GeneratedMesh box_mesh = render::geometry_tools::unit_cube(box);
    require_mesh_desc(box_mesh.desc());
    REQUIRE(box_mesh.vertices.size() == 24u);
    REQUIRE(box_mesh.indices.size() == 36u);
    REQUIRE(box_mesh.local_bounds.min.x == -2.0f);
    REQUIRE(box_mesh.local_bounds.max.z == 1.0f);
    REQUIRE(box_mesh.vertices[0].color == 0xFF3366AAu);

    render::geometry_tools::ConeDesc cone{};
    cone.segments = 12;
    cone.radius = 0.75f;
    cone.height = 1.5f;
    cone.cap = false;
    const render::geometry_tools::GeneratedMesh cone_mesh = render::geometry_tools::cone(cone);
    require_mesh_desc(cone_mesh.desc());
    REQUIRE(cone_mesh.indices.size() == 12u * 3u);

    render::geometry_tools::SphereDesc sphere{};
    sphere.slices = 10;
    sphere.stacks = 5;
    sphere.radius = 2.0f;
    const render::geometry_tools::GeneratedMesh sphere_mesh = render::geometry_tools::uv_sphere(sphere);
    require_mesh_desc(sphere_mesh.desc());
    REQUIRE(sphere_mesh.vertices.size() == 66u);
    REQUIRE(sphere_mesh.indices.size() == 300u);
    REQUIRE(sphere_mesh.local_bounds.min.y <= -2.0f);
    REQUIRE(sphere_mesh.local_bounds.max.y >= 2.0f);
}

TEST_CASE("geometry tools build parameterized terrain meshes", "[render][geometry_tools]") {
    render::geometry_tools::TerrainDesc terrain{};
    terrain.preset = render::geometry_tools::TerrainPreset::Volcanic;
    terrain.cells = 8;
    terrain.half_extent = 4.0f;
    terrain.height_scale = 2.0f;
    terrain.noise_strength = 0.04f;
    terrain.terrace_strength = 0.5f;
    terrain.seed = 42u;
    terrain.low_color = 0xFF101010u;
    terrain.mid_color = 0xFF303030u;
    terrain.high_color = 0xFF805030u;
    terrain.water_color = 0xFF204060u;

    const render::geometry_tools::GeneratedMesh mesh = render::geometry_tools::terrain(terrain);
    require_mesh_desc(mesh.desc());
    require_height_variation(mesh.desc());
    REQUIRE(mesh.vertices.size() == 81u);
    REQUIRE(mesh.indices.size() == 8u * 8u * 6u);
    REQUIRE(mesh.local_bounds.min.x == -4.0f);
    REQUIRE(mesh.local_bounds.max.z == 4.0f);
}
