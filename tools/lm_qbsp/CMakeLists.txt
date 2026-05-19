# SPDX-License-Identifier: MIT
# lm_qbsp — BSP compiler (id-inspired).
add_library(lm_qbsp_lib STATIC Qbsp.cpp)
target_link_libraries(lm_qbsp_lib PUBLIC psynder_core psynder_math psynder_world_bsp psynder_asset)
target_include_directories(lm_qbsp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lm_qbsp_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_executable(lm_qbsp main.cpp)
target_link_libraries(lm_qbsp PRIVATE lm_qbsp_lib)
