# SPDX-License-Identifier: MIT
# Header-only utilities shared by sample binaries.
#
# `psynder_samples_common` is an INTERFACE library — no .cpp, no .a, just
# include path propagation so any sample can `#include "common/PngWriter.h"`.
# Owned by lane 25 (samples-tests).
add_library(psynder_samples_common INTERFACE)
target_include_directories(psynder_samples_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(psynder_samples_common INTERFACE psynder_core)
