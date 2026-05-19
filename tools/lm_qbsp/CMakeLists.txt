# SPDX-License-Identifier: MIT
# lm_qbsp — BSP compiler (id-inspired).
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.c)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(lm_qbsp ${SRC})
target_link_libraries(lm_qbsp PRIVATE psynder_core psynder_math psynder_world_bsp psynder_asset)
