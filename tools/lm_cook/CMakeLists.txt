# SPDX-License-Identifier: MIT
# lm_cook — asset cooker (PNG/WAV/OBJ/glTF → .lmt/.lma/.lmm).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.c)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(lm_cook ${SRC})
target_link_libraries(lm_cook PRIVATE psynder_core psynder_math psynder_asset)
