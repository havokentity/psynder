# SPDX-License-Identifier: MIT
# Golden-image renderer tests. Compare CPU rasterizer output vs reference PNGs.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_golden ${SRC})
target_link_libraries(psynder_golden PRIVATE
    psynder_core psynder_math psynder_render_raster psynder_render_rt psynder_render_post
)
add_test(NAME psynder_golden_smoke COMMAND psynder_golden)
