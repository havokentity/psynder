# SPDX-License-Identifier: MIT
# Render subsystem aggregator. Adds root render utilities plus raster, rt, post sublanes.
add_library(psynder_render_core STATIC
    TextureGenerators.cpp
)
add_library(psynder::render_core ALIAS psynder_render_core)
target_link_libraries(psynder_render_core PUBLIC psynder_common psynder_core)
target_include_directories(psynder_render_core PUBLIC ${CMAKE_SOURCE_DIR}/engine)
set_target_properties(psynder_render_core PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_subdirectory(raster)
add_subdirectory(rt)
add_subdirectory(post)
add_subdirectory(hybrid)

# Convenience union-target so dependents can just link psynder_render.
add_library(psynder_render INTERFACE)
target_link_libraries(psynder_render INTERFACE
    psynder_render_core
    psynder_render_raster
    psynder_render_rt
    psynder_render_post
    psynder_render_hybrid
)
add_library(psynder::render ALIAS psynder_render)
