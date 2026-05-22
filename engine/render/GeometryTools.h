// SPDX-License-Identifier: MIT
// Psynder — fixed-storage geometry descriptors for app/sample authoring.

#pragma once

#include "render/Geometry.h"

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

MeshDesc textured_triangle(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc unit_cube(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc pyramid(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc cone(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc uv_sphere(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc geodesic_sphere(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc sierpinski_tetrahedron(const TextureAsset* base_color_asset = nullptr) noexcept;
MeshDesc sierpinski_carpet(const TextureAsset* base_color_asset = nullptr) noexcept;

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
