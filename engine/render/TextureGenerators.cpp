// SPDX-License-Identifier: MIT
// Psynder - deterministic CPU texture generators for samples, tools, and editor defaults.

#include "render/TextureGenerators.h"

#include "core/HashHelpers.h"
#include "render/Color.h"
#include "render/Texture.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace psynder::render::texture_generators {
namespace {

[[nodiscard]] u32 safe_dim(u32 value) noexcept {
    return std::max(1u, value);
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

[[nodiscard]] u8 clamp_u8(i32 value) noexcept {
    return static_cast<u8>(std::clamp(value, 0, 255));
}

[[nodiscard]] f32 hash01(u32 x, u32 y, u32 seed = 0u) noexcept {
    return hash_helpers::hash2_unit24(x, y, seed);
}

struct Rgba {
    i32 r = 0;
    i32 g = 0;
    i32 b = 0;
    i32 a = 255;
};

[[nodiscard]] Rgba unpack(u32 color) noexcept {
    return {static_cast<i32>(color & 0xFFu),
            static_cast<i32>((color >> 8u) & 0xFFu),
            static_cast<i32>((color >> 16u) & 0xFFu),
            static_cast<i32>((color >> 24u) & 0xFFu)};
}

[[nodiscard]] u32 pack(Rgba color) noexcept {
    return rgba8(clamp_u8(color.r), clamp_u8(color.g), clamp_u8(color.b), clamp_u8(color.a));
}

[[nodiscard]] u32 add_luma(u32 color, i32 luma) noexcept {
    const Rgba c = unpack(color);
    return pack({c.r + luma, c.g + luma, c.b + luma, c.a});
}

[[nodiscard]] u32 lerp_color(u32 a, u32 b, f32 t) noexcept {
    const Rgba ca = unpack(a);
    const Rgba cb = unpack(b);
    const f32 u = clamp01(t);
    const auto mix = [u](i32 x, i32 y) {
        return static_cast<i32>(static_cast<f32>(x) +
                                (static_cast<f32>(y - x) * u));
    };
    return pack({mix(ca.r, cb.r), mix(ca.g, cb.g), mix(ca.b, cb.b), mix(ca.a, cb.a)});
}

[[nodiscard]] Texture2D make_texture(u32 width, u32 height, std::vector<u32> pixels) {
    return Texture2D::from_rgba8(safe_dim(width), safe_dim(height), std::move(pixels));
}

}  // namespace

Texture2D solid(const SolidDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    return make_texture(width, height, std::vector<u32>(static_cast<usize>(width) * height, desc.color));
}

Texture2D checker(const CheckerDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    const u32 cell = safe_dim(desc.cell_size);
    std::vector<u32> pixels(static_cast<usize>(width) * height);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            pixels[static_cast<usize>(y) * width + x] =
                (((x / cell) ^ (y / cell)) & 1u) != 0u ? desc.color_b : desc.color_a;
        }
    }
    return make_texture(width, height, std::move(pixels));
}

Texture2D grid(const GridDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    const u32 major_step = safe_dim(desc.major_step);
    const u32 minor_step = safe_dim(desc.minor_step);
    const u32 line_width = safe_dim(desc.line_width);
    std::vector<u32> pixels(static_cast<usize>(width) * height, desc.background);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const bool major_line = (x % major_step) < line_width || (y % major_step) < line_width;
            const bool minor_line = (x % minor_step) < line_width || (y % minor_step) < line_width;
            pixels[static_cast<usize>(y) * width + x] =
                major_line ? desc.major : (minor_line ? desc.minor : desc.background);
        }
    }
    return make_texture(width, height, std::move(pixels));
}

Texture2D vertical_gradient(const GradientDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    std::vector<u32> pixels(static_cast<usize>(width) * height);
    const f32 denom = static_cast<f32>(std::max(1u, height - 1u));
    for (u32 y = 0; y < height; ++y) {
        const u32 row_color = lerp_color(desc.top, desc.bottom, static_cast<f32>(y) / denom);
        for (u32 x = 0; x < width; ++x)
            pixels[static_cast<usize>(y) * width + x] = row_color;
    }
    return make_texture(width, height, std::move(pixels));
}

