# SPDX-License-Identifier: MIT
# Psynder — golden-image regression harness CMake helpers.
# Adapted from dmonte's `pt_add_golden_cell` (tests/CMakeLists.txt). The
# layout is mirrored:
#
#   tests/golden/cases/<scene>.cfg                    -- fixture cfg replayed
#                                                        via --smoke-exec
#   tests/golden/<host>/<scene>__<backend>.png        -- committed reference
#   <build>/golden_actual/<scene>__<backend>.png      -- this run's output
#   <build>/golden_diff/<scene>__<backend>.png        -- pixel-diff overlay
#
# <host> = CMAKE_SYSTEM_NAME (Darwin / Linux / Windows). Goldens are host-
# specific because FP rounding + driver versions vary; isolating baselines
# per host means "regenerate on this host only" is the safe default.
#
# Two tests per cell:
#   <cell>_render  -- invokes the sample binary in --smoke-frames mode,
#                     writes the actual PNG.
#   <cell>_diff    -- DEPENDS on _render; invokes the `imgdiff` CLI to
#                     compare actual vs the host's stored golden.
#
# Missing-golden cells fire `tests/golden/missing_golden.cmake` which exits
# non-zero with a regen hint; WILL_FAIL inverts that to PASS so a missing
# golden surfaces to the maintainer without breaking ctest.

include_guard(GLOBAL)

function(psynder_add_golden_cell)
    set(_options)
    set(_oneval SCENE BACKEND FRAMES MAX_DELTA MEAN_DELTA FAIL_PERCENT)
    set(_multival EXTRA)
    cmake_parse_arguments(GOLD "${_options}" "${_oneval}" "${_multival}" ${ARGN})

    foreach(_req SCENE BACKEND)
        if("${GOLD_${_req}}" STREQUAL "")
            message(FATAL_ERROR
                "psynder_add_golden_cell: SCENE/BACKEND required (missing ${_req})")
        endif()
    endforeach()

    # Default tolerances. Use NOT DEFINED so a caller passing 0 takes effect.
    if(NOT DEFINED GOLD_FRAMES)
        set(GOLD_FRAMES 30)
    endif()
    if(NOT DEFINED GOLD_MAX_DELTA)
        set(GOLD_MAX_DELTA 16)
    endif()
    if(NOT DEFINED GOLD_MEAN_DELTA)
        set(GOLD_MEAN_DELTA 2)
    endif()
    if(NOT DEFINED GOLD_FAIL_PERCENT)
        set(GOLD_FAIL_PERCENT 1)
    endif()

    set(_host       ${CMAKE_SYSTEM_NAME})
    set(_cell_name  golden_${GOLD_SCENE}__${GOLD_BACKEND})
    set(_scene_cfg  ${CMAKE_SOURCE_DIR}/tests/golden/cases/${GOLD_SCENE}.cfg)
    set(_golden_png ${CMAKE_SOURCE_DIR}/tests/golden/${_host}/${GOLD_SCENE}__${GOLD_BACKEND}.png)
    set(_actual_png ${CMAKE_BINARY_DIR}/golden_actual/${GOLD_SCENE}__${GOLD_BACKEND}.png)
    set(_diff_png   ${CMAKE_BINARY_DIR}/golden_diff/${GOLD_SCENE}__${GOLD_BACKEND}.png)
    set(_workdir    ${CMAKE_BINARY_DIR}/golden_workdir/${_cell_name})

    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/golden_actual)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/golden_diff)
    file(MAKE_DIRECTORY ${_workdir})

    # Pick a sample binary by backend. For Psynder, "backend" means
    # software (the only one we have); kept as a slot for future variants.
    set(_sample sample_00_clear)
    if(GOLD_SCENE MATCHES "^triangle")
        set(_sample sample_01_triangle)
    elseif(GOLD_SCENE MATCHES "^crate")
        set(_sample sample_02_textured_quad)
    elseif(GOLD_SCENE MATCHES "^quake")
        set(_sample sample_03_quake_room)
    elseif(GOLD_SCENE MATCHES "^nfs")
        set(_sample sample_04_nfs_track)
    elseif(GOLD_SCENE MATCHES "^hybrid")
        set(_sample sample_05_hybrid_night)
    elseif(GOLD_SCENE MATCHES "^tactical")
        set(_sample sample_06_tactical_map)
    endif()

    set(_render_args
        --smoke-frames ${GOLD_FRAMES}
        --smoke-capture-out ${_actual_png}
    )
    if(EXISTS ${_scene_cfg})
        list(APPEND _render_args --smoke-exec ${_scene_cfg})
    endif()
    foreach(_e IN LISTS GOLD_EXTRA)
        list(APPEND _render_args ${_e})
    endforeach()

    if(NOT TARGET ${_sample})
        return()
    endif()

    add_test(NAME ${_cell_name}_render
        COMMAND $<TARGET_FILE:${_sample}> ${_render_args}
        WORKING_DIRECTORY ${_workdir})
    set_tests_properties(${_cell_name}_render PROPERTIES
        TIMEOUT 120
        LABELS  "golden;golden_${GOLD_BACKEND}")

    if(NOT TARGET imgdiff)
        message(STATUS
            "golden: imgdiff target unavailable; ${_cell_name}_diff not registered")
        return()
    endif()

    if(EXISTS ${_golden_png})
        add_test(NAME ${_cell_name}_diff
            COMMAND $<TARGET_FILE:imgdiff>
                ${_actual_png}
                ${_golden_png}
                --max-delta    ${GOLD_MAX_DELTA}
                --mean-delta   ${GOLD_MEAN_DELTA}
                --fail-percent ${GOLD_FAIL_PERCENT}
                --diff         ${_diff_png}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
        set_tests_properties(${_cell_name}_diff PROPERTIES
            TIMEOUT 30
            DEPENDS ${_cell_name}_render
            LABELS  "golden;golden_${GOLD_BACKEND}")
    else()
        message(STATUS
            "golden: missing baseline on this host -- ${_golden_png}\n"
            "        Regenerate via:\n"
            "          ctest --test-dir <build> -R ^${_cell_name}_render$\n"
            "          cmake -E make_directory \"${CMAKE_SOURCE_DIR}/tests/golden/${_host}\"\n"
            "          cmake -E copy \"${_actual_png}\" \"${_golden_png}\"")
        add_test(NAME ${_cell_name}_diff
            COMMAND ${CMAKE_COMMAND}
                -DGOLDEN=${_golden_png}
                -DCELL=${_cell_name}
                -DACTUAL=${_actual_png}
                -P ${CMAKE_SOURCE_DIR}/cmake/missing_golden.cmake)
        set_tests_properties(${_cell_name}_diff PROPERTIES
            WILL_FAIL TRUE
            DEPENDS   ${_cell_name}_render
            LABELS    "golden;golden_${GOLD_BACKEND};golden_missing")
    endif()
endfunction()
