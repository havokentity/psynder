# SPDX-License-Identifier: MIT
# lm_mapimport — one-way TrenchBroom .map → .psylevel bridge.
add_library(lm_mapimport_lib STATIC MapImport.cpp CurveBrush.cpp)
# Reuses the .map parser + BSP compiler from lm_qbsp_lib.
target_link_libraries(lm_mapimport_lib
    PUBLIC psynder_core psynder_math psynder_world_bsp psynder_asset
    PUBLIC lm_qbsp_lib)
target_include_directories(lm_mapimport_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lm_mapimport_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_executable(lm_mapimport main.cpp)
target_link_libraries(lm_mapimport PRIVATE lm_mapimport_lib)
