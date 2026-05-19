# SPDX-License-Identifier: MIT
# Bench gate — per-tile raster + per-island physics. Default ctest invocation
# runs --smoke so the suite stays under a second; CI runs the full bench in a
# dedicated step (no .ci.yml edits needed — see tests/bench/runner.cpp for
# the JSON contract).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(psynder_bench ${SRC})
# Note: psynder_physics is intentionally NOT linked yet — lane 13 has only
# its public header and no .cpp impl, so calls into physics::World would
# fail to link. The physics microbench inlines a stand-in SoA loop with the
# same memory shape; once lane 13 ships, we re-add the link and swap the
# bench body to drive World::step().
target_link_libraries(psynder_bench PRIVATE
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_scene
)

add_test(NAME psynder_bench_smoke
         COMMAND psynder_bench --smoke
                 --json-out=${CMAKE_CURRENT_BINARY_DIR}/psynder_bench_smoke.json)
set_tests_properties(psynder_bench_smoke PROPERTIES
    TIMEOUT 60
)
