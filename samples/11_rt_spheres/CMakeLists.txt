# SPDX-License-Identifier: MIT
# Sample 11 — reflective sphere room you fly/walk through (CPU raytracing).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_11_rt_spheres ${SRC})
target_link_libraries(sample_11_rt_spheres PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post
    psynder_scene psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)
