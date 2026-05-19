# SPDX-License-Identifier: MIT
# Psynder — per-target warning flags helper.

function(psynder_set_warnings target)
    if(MSVC)
        target_compile_options(${target} INTERFACE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /MP
            /EHsc
            /wd4324
        )
        # Disable some MSVC-noise checks that fire on perfectly fine SoA code
        target_compile_definitions(${target} INTERFACE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    else()
        target_compile_options(${target} INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wmisleading-indentation
            -Wnull-dereference
            -Wdouble-promotion
            -Wformat=2
            -Wno-unused-parameter
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
           CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            target_compile_options(${target} INTERFACE
                -Wno-gnu-zero-variadic-macro-arguments
                -Wno-c99-extensions
            )
        endif()
    endif()
endfunction()
