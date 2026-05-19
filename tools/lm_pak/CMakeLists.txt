# SPDX-License-Identifier: MIT
# lm_pak — .lmpak archive packer / unpacker.
#
# A thin internal static lib `lm_pak_lib` carries the implementation so the
# unit tests can drive the entrypoints (pack_blobs, read_index, ...) without
# spawning processes; the executable is a one-line shell around cli_main.
add_library(lm_pak_lib STATIC Lmpak.cpp)
target_link_libraries(lm_pak_lib PUBLIC psynder_core psynder_asset)
target_include_directories(lm_pak_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lm_pak_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_executable(lm_pak main.cpp)
target_link_libraries(lm_pak PRIVATE lm_pak_lib)

# ─── Build-time fixture pipeline ──────────────────────────────────────────
#
# Wave-D end-to-end: cook `assets/fixtures/*.{png,wav}` into `.lmt`/`.lma`,
# then `lm_pak pack` the cooked directory into `${CMAKE_BINARY_DIR}/assets/
# demo.lmpak`. The unit test `tools_lm_fixture.cpp` reads the produced
# archive back through the lm_pak library API (round-trip + FNV-1a-64
# index verification) and also exercises lane-05's Vfs::mount_pak with
# the same file.
#
# A configure-time header (`LmpakFixturePaths.h`) carries the absolute
# path of the produced .lmpak into the test TU — we can't poke into
# tests/unit/CMakeLists.txt from this lane.
set(_lm_fixtures_src    "${CMAKE_SOURCE_DIR}/assets/fixtures")
set(_lm_fixtures_cooked "${CMAKE_BINARY_DIR}/fixtures_cooked")
set(_lm_fixtures_pak    "${CMAKE_BINARY_DIR}/assets/demo.lmpak")

set(_lm_fixture_png   "${_lm_fixtures_src}/crate.png")
set(_lm_fixture_wav   "${_lm_fixtures_src}/tone_440hz.wav")
set(_lm_cooked_lmt    "${_lm_fixtures_cooked}/crate.lmt")
set(_lm_cooked_lma    "${_lm_fixtures_cooked}/tone_440hz.lma")

# Configure-time header so the test can locate the produced archive.
set(PSYNDER_FIXTURE_LMPAK_PATH "${_lm_fixtures_pak}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/LmpakFixturePaths.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/LmpakFixturePaths.h"
    @ONLY
)
target_include_directories(lm_pak_lib PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/include"
)

# Cook PNG -> .lmt
add_custom_command(
    OUTPUT  "${_lm_cooked_lmt}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_lm_fixtures_cooked}"
    COMMAND $<TARGET_FILE:lm_cook> "${_lm_fixture_png}" "${_lm_fixtures_cooked}"
    DEPENDS lm_cook "${_lm_fixture_png}"
    COMMENT "Cooking ${_lm_fixture_png} -> ${_lm_cooked_lmt}"
    VERBATIM
)

# Cook WAV -> .lma
add_custom_command(
    OUTPUT  "${_lm_cooked_lma}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_lm_fixtures_cooked}"
    COMMAND $<TARGET_FILE:lm_cook> "${_lm_fixture_wav}" "${_lm_fixtures_cooked}"
    DEPENDS lm_cook "${_lm_fixture_wav}"
    COMMENT "Cooking ${_lm_fixture_wav} -> ${_lm_cooked_lma}"
    VERBATIM
)

# Pack cooked dir -> demo.lmpak
add_custom_command(
    OUTPUT  "${_lm_fixtures_pak}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/assets"
    COMMAND $<TARGET_FILE:lm_pak> pack "${_lm_fixtures_cooked}" "${_lm_fixtures_pak}"
    DEPENDS lm_pak "${_lm_cooked_lmt}" "${_lm_cooked_lma}"
    COMMENT "Packing ${_lm_fixtures_cooked} -> ${_lm_fixtures_pak}"
    VERBATIM
)

add_custom_target(psynder_fixture_pak ALL
    DEPENDS "${_lm_fixtures_pak}"
)

# Ensure the unit test executable can find the .lmpak when run from any
# directory: build it before the unit tests do. We can't modify
# tests/unit/CMakeLists.txt, so we DEFER a hook to attach the dependency
# once that subdirectory has defined `psynder_unit`.
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
    _psynder_lm_pak_fixture_hook
)
function(_psynder_lm_pak_fixture_hook)
    if(TARGET psynder_unit)
        add_dependencies(psynder_unit psynder_fixture_pak)
    endif()
endfunction()
