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
#include "core/console/Completion.h"
#include "core/console/Console.h"
#include "script/internal/ReplHook.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// POSIX sockets — works on macOS + Linux. On Windows we'd want winsock2 +
// WSAStartup; that lives behind the same fence as the platform-win32 lane.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static inline int psy_close(socket_t s) {
    return closesocket(s);
}
static inline int psy_errno() {
    return WSAGetLastError();
}
#define PSY_EAGAIN WSAEWOULDBLOCK
#define PSY_EINTR WSAEINTR
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static inline int psy_close(socket_t s) {
    return ::close(s);
}
static inline int psy_errno() {
    return errno;
}
#define PSY_EAGAIN EAGAIN
#define PSY_EINTR EINTR
#endif

namespace psynder::editor::ipc::internal {

namespace {
// GUID per RFC 6455 §1.3 — concatenated with Sec-WebSocket-Key for handshake.
constexpr const char* kWsAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view kPanelIndexName = "index.html";
constexpr std::string_view kLegacyChannelProfiler = "profiler";
constexpr std::string_view kLegacyChannelSchema = "schema";
constexpr std::size_t kMaxOutboundFramesPerConnection = 256;

::psynder::console::ExecuteResult dispatch_editor_console(std::string_view text,
                                                          std::string_view mode,
                                                          bool repl_live) {
    ::psynder::console::ExecuteResult result;
    if (mode == "lua") {
        if (!repl_live) {
            result.ok = false;
            result.error = "lua: REPL backend is not installed";
            return result;
        }
        result.ok = ::psynder::script::dispatch_repl(text, result.output);
        if (!result.ok) {
            result.error = std::move(result.output);
            result.output.clear();
        }
        return result;
    }

    return ::psynder::console::Console::Get().ExecuteScript(text);
}

::psynder::u8 completion_kind(::psynder::console::CompletionKind kind) noexcept {
    using Kind = ::psynder::console::CompletionKind;
    switch (kind) {
        case Kind::Cvar:
            return 0;
        case Kind::Command:
            return 1;
        case Kind::Value:
            return 2;
    }
    return 0;
}

proto::ConsoleCompletionReply build_console_completion_reply(
    const proto::ConsoleCompletionQuery& query) {
    const std::size_t cursor =
        std::min<std::size_t>(query.cursor, query.input.size());
    const auto token = ::psynder::console::CurrentToken(query.input, cursor);
    const auto matches =
        ::psynder::console::BuildCompletions(token, /*max_results*/ 24, /*description_clip*/ 96);

    proto::ConsoleCompletionReply reply;
    reply.id = query.id;
    reply.start = static_cast<::psynder::u32>(
        std::min<std::size_t>(token.start, query.input.size()));
    reply.end = static_cast<::psynder::u32>(
        std::min<std::size_t>(token.end, query.input.size()));
    reply.names.reserve(matches.size());
    reply.kinds.reserve(matches.size());
    reply.values.reserve(matches.size());
    reply.descriptions.reserve(matches.size());
    for (const auto& match : matches) {
        reply.names.push_back(match.name);
        reply.kinds.push_back(completion_kind(match.kind));
        reply.values.push_back(match.value);
        reply.descriptions.push_back(match.description);
    }
    return reply;
}

std::vector<std::string> subscription_aliases(std::string_view channel) {
    std::vector<std::string> out;
    out.emplace_back(channel);

    if (channel == kLegacyChannelProfiler) {
        // The React profiler panel predates the generated StatsFrame and
        // subscribes to "profiler"; generated C++ stats use "stats", while
        // Wave-B perf deltas use "perf".
        out.emplace_back(proto::channels::kstats);
        out.emplace_back(proto::channels::kperf);
    } else if (channel == proto::channels::kstats || channel == proto::channels::kperf) {
        out.emplace_back(kLegacyChannelProfiler);
        if (channel == proto::channels::kstats)
            out.emplace_back(proto::channels::kperf);
        else
            out.emplace_back(proto::channels::kstats);
    } else if (channel == kLegacyChannelSchema) {
        out.emplace_back(proto::channels::kschemas);
    } else if (channel == proto::channels::kschemas) {
        out.emplace_back(kLegacyChannelSchema);
    }

    return out;
}

void subscribe_channel(Connection& conn, std::string_view channel) {
    std::lock_guard<std::mutex> lk(conn.sub_mu);
    for (auto& alias : subscription_aliases(channel)) {
        conn.subscribed.insert(std::move(alias));
    }
}

void unsubscribe_channel(Connection& conn, std::string_view channel) {
    std::lock_guard<std::mutex> lk(conn.sub_mu);
    for (const auto& alias : subscription_aliases(channel)) {
        conn.subscribed.erase(alias);
    }
}

struct LegacyEnvelope {
    std::string channel;
    std::string type;
    std::string prop_id;
};

bool decode_legacy_payload(msgpack::Reader& r, LegacyEnvelope& out) {
    ::psynder::u32 count = 0;
    if (!r.map_header(count))
        return r.skip();

    for (::psynder::u32 i = 0; i < count; ++i) {
        std::string key;
        if (!r.str(key))
            return false;
        if (key == "prop_id") {
            if (!r.str(out.prop_id))
                return false;
        } else if (!r.skip()) {
            return false;
        }
    }
    return true;
}

bool decode_legacy_envelope(std::span<const ::psynder::u8> payload, LegacyEnvelope& out) {
    msgpack::Reader r(payload.data(), payload.size());
    ::psynder::u32 count = 0;
    if (!r.map_header(count))
        return false;

    for (::psynder::u32 i = 0; i < count; ++i) {
        std::string key;
        if (!r.str(key))
            return false;
        if (key == "ch") {
            if (!r.str(out.channel))
                return false;
        } else if (key == "type") {
            if (!r.str(out.type))
                return false;
        } else if (key == "payload") {
            if (!decode_legacy_payload(r, out))
                return false;
        } else if (!r.skip()) {
            return false;
        }
    }
    return !out.channel.empty() && !out.type.empty();
}

bool handle_legacy_subscription(Connection& conn, const LegacyEnvelope& env) {
    if (env.type == "subscribe") {
        subscribe_channel(conn, env.channel);
        return true;
    }
    if (env.type == "unsubscribe") {
        unsubscribe_channel(conn, env.channel);
        return true;
    }
    return false;
}

std::vector<::psynder::u8> encode_legacy_command_ack(std::string_view channel,
                                                     std::string_view command,
                                                     bool ok,
                                                     std::string_view text) {
    msgpack::Writer w;
    w.map_header(4);
    w.str("v");
    w.u32_(proto::kProtocolVersion);
    w.str("ch");
    w.str(channel);
    w.str("type");
    w.str("command_ack");
    w.str("payload");
    w.map_header(3);
    w.str("command");
    w.str(command);
    w.str("ok");
    w.boolean(ok);
    w.str("text");
    w.str(text);
    return w.buffer();
}

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
            if (psy_errno() == PSY_EINTR)
                continue;
            return false;
        }
        if (sent == 0)
            return false;
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
            if (psy_errno() == PSY_EINTR)
                continue;
            return -1;
        }
        return static_cast<::psynder::isize>(got);
    }
}

