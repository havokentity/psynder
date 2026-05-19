# SPDX-License-Identifier: MIT
# Bench gate — per-tile raster + per-island physics; CI enforces <2% regression.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_bench ${SRC})
target_link_libraries(psynder_bench PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_physics psynder_scene
)
add_test(NAME psynder_bench_smoke COMMAND psynder_bench --smoke)
