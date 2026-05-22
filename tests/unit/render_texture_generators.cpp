// SPDX-License-Identifier: MIT
// Psynder - render texture generator tests.

#include "render/Color.h"
#include "render/Texture.h"
#include "render/TextureGenerators.h"

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

using namespace psynder;
using namespace psynder::render;

namespace {

[[nodiscard]] usize unique_color_count(const Texture2D& texture) {
    std::unordered_set<u32> colors;
    colors.reserve(texture.pixels().size());
    for (const u32 pixel : texture.pixels())
        colors.insert(pixel);
    return colors.size();
}

}  // namespace

TEST_CASE("texture generators: solid and checker produce expected texels", "[render][texture]") {
    const Texture2D solid = texture_generators::solid({.width = 2, .height = 3, .color = rgba8(1, 2, 3)});
    REQUIRE(solid.width() == 2u);
    REQUIRE(solid.height() == 3u);
    REQUIRE(solid.pixels().size() == 6u);
    for (const u32 pixel : solid.pixels())
        REQUIRE(pixel == rgba8(1, 2, 3));

    texture_generators::CheckerDesc checker_desc{};
    checker_desc.width = 4;
    checker_desc.height = 4;
    checker_desc.cell_size = 2;
    checker_desc.color_a = rgba8(10, 20, 30);
    checker_desc.color_b = rgba8(40, 50, 60);
    const Texture2D checker = texture_generators::checker(checker_desc);
    REQUIRE(checker.pixels()[0] == checker_desc.color_a);
    REQUIRE(checker.pixels()[2] == checker_desc.color_b);
    REQUIRE(checker.pixels()[8] == checker_desc.color_b);
    REQUIRE(checker.pixels()[10] == checker_desc.color_a);

    const Texture2D fallback = fallback_checker_texture();
    REQUIRE(fallback.width() == 16u);
    REQUIRE(fallback.height() == 16u);
    REQUIRE(fallback.pixels()[0] == rgba8(0, 0, 0));
    REQUIRE(fallback.pixels()[1] == rgba8(0xFF, 0, 0xFF));
}

TEST_CASE("texture generators: rich procedural textures are deterministic", "[render][texture]") {
    const Texture2D crate_a = texture_generators::wooden_crate();
    const Texture2D crate_b = texture_generators::wooden_crate();
    REQUIRE(crate_a.width() == 128u);
    REQUIRE(crate_a.height() == 128u);
    REQUIRE(crate_a.pixels() == crate_b.pixels());
    REQUIRE(unique_color_count(crate_a) > 32u);

    texture_generators::BrickDesc bricks_desc{};
    bricks_desc.width = 64;
    bricks_desc.height = 32;
    const Texture2D bricks = texture_generators::bricks(bricks_desc);
    REQUIRE(bricks.width() == 64u);
    REQUIRE(bricks.height() == 32u);
    REQUIRE(unique_color_count(bricks) > 4u);

    const Texture2D facade_a = texture_generators::building_facade();
    const Texture2D facade_b = texture_generators::building_facade();
    REQUIRE(facade_a.width() == 64u);
    REQUIRE(facade_a.height() == 64u);
    REQUIRE(facade_a.pixels() == facade_b.pixels());
    REQUIRE(unique_color_count(facade_a) > 16u);

    texture_generators::ValueNoiseDesc noise_desc{};
    noise_desc.width = 16;
    noise_desc.height = 16;
    noise_desc.seed = 123;
    const Texture2D noise_a = texture_generators::value_noise(noise_desc);
    const Texture2D noise_b = texture_generators::value_noise(noise_desc);
    REQUIRE(noise_a.pixels() == noise_b.pixels());
    REQUIRE(unique_color_count(noise_a) > 16u);
}
