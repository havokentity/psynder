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
