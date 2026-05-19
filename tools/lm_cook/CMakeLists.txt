# SPDX-License-Identifier: MIT
# lm_cook — asset cooker (PNG/WAV/OBJ/glTF → .lmt/.lma/.lmm).
add_library(lm_cook_lib STATIC
    CookFormats.cpp
    SourceParsers.cpp
    Cook.cpp
    third_party/stb_impl.c
)
target_link_libraries(lm_cook_lib PUBLIC psynder_core psynder_math psynder_asset)
target_include_directories(lm_cook_lib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
set_target_properties(lm_cook_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)
# stb_image is a C library; turn off the noisy warnings on its TU so the
# strict C++ flags don't bleed in.
set_source_files_properties(third_party/stb_impl.c PROPERTIES
    COMPILE_FLAGS "-w"
)

add_executable(lm_cook main.cpp)
target_link_libraries(lm_cook PRIVATE lm_cook_lib)
