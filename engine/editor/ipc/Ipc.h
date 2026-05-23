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

struct StatsTick {
    u64 frame_index = 0;
    f32 cpu_ms = 0.0f;
    f32 gpu_ms = 0.0f;
    u32 draw_calls = 0;
    u32 entities = 0;
};

class Server {
   public:
    static Server& Get();

    bool start(const ServerDesc& desc);
    void stop();

    // Push a state delta to all connected panels (one-directional engine→UI).
    void broadcast(std::string_view channel, std::span<const u8> msgpack_payload);
    void broadcast_stats_tick(const StatsTick& tick);

    [[nodiscard]] bool has_subscribers(std::string_view channel) const;

    // Pump once per frame to dispatch any incoming command RPCs onto the
    // console queue.
    void pump();

    const std::string& session_token() const noexcept;
};

}  // namespace psynder::editor::ipc
