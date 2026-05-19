// SPDX-License-Identifier: MIT
// Psynder editor IPC — server implementation.
//
// Self-contained WebSocket + HTTP server on 127.0.0.1. We deliberately *do
// not* pull in a third-party WebSocket library: the editor IPC is localhost
// only, low-fanout (a handful of editor panels), and the framing logic we
// need is small enough to write directly. Documenting the choice here so
// future maintainers don't reach for uWebSockets / websocketpp without
// reading: their dependency footprints (libuv / Boost.Asio) dwarf the
// few hundred lines of framing we actually use.
//
// Threading: one OS thread per client. Editor panels are O(handful); we are
// not optimising for fanout. Inbound commands land on the queue drained by
// pump() on the engine main thread.
//
// Auth: per-startup random session token (crypto::make_session_token()).
// The token is delivered to the browser via the launch URL fragment; the
// browser then re-sends it in the WebSocket open URL's fragment. We do a
// constant-time compare to defend against timing attacks (overkill for
// localhost but trivial to implement and avoids static-analysis flags).

#include "editor/ipc/Ipc.h"
#include "editor/ipc/internal/Crypto.h"
#include "editor/ipc/internal/HttpParse.h"
#include "editor/ipc/internal/Msgpack.h"
#include "editor/ipc/internal/PanelHtml.h"
#include "editor/ipc/internal/Server.h"
#include "editor/ipc/internal/WsFrame.h"
#include "editor/ipc/proto/Protocol.gen.h"

#include "core/Log.h"
#include "script/internal/ReplHook.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// POSIX sockets — works on macOS + Linux. On Windows we'd want winsock2 +
// WSAStartup; that lives behind the same fence as the platform-win32 lane.
#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static inline int psy_close(socket_t s) { return closesocket(s); }
static inline int psy_errno() { return WSAGetLastError(); }
#  define PSY_EAGAIN  WSAEWOULDBLOCK
#  define PSY_EINTR   WSAEINTR
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static inline int psy_close(socket_t s) { return ::close(s); }
static inline int psy_errno() { return errno; }
#  define PSY_EAGAIN  EAGAIN
#  define PSY_EINTR   EINTR
#endif

