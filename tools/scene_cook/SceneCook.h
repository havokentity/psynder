// SPDX-License-Identifier: MIT
// Psynder scene cooker library API.

#pragma once

#include "core/Types.h"

#include <filesystem>
#include <string>

namespace psynder::tools {

struct SceneCookStats {
    usize bytes = 0u;
    usize transforms = 0u;
    usize cameras = 0u;
    usize mesh_instances = 0u;
    usize behavior_spin_ops = 0u;
};

[[nodiscard]] bool cook_psyscene_json_file(const std::filesystem::path& input,
                                           const std::filesystem::path& output,
                                           SceneCookStats* stats = nullptr,
                                           std::string* error = nullptr);

int scene_cook_cli_main(int argc, char** argv);

}  // namespace psynder::tools
