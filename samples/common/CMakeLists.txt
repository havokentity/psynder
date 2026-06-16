# SPDX-License-Identifier: MIT
# Header-only utilities shared by sample binaries.
#
# `psynder_samples_common` is an INTERFACE library — no .cpp, no .a, just
# include path propagation so any sample can `#include "common/PngWriter.h"`
# or `#include "common/CharacterController.h"`.
# Owned by lane 25 (samples-tests).
#
# CharacterController.h pulls in the public math + platform + console headers,
# so the interface lib propagates those deps too. Every current consumer
# already links them explicitly; listing them here keeps the shared header
# self-sufficient for samples that adopt it later (e.g. 05 / 06).
add_library(psynder_samples_common INTERFACE)
target_include_directories(psynder_samples_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(psynder_samples_common INTERFACE
    PSYNDER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(psynder_samples_common INTERFACE
    psynder_core psynder_math psynder_asset psynder_platform)
