# SPDX-License-Identifier: MIT
# Sample 14 — offline-baked lightmaps. Builds a small 3D room, bakes a
# lightmap for it in-process with the real lm_bake library, round-trips the
# atlas through a .lmlight file, then renders the room lit by the reloaded
# lightmap via the hybrid renderer facade (per-vertex albedo*lumel). B toggles
# baked vs. flat. Links lm_bake_lib for bake()/write_lmlight()/read_lmlight().
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_14_lightmap_quake ${SRC})
target_link_libraries(sample_14_lightmap_quake PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt
    psynder_world_bsp psynder_scene psynder_asset psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
    lm_bake_lib
)
