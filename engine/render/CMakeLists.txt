# SPDX-License-Identifier: MIT
# Render subsystem aggregator. Adds raster, rt, post sublanes.
add_subdirectory(raster)
add_subdirectory(rt)
add_subdirectory(post)

# Convenience union-target so dependents can just link psynder_render.
add_library(psynder_render INTERFACE)
target_link_libraries(psynder_render INTERFACE
    psynder_render_raster
    psynder_render_rt
    psynder_render_post
)
add_library(psynder::render ALIAS psynder_render)
