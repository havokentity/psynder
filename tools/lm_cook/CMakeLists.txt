# SPDX-License-Identifier: MIT
# lm_cook — asset cooker (PNG/WAV/OBJ/glTF → .lmt/.lma/.lmm).
add_library(lm_cook_lib STATIC
    CookFormats.cpp
    SourceParsers.cpp
    Cook.cpp
)
target_link_libraries(lm_cook_lib PUBLIC psynder_core psynder_math psynder_asset)
target_include_directories(lm_cook_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lm_cook_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_executable(lm_cook main.cpp)
target_link_libraries(lm_cook PRIVATE lm_cook_lib)