std::filesystem::path editor_web_dist_dir() {
    auto source_file = std::filesystem::path{__FILE__};
    return source_file.parent_path().parent_path() / "web" / "dist";
}

bool path_segment_is_safe(std::string_view s) noexcept {
    return !s.empty() && s.find("..") == std::string_view::npos &&
           s.find('\\') == std::string_view::npos;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string body;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size > 0)
        body.resize(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!body.empty())
        in.read(body.data(), static_cast<std::streamsize>(body.size()));
    return body;
}

std::string_view content_type_for(std::string_view path) noexcept {
    if (path.ends_with(".css"))
        return "text/css; charset=utf-8";
    if (path.ends_with(".js") || path.ends_with(".mjs"))
        return "text/javascript; charset=utf-8";
    if (path.ends_with(".json") || path.ends_with(".map"))
        return "application/json; charset=utf-8";
    if (path.ends_with(".html"))
        return "text/html; charset=utf-8";
    if (path.ends_with(".svg"))
        return "image/svg+xml";
    if (path.ends_with(".png"))
        return "image/png";
    if (path.ends_with(".jpg") || path.ends_with(".jpeg"))
        return "image/jpeg";
    if (path.ends_with(".webp"))
        return "image/webp";
    return "application/octet-stream";
}

}  // namespace

