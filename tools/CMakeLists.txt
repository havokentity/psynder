# SPDX-License-Identifier: MIT
# Offline tools — Lane 24.
add_subdirectory(lm_pak)
add_subdirectory(lm_cook)
add_subdirectory(lm_qbsp)
add_subdirectory(lm_bake)
add_subdirectory(lm_mapimport)
# dmonte_pt is an optional reference path tracer; opt in explicitly.
option(PSYNDER_BUILD_DMONTE_PT "Build the optional dmonte_pt reference path tracer (Embree dep)" OFF)
if(PSYNDER_BUILD_DMONTE_PT)
    add_subdirectory(dmonte_pt)
endif()
