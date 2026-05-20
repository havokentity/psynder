# SPDX-License-Identifier: MIT
# Sample 03 — M3 demo. Walking-POV BSP room. Synthetic 4-leaf BSP map built
# in-memory; WASD + mouse-look camera; PVS-driven leaf walk; faces submitted
# through Rasterizer::submit() with per-face vertex colours so the BSP
# structure reads at a glance.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_03_quake_room ${SRC})
target_link_libraries(sample_03_quake_room PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster
    psynder_world_bsp psynder_scene psynder_asset psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