// ─── Singleton wiring ──────────────────────────────────────────────────────
Server& Server::Get() {
    static Server s;
    return s;
}

Server::~Server() {
    stop();
}

bool Server::validate_token(std::string_view t) const noexcept {
    if (!require_token_)
        return true;
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
                while (end < tail.size() && tail[end] != '&')
                    ++end;
                std::string_view kv = tail.substr(start, end - start);
                auto eq = kv.find('=');
                if (eq != std::string_view::npos) {
                    if (kv.substr(0, eq) == "token") {
                        return std::string(kv.substr(eq + 1));
                    }
                }
                if (end >= tail.size())
                    break;
                start = end + 1;
            }
            // also check for fragment after query — keep scanning.
            pos = path.find(sep, pos + 1);
        }
        return {};
    };
    std::string s = find_after('?');
    if (s.empty())
        s = find_after('#');
    return s;
}

// ─── Lifecycle ─────────────────────────────────────────────────────────────
bool Server::start(const char* bind_host, ::psynder::u16 port, bool require_session_token) {
    if (running_.load())
        return true;
    bind_host_ = bind_host ? bind_host : "127.0.0.1";
    port_ = port;
    require_token_ = require_session_token;
    session_token_ = crypto::make_session_token();

#if !defined(_WIN32)
    // Ignore SIGPIPE on Unix — broken-pipe writes return EPIPE from send()
    // rather than killing the process. WebSocket clients close in odd ways.
    static bool sigpipe_ignored = []() {
        struct sigaction sa {};
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
    ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

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

    // ConsoleFrame messages carry an explicit mode: engine console or Lua.
    // Keep the script REPL installed for the Lua tab.
    install_repl_backend();

    PSY_LOG_INFO("editor-ipc: listening on {}:{}", bind_host_, port_);
    return true;
}

void Server::install_repl_backend() {
    // The script lane's `dispatch_repl(...)` already falls back to the
    // default Vm evaluator when no custom backend is installed (see
    // engine/script/internal/ReplHook.cpp). Calling this method here keeps the
    // Lua tab available without the IPC server owning the script VM.
    //
    // We deliberately do NOT install our own backend that supplants the
    // script lane's default — tests opt in to a fake backend via
    // `script::set_repl_backend(...)`. This method is therefore mostly a
    // documentation hook + state flag for `pump()` to consult.
    repl_installed_.store(true, std::memory_order_release);
}

void Server::stop() {
    if (!running_.exchange(false))
        return;

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
    if (accept_thread_.joinable())
        accept_thread_.join();

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
        if (c->worker.joinable())
            c->worker.join();
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
            if (!running_.load())
                break;
            if (psy_errno() == PSY_EINTR)
                continue;
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
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        auto conn = std::make_shared<Connection>();
        conn->sock = static_cast<int>(s);
        {
            std::lock_guard<std::mutex> lk(conns_mu_);
            conns_.push_back(conn);
        }
        std::weak_ptr<Connection> wconn = conn;
        conn->worker = std::thread([this, wconn]() {
            if (auto c = wconn.lock())
                this->client_loop(c);
        });
    }
}

// ─── HTTP helpers ──────────────────────────────────────────────────────────
void Server::send_http(Connection& conn,
                       int status,
                       std::string_view reason,
                       std::string_view content_type,
                       std::string_view body) {
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
    if (conn.sock < 0)
        return;
    send_all(conn.sock, reinterpret_cast<const ::psynder::u8*>(head.data()), head.size());
    if (!body.empty()) {
        send_all(conn.sock, reinterpret_cast<const ::psynder::u8*>(body.data()), body.size());
    }
}

