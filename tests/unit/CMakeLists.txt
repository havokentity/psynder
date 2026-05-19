# SPDX-License-Identifier: MIT
# Unit tests. Catch2-based.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
if(NOT SRC)
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()

if(TARGET Catch2::Catch2WithMain)
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE
        Catch2::Catch2WithMain
        psynder_core psynder_math psynder_simd psynder_scene
        psynder_world_bsp psynder_render_raster psynder_asset
    )
    include(Catch)
    catch_discover_tests(psynder_unit)
else()
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE psynder_core psynder_math psynder_render_rt)
    add_test(NAME psynder_unit_smoke COMMAND psynder_unit)
endif()
