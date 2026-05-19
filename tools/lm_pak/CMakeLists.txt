# SPDX-License-Identifier: MIT
# lm_pak — .lmpak archive packer / unpacker.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.c)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(lm_pak ${SRC})
target_link_libraries(lm_pak PRIVATE psynder_core psynder_asset)
