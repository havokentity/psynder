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
        ${PSY_UNIT_TEST_LIBS}
    )
    include(Catch)
    catch_discover_tests(psynder_unit)
else()
    add_executable(psynder_unit ${SRC})
    target_link_libraries(psynder_unit PRIVATE psynder_core psynder_math)
    add_test(NAME psynder_unit_smoke COMMAND psynder_unit)
endif()
