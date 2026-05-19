# SPDX-License-Identifier: MIT
# Psynder — convenience helpers used by per-lane subdirectory CMakeLists.
#
# The helpers exist so lane CMakeLists.txt files stay tiny and uniform.
# Each lane creates a static library psynder_<name>; the helpers wire in
# the standard interface target + warning flags + include paths.

# Globs all .cpp / .c / .mm under the lane directory and creates a static
# library out of them. If the glob is empty, falls back to a stub.cpp so
# the target still produces a library (zero-source static libs are not
# portable across all generators).
function(psynder_add_lane name)
    set(options "")
    set(one_value_args "")
    set(multi_value_args LINKS DEPS PRIVATE_DEFS PUBLIC_DEFS)
    cmake_parse_arguments(LANE "${options}" "${one_value_args}"
                          "${multi_value_args}" ${ARGN})

    file(GLOB_RECURSE LANE_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.mm"
    )

    # Filter out platform-specific sources that don't belong to the host
    if(NOT PSYNDER_PLATFORM_MACOS)
        list(FILTER LANE_SOURCES EXCLUDE REGEX ".*\\.mm$")
    endif()

    if(LANE_SOURCES STREQUAL "")
        # Ensure stub exists for empty libs
        set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_psynder_${name}_stub.cpp")
        if(NOT EXISTS "${STUB}")
            file(WRITE "${STUB}"
"// SPDX-License-Identifier: MIT\n"
"// Stub TU for psynder_${name} — replace by the lane agent.\n"
"namespace psynder::${name}_stub { void anchor() {} }\n"
            )
        endif()
        set(LANE_SOURCES "${STUB}")
    endif()

    add_library(psynder_${name} STATIC ${LANE_SOURCES})
    add_library(psynder::${name} ALIAS psynder_${name})

    target_link_libraries(psynder_${name} PUBLIC psynder_common ${LANE_LINKS})

    if(LANE_PUBLIC_DEFS)
        target_compile_definitions(psynder_${name} PUBLIC ${LANE_PUBLIC_DEFS})
    endif()
    if(LANE_PRIVATE_DEFS)
        target_compile_definitions(psynder_${name} PRIVATE ${LANE_PRIVATE_DEFS})
    endif()

    # Lane libraries always see the engine root so they can include from
    # any sibling lane via "<sibling>/Header.h"
    target_include_directories(psynder_${name} PUBLIC ${CMAKE_SOURCE_DIR}/engine)

    set_target_properties(psynder_${name} PROPERTIES
        CXX_STANDARD          23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )
endfunction()

# Convenience: a lane that's just placeholders right now.
function(psynder_add_lane_stub name)
    psynder_add_lane(${name})
endfunction()