Texture2D value_noise(const ValueNoiseDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    const Rgba base = unpack(desc.base_color);
    const f32 amplitude = std::max(0.0f, desc.amplitude);
    std::vector<u32> pixels(static_cast<usize>(width) * height);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const f32 n = (hash01(x, y, desc.seed) - 0.5f) * 2.0f * amplitude;
            pixels[static_cast<usize>(y) * width + x] =
                pack({base.r + static_cast<i32>(static_cast<f32>(base.r) * n),
                      base.g + static_cast<i32>(static_cast<f32>(base.g) * n),
                      base.b + static_cast<i32>(static_cast<f32>(base.b) * n),
                      base.a});
        }
    }
    return make_texture(width, height, std::move(pixels));
}

Texture2D wood_planks(const WoodPlanksDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    const u32 plank_count = safe_dim(desc.plank_count);
    const u32 plank_w = std::max(1u, width / plank_count);
    const u32 groove_w = std::max(1u, width / 64u);
    const f32 grain_strength = std::max(0.0f, desc.grain_strength);
    const f32 speckle_strength = std::max(0.0f, desc.speckle_strength);
    std::vector<u32> pixels(static_cast<usize>(width) * height);

    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 plank = std::min(plank_count - 1u, x / plank_w);
            const bool groove = (x % plank_w) < groove_w && plank > 0u;
            const f32 wave = std::cos((static_cast<f32>(x) + static_cast<f32>(plank) * 11.0f) * 0.9f);
            const f32 speckle = (hash01(x, y, plank) - 0.5f) * 2.0f;
            const i32 plank_shift = static_cast<i32>((plank * 37u) % 37u) - 18 +
                                    static_cast<i32>(plank * 6u);
            const i32 luma = plank_shift +
                             static_cast<i32>(wave * 64.0f * grain_strength) +
                             static_cast<i32>(speckle * 96.0f * speckle_strength);
            pixels[static_cast<usize>(y) * width + x] =
                groove ? desc.groove : add_luma(desc.base, luma);
        }
    }
    return make_texture(width, height, std::move(pixels));
}

Texture2D wooden_crate(const WoodenCrateDesc& desc) {
    const u32 dim = safe_dim(desc.dimension);
    WoodPlanksDesc planks{};
    planks.width = dim;
    planks.height = dim;
    planks.plank_count = desc.plank_count;
    planks.base = desc.wood;
    planks.groove = desc.groove;
    planks.grain_strength = desc.grain_strength;
    planks.speckle_strength = desc.speckle_strength;
    Texture2D texture = wood_planks(planks);
    std::vector<u32>& pixels = texture.pixels();

    const u32 frame = std::max(1u, dim / 12u);
    const f32 dimf = static_cast<f32>(dim);
    const f32 bar = std::max(0.0f, desc.batten_half_width);
    const f32 bolt_in = static_cast<f32>(frame) * 0.5f;
    const f32 bolt_rad = static_cast<f32>(frame) * 0.30f;
    const f32 corners[4][2] = {
        {bolt_in, bolt_in},
        {dimf - bolt_in, bolt_in},
        {bolt_in, dimf - bolt_in},
        {dimf - bolt_in, dimf - bolt_in},
    };

    for (u32 y = 0; y < dim; ++y) {
        for (u32 x = 0; x < dim; ++x) {
            const f32 fx = static_cast<f32>(x) / dimf;
            const f32 fy = static_cast<f32>(y) / dimf;
            const bool in_frame =
                (x < frame) || (y < frame) || (x >= dim - frame) || (y >= dim - frame);
            const bool on_batten = (std::fabs(fx - fy) < bar) ||
                                   (std::fabs(fx - (1.0f - fy)) < bar);

            bool on_bolt = false;
            for (const auto& c : corners) {
                const f32 dx = static_cast<f32>(x) + 0.5f - c[0];
                const f32 dy = static_cast<f32>(y) + 0.5f - c[1];
                if (dx * dx + dy * dy <= bolt_rad * bolt_rad) {
                    on_bolt = true;
                    break;
                }
            }

            u32& pixel = pixels[static_cast<usize>(y) * dim + x];
            if (on_bolt) {
                const i32 luma = static_cast<i32>((hash01(x, y, 91u) - 0.5f) * 30.0f);
                pixel = add_luma(desc.bolt, luma);
            } else if (in_frame || on_batten) {
                const i32 luma = static_cast<i32>((hash01(x, y, 37u) - 0.5f) * 16.0f);
                pixel = add_luma(desc.frame, luma);
            }
        }
    }
    return texture;
}

