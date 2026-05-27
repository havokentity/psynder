# SPDX-License-Identifier: MIT
# Unit tests. Catch2-based.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()

# Per-lane test deps: each lane appends its lib here so its tools_*.cpp /
# core_*.cpp / etc. test files compile and link against the right targets.
# Tool libs may not exist when PSYNDER_BUILD_TOOLS=OFF, so guard each with
# a TARGET check.
set(PSY_UNIT_TEST_LIBS psynder_core psynder_math psynder_simd psynder_scene)
foreach(lib IN ITEMS lm_pak_lib lm_cook_lib lm_qbsp_lib lm_bake_lib lm_mapimport_lib)
    if(TARGET ${lib})
        list(APPEND PSY_UNIT_TEST_LIBS ${lib})
    endif()
endforeach()

if(TARGET Catch2::Catch2WithMain)
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE
        Catch2::Catch2WithMain
        psynder_core psynder_math psynder_simd psynder_scene
        psynder_world_bsp psynder_world_outdoor
        psynder_render_raster psynder_render_rt psynder_render_post
        psynder_asset psynder_audio psynder_net psynder_platform
        psynder_ui_imm psynder_ui_console psynder_editor_core
    )
    # Lane-19 (editor-ipc) tests need to link against the IPC server library.
    if(TARGET psynder_editor_ipc)
        target_link_libraries(psynder_unit PRIVATE psynder_editor_ipc)
    endif()
    # Lane-18 (editor-world) level-source loader tests.
    if(TARGET psynder_editor_world)
        target_link_libraries(psynder_unit PRIVATE psynder_editor_world)
    endif()
    # Editor PLAY MODE physics runtime tests (editor/play).
    if(TARGET psynder_editor_play)
        target_link_libraries(psynder_unit PRIVATE psynder_editor_play)
    endif()
    # Platform lanes (21/22/23) export internal symbols for host-specific
    # tests; link the host's platform lib directly so those tests resolve.
    if(TARGET psynder_platform_macos)
        target_link_libraries(psynder_unit PRIVATE psynder_platform_macos)
    endif()
    if(TARGET psynder_platform_win32)
        target_link_libraries(psynder_unit PRIVATE psynder_platform_win32)
    endif()
    if(TARGET psynder_platform_linux)
        target_link_libraries(psynder_unit PRIVATE psynder_platform_linux)
    endif()
    if(TARGET psynder_platform_shim)
        target_link_libraries(psynder_unit PRIVATE psynder_platform_shim)
    endif()
    # Tool libs (lane 24) when PSYNDER_BUILD_TOOLS=ON.
    foreach(_tlib IN ITEMS lm_pak_lib lm_cook_lib lm_qbsp_lib lm_bake_lib lm_mapimport_lib scene_cook_lib)
        if(TARGET ${_tlib})
            target_link_libraries(psynder_unit PRIVATE ${_tlib})
        endif()
    endforeach()
    include(Catch)
    catch_discover_tests(psynder_unit)
    # Serialize the IPC live-server tests so they don't race on port
    # acquisition under parallel ctest. pick_port() binds to port 0,
    # reads the assigned port, closes the socket, and the server
    # re-binds — under parallel ctest another process can grab the
    # port in between. We can't apply RESOURCE_LOCK via catch_discover's
    # PROPERTIES (it would lock every test); the deferred call attaches
    # it after discovery has populated the test list.
    function(_psynder_apply_ipc_resource_lock)
        get_property(_tests DIRECTORY PROPERTY TESTS)
        foreach(_t IN LISTS _tests)
            if("${_t}" MATCHES "ipc:")
                set_tests_properties(${_t} PROPERTIES RESOURCE_LOCK psynder_ipc_port)
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER CALL _psynder_apply_ipc_resource_lock)
else()
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE psynder_core psynder_math psynder_render_rt)
    add_test(NAME psynder_unit_smoke COMMAND psynder_unit)
endif()
