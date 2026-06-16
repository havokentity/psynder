# SPDX-License-Identifier: MIT
# Bench gate — per-tile raster + per-island physics + per-lane microbenches.
# Each *.cpp under tests/bench/ becomes its own executable so each lane can
# drop a bench file without colliding on main(). The canonical roll-up
# binary is `psynder_bench` (runner.cpp); per-lane benches use the file
# basename as the target name (simd_throughput.cpp → psynder_bench_simd_throughput).
file(GLOB BENCH_SOURCES CONFIGURE_DEPENDS *.cpp)
if(NOT BENCH_SOURCES)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(BENCH_SOURCES "${STUB}")
endif()

# Common link set for every bench TU.
set(BENCH_COMMON_LINKS
    psynder_core psynder_math psynder_simd psynder_jobs
    psynder_render_raster psynder_render_rt psynder_scene psynder_physics
)

foreach(_src IN LISTS BENCH_SOURCES)
    get_filename_component(_name "${_src}" NAME_WE)
    if(_name STREQUAL "runner")
        set(_target psynder_bench)
    else()
        set(_target psynder_bench_${_name})
    endif()
    add_executable(${_target} ${_src})
    target_link_libraries(${_target} PRIVATE ${BENCH_COMMON_LINKS})
endforeach()

# Smoke test the roll-up bench. Per-lane benches don't get ctest entries
# unless the owning lane adds them; the bench gate doesn't run them by
# default to keep ctest snappy.
if(TARGET psynder_bench)
    add_test(NAME psynder_bench_smoke
             COMMAND psynder_bench --smoke
                     --json-out=${CMAKE_CURRENT_BINARY_DIR}/psynder_bench_smoke.json)
    set_tests_properties(psynder_bench_smoke PROPERTIES
        TIMEOUT 60
    )
endif()
