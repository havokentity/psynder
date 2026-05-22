// SPDX-License-Identifier: MIT
// Psynder — CPU image loader unit tests.

#include "asset/Vault.h"
#include "render/Texture.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace psynder;

namespace {

std::filesystem::path unique_ppm_path(const char* stem) {
    const auto unique = std::to_string(reinterpret_cast<std::uintptr_t>(stem));
    return std::filesystem::temp_directory_path() / (std::string(stem) + "_" + unique + ".ppm");
}

void write_bytes(const std::filesystem::path& path,
                 const std::string& header,
                 const std::array<u8, 12>& rgb) {
    std::ofstream out(path, std::ios::binary);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

render::TextureLoadStatus wait_until_done(render::TextureLoad& request) {
    for (u32 spin = 0; spin < 200u; ++spin) {
        const render::TextureLoadStatus status = request.status();
        if (status != render::TextureLoadStatus::Pending)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return request.status();
}

render::TextureLoadStatus wait_until_done(const render::TextureAsset& texture) {
    for (u32 spin = 0; spin < 200u; ++spin) {
        const render::TextureLoadStatus status = texture.status();
        if (status != render::TextureLoadStatus::Pending)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return texture.status();
}

}  // namespace

TEST_CASE("async ppm texture loader expands P6 RGB8 into RGBA8 pixels", "[render][image]") {
    const std::filesystem::path path = unique_ppm_path("psynder_ppm_loader");
    const std::array<u8, 12> rgb = {
        255,
        0,
        0,
        0,
        255,
        0,
        0,
        0,
        255,
        17,
        34,
        51,
    };
    write_bytes(path, "P6\n# comment between tokens\n2 2\n255\n", rgb);

    REQUIRE(asset::Vault::Get().mount_directory(path.parent_path().string()));
    render::TextureLoad request = render::load_ppm_texture_async(path.filename().string());
    REQUIRE(wait_until_done(request) == render::TextureLoadStatus::Ready);
    std::filesystem::remove(path);

    render::Texture2D texture{};
    REQUIRE(request.take_if_ready(texture));
    REQUIRE(texture.width() == 2u);
    REQUIRE(texture.height() == 2u);
    REQUIRE(texture.pixels().size() == 4u);
    REQUIRE(texture.pixels()[0] == 0xFF0000FFu);
    REQUIRE(texture.pixels()[1] == 0xFF00FF00u);
    REQUIRE(texture.pixels()[2] == 0xFFFF0000u);
    REQUIRE(texture.pixels()[3] == 0xFF332211u);
}

TEST_CASE("async ppm texture loader rejects unsupported PPM variants", "[render][image]") {
    const std::filesystem::path path = unique_ppm_path("psynder_ppm_loader_bad");
    const std::array<u8, 12> rgb = {};
    write_bytes(path, "P6\n2 2\n65535\n", rgb);

    REQUIRE(asset::Vault::Get().mount_directory(path.parent_path().string()));
    render::TextureLoad request = render::load_ppm_texture_async(path.filename().string());
    REQUIRE(wait_until_done(request) == render::TextureLoadStatus::Failed);
    std::filesystem::remove(path);

    render::Texture2D texture{};
    REQUIRE_FALSE(request.take_if_ready(texture));
    REQUIRE_FALSE(texture.valid());
}

TEST_CASE("texture asset resolves async PPM loads behind a stable view", "[render][image]") {
    const std::filesystem::path path = unique_ppm_path("psynder_texture_asset");
    const std::array<u8, 12> rgb = {
        4,
        8,
        16,
        32,
        64,
        128,
        255,
        128,
        64,
        1,
        2,
        3,
    };
    write_bytes(path, "P6\n2 2\n255\n", rgb);

    REQUIRE(asset::Vault::Get().mount_directory(path.parent_path().string()));
    render::TextureAsset texture{};
    texture.load_ppm(path.filename().string());
    REQUIRE(wait_until_done(texture) == render::TextureLoadStatus::Ready);
    std::filesystem::remove(path);

    const render::TextureView view = texture.view();
    REQUIRE(view.valid());
    REQUIRE(view.width == 2u);
    REQUIRE(view.height == 2u);
    REQUIRE(view.texels[0] == 0xFF100804u);
    REQUIRE(view.texels[3] == 0xFF030201u);
}