namespace psynder::editor::ipc::internal {

namespace {
// GUID per RFC 6455 §1.3 — concatenated with Sec-WebSocket-Key for handshake.
constexpr const char* kWsAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool send_all(socket_t s, const ::psynder::u8* buf, ::psynder::usize n) {
    while (n) {
        // ::send takes size_t on POSIX and int on Win32 — keep portable.
#if defined(_WIN32)
        const int chunk = static_cast<int>(n > 0x7FFFFFFFu ? 0x7FFFFFFFu : n);
        auto sent = ::send(s, reinterpret_cast<const char*>(buf), chunk, 0);
#else
        auto sent = ::send(s, reinterpret_cast<const char*>(buf), n, 0);
#endif
        if (sent < 0) {
            if (psy_errno() == PSY_EINTR) continue;
            return false;
        }
        if (sent == 0) return false;
        buf += sent;
        n -= static_cast<::psynder::usize>(sent);
    }
    return true;
}

// Recv up to `n` bytes; returns 0 on EOF, -1 on error.
::psynder::isize recv_some(socket_t s, ::psynder::u8* buf, ::psynder::usize n) {
    for (;;) {
#if defined(_WIN32)
        const int chunk = static_cast<int>(n > 0x7FFFFFFFu ? 0x7FFFFFFFu : n);
        auto got = ::recv(s, reinterpret_cast<char*>(buf), chunk, 0);
#else
        auto got = ::recv(s, reinterpret_cast<char*>(buf), n, 0);
#endif
        if (got < 0) {
            if (psy_errno() == PSY_EINTR) continue;
            return -1;
        }
        return static_cast<::psynder::isize>(got);
    }
}

}  // namespace

// ─── Singleton wiring ──────────────────────────────────────────────────────
Server& Server::Get() {
    static Server s;
    return s;
}

bool Server::validate_token(std::string_view t) const noexcept {
    if (!require_token_) return true;
    return crypto::constant_time_equal(t, session_token_);
}

std::string Server::extract_token(std::string_view path) {
    // Look for "token=" after either '?' or '#'. URL fragments aren't sent
    // in real HTTP requests by browsers, so for the HTTP bootstrap the engine
    // builds a `?token=...` URL when launching Chrome; for the WS open the
    // JS client encodes the token into the URL the WebSocket constructor
    // resolves (and the server sees the full URI).
    auto find_after = [&](char sep) -> std::string {
        auto pos = path.find(sep);
        while (pos != std::string_view::npos) {
            std::string_view tail = path.substr(pos + 1);
            // Split tail on '&'
            ::psynder::usize start = 0;
            while (start <= tail.size()) {
                ::psynder::usize end = start;
                while (end < tail.size() && tail[end] != '&') ++end;
                std::string_view kv = tail.substr(start, end - start);
                auto eq = kv.find('=');
                if (eq != std::string_view::npos) {
                    if (kv.substr(0, eq) == "token") {
                        return std::string(kv.substr(eq + 1));
                    }
                }
                if (end >= tail.size()) break;
                start = end + 1;
            }
            // also check for fragment after query — keep scanning.
            pos = path.find(sep, pos + 1);
        }
        return {};
    };
    std::string s = find_after('?');
    if (s.empty()) s = find_after('#');
    return s;
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────
bool Server::start(const char* bind_host, ::psynder::u16 port, bool require_session_token) {
    if (running_.load()) return true;
    bind_host_ = bind_host ? bind_host : "127.0.0.1";
    port_ = port;
    require_token_ = require_session_token;
    session_token_ = crypto::make_session_token();

#if !defined(_WIN32)
    // Ignore SIGPIPE on Unix — broken-pipe writes return EPIPE from send()
    // rather than killing the process. WebSocket clients close in odd ways.
    static bool sigpipe_ignored = [](){
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGPIPE, &sa, nullptr);
        return true;
    }();
    (void)sigpipe_ignored;
#endif

    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_ == kInvalidSocket) {
        PSY_LOG_ERROR("editor-ipc: socket() failed errno={}", psy_errno());
        return false;
    }
    int one = 1;
    ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, bind_host_.c_str(), &addr.sin_addr) != 1) {
        PSY_LOG_ERROR("editor-ipc: bad bind host {}", bind_host_);
        psy_close(listen_sock_);
        listen_sock_ = kInvalidSocket;
        return false;
    }
    if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        PSY_LOG_ERROR("editor-ipc: bind {}:{} failed errno={}", bind_host_, port_, psy_errno());
        psy_close(listen_sock_);
        listen_sock_ = kInvalidSocket;
        return false;
    }
    if (::listen(listen_sock_, 8) != 0) {
        PSY_LOG_ERROR("editor-ipc: listen failed errno={}", psy_errno());
        psy_close(listen_sock_);
        listen_sock_ = kInvalidSocket;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread([this]() { this->accept_loop(); });

    // Wave-B: route ConsoleFrame messages through the script lane's REPL hook
    // by default. The script lane installs its own evaluator; we only verify
    // that a backend is registered so `dispatch_repl` is callable from pump().
    install_repl_backend();

    PSY_LOG_INFO("editor-ipc: listening on {}:{}", bind_host_, port_);
    return true;
}

void Server::install_repl_backend() {
    // The script lane's `dispatch_repl(...)` already falls back to the
    // default Vm evaluator when no custom backend is installed (see
    // engine/script/internal/ReplHook.cpp). Calling this method here is the
    // explicit hand-off point: the IPC server now considers the REPL wiring
    // live, and `pump()` will forward ConsoleCmd text through Lua.
    //
    // We deliberately do NOT install our own backend that supplants the
    // script lane's default — tests opt in to a fake backend via
    // `script::set_repl_backend(...)`. This method is therefore mostly a
    // documentation hook + state flag for `pump()` to consult.
    repl_installed_.store(true, std::memory_order_release);
}

