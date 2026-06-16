# SPDX-License-Identifier: MIT
# psynder_editor_ipc — Lane 19. Owns: local HTTP + WebSocket server, msgpack
# protocol, IDL stub gen.
#
# WebSocket choice: NO third-party WS lib. The editor IPC is localhost-only,
# low-fanout (a handful of editor panels). The RFC 6455 framing we need lives
# in engine/editor/ipc/internal/WsFrame.h (~80 lines); the dependency
# footprint of uWebSockets (libuv) or websocketpp (Boost.Asio or standalone
# Asio) dwarfs that. Documented also at the top of Server.cpp.

# ─── IDL stub generation ───────────────────────────────────────────────────
set(IPC_IDL              "${CMAKE_CURRENT_SOURCE_DIR}/protocol.psy")
set(IPC_IDL_GEN          "${CMAKE_CURRENT_SOURCE_DIR}/idl/psyidl.py")
set(IPC_CPP_OUT_DIR      "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(IPC_CPP_OUT          "${IPC_CPP_OUT_DIR}/Protocol.gen.h")
set(IPC_TS_OUT_DIR       "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(IPC_TS_OUT           "${IPC_TS_OUT_DIR}/protocol.gen.ts")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT  ${IPC_CPP_OUT} ${IPC_TS_OUT}
    COMMAND ${Python3_EXECUTABLE} ${IPC_IDL_GEN}
            --input   ${IPC_IDL}
            --cpp-out ${IPC_CPP_OUT}
            --ts-out  ${IPC_TS_OUT}
    DEPENDS ${IPC_IDL} ${IPC_IDL_GEN}
    COMMENT "[ipc] regenerating Protocol.gen.h + protocol.gen.ts from protocol.psy"
    VERBATIM
)
add_custom_target(psynder_editor_ipc_idl_gen
    DEPENDS ${IPC_CPP_OUT} ${IPC_TS_OUT}
)

# ─── Library ───────────────────────────────────────────────────────────────
# Note: the Lanes helper globs *.cpp under this directory; Server.cpp picks up
# automatically. We add include dirs for the generated header.
psynder_add_lane(editor_ipc LINKS psynder_core psynder_script)

add_dependencies(psynder_editor_ipc psynder_editor_ipc_idl_gen)

# Generated header lives under build/<preset>/engine/editor/ipc/proto/.
# Engine source uses #include "editor/ipc/proto/Protocol.gen.h" — that resolves
# against this build-tree directory hierarchy. ${CMAKE_CURRENT_BINARY_DIR}
# itself is .../build/<preset>/engine/editor/ipc; to make
# "editor/ipc/proto/..." resolve we expose .../engine/ as an include root.
target_include_directories(psynder_editor_ipc PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/../.."
)

# Windows: link winsock2
if(WIN32)
    target_link_libraries(psynder_editor_ipc PUBLIC ws2_32)
endif()

# Tag the generated sources so IDEs treat them as derived, not user-editable.
set_source_files_properties(${IPC_CPP_OUT} ${IPC_TS_OUT} PROPERTIES GENERATED TRUE)
