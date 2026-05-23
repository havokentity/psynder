# SPDX-License-Identifier: MIT
# scene_cook — .psyscene.json authoring source → cooked SoA .psyscene blob.
add_executable(scene_cook main.cpp)
target_link_libraries(scene_cook PRIVATE psynder_core psynder_math psynder_scene)