void Server::stop() {
    if (!running_.exchange(false)) return;

    // Shutdown listening socket — accept_loop will fall out.
    if (listen_sock_ != kInvalidSocket) {
#if defined(_WIN32)
        ::shutdown(listen_sock_, SD_BOTH);
#else
        ::shutdown(listen_sock_, SHUT_RDWR);
#endif
        psy_close(listen_sock_);
        listen_sock_ = kInvalidSocket;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    // Tear down all connections.
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        snapshot.swap(conns_);
    }
    for (auto& c : snapshot) {
        c->alive.store(false);
        if (c->sock != -1) {
#if defined(_WIN32)
            ::shutdown(c->sock, SD_BOTH);
#else
            ::shutdown(c->sock, SHUT_RDWR);
#endif
            psy_close(c->sock);
            c->sock = -1;
        }
        c->out_cv.notify_all();
        if (c->worker.joinable()) c->worker.join();
    }
}

// ─── Accept loop ───────────────────────────────────────────────────────────
void Server::accept_loop() {
    while (running_.load()) {
        sockaddr_in client{};
#if defined(_WIN32)
        int alen = sizeof(client);
#else
        socklen_t alen = sizeof(client);
#endif
        socket_t s = ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&client), &alen);
        if (s == kInvalidSocket) {
            if (!running_.load()) break;
            if (psy_errno() == PSY_EINTR) continue;
            // Brief sleep so accept loop never spins hot on persistent errors.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Localhost only — clamp by checking the peer address.
        if (client.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            PSY_LOG_WARN("editor-ipc: refusing non-loopback peer");
            psy_close(s);
            continue;
        }
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));
        auto conn = std::make_shared<Connection>();
        conn->sock = static_cast<int>(s);
        {
            std::lock_guard<std::mutex> lk(conns_mu_);
            conns_.push_back(conn);
        }
        std::weak_ptr<Connection> wconn = conn;
        conn->worker = std::thread([this, wconn]() {
            if (auto c = wconn.lock()) this->client_loop(c);
        });
    }
}

// ─── HTTP helpers ──────────────────────────────────────────────────────────
void Server::send_http(Connection& conn, int status, std::string_view reason,
                       std::string_view content_type, std::string_view body) {
    std::string head;
    head.reserve(128 + body.size());
    head += "HTTP/1.1 ";
    head += std::to_string(status);
    head += " ";
    head += reason;
    head += "\r\nContent-Type: ";
    head += content_type;
    head += "\r\nContent-Length: ";
    head += std::to_string(body.size());
    head += "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
    if (conn.sock < 0) return;
    send_all(conn.sock, reinterpret_cast<const ::psynder::u8*>(head.data()), head.size());
    if (!body.empty()) {
        send_all(conn.sock, reinterpret_cast<const ::psynder::u8*>(body.data()), body.size());
    }
}

void Server::serve_static(Connection& conn, const std::string& path) {
    // path includes everything after the leading scheme/host, so it starts
    // with '/'. We map:
    //   /              -> bootstrap HTML
    //   /panels/<name> -> bootstrap HTML
    //   /healthz       -> "ok\n"
    //   /protocol.json -> a tiny version stamp
    std::string clean = path;
    auto qpos = clean.find_first_of("?#");
    if (qpos != std::string::npos) clean.resize(qpos);

    if (clean == "/healthz") {
        send_http(conn, 200, "OK", "text/plain", "ok\n");
        return;
    }
    if (clean == "/protocol.json") {
        std::string body = "{\"version\":";
        body += std::to_string(static_cast<unsigned>(proto::kProtocolVersion));
        body += "}\n";
        send_http(conn, 200, "OK", "application/json", body);
        return;
    }
    if (clean == "/" || clean.rfind("/panels/", 0) == 0) {
        send_http(conn, 200, "OK", "text/html; charset=utf-8", kPanelBootstrapHtml);
        return;
    }
    send_http(conn, 404, "Not Found", "text/plain", "not found\n");
}

