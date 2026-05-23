// SPDX-License-Identifier: MIT
// Psynder — source-visible asset precook declarations.

#pragma once

#include "core/BuildMeta.h"

namespace psynder::asset {

struct PrecookPsySceneRequest {
    const char* output_folder;
    const char* source;
};

}  // namespace psynder::asset

#define PSYNDER_PRECOOK_PSYSCENE(path_literal)                                      \
    [[maybe_unused]] static constexpr ::psynder::asset::PrecookPsySceneRequest      \
        PSYNDER_DETAIL_JOIN(psynder_precook_psyscene_, __COUNTER__){nullptr,        \
                                                                    path_literal}