void Server::serve_static(Connection& conn, const std::string& path) {
    // path includes everything after the leading scheme/host, so it starts
    // with '/'. We map:
    //   /              -> bundled React index
    //   /panels/<name> -> bundled React index
    //   /assets/<file> -> bundled React asset
    //   /healthz       -> "ok\n"
    //   /protocol.json -> a tiny version stamp
    // If the web bundle is missing, fall back to the tiny bootstrap page so
    // the IPC server remains inspectable in minimal source-only builds.
    std::string clean = path;
    auto qpos = clean.find_first_of("?#");
    if (qpos != std::string::npos)
        clean.resize(qpos);

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
    if (clean.rfind("/assets/", 0) == 0) {
        const std::string_view asset_rel(clean.data() + 1, clean.size() - 1);
        if (!path_segment_is_safe(asset_rel)) {
            send_http(conn, 400, "Bad Request", "text/plain", "bad asset path\n");
            return;
        }
        const auto asset_path = editor_web_dist_dir() / std::filesystem::path{std::string{asset_rel}};
        if (auto body = read_text_file(asset_path)) {
            send_http(conn, 200, "OK", content_type_for(clean), *body);
            return;
        }
        send_http(conn, 404, "Not Found", "text/plain", "asset not found\n");
        return;
    }
    if (clean == "/" || clean.rfind("/panels/", 0) == 0) {
        if (auto body = read_text_file(editor_web_dist_dir() / std::string{kPanelIndexName})) {
            send_http(conn, 200, "OK", "text/html; charset=utf-8", *body);
        } else {
            send_http(conn, 200, "OK", "text/html; charset=utf-8", kPanelBootstrapHtml);
        }
        return;
    }
    send_http(conn, 404, "Not Found", "text/plain", "not found\n");
}

