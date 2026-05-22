// SPDX-License-Identifier: MIT
// Psynder — PNG writer unit tests.

#include "render/PngWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace psynder;

namespace {

std::vector<u8> read_binary_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path unique_png_path(const char* stem) {
    const auto unique = std::to_string(reinterpret_cast<std::uintptr_t>(stem));
    return std::filesystem::temp_directory_path() / (std::string(stem) + "_" + unique + ".png");
}

}  // namespace

TEST_CASE("png writer rejects invalid inputs", "[render][png]") {
    const std::array<u8, 3> rgb = {255, 0, 0};
    const std::array<u32, 1> rgba = {0xFF0000FFu};

    REQUIRE_FALSE(render::write_png_rgb8(nullptr, rgb.data(), 1, 1));
    REQUIRE_FALSE(render::write_png_rgb8("ignored.png", nullptr, 1, 1));
    REQUIRE_FALSE(render::write_png_rgb8("ignored.png", rgb.data(), 0, 1));
    REQUIRE_FALSE(render::write_png_rgb8("ignored.png", rgb.data(), 1, 0));

    REQUIRE_FALSE(render::write_png_rgba8_framebuffer(nullptr, rgba.data(), 1, 1));
    REQUIRE_FALSE(render::write_png_rgba8_framebuffer("ignored.png", nullptr, 1, 1));
    REQUIRE_FALSE(render::write_png_rgba8_framebuffer("ignored.png", rgba.data(), 0, 1));
    REQUIRE_FALSE(render::write_png_rgba8_framebuffer("ignored.png", rgba.data(), 1, 0));
}

TEST_CASE("png writer emits deterministic png envelope", "[render][png]") {
    const std::filesystem::path out = unique_png_path("psynder_png_writer_test");
    std::filesystem::remove(out);

    const std::array<u32, 4> pixels = {
        0xFF332211u,
        0xFF665544u,
        0xFF998877u,
        0xFFCCBBAAu,
    };

    REQUIRE(render::write_png_rgba8_framebuffer(out.string().c_str(), pixels.data(), 2, 2));

    const std::vector<u8> bytes = read_binary_file(out);
    std::filesystem::remove(out);

    REQUIRE(bytes.size() > 33u);
    const std::array<u8, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
    for (usize i = 0; i < signature.size(); ++i) {
        REQUIRE(bytes[i] == signature[i]);
    }

    // IHDR chunk: length=13, tag="IHDR", width=2, height=2, bit-depth=8,
    // color-type=2 (RGB truecolor).
    REQUIRE(bytes[8] == 0u);
    REQUIRE(bytes[9] == 0u);
    REQUIRE(bytes[10] == 0u);
    REQUIRE(bytes[11] == 13u);
    REQUIRE(bytes[12] == static_cast<u8>('I'));
    REQUIRE(bytes[13] == static_cast<u8>('H'));
    REQUIRE(bytes[14] == static_cast<u8>('D'));
    REQUIRE(bytes[15] == static_cast<u8>('R'));
    REQUIRE(bytes[16] == 0u);
    REQUIRE(bytes[17] == 0u);
    REQUIRE(bytes[18] == 0u);
    REQUIRE(bytes[19] == 2u);
    REQUIRE(bytes[20] == 0u);
    REQUIRE(bytes[21] == 0u);
    REQUIRE(bytes[22] == 0u);
    REQUIRE(bytes[23] == 2u);
    REQUIRE(bytes[24] == 8u);
    REQUIRE(bytes[25] == 2u);
}
