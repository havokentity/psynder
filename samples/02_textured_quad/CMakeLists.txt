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
    psynder_render_raster psynder_render_post psynder_asset psynder_platform
)

# Smoke add_test() lives in tests/CMakeLists.txt because the root project
# only calls enable_testing() after add_subdirectory(samples).
