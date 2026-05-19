# SPDX-License-Identifier: MIT
# dmonte_pt — optional reference path tracer for golden-image tests.
# Carries an Embree dep; opted in by PSYNDER_BUILD_DMONTE_PT.
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.c)
if(SRC STREQUAL "")
    set(STUB "${CMAKE_CURRENT_BINARY_DIR}/_stub.cpp")
    file(WRITE "${STUB}" "int main(){return 0;}\n")
    set(SRC "${STUB}")
endif()
add_executable(dmonte_pt ${SRC})
target_link_libraries(dmonte_pt PRIVATE psynder_core psynder_math)
# Embree wiring is opt-in; the lane sets it up if PSYNDER_BUILD_DMONTE_PT is ON.