bool Server::handle_http_upgrade(Connection& conn, const std::string& path,
                                 const std::string& sec_ws_key,
                                 const std::string& /*sec_ws_protocol*/) {
    // Validate session token first.
    std::string token = extract_token(path);
    if (require_token_) {
        if (!validate_token(token)) {
            send_http(conn, 401, "Unauthorized", "text/plain", "bad token\n");
            return false;
        }
    }

    std::string accept_src = sec_ws_key + kWsAcceptGuid;
    auto digest = crypto::sha1(accept_src);
    std::string accept = crypto::base64_encode(digest.data(), digest.size());

    std::string head;
    head.reserve(192);
    head += "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
    head += accept;
    head += "\r\n\r\n";
    if (conn.sock < 0) return false;
    if (!send_all(conn.sock, reinterpret_cast<const ::psynder::u8*>(head.data()), head.size())) {
        return false;
    }
    conn.authed.store(true);

    // Send the WelcomeFrame — opcode 2, msgpack body.
    msgpack::Writer w;
    w.u16_(proto::opcodes::kWelcomeFrame);
    proto::Welcome wel;
    wel.accepted = true;
    wel.server_ver = proto::kProtocolVersion;
    wel.server_build = "psynder-wave-a";
    wel.reason = "";
    proto::Welcome_encode(w, wel);
    auto frame = wsframe::encode_server_binary(w.data(), w.size());
    enqueue(conn, std::move(frame));
    return true;
}

void Server::enqueue(Connection& conn, std::vector<::psynder::u8> frame) {
    {
        std::lock_guard<std::mutex> lk(conn.out_mu);
        conn.out_queue.emplace_back(std::move(frame));
    }
    conn.out_cv.notify_all();
}

