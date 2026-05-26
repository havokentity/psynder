// SPDX-License-Identifier: MIT
// Psynder - scene load failure status propagation tests.

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
    static u32 counter = 0u;
    fs::path dir = fs::temp_directory_path() / "psynder_scene_load_failure_status";
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

std::vector<u8> malformed_scene_bytes() {
    scene::SceneFileHeader header{};
    header.magic = 0xDEADBEEFu;
    header.file_bytes = sizeof(header);

    std::vector<u8> bytes(sizeof(scene::SceneFileHeader), 0u);
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

TEST_CASE("scene async file request preserves parse failure status and error",
          "[scene][scene_file][async][failure]") {
    AssetReset assets;
    const fs::path dir = make_scratch_dir("file_parse_failure");
    const std::vector<u8> bytes = malformed_scene_bytes();
    write_bytes(dir / "bad_magic.psyscene", std::span<const u8>{bytes.data(), bytes.size()});
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    scene::SceneFileRequest request;
    request.load_async("bad_magic.psyscene");

    auto ready_or_failed = +[](void* user) {
        auto* req = static_cast<scene::SceneFileRequest*>(user);
        return req->ready() || req->failed();
    };
    REQUIRE(wait_until(std::chrono::seconds(1), ready_or_failed, &request));

    REQUIRE(request.state() == scene::SceneFileRequest::State::Failed);
    REQUIRE(request.failed());
    REQUIRE_FALSE(request.ready());
    REQUIRE(request.error() == "psyscene: bad magic");

    scene::SceneFileLoaded loaded{};
    REQUIRE_FALSE(request.consume(loaded));
    REQUIRE(loaded.bytes.empty());
    REQUIRE(loaded.view.header == nullptr);
    REQUIRE(request.state() == scene::SceneFileRequest::State::Failed);
}

TEST_CASE("scene load request propagates parse failures without ready side effects",
          "[scene][scene_file][async][failure]") {
    AssetReset assets;
    RegistryReset registry_reset;
    const fs::path dir = make_scratch_dir("load_parse_failure");
    const std::vector<u8> bytes = malformed_scene_bytes();
    write_bytes(dir / "bad_magic.psyscene", std::span<const u8>{bytes.data(), bytes.size()});
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene_ref{registry};
    scene::SceneLoadRequest request;
    std::vector<scene::SceneLoadStage> progress;
    std::string error;
    u32 ready_callbacks = 0u;
    u32 error_callbacks = 0u;

    request.on_progress([&](const scene::SceneLoadProgress& update) {
        progress.push_back(update.stage);
    });
    request.on_ready([&](const scene::SceneLoadResult&) {
        ++ready_callbacks;
    });
    request.on_error([&](std::string_view message) {
        ++error_callbacks;
        error.assign(message);
    });

    request.load_async("bad_magic.psyscene");
    REQUIRE(request.pending());
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
    REQUIRE_FALSE(request.pending());
    REQUIRE(request.stage() == scene::SceneLoadStage::Failed);
    REQUIRE(request.error() == "psyscene: bad magic");
    REQUIRE(error == request.error());
    REQUIRE(error_callbacks == 1u);
    REQUIRE(ready_callbacks == 0u);

    REQUIRE(progress.size() == 3u);
    REQUIRE(progress[0] == scene::SceneLoadStage::Queued);
    REQUIRE(progress[1] == scene::SceneLoadStage::Reading);
    REQUIRE(progress[2] == scene::SceneLoadStage::Failed);

    REQUIRE(request.result().instantiate.cameras == 0u);
    REQUIRE(request.result().instantiate.mesh_instances == 0u);
    REQUIRE(request.result().mesh_entities.empty());
    REQUIRE(request.loaded_file().view.header == nullptr);
    REQUIRE(registry.snapshot_live_entities(std::span<Entity>{}) == 0u);

    REQUIRE_FALSE(request.update(scene_ref));
    REQUIRE(error_callbacks == 1u);
    REQUIRE(ready_callbacks == 0u);
}
