// SPDX-License-Identifier: MIT
// Psynder — source-visible asset precook declarations.

#pragma once

namespace psynder::asset {

struct PrecookPsySceneRequest {
    const char* output_folder;
    const char* source;
};

}  // namespace psynder::asset

#define PSYNDER_DETAIL_JOIN2(a, b) a##b
#define PSYNDER_DETAIL_JOIN(a, b) PSYNDER_DETAIL_JOIN2(a, b)

#define PSYNDER_PRECOOK_PSYSCENE(path_literal)                                      \
    [[maybe_unused]] static constexpr ::psynder::asset::PrecookPsySceneRequest      \
        PSYNDER_DETAIL_JOIN(psynder_precook_psyscene_, __COUNTER__){nullptr,        \
                                                                    path_literal}

#define PSYNDER_PRECOOK_PSYSCENE_TO(output_folder_literal, path_literal)            \
    [[maybe_unused]] static constexpr ::psynder::asset::PrecookPsySceneRequest      \
        PSYNDER_DETAIL_JOIN(psynder_precook_psyscene_, __COUNTER__){                \
            output_folder_literal, path_literal}
