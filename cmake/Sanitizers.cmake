# SPDX-License-Identifier: MIT
# Psynder — sanitizer toggles for debug Linux/Mac.

if(PSYNDER_ENABLE_SANITIZERS AND NOT MSVC)
    message(STATUS "Sanitizers enabled: ASan + UBSan")
    add_compile_options(
        -fsanitize=address
        -fsanitize=undefined
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    )
    add_link_options(
        -fsanitize=address
        -fsanitize=undefined
    )
endif()