Texture2D bricks(const BrickDesc& desc) {
    const u32 width = safe_dim(desc.width);
    const u32 height = safe_dim(desc.height);
    const u32 brick_w = safe_dim(desc.brick_width);
    const u32 brick_h = safe_dim(desc.brick_height);
    const u32 mortar_w = safe_dim(desc.mortar_width);
    const f32 shade_strength = std::max(0.0f, desc.shade_strength);
    std::vector<u32> pixels(static_cast<usize>(width) * height);

    for (u32 y = 0; y < height; ++y) {
        const u32 row = y / brick_h;
        const u32 row_offset = (row & 1u) != 0u ? brick_w / 2u : 0u;
        for (u32 x = 0; x < width; ++x) {
            const u32 local_x = (x + row_offset) % brick_w;
            const u32 local_y = y % brick_h;
            const bool mortar = local_x < mortar_w || local_y < mortar_w;
            const i32 luma = static_cast<i32>((hash01(x / brick_w, row, 17u) - 0.5f) *
                                              255.0f * shade_strength);
            pixels[static_cast<usize>(y) * width + x] =
                mortar ? desc.mortar : add_luma(desc.brick, luma);
        }
    }
    return make_texture(width, height, std::move(pixels));
}

Texture2D building_facade(const BuildingFacadeDesc& desc) {
    const u32 dim = safe_dim(desc.dimension);
    const u32 columns = safe_dim(desc.columns);
    const u32 rows = safe_dim(desc.rows);
    const u32 roof_h = desc.roof_height == 0u ? std::max(3u, dim / 8u)
                                              : std::min(desc.roof_height, dim);
    const u32 cell_w = std::max(1u, dim / columns);
    const u32 cell_h = std::max(1u, (dim - roof_h) / rows);
    const u32 pane_inset = std::max(2u, cell_w / 6u);
    std::vector<u32> pixels(static_cast<usize>(dim) * dim);

    for (u32 y = 0; y < dim; ++y) {
        for (u32 x = 0; x < dim; ++x) {
            u32 pixel = add_luma(desc.concrete,
                                 static_cast<i32>((hash01(x, y, 101u) - 0.5f) *
                                                  desc.concrete_speckle));

            if (y < roof_h) {
                pixel = add_luma(desc.roof,
                                 static_cast<i32>((hash01(x, y, 102u) - 0.5f) *
                                                  desc.roof_speckle));
            } else {
                const u32 wy = y - roof_h;
                const u32 col_in = x % cell_w;
                const u32 row_in = wy % cell_h;
                const bool in_window_region = wy < rows * cell_h;
                const bool in_pane =
                    in_window_region && col_in >= pane_inset && col_in < cell_w - pane_inset &&
                    row_in >= pane_inset && row_in < cell_h - pane_inset;
                const bool in_mullion =
                    in_window_region && (col_in < pane_inset || col_in >= cell_w - pane_inset ||
                                         row_in < pane_inset || row_in >= cell_h - pane_inset);

                if (in_pane) {
                    const f32 gy = static_cast<f32>(row_in) / static_cast<f32>(cell_h);
                    const i32 grad = static_cast<i32>((0.5f - gy) * 24.0f);
                    const i32 speckle =
                        static_cast<i32>((hash01(x, y, 103u) - 0.5f) * desc.window_speckle);
                    pixel = add_luma(desc.window, grad + speckle);
                } else if (in_mullion) {
                    pixel = desc.mullion;
                }
            }

            pixels[static_cast<usize>(y) * dim + x] = pixel;
        }
    }
    return make_texture(dim, dim, std::move(pixels));
}

}  // namespace psynder::render::texture_generators
