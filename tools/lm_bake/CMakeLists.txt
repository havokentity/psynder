# SPDX-License-Identifier: MIT
# lm_bake — offline radiosity / path-traced lightmap baker.
add_library(lm_bake_lib STATIC Bake.cpp)
target_link_libraries(lm_bake_lib PUBLIC
    psynder_core psynder_math psynder_simd psynder_jobs psynder_render_rt psynder_asset)
target_include_directories(lm_bake_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lm_bake_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_executable(lm_bake main.cpp)
target_link_libraries(lm_bake PRIVATE lm_bake_lib)
