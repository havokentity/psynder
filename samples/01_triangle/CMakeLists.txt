# SPDX-License-Identifier: MIT
# Sample 01 — M1 demo. Rotating textured triangle.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(sample_01_triangle ${SRC})
target_link_libraries(sample_01_triangle PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_platform
    psynder_editor_core
    psynder_samples_common
)

# Stage the loose texture next to the binary. The app host mounts the binary
# directory and the source asset root into the runtime Vault.
add_custom_command(TARGET sample_01_triangle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:sample_01_triangle>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/crate.ppm
        $<TARGET_FILE_DIR:sample_01_triangle>/assets/crate.ppm
    VERBATIM)

# Smoke add_test() lives in tests/CMakeLists.txt because the root project
# only calls enable_testing() after add_subdirectory(samples).
