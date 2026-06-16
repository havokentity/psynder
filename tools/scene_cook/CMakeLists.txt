# SPDX-License-Identifier: MIT
# scene_cook — .psyscene.json authoring source → cooked SoA .psyscene blob.
add_library(scene_cook_lib STATIC SceneCook.cpp)
target_link_libraries(scene_cook_lib PUBLIC psynder_core psynder_math psynder_scene)
target_include_directories(scene_cook_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(scene_cook_lib PROPERTIES
    CXX_STANDARD          23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

add_executable(scene_cook main.cpp)
target_link_libraries(scene_cook PRIVATE scene_cook_lib)
