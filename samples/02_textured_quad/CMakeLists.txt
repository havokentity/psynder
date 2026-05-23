# SPDX-License-Identifier: MIT
# Sample 02 — M2 demo. Rotating crate room with bilinear + mipmaps + Z.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_02_textured_quad ${SRC})
target_link_libraries(sample_02_textured_quad PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_scene psynder_render_raster psynder_render_post psynder_asset psynder_platform
    psynder_samples_common
    psynder_ui_imm psynder_editor_core
)

set(_sample02_scene_src "${CMAKE_CURRENT_SOURCE_DIR}/assets/crate_room.psyscene.json")
set(_sample02_scene_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/crate_room.psyscene")
if(TARGET scene_cook)
    add_custom_command(
        OUTPUT "${_sample02_scene_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
        COMMAND $<TARGET_FILE:scene_cook> "${_sample02_scene_src}" "${_sample02_scene_out}"
        DEPENDS scene_cook "${_sample02_scene_src}"
        COMMENT "Cooking sample 02 PsyScene"
        VERBATIM
    )
    add_custom_target(sample_02_textured_quad_assets DEPENDS "${_sample02_scene_out}")
    add_dependencies(sample_02_textured_quad sample_02_textured_quad_assets)
endif()

# Smoke add_test() lives in tests/CMakeLists.txt because the root project
# only calls enable_testing() after add_subdirectory(samples).
