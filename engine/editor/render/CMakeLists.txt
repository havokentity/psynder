# SPDX-License-Identifier: MIT
# psynder_editor_render -- editor-side render glue. Gathers the active scene
# into the software raytracer's TLAS + the lightmap baker's BakeScene, then
# calls the existing engine kernels (render::RenderingSystem::render_rt and
# tools::bake::bake). Frozen entry points the editor host calls.
#
# Linkage: render (RenderingSystem + raster + rt FrameRenderer), scene (Scene),
# and tools/lm_bake (the bake kernel). lm_bake_lib is defined later in the
# top-level configure (under PSYNDER_BUILD_TOOLS) so we resolve it via a
# deferred call once the whole tree has been processed; the bake include dir
# is added directly so EditorRender.cpp's `#include "Bake.h"` always compiles.
psynder_add_lane(editor_render
    LINKS
        psynder_core
        psynder_math
        psynder_scene
        psynder_render
)

# PUBLIC so dependents that include EditorRender.h (which pulls in Bake.h)
# resolve the bake header without their own per-target edit.
target_include_directories(psynder_editor_render PUBLIC
    ${CMAKE_SOURCE_DIR}/tools/lm_bake
    ${CMAKE_CURRENT_SOURCE_DIR})

# Link lm_bake_lib once the tools subtree has been added. When tools are off
# the bake kernel is unavailable; the link is simply skipped and the bake path
# fails to resolve at link time only if it is actually referenced -- guard the
# whole module behind PSYNDER_BUILD_TOOLS by requiring the target.
function(_psynder_editor_render_link_bake)
    if(TARGET lm_bake_lib)
        target_link_libraries(psynder_editor_render PUBLIC lm_bake_lib)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL _psynder_editor_render_link_bake)
