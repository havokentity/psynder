// SPDX-License-Identifier: MIT
// Psynder — fixed-storage geometry descriptors for app/sample authoring.

#pragma once

#include "render/Geometry.h"

#include <vector>

namespace psynder::render::geometry_tools {

enum class TerrainPreset : u8 {
    Island,
    Mountainous,
    Desert,
    RollingHills,
    Canyon,
    Volcanic,
    Arctic,
};

struct GeneratedMesh {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
    math::Aabb local_bounds = math::aabb_empty();

    [[nodiscard]] MeshDesc desc() const noexcept;
};

struct BoxDesc {
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    math::Vec2 uv_repeat{1.0f, 1.0f};
    u32 color = 0xFFFFFFFFu;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
};

struct PyramidDesc {
    f32 half_width = 0.5f;
    f32 half_depth = 0.5f;
    f32 height = 0.9f;
    math::Vec2 uv_repeat{1.0f, 1.0f};
    u32 color = 0xFFFFFFFFu;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
};

struct ConeDesc {
    f32 radius = 0.5f;
    f32 height = 0.9f;
    u32 segments = 24;
    bool cap = true;
    math::Vec2 uv_repeat{1.0f, 1.0f};
    u32 color = 0xFFFFFFFFu;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
};

struct SphereDesc {
    f32 radius = 1.0f;
    u32 slices = 24;
    u32 stacks = 12;
    math::Vec2 uv_repeat{1.0f, 1.0f};
    u32 color = 0xFFFFFFFFu;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
};

struct TerrainDesc {
    TerrainPreset preset = TerrainPreset::Island;
    u32 cells = 32;
    f32 half_extent = 3.0f;
    f32 height_scale = 1.0f;
    f32 height_bias = 0.0f;
    f32 feature_scale = 1.0f;
    f32 noise_strength = 0.0f;
    f32 terrace_strength = 0.0f;
    f32 water_level = -0.03f;
    u32 seed = 1u;
    math::Vec2 uv_repeat{1.0f, 1.0f};
    u32 low_color = 0;
    u32 mid_color = 0;
    u32 high_color = 0;
    u32 water_color = 0;
    TextureView base_color{};
    const TextureAsset* base_color_asset = nullptr;
    raster::CullMode cull = raster::CullMode::Back;
};

MeshDesc textured_triangle(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc unit_cube(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc pyramid(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc cone(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc uv_sphere(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc geodesic_sphere(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc sierpinski_tetrahedron(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc sierpinski_carpet(const TextureAsset* base_color_asset = nullptr) noexcept;

GeneratedMesh unit_cube(const BoxDesc& desc);
GeneratedMesh pyramid(const PyramidDesc& desc);
GeneratedMesh cone(const ConeDesc& desc);
GeneratedMesh uv_sphere(const SphereDesc& desc);
GeneratedMesh terrain(const TerrainDesc& desc);

const char* terrain_preset_name(TerrainPreset preset) noexcept;
MeshDesc terrain(TerrainPreset preset, const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc island_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc mountainous_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc desert_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc rolling_hills_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc canyon_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc volcanic_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc arctic_terrain(const TextureAsset* base_color_asset = nullptr) noexcept;

}  // namespace psynder::render::geometry_tools