// ─── Client loop ───────────────────────────────────────────────────────────
void Server::client_loop(std::shared_ptr<Connection> conn) {
    std::vector<::psynder::u8> rxbuf;
    rxbuf.reserve(4096);

    // Outbound pump thread — peels from conn->out_queue and sends. Sharing
    // the socket FD between read and write threads on POSIX is safe.
    std::thread tx([conn]() {
        while (conn->alive.load()) {
            std::vector<::psynder::u8> chunk;
            {
                std::unique_lock<std::mutex> lk(conn->out_mu);
                conn->out_cv.wait(lk, [&]() {
                    return !conn->out_queue.empty() || !conn->alive.load();
                });
                if (!conn->alive.load()) break;
                chunk = std::move(conn->out_queue.front());
                conn->out_queue.pop_front();
            }
            if (conn->sock < 0) break;
            if (!send_all(conn->sock, chunk.data(), chunk.size())) break;
        }
    });

    // Receive bytes until handshake complete, then frame-by-frame.
    bool handshake_done = false;
    while (conn->alive.load()) {
        ::psynder::u8 tmp[4096];
        ::psynder::isize got = recv_some(conn->sock, tmp, sizeof(tmp));
        if (got <= 0) break;
        rxbuf.insert(rxbuf.end(), tmp, tmp + got);

        if (!handshake_done) {
            http::Request req;
            ::psynder::usize consumed = http::parse_request(
                reinterpret_cast<const char*>(rxbuf.data()), rxbuf.size(), req);
            if (consumed == 0) continue;  // wait for more
            if (consumed == SIZE_MAX) {
                send_http(*conn, 400, "Bad Request", "text/plain", "bad http\n");
                break;
            }
            rxbuf.erase(rxbuf.begin(),
                        rxbuf.begin() + static_cast<std::ptrdiff_t>(consumed));

            // Is this an upgrade?
            const std::string* up = req.find("Upgrade");
            if (up && (*up == "websocket" || *up == "WebSocket")) {
                const std::string* key = req.find("Sec-WebSocket-Key");
                const std::string* sub = req.find("Sec-WebSocket-Protocol");
                if (!key) {
                    send_http(*conn, 400, "Bad Request", "text/plain", "missing key\n");
                    break;
                }
                if (!handle_http_upgrade(*conn, req.path, *key, sub ? *sub : "")) {
                    break;
                }
                handshake_done = true;
                continue;
            }
            // Non-upgrade HTTP — serve static & close.
            serve_static(*conn, req.path);
            break;
        }

        // WebSocket frame mode
        while (conn->alive.load()) {
            wsframe::Frame fr;
            ::psynder::usize used = wsframe::try_parse_client(rxbuf.data(), rxbuf.size(), fr);
            if (used == 0) break;
            if (used == SIZE_MAX) {
                conn->alive.store(false);
                break;
            }
            rxbuf.erase(rxbuf.begin(),
                        rxbuf.begin() + static_cast<std::ptrdiff_t>(used));

            if (fr.op == wsframe::Op::Close) {
                auto reply = wsframe::encode_close(1000, "");
                enqueue(*conn, std::move(reply));
                conn->alive.store(false);
                break;
            }
            if (fr.op == wsframe::Op::Ping) {
                auto reply = wsframe::encode_server(wsframe::Op::Pong, fr.payload.data(),
                                                    fr.payload.size(), true);
                enqueue(*conn, std::move(reply));
                continue;
            }
            if (fr.op == wsframe::Op::Pong) continue;
            if (fr.op != wsframe::Op::Binary && fr.op != wsframe::Op::Text) continue;

            // Binary frame: opcode (u16) + msgpack body.
            msgpack::Reader r(fr.payload);
            ::psynder::u16 op = 0;
            if (!r.u16_(op)) continue;
            switch (op) {
            case proto::opcodes::kSubscribeFrame: {
                proto::Subscribe sub;
                if (proto::Subscribe_decode(r, sub)) {
                    std::lock_guard<std::mutex> lk(conn->sub_mu);
                    conn->subscribed.insert(sub.channel);
                }
                break;
            }
            case proto::opcodes::kUnsubscribeFrame: {
                proto::Unsubscribe usub;
                if (proto::Unsubscribe_decode(r, usub)) {
                    std::lock_guard<std::mutex> lk(conn->sub_mu);
                    conn->subscribed.erase(usub.channel);
                }
                break;
            }
            case proto::opcodes::kConsoleFrame: {
                proto::ConsoleCmd cmd;
                if (proto::ConsoleCmd_decode(r, cmd)) {
                    InboundCmd ic;
                    ic.channel = proto::channels::kconsole;
                    ic.payload.assign(reinterpret_cast<const ::psynder::u8*>(cmd.text.data()),
                                      reinterpret_cast<const ::psynder::u8*>(cmd.text.data()) + cmd.text.size());
                    ic.conn = conn;  // weak ref so pump() can ship the reply.
                    std::lock_guard<std::mutex> lk(inbound_mu_);
                    inbound_.emplace_back(std::move(ic));
                }
                break;
            }
            default:
                // Unknown frame: ignore (forward-compat).
                break;
            }
        }
    }

    conn->alive.store(false);
    conn->out_cv.notify_all();
    if (tx.joinable()) tx.join();
    if (conn->sock != -1) {
#if defined(_WIN32)
        ::shutdown(conn->sock, SD_BOTH);
#else
        ::shutdown(conn->sock, SHUT_RDWR);
#endif
        psy_close(conn->sock);
        conn->sock = -1;
    }
}

// ─── Pub/sub broadcast & pump ──────────────────────────────────────────────
void Server::broadcast(std::string_view channel, std::span<const ::psynder::u8> payload) {
    auto frame = wsframe::encode_server_binary(payload.data(), payload.size());
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        snapshot = conns_;
    }
    for (auto& c : snapshot) {
        if (!c || !c->authed.load() || !c->alive.load()) continue;
        bool subscribed = false;
        {
            std::lock_guard<std::mutex> lk(c->sub_mu);
            subscribed = c->subscribed.count(std::string(channel)) > 0;
        }
        if (!subscribed) continue;
        enqueue(*c, frame);  // copy intentionally — each conn owns its outbound buf.
    }

    // Garbage-collect dead connections opportunistically.
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [](const std::shared_ptr<Connection>& c) {
                                        return !c || !c->alive.load();
                                    }),
                     conns_.end());
    }
}

