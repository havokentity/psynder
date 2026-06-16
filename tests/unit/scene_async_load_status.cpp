// SPDX-License-Identifier: MIT
// Psynder — scene async load status regression tests.

#include <catch2/catch_test_macros.hpp>

#include "asset/Vfs.h"
#include "asset/VfsInternal.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneFile.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace psynder;

namespace {

namespace fs = std::filesystem;

struct AssetReset {
    AssetReset() { asset::internal::reset_for_tests(); }
    ~AssetReset() { asset::internal::reset_for_tests(); }
};

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

fs::path make_scratch_dir(std::string_view tag) {
    static u32 counter = 0;
    fs::path dir = fs::temp_directory_path() / "psynder_scene_async_load_status";
    dir /= std::string{tag} + "_" + std::to_string(++counter);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_bytes(const fs::path& path, std::span<const u8> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> minimal_scene_file() {
    scene::SceneFileHeader header{};
    header.file_bytes = sizeof(header);

    std::vector<u8> bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

bool wait_until(std::chrono::milliseconds timeout, bool (*predicate)(void*), void* user) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate(user))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate(user);
}

}  // namespace

TEST_CASE("scene async file request reports ready once and consume resets status",
          "[scene][scene_file][async]") {
    AssetReset assets;
    const fs::path dir = make_scratch_dir("ready");
    const std::vector<u8> bytes = minimal_scene_file();
    write_bytes(dir / "empty.psyscene", std::span<const u8>{bytes.data(), bytes.size()});
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    scene::SceneFileRequest request;
    REQUIRE(request.state() == scene::SceneFileRequest::State::Idle);

    request.load_async("empty.psyscene");
    auto ready_or_failed = +[](void* user) {
        auto* req = static_cast<scene::SceneFileRequest*>(user);
        return req->ready() || req->failed();
    };
    REQUIRE(wait_until(std::chrono::seconds(1), ready_or_failed, &request));
    REQUIRE(request.ready());
    REQUIRE_FALSE(request.failed());

    scene::SceneFileLoaded loaded{};
    REQUIRE(request.consume(loaded));
    REQUIRE(request.state() == scene::SceneFileRequest::State::Idle);
    REQUIRE_FALSE(request.consume(loaded));
    REQUIRE(loaded.bytes.size() == bytes.size());
    REQUIRE(loaded.view.header != nullptr);
    REQUIRE(loaded.view.mesh_instances.empty());
}

TEST_CASE("scene async load request surfaces read failures without instantiating",
          "[scene][scene_file][async]") {
    AssetReset assets;
    RegistryReset registry_reset;
    const fs::path dir = make_scratch_dir("failed");
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene_ref{registry};
    scene::SceneLoadRequest request;
    std::vector<scene::SceneLoadStage> progress;
    std::string error;
    request.on_progress([&](const scene::SceneLoadProgress& update) {
        progress.push_back(update.stage);
    });
    request.on_error([&](std::string_view message) {
        error.assign(message);
    });

    request.load_async("missing.psyscene");
    REQUIRE(request.stage() == scene::SceneLoadStage::Reading);
    auto completed = +[](void* user) {
        auto* pair = static_cast<std::pair<scene::SceneLoadRequest*, scene::Scene*>*>(user);
        pair->first->update(*pair->second);
        return pair->first->ready() || pair->first->failed();
    };
    std::pair<scene::SceneLoadRequest*, scene::Scene*> state{&request, &scene_ref};
    REQUIRE(wait_until(std::chrono::seconds(1), completed, &state));

    REQUIRE(request.failed());
    REQUIRE_FALSE(request.ready());
    REQUIRE_FALSE(error.empty());
    REQUIRE(request.error() == error);
    REQUIRE(progress.size() >= 3u);
    REQUIRE(progress.front() == scene::SceneLoadStage::Queued);
    REQUIRE(progress[1] == scene::SceneLoadStage::Reading);
    REQUIRE(progress.back() == scene::SceneLoadStage::Failed);
    REQUIRE(registry.snapshot_live_entities(std::span<Entity>{}) == 0u);
}
