// SPDX-License-Identifier: MIT
// Psynder editor IPC — server internals.
//
// Lane-private. The public surface (engine/editor/ipc/Ipc.h) is a thin
// facade over the singleton implementation here.

#pragma once

#include "core/Types.h"
#include "editor/ipc/internal/WsFrame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace psynder::editor::ipc::internal {

// Each connected client lives in a Connection. The accept-loop thread owns it
// and runs its read/write loop on its own OS thread (one thread per client —
// editor panels are O(handful) so this is fine).
struct Connection {
    int sock = -1;
    std::thread worker;
    std::atomic<bool> authed{false};
    std::atomic<bool> alive{true};

    // Outbound queue — broadcast() drops msgpack frames here; the worker
    // thread drains it. Mutex+CV because cross-thread; not on a hot path.
    std::mutex out_mu;
    std::condition_variable out_cv;
    std::deque<std::vector<::psynder::u8>> out_queue;

    // Subscribed channel names — the client tells us via SubscribeFrame.
    std::mutex sub_mu;
    std::unordered_set<std::string> subscribed;
};

class Server {
public:
    static Server& Get();

    bool start(const char* bind_host, ::psynder::u16 port, bool require_session_token);
    void stop();

    // Push a state delta to all connected, authenticated, subscribed clients.
    void broadcast(std::string_view channel, std::span<const ::psynder::u8> payload);

    // Pump once per frame; drains inbound commands.
    void pump();

    const std::string& session_token() const noexcept { return session_token_; }

    // For tests: validate a token string against the current session token.
    bool validate_token(std::string_view t) const noexcept;

    // Inbound command queue — populated by client workers when they receive
    // ConsoleFrame / other client→server frames; drained by pump().
    struct InboundCmd {
        std::string channel;
        std::vector<::psynder::u8> payload;
    };

private:
    void accept_loop();
    void client_loop(std::shared_ptr<Connection> conn);

    bool handle_http_upgrade(Connection& conn, const std::string& path,
                             const std::string& sec_ws_key,
                             const std::string& sec_ws_protocol);
    void send_http(Connection& conn, int status, std::string_view reason,
                   std::string_view content_type, std::string_view body);
    void serve_static(Connection& conn, const std::string& path);
    void enqueue(Connection& conn, std::vector<::psynder::u8> frame);

    // Internal: parse a query/fragment param value (key=...) from `path`. We
    // look at both ?token=... and #token=... because the spec says clients
    // pass it via fragment, but real browsers strip fragments — we accept
    // both for robustness.
    static std::string extract_token(std::string_view path);

private:
    std::atomic<bool>  running_{false};
    int                listen_sock_ = -1;
    std::thread        accept_thread_;
    bool               require_token_ = true;
    ::psynder::u16     port_ = 0;
    std::string        bind_host_;
    std::string        session_token_;

    std::mutex                                  conns_mu_;
    std::vector<std::shared_ptr<Connection>>    conns_;

    std::mutex                                  inbound_mu_;
    std::deque<InboundCmd>                      inbound_;
};

}  // namespace psynder::editor::ipc::internal
