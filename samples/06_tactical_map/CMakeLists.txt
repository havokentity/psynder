# SPDX-License-Identifier: MIT
# Sample 06 — M6 demo. Patrol a 4km×4km tactical map at dawn.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_06_tactical_map ${SRC})
target_link_libraries(sample_06_tactical_map PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_render_post
    psynder_world_outdoor psynder_scene psynder_physics psynder_asset
    psynder_audio psynder_editor psynder_platform
)
