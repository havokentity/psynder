# SPDX-License-Identifier: MIT
# Sample 13 — Quake-style indoor room lit by runtime raytraced shadows,
# walked first-person via the shared character controller.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_13_rt_quake ${SRC})
target_link_libraries(sample_13_rt_quake PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post
    psynder_world_outdoor psynder_scene psynder_physics psynder_asset psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
