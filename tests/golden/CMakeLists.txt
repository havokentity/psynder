# SPDX-License-Identifier: MIT
# Golden-image renderer tests. Compare CPU rasterizer output vs reference
# images (PPM on disk — see runner.cpp for rationale). Tolerance: 0.5%
# per-pixel mismatch, per DESIGN.md §14.
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

# Point the runner at the in-tree cases + references. The runner also has a
# cwd-walking fallback, so this is just an optimization.
add_test(NAME psynder_golden
         COMMAND psynder_golden
                 --cases-dir=${CMAKE_CURRENT_SOURCE_DIR}/cases
                 --references-dir=${CMAKE_CURRENT_SOURCE_DIR}/references
                 --tolerance-pct=0.5)
set_tests_properties(psynder_golden PROPERTIES
    TIMEOUT 60
)