bool Server::handle_http_upgrade(Connection& conn,
                                 const std::string& path,
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
    head +=
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    head += accept;
    head += "\r\n\r\n";
    if (conn.sock < 0)
        return false;
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
    try {
        std::lock_guard<std::mutex> lk(conn.out_mu);
        while (conn.out_queue.size() >= kMaxOutboundFramesPerConnection)
            conn.out_queue.pop_front();
        conn.out_queue.emplace_back(std::move(frame));
    } catch (const std::exception& e) {
        conn.alive.store(false);
        PSY_LOG_WARN("editor-ipc: dropping outbound frame after enqueue failure: {}", e.what());
        return;
    } catch (...) {
        conn.alive.store(false);
        PSY_LOG_WARN("editor-ipc: dropping outbound frame after unknown enqueue failure");
        return;
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
                conn->out_cv.wait(lk,
                                  [&]() { return !conn->out_queue.empty() || !conn->alive.load(); });
                if (!conn->alive.load())
                    break;
                chunk = std::move(conn->out_queue.front());
                conn->out_queue.pop_front();
            }
            if (conn->sock < 0)
                break;
            if (!send_all(conn->sock, chunk.data(), chunk.size()))
                break;
        }
    });

    // Receive bytes until handshake complete, then frame-by-frame.
    bool handshake_done = false;
    while (conn->alive.load()) {
        ::psynder::u8 tmp[4096];
        ::psynder::isize got = recv_some(conn->sock, tmp, sizeof(tmp));
        if (got <= 0)
            break;
        rxbuf.insert(rxbuf.end(), tmp, tmp + got);

        if (!handshake_done) {
            http::Request req;
            ::psynder::usize consumed =
                http::parse_request(reinterpret_cast<const char*>(rxbuf.data()), rxbuf.size(), req);
            if (consumed == 0)
                continue;  // wait for more
            if (consumed == SIZE_MAX) {
                send_http(*conn, 400, "Bad Request", "text/plain", "bad http\n");
                break;
            }
            rxbuf.erase(rxbuf.begin(), rxbuf.begin() + static_cast<std::ptrdiff_t>(consumed));

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
            if (used == 0)
                break;
            if (used == SIZE_MAX) {
                conn->alive.store(false);
                break;
            }
            rxbuf.erase(rxbuf.begin(), rxbuf.begin() + static_cast<std::ptrdiff_t>(used));

            if (fr.op == wsframe::Op::Close) {
                auto reply = wsframe::encode_close(1000, "");
                enqueue(*conn, std::move(reply));
                conn->alive.store(false);
                break;
            }
            if (fr.op == wsframe::Op::Ping) {
                auto reply =
                    wsframe::encode_server(wsframe::Op::Pong, fr.payload.data(), fr.payload.size(), true);
                enqueue(*conn, std::move(reply));
                continue;
            }
            if (fr.op == wsframe::Op::Pong)
                continue;
            if (fr.op != wsframe::Op::Binary && fr.op != wsframe::Op::Text)
                continue;

            // Binary frame: opcode (u16) + msgpack body.
            msgpack::Reader r(fr.payload);
            ::psynder::u16 op = 0;
            if (!r.u16_(op)) {
                LegacyEnvelope env;
                const std::span<const ::psynder::u8> legacy_payload(fr.payload.data(),
                                                                    fr.payload.size());
                if (!decode_legacy_envelope(legacy_payload, env))
                    continue;
                if (handle_legacy_subscription(*conn, env))
                    continue;
                if (env.channel == proto::channels::kselection &&
                    env.type == "spawn_prop" &&
                    !env.prop_id.empty()) {
                    std::string text = "editor_spawn_prop ";
                    text += env.prop_id;

                    InboundCmd ic;
                    ic.channel = "console";
                    ic.payload.assign(reinterpret_cast<const ::psynder::u8*>(text.data()),
                                      reinterpret_cast<const ::psynder::u8*>(text.data()) +
                                          text.size());
                    ic.conn = conn;
                    ic.reply_channel = proto::channels::kselection;
                    ic.reply_type = "command_ack";
                    ic.reply_command = "spawn_prop";
                    std::lock_guard<std::mutex> lk(inbound_mu_);
                    inbound_.emplace_back(std::move(ic));
                }
                continue;
            }
            switch (op) {
                case proto::opcodes::kSubscribeFrame: {
                    proto::Subscribe sub;
                    if (proto::Subscribe_decode(r, sub)) {
                        subscribe_channel(*conn, sub.channel);
                    }
                    break;
                }
                case proto::opcodes::kUnsubscribeFrame: {
                    proto::Unsubscribe usub;
                    if (proto::Unsubscribe_decode(r, usub)) {
                        unsubscribe_channel(*conn, usub.channel);
                    }
                    break;
                }
                case proto::opcodes::kConsoleFrame: {
                    proto::ConsoleCmd cmd;
                    if (proto::ConsoleCmd_decode(r, cmd)) {
                        InboundCmd ic;
                        ic.channel = cmd.mode.empty() ? std::string{"console"} : std::move(cmd.mode);
                        ic.payload.assign(reinterpret_cast<const ::psynder::u8*>(cmd.text.data()),
                                          reinterpret_cast<const ::psynder::u8*>(cmd.text.data()) +
                                              cmd.text.size());
                        ic.conn = conn;  // weak ref so pump() can ship the reply.
                        std::lock_guard<std::mutex> lk(inbound_mu_);
                        inbound_.emplace_back(std::move(ic));
                    }
                    break;
                }
                case proto::opcodes::kConsoleCompletionQueryFrame: {
                    proto::ConsoleCompletionQuery query;
                    if (proto::ConsoleCompletionQuery_decode(r, query)) {
                        InboundCompletion ic;
                        ic.id = query.id;
                        ic.cursor = query.cursor;
                        ic.input = std::move(query.input);
                        ic.conn = conn;
                        std::lock_guard<std::mutex> lk(inbound_mu_);
                        inbound_completions_.emplace_back(std::move(ic));
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
    if (tx.joinable())
        tx.join();
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
    try {
        auto frame = wsframe::encode_server_binary(payload.data(), payload.size());
        std::vector<std::shared_ptr<Connection>> snapshot;
        {
            std::lock_guard<std::mutex> lk(conns_mu_);
            snapshot = conns_;
        }
        const std::string channel_str(channel);
        auto aliases = subscription_aliases(channel);
        for (auto& c : snapshot) {
            if (!c || !c->authed.load() || !c->alive.load())
                continue;
            bool subscribed = false;
            {
                std::lock_guard<std::mutex> lk(c->sub_mu);
                subscribed = c->subscribed.count(channel_str) > 0;
                if (!subscribed) {
                    for (const auto& alias : aliases) {
                        if (c->subscribed.count(alias) > 0) {
                            subscribed = true;
                            break;
                        }
                    }
                }
            }
            if (!subscribed)
                continue;
            enqueue(*c, frame);  // copy intentionally — each conn owns its outbound buf.
        }

        // Garbage-collect dead connections opportunistically.
        {
            std::lock_guard<std::mutex> lk(conns_mu_);
            conns_.erase(std::remove_if(conns_.begin(),
                                        conns_.end(),
                                        [](const std::shared_ptr<Connection>& c) {
                                            return !c || !c->alive.load();
                                        }),
                         conns_.end());
        }
    } catch (const std::exception& e) {
        PSY_LOG_WARN("editor-ipc: broadcast '{}' dropped after exception: {}", channel, e.what());
    } catch (...) {
        PSY_LOG_WARN("editor-ipc: broadcast '{}' dropped after unknown exception", channel);
    }
}

bool Server::has_subscribers(std::string_view channel) {
    try {
        std::vector<std::shared_ptr<Connection>> snapshot;
        {
            std::lock_guard<std::mutex> lk(conns_mu_);
            snapshot = conns_;
        }
        const std::string channel_str(channel);
        auto aliases = subscription_aliases(channel);
        for (auto& c : snapshot) {
            if (!c || !c->authed.load() || !c->alive.load())
                continue;
            std::lock_guard<std::mutex> sub_lk(c->sub_mu);
            if (c->subscribed.count(channel_str) > 0)
                return true;
            for (const auto& alias : aliases) {
                if (c->subscribed.count(alias) > 0)
                    return true;
            }
        }
    } catch (const std::exception& e) {
        PSY_LOG_WARN("editor-ipc: subscriber check '{}' failed: {}", channel, e.what());
    } catch (...) {
        PSY_LOG_WARN("editor-ipc: subscriber check '{}' failed with unknown exception", channel);
    }
    return false;
}

void Server::push_scene_delta(std::string_view slice_name,
                              std::span<const ::psynder::u8> msgpack_payload) {
    // Build the SceneDeltaFrame: u16 opcode then SceneDeltaSlice{slice,
    // payload}. The payload itself is opaque to us — we only re-frame it.
    msgpack::Writer w;
    w.u16_(proto::opcodes::kSceneDeltaFrame);
    proto::SceneDeltaSlice sd;
    sd.slice.assign(slice_name.data(), slice_name.size());
    sd.payload.assign(msgpack_payload.data(), msgpack_payload.data() + msgpack_payload.size());
    proto::SceneDeltaSlice_encode(w, sd);
    auto frame = wsframe::encode_server_binary(w.data(), w.size());

    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        snapshot = conns_;
    }
    // Deliver to every authenticated connection whose subscription set
    // contains `slice_name`, a known alias, or the physical "scene" channel.
    // The slice and the WS subscription channel are the same string by
    // convention, while "scene" remains a catch-all for clients that want to
    // demux SceneDeltaSlice themselves.
    const std::string slice_str(slice_name);
    auto aliases = subscription_aliases(slice_name);
    for (auto& c : snapshot) {
        if (!c || !c->authed.load() || !c->alive.load())
            continue;
        bool subscribed = false;
        {
            std::lock_guard<std::mutex> lk(c->sub_mu);
            subscribed = c->subscribed.count(slice_str) > 0 ||
                         c->subscribed.count(proto::channels::kscene) > 0;
            if (!subscribed) {
                for (const auto& alias : aliases) {
                    if (c->subscribed.count(alias) > 0) {
                        subscribed = true;
                        break;
                    }
                }
            }
        }
        if (!subscribed)
            continue;
        enqueue(*c, frame);
    }

    // Same opportunistic GC as broadcast(): if a connection died between
    // accept and now its slot is removed here so the conns_ vector doesn't
    // grow without bound across a long session.
    {
        std::lock_guard<std::mutex> lk(conns_mu_);
        conns_.erase(std::remove_if(conns_.begin(),
                                    conns_.end(),
                                    [](const std::shared_ptr<Connection>& c) {
                                        return !c || !c->alive.load();
                                    }),
                     conns_.end());
    }
}

void Server::pump() {
    std::deque<InboundCmd> local;
    std::deque<InboundCompletion> completions;
    {
        std::lock_guard<std::mutex> lk(inbound_mu_);
        local.swap(inbound_);
        completions.swap(inbound_completions_);
    }

    for (auto& completion : completions) {
        auto conn = completion.conn.lock();
        if (!conn || !conn->alive.load())
            continue;

        proto::ConsoleCompletionQuery query;
        query.id = completion.id;
        query.input = std::move(completion.input);
        query.cursor = completion.cursor;
        proto::ConsoleCompletionReply reply = build_console_completion_reply(query);

        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleCompletionReplyFrame);
        proto::ConsoleCompletionReply_encode(w, reply);
        auto frame = wsframe::encode_server_binary(w.data(), w.size());
        enqueue(*conn, std::move(frame));
    }

    const bool repl_live = repl_installed_.load(std::memory_order_acquire);
    for (auto& cmd : local) {
        std::string text(reinterpret_cast<const char*>(cmd.payload.data()), cmd.payload.size());
        PSY_LOG_INFO("editor-ipc: console cmd ({}): {}", cmd.channel, text);
        auto result = dispatch_editor_console(text, cmd.channel, repl_live);
        auto& console = ::psynder::console::Console::Get();
        if (cmd.channel == "console")
            console.PushHistory(text);

        std::string mirror_line;
        if (cmd.channel == "lua") {
            mirror_line = "[lua] ";
            mirror_line += text;
        } else {
            mirror_line = text;
        }
        console.NotifyExternalExecution(mirror_line, result);

        auto conn = cmd.conn.lock();
        if (!conn || !conn->alive.load())
            continue;

        if (!cmd.reply_channel.empty()) {
            const std::string_view reply_text =
                !result.error.empty() ? std::string_view{result.error} : std::string_view{result.output};
            auto ack = encode_legacy_command_ack(cmd.reply_channel,
                                                 cmd.reply_command,
                                                 result.ok,
                                                 reply_text);
            auto frame = wsframe::encode_server_binary(ack.data(), ack.size());
            enqueue(*conn, std::move(frame));
            continue;
        }

        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleReplyFrame);
        proto::ConsoleReply rep;
        rep.ok = result.ok;
        rep.text = !result.error.empty() ? std::move(result.error) : std::move(result.output);
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

void Server::broadcast_stats_tick(const StatsTick& tick) {
    msgpack::Writer w;
    w.u16_(proto::opcodes::kStatsFrame);
    w.map_header(6);
    w.str("frame");
    w.u64_(tick.frame_index);
    w.str("cpu_ms");
    w.f32_(tick.cpu_ms);
    w.str("render_ms");
    w.f32_(tick.render_ms);
    w.str("draw_calls");
    w.u32_(tick.draw_calls);
    w.str("entities");
    w.u32_(tick.entities);
    w.str("sections");
    w.array_header(tick.sections.size());
    for (const StatsSection& section : tick.sections) {
        w.map_header(2);
        w.str("name");
        w.str(section.name);
        w.str("ms");
        w.f32_(section.ms);
    }
    internal::Server::Get().broadcast(proto::channels::kstats,
                                      std::span<const ::psynder::u8>(w.data(), w.size()));
}

bool Server::has_subscribers(std::string_view channel) const {
    return internal::Server::Get().has_subscribers(channel);
}

void Server::pump() {
    internal::Server::Get().pump();
}

const std::string& Server::session_token() const noexcept {
    return internal::Server::Get().session_token();
}

}  // namespace psynder::editor::ipc
