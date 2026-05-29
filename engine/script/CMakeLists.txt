# SPDX-License-Identifier: MIT
# psynder_script — Lane 15. Owns: Lua 5.4 binding, system/component
# registration macros, REPL. Scripts run on a dedicated game thread; engine
# workers never enter the VM (DESIGN.md §10.5).

# ─── Lua 5.4 — prefer find_package, else FetchContent (lane-local) ───────
# The root Dependencies.cmake already attempts find_package(Lua 5.4 QUIET) and
# leaves the fetch to us so we don't burden every lane with the upstream
# tarball. We mirror the same find first, then FetchContent the official
# release tarball and build a static library out of the documented C sources.
if(NOT TARGET psynder_lua)
    find_package(Lua 5.4 QUIET)
    if(Lua_FOUND)
        add_library(psynder_lua INTERFACE)
        target_include_directories(psynder_lua INTERFACE ${LUA_INCLUDE_DIR})
        target_link_libraries(psynder_lua INTERFACE ${LUA_LIBRARIES})
        message(STATUS "[script] Lua 5.4 from find_package: ${LUA_INCLUDE_DIR}")
    else()
        include(FetchContent)
        set(FETCHCONTENT_QUIET FALSE)
        message(STATUS "[script] Lua 5.4 via FetchContent (lua-5.4.7)")
        FetchContent_Declare(lua_upstream
            URL      https://www.lua.org/ftp/lua-5.4.7.tar.gz
            URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
        )
        FetchContent_MakeAvailable(lua_upstream)

        # Lua ships a hand-rolled Makefile; build the static library ourselves
        # so it slots into our CMake graph. Exclude lua.c / luac.c (the REPL +
        # compiler driver mains) — we link only the library.
        file(GLOB LUA_C_SOURCES CONFIGURE_DEPENDS
            "${lua_upstream_SOURCE_DIR}/src/*.c"
        )
        list(FILTER LUA_C_SOURCES EXCLUDE REGEX ".*/(lua|luac)\\.c$")

        add_library(psynder_lua STATIC ${LUA_C_SOURCES})
        target_include_directories(psynder_lua PUBLIC
            "${lua_upstream_SOURCE_DIR}/src"
        )
        target_compile_features(psynder_lua PUBLIC c_std_99)
        if(UNIX AND NOT APPLE)
            target_compile_definitions(psynder_lua PUBLIC LUA_USE_LINUX)
            target_link_libraries(psynder_lua PUBLIC dl m)
        elseif(APPLE)
            target_compile_definitions(psynder_lua PUBLIC LUA_USE_MACOSX)
            target_link_libraries(psynder_lua PUBLIC m)
        elseif(WIN32)
            # Default Lua build settings work for Win32; nothing extra needed.
        endif()
        # Quiet warnings on the upstream C sources — not our code, not our churn.
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(psynder_lua PRIVATE -w)
        endif()
        set_target_properties(psynder_lua PROPERTIES
            POSITION_INDEPENDENT_CODE ON
        )
        add_library(Lua::Lua ALIAS psynder_lua)
    endif()
endif()

psynder_add_lane(script LINKS psynder_core psynder_scene psynder_lua)

# ─── PsyGraph visual-scripting core (M-PSYGRAPH) ─────────────────────────
# A separate, dependency-light static library under psygraph/. The script
# lane's psynder_add_lane() glob is GLOB_RECURSE, so it also picked up the
# psygraph/*.cpp sources; strip them off psynder_script so they compile only
# into psynder_psygraph (no duplicate symbols). Then build + link the lib.
get_target_property(_PSY_SCRIPT_SRCS psynder_script SOURCES)
if(_PSY_SCRIPT_SRCS)
    list(FILTER _PSY_SCRIPT_SRCS EXCLUDE REGEX "/psygraph/")
    set_target_properties(psynder_script PROPERTIES SOURCES "${_PSY_SCRIPT_SRCS}")
endif()

add_subdirectory(psygraph)
target_link_libraries(psynder_script PUBLIC psynder_psygraph)

# ─── Hook psynder_script onto the shared unit-test binary ────────────────
# The shared `psynder_unit` target is created later in the build by lane 25
# (tests/unit/CMakeLists.txt), so we cannot reference it at parse time of
# this file. Defer the link wiring to end-of-configure so the lane's tests
# under tests/unit/script_*.cpp can call into psynder::script::Vm without
# requiring lane 25 to know about every other lane's library by name.
#
# Each lane brief assigns ownership of `tests/unit/<lane>_*.cpp` to the
# corresponding lane; the unit binary itself is shared. Deferring the link
# from each lane is the only way to keep ownership boundaries clean while
# still producing a single test binary.
function(psynder_script_attach_to_unit_target)
    if(TARGET psynder_unit)
        target_link_libraries(psynder_unit PRIVATE psynder_script)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    psynder_script_attach_to_unit_target)
