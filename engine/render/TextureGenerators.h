// SPDX-License-Identifier: MIT
// Psynder - deterministic CPU texture generators for samples, tools, and editor defaults.

#pragma once

#include "core/Types.h"

namespace psynder::render {

class Texture2D;

}  // namespace psynder::render

namespace psynder::render::texture_generators {

struct SolidDesc {
    u32 width = 1;
    u32 height = 1;
    u32 color = 0xFFFFFFFFu;
};

struct CheckerDesc {
    u32 width = 64;
    u32 height = 64;
    u32 cell_size = 8;
    u32 color_a = 0xFFFF00FFu;
    u32 color_b = 0xFF000000u;
};

struct GridDesc {
    u32 width = 128;
    u32 height = 128;
    u32 major_step = 32;
    u32 minor_step = 8;
    u32 line_width = 1;
    u32 background = 0xFF202028u;
    u32 minor = 0xFF343848u;
    u32 major = 0xFF70C0FFu;
};

struct GradientDesc {
    u32 width = 128;
    u32 height = 128;
    u32 top = 0xFF101828u;
    u32 bottom = 0xFF385878u;
};

struct ValueNoiseDesc {
    u32 width = 128;
    u32 height = 128;
    u32 seed = 1;
    u32 base_color = 0xFF808080u;
    f32 amplitude = 0.35f;
};

struct WoodPlanksDesc {
    u32 width = 128;
    u32 height = 128;
    u32 plank_count = 6;
    u32 base = 0xFF386796u;
    u32 groove = 0xFF142840u;
    f32 grain_strength = 0.18f;
    f32 speckle_strength = 0.12f;
};

struct WoodenCrateDesc {
    u32 dimension = 128;
    u32 plank_count = 6;
    u32 wood = 0xFF386796u;
    u32 groove = 0xFF142840u;
    u32 frame = 0xFF1A324Eu;
    u32 bolt = 0xFF68605Cu;
    f32 batten_half_width = 0.06f;
    f32 grain_strength = 0.18f;
    f32 speckle_strength = 0.12f;
};

struct BrickDesc {
    u32 width = 128;
    u32 height = 128;
    u32 brick_width = 32;
    u32 brick_height = 16;
    u32 mortar_width = 2;
    u32 brick = 0xFF405FB0u;
    u32 mortar = 0xFF708090u;
    f32 shade_strength = 0.12f;
};

struct BuildingFacadeDesc {
    u32 dimension = 64;
    u32 columns = 3;
    u32 rows = 3;
    u32 roof_height = 0;
    u32 concrete = 0xFF969896u;
    u32 mullion = 0xFF403C3Au;
    u32 roof = 0xFF3C4246u;
    u32 window = 0xFF60B0C4u;
    f32 concrete_speckle = 18.0f;
    f32 roof_speckle = 12.0f;
    f32 window_speckle = 14.0f;
};

[[nodiscard]] Texture2D solid(const SolidDesc& desc = {});
[[nodiscard]] Texture2D checker(const CheckerDesc& desc = {});
[[nodiscard]] Texture2D grid(const GridDesc& desc = {});
[[nodiscard]] Texture2D vertical_gradient(const GradientDesc& desc = {});
[[nodiscard]] Texture2D value_noise(const ValueNoiseDesc& desc = {});
[[nodiscard]] Texture2D wood_planks(const WoodPlanksDesc& desc = {});
[[nodiscard]] Texture2D wooden_crate(const WoodenCrateDesc& desc = {});
[[nodiscard]] Texture2D bricks(const BrickDesc& desc = {});
[[nodiscard]] Texture2D building_facade(const BuildingFacadeDesc& desc = {});

}  // namespace psynder::render::texture_generators
