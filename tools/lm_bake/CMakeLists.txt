# SPDX-License-Identifier: MIT
# lm_bake — offline radiosity / path-traced lightmap baker.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.c)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(lm_bake ${SRC})
target_link_libraries(lm_bake PRIVATE psynder_core psynder_math psynder_simd psynder_jobs psynder_render_rt psynder_asset)