void Server::push_scene_delta(std::string_view slice_name,
                              std::span<const ::psynder::u8> msgpack_payload) {
    // Build the SceneDeltaFrame: u16 opcode then SceneDeltaSlice{slice,
    // payload}. The payload itself is opaque to us — we only re-frame it.
    msgpack::Writer w;
    w.u16_(proto::opcodes::kSceneDeltaFrame);
    proto::SceneDeltaSlice sd;
    sd.slice.assign(slice_name.data(), slice_name.size());
    sd.payload.assign(msgpack_payload.data(),
                      msgpack_payload.data() + msgpack_payload.size());
    proto::SceneDeltaSlice_encode(w, sd);
    auto frame = wsframe::encode_server_binary(w.data(), w.size());

    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        snapshot = conns_;
    }
    // Deliver to every authenticated connection whose subscription set
    // contains `slice_name`. The slice and the WS subscription channel are
    // the same string by convention so lane 20 panels can keep using the
    // existing SubscribeFrame mechanism.
    const std::string slice_str(slice_name);
    for (auto& c : snapshot) {
        if (!c || !c->authed.load() || !c->alive.load()) continue;
        bool subscribed = false;
        {
            std::lock_guard<std::mutex> lk(c->sub_mu);
            subscribed = c->subscribed.count(slice_str) > 0;
        }
        if (!subscribed) continue;
        enqueue(*c, frame);
    }

    // Same opportunistic GC as broadcast(): if a connection died between
    // accept and now its slot is removed here so the conns_ vector doesn't
    // grow without bound across a long session.
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                    [](const std::shared_ptr<Connection>& c) {
                                        return !c || !c->alive.load();
                                    }),
                     conns_.end());
    }
}

void Server::pump() {
    std::deque<InboundCmd> local;
    {
        std::lock_guard<std::mutex> lk(inbound_mu_);
        local.swap(inbound_);
    }
    const bool repl_live = repl_installed_.load(std::memory_order_acquire);
    for (auto& cmd : local) {
        std::string text(reinterpret_cast<const char*>(cmd.payload.data()), cmd.payload.size());
        PSY_LOG_INFO("editor-ipc: console cmd ({}): {}", cmd.channel, text);
        if (!repl_live) continue;

        // Route through the script lane's REPL hook. `dispatch_repl` looks up
        // whatever backend is currently installed (default = Vm::execute_repl,
        // tests may override). The result string goes back to the originating
        // panel as a ConsoleReply frame (opcode 21).
        std::string out;
        const bool ok = ::psynder::script::dispatch_repl(text, out);

        auto conn = cmd.conn.lock();
        if (!conn || !conn->alive.load()) continue;

        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleReplyFrame);
        proto::ConsoleReply rep;
        rep.ok = ok;
        rep.text = std::move(out);
        proto::ConsoleReply_encode(w, rep);
        auto frame = wsframe::encode_server_binary(w.data(), w.size());
        enqueue(*conn, std::move(frame));
    }
}

}  // namespace psynder::editor::ipc::internal


// ─── Public-header facade ──────────────────────────────────────────────────
namespace psynder::editor::ipc {

Server& Server::Get() {
    static Server s;
    return s;
}

bool Server::start(const ServerDesc& desc) {
    return internal::Server::Get().start(desc.bind_host, desc.port, desc.require_session_token);
}

void Server::stop() {
    internal::Server::Get().stop();
}

void Server::broadcast(std::string_view channel, std::span<const ::psynder::u8> msgpack_payload) {
    internal::Server::Get().broadcast(channel, msgpack_payload);
}

void Server::pump() {
    internal::Server::Get().pump();
}

const std::string& Server::session_token() const noexcept {
    return internal::Server::Get().session_token();
}

}  // namespace psynder::editor::ipc
