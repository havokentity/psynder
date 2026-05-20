// SPDX-License-Identifier: MIT
// Psynder — editor IPC server. Local WebSocket + HTTP on 127.0.0.1:7654;
// msgpack frame format. Lane 19 owns.

#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>

namespace psynder::editor::ipc {

struct ServerDesc {
    const char* bind_host = "127.0.0.1";
    u16 port = 7654;
    bool require_session_token = true;
};

class Server {
   public:
    static Server& Get();

    bool start(const ServerDesc& desc);
    void stop();

    // Push a state delta to all connected panels (one-directional engine→UI).
    void broadcast(std::string_view channel, std::span<const u8> msgpack_payload);

    // Pump once per frame to dispatch any incoming command RPCs onto the
    // console queue.
    void pump();

    const std::string& session_token() const noexcept;
};

}  // namespace psynder::editor::ipc
