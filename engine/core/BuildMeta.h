// SPDX-License-Identifier: MIT
// Psynder — source-visible build metadata declarations.

#pragma once

namespace psynder::build {

struct RuntimeBundleRequest {
    const char* output_folder;
};

}  // namespace psynder::build

#define PSYNDER_DETAIL_JOIN2(a, b) a##b
#define PSYNDER_DETAIL_JOIN(a, b) PSYNDER_DETAIL_JOIN2(a, b)

#define PSYNDER_RUNTIME_BUNDLE(output_folder_literal)                               \
    [[maybe_unused]] static constexpr ::psynder::build::RuntimeBundleRequest        \
        PSYNDER_DETAIL_JOIN(psynder_runtime_bundle_, __COUNTER__){                  \
            output_folder_literal}
