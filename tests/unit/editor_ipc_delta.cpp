// SPDX-License-Identifier: MIT
// Psynder editor IPC — Wave-B coverage for the slice-scoped delta push,
// schema welcome, and console command routing. Lane 19.
//
// Each test stands up a live server on an ephemeral port, hand-rolls the
// HTTP+WS upgrade, then exchanges binary frames over the raw socket. We do
// not reuse the helpers from editor_ipc_session.cpp because that file is
// owned by Wave-A — sharing utilities across TU boundaries would force a
// shared header, and the helpers are small enough to inline. The shape
// mirrors session.cpp exactly so reviewers can diff the two side-by-side.
//
// What we verify, by test case:
//
//   1. push_scene_delta routes only to *slice* subscribers. Connection A
//      subscribes to "perf", connection B to "selection". A delta pushed to
//      the "perf" slice MUST land on A and MUST NOT land on B. This is the
//      load-bearing invariant lane 20 React panels rely on: each panel can
//      open one WebSocket and selectively pick up only its own slice.
//
//   2. Protocol bumps remain decodable by older clients. The Welcome frame
//      is unchanged byte-shape; older decoders see the current server_ver and can
//      gate features off it. The forward-compat fallback for unknown opcodes
//      is also asserted (the server's `default:` arm in client_loop must
//      drop unknown frames silently, not disconnect).
//
//   3. ConsoleCmd carries an explicit mode. `console` routes to the engine
//      command/cvar registry and `lua` routes to `script::dispatch_repl`.
//      Completion frames route to core/console/Completion, which is the
//      single source of truth used by both native and web consoles.

#include "core/console/Console.h"
#include "editor/ipc/Ipc.h"
#include "editor/ipc/internal/Crypto.h"
#include "editor/ipc/internal/Msgpack.h"
#include "editor/ipc/internal/Server.h"
#include "editor/ipc/internal/WsFrame.h"
#include "editor/ipc/proto/Protocol.gen.h"
#include "script/internal/ReplHook.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace psynder;
using namespace psynder::editor::ipc;

namespace {

#if defined(_WIN32)
using sock_t = SOCKET;
inline int sock_close(sock_t s) {
    return closesocket(s);
}
inline bool sock_valid(sock_t s) {
    return s != INVALID_SOCKET;
}
constexpr sock_t kBadSock = INVALID_SOCKET;
#else
using sock_t = int;
inline int sock_close(sock_t s) {
    return ::close(s);
}
inline bool sock_valid(sock_t s) {
    return s >= 0;
}
constexpr sock_t kBadSock = -1;
#endif

::psynder::u16 pick_port() {
    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sock_valid(s));
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
#if defined(_WIN32)
    int alen = sizeof(addr);
#else
    socklen_t alen = sizeof(addr);
#endif
    REQUIRE(::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen) == 0);
    ::psynder::u16 p = ntohs(addr.sin_port);
    sock_close(s);
    return p;
}

sock_t connect_local(::psynder::u16 port) {
    sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_valid(s))
        return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        sock_close(s);
        return kBadSock;
    }
    return s;
}

bool send_all(sock_t s, const ::psynder::u8* data, size_t n) {
    while (n) {
#if defined(_WIN32)
        auto sent = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(n), 0);
#else
        auto sent = ::send(s, reinterpret_cast<const char*>(data), n, 0);
#endif
        if (sent <= 0)
            return false;
        data += sent;
        n -= static_cast<size_t>(sent);
    }
    return true;
}

bool send_all_sv(sock_t s, std::string_view sv) {
    return send_all(s, reinterpret_cast<const ::psynder::u8*>(sv.data()), sv.size());
}

void set_recv_timeout(sock_t s, std::chrono::milliseconds ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(ms.count());
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv;
    tv.tv_sec = ms.count() / 1000;
    tv.tv_usec = static_cast<int>((ms.count() % 1000) * 1000);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// Read the HTTP response head off the socket, leaving any extra bytes in
// `tail` for the caller. Mirrors session.cpp's helper precisely.
std::string read_http_head(sock_t s,
                           std::vector<::psynder::u8>& tail,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    set_recv_timeout(s, timeout);
    tail.clear();
    std::string buf;
    char tmp[1024];
    for (;;) {
        auto got = ::recv(s, tmp, sizeof(tmp), 0);
        if (got <= 0)
            break;
        size_t prev = buf.size();
        buf.append(tmp, static_cast<size_t>(got));
        auto pos = buf.find("\r\n\r\n", prev > 3 ? prev - 3 : 0);
        if (pos != std::string::npos) {
            size_t head_end = pos + 4;
            if (head_end < buf.size()) {
                tail.assign(reinterpret_cast<const ::psynder::u8*>(buf.data() + head_end),
                            reinterpret_cast<const ::psynder::u8*>(buf.data() + buf.size()));
                buf.resize(head_end);
            }
            break;
        }
    }
    return buf;
}

std::vector<::psynder::u8> read_exact(sock_t s,
                                      size_t want,
                                      std::vector<::psynder::u8>& tail,
                                      std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    set_recv_timeout(s, timeout);
    std::vector<::psynder::u8> out;
    out.reserve(want);
    while (!tail.empty() && out.size() < want) {
        out.push_back(tail.front());
        tail.erase(tail.begin());
    }
    while (out.size() < want) {
        char tmp[512];
        auto got = ::recv(s, tmp, sizeof(tmp), 0);
        if (got <= 0)
            break;
        // Push into `out` up to `want`; spill any extra back into `tail` so
        // subsequent reads can consume it. (Without this spill the helper
        // silently drops bytes when the server merges multiple frames into
        // one recv — e.g. the WS header + payload landing in one packet on
        // localhost, where the 22-byte Welcome frame arrives whole.)
        ::psynder::isize i = 0;
        while (i < got && out.size() < want) {
            out.push_back(static_cast<::psynder::u8>(tmp[i]));
            ++i;
        }
        while (i < got) {
            tail.push_back(static_cast<::psynder::u8>(tmp[i]));
            ++i;
        }
    }
    return out;
}

// Pull one server-sent WebSocket frame off the socket. Server frames are
// unmasked and we expect single-frame (FIN=1) binary; everything else is a
// protocol error in our test surface.
std::vector<::psynder::u8> recv_ws_binary(
    sock_t s,
    std::vector<::psynder::u8>& tail,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    auto hdr = read_exact(s, 2, tail, timeout);
    REQUIRE(hdr.size() == 2);
    REQUIRE(hdr[0] == 0x82);        // FIN + binary
    REQUIRE((hdr[1] & 0x80) == 0);  // server frames are unmasked
    size_t len = hdr[1] & 0x7F;
    if (len == 126) {
        auto ext = read_exact(s, 2, tail, timeout);
        REQUIRE(ext.size() == 2);
        len = (static_cast<size_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        auto ext = read_exact(s, 8, tail, timeout);
        REQUIRE(ext.size() == 8);
        len = 0;
        for (auto b : ext)
            len = (len << 8) | b;
    }
    return read_exact(s, len, tail, timeout);
}

// Frame and send a client→server WebSocket binary message. Masking key is
// fixed (zeros) because the server XORs anyway; using zeros keeps the test
// payload visible in tcpdump if anyone ever needs to debug.
void send_ws_client_binary(sock_t s, const ::psynder::u8* data, size_t n) {
    std::vector<::psynder::u8> hdr;
    hdr.push_back(0x82);  // FIN + binary
    if (n <= 125) {
        hdr.push_back(static_cast<::psynder::u8>(0x80 | n));
    } else if (n <= 0xFFFF) {
        hdr.push_back(static_cast<::psynder::u8>(0x80 | 126));
        hdr.push_back(static_cast<::psynder::u8>(n >> 8));
        hdr.push_back(static_cast<::psynder::u8>(n));
    } else {
        hdr.push_back(static_cast<::psynder::u8>(0x80 | 127));
        for (int i = 7; i >= 0; --i)
            hdr.push_back(static_cast<::psynder::u8>(n >> (i * 8)));
    }
    // mask key — four zero bytes, payload XOR is identity.
    hdr.push_back(0);
    hdr.push_back(0);
    hdr.push_back(0);
    hdr.push_back(0);
    REQUIRE(send_all(s, hdr.data(), hdr.size()));
    if (n)
        REQUIRE(send_all(s, data, n));
}

// Run an HTTP+WS upgrade and consume the WelcomeFrame; returns the parsed
// Welcome and leaves `tail` ready for further server frames.
sock_t open_panel(::psynder::u16 port,
                  const std::string& token,
                  proto::Welcome& welcome,
                  std::vector<::psynder::u8>& tail) {
    sock_t s = connect_local(port);
    REQUIRE(sock_valid(s));
    std::string upgrade = "GET /ws?token=" + token +
                          " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(send_all_sv(s, upgrade));
    auto head = read_http_head(s, tail);
    REQUIRE(head.rfind("HTTP/1.1 101", 0) == 0);

    auto pl = recv_ws_binary(s, tail);
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kWelcomeFrame);
    REQUIRE(proto::Welcome_decode(r, welcome));
    return s;
}

// Send a SubscribeFrame for `channel` from the client.
void client_subscribe(sock_t s, std::string_view channel) {
    msgpack::Writer w;
    w.u16_(proto::opcodes::kSubscribeFrame);
    proto::Subscribe sub;
    sub.channel.assign(channel.data(), channel.size());
    proto::Subscribe_encode(w, sub);
    send_ws_client_binary(s, w.data(), w.size());
}

// Poll until both clients have registered their subscriptions on the server.
// Subscribe is async — the server reads the frame on the client thread, so
// after we send it we need to give the server a moment before pushing a
// delta. We do this by polling with a short read deadline on a *no-op*
// frame, but the simpler approach is just a 50 ms sleep — the connection
// thread reaches the Subscribe handler well within that window on a quiet
// box.
void wait_for_subscriptions() {
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

// RAII guard so the singleton stops cleanly even if Catch2 REQUIRE throws.
struct ServerGuard {
    Server* srv = &Server::Get();
    ServerGuard() = default;
    ~ServerGuard() { srv->stop(); }
};

// Thread-local capture for the REPL stub backend. The stub MUST be noexcept
// and may run on the engine pump thread, so we use a plain atomic counter
// and a std::string with a mutex.
struct StubBackendState {
    std::atomic<int> calls{0};
    std::mutex mu;
    std::string last_line;
};
StubBackendState g_stub{};
struct ExternalMirrorState {
    std::atomic<int> calls{0};
    std::mutex mu;
    std::string line;
    std::string output;
    std::string error;
};
ExternalMirrorState g_external_mirror{};

bool stub_repl_backend(std::string_view line, std::string& out) noexcept {
    g_stub.calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_stub.mu);
        g_stub.last_line.assign(line);
    }
    // The test sends "1+1". A real Lua VM would return "2"; we hard-code that
    // because the test contract is routing, not arithmetic.
    if (line == "1+1") {
        out = "2";
        return true;
    }
    out = "(stub: ";
    out.append(line);
    out += ")";
    return true;
}

void external_mirror_sink(std::string_view line,
                          const ::psynder::console::ExecuteResult& result) {
    g_external_mirror.calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_external_mirror.mu);
    g_external_mirror.line.assign(line);
    g_external_mirror.output = result.output;
    g_external_mirror.error = result.error;
}

}  // namespace

TEST_CASE("ipc: push_scene_delta routes per slice", "[ipc][delta]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string tok = srv.session_token();

    // Two panels: A on "perf", B on "selection".
    proto::Welcome wA, wB;
    std::vector<::psynder::u8> tailA, tailB;
    sock_t sA = open_panel(port, tok, wA, tailA);
    sock_t sB = open_panel(port, tok, wB, tailB);
    REQUIRE(sock_valid(sA));
    REQUIRE(sock_valid(sB));

    client_subscribe(sA, proto::channels::kperf);
    client_subscribe(sB, proto::channels::kselection);
    wait_for_subscriptions();

    // Push a delta to the "perf" slice. Payload is opaque msgpack — we use a
    // single u32 frame index so the assert side has something to verify.
    msgpack::Writer payload;
    payload.u32_(42u);
    internal::Server::Get().push_scene_delta(proto::channels::kperf,
                                             std::span<const ::psynder::u8>(payload.data(),
                                                                            payload.size()));

    // A should receive a SceneDeltaFrame; B should NOT.
    auto pl = recv_ws_binary(sA, tailA, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kSceneDeltaFrame);
    proto::SceneDeltaSlice sd;
    REQUIRE(proto::SceneDeltaSlice_decode(r, sd));
    REQUIRE(sd.slice == proto::channels::kperf);
    // Round-trip the payload back through a reader for byte-exact compare.
    REQUIRE(sd.payload.size() == payload.size());
    REQUIRE(std::memcmp(sd.payload.data(), payload.data(), payload.size()) == 0);

    // For B, set a short timeout and assert no bytes arrive. A 200 ms wait
    // is plenty: the server's broadcast loop is synchronous on the
    // push_scene_delta caller, so any cross-subscription leak would already
    // be in the kernel buffer for B.
    set_recv_timeout(sB, std::chrono::milliseconds(200));
    char dummy = 0;
    auto leaked = ::recv(sB, &dummy, 1, 0);
    REQUIRE(leaked <= 0);  // 0 = EOF, -1 = timeout — both acceptable

    sock_close(sA);
    sock_close(sB);
}

TEST_CASE("ipc: stats frame carries software render sections", "[ipc][stats][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string tok = srv.session_token();

    proto::Welcome welcome;
    std::vector<::psynder::u8> tail;
    sock_t s = open_panel(port, tok, welcome, tail);
    REQUIRE(sock_valid(s));

    client_subscribe(s, "profiler");
    wait_for_subscriptions();

    const StatsSection sections[] = {
        {"host/frame", 12.0f},
        {"render", 2.5f},
    };
    srv.broadcast_stats_tick(StatsTick{
        77u,
        12.5f,
        2.5f,
        9u,
        3u,
        std::span<const StatsSection>{sections},
    });

    auto pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kStatsFrame);

    ::psynder::u32 field_count = 0;
    REQUIRE(r.map_header(field_count));
    REQUIRE(field_count == 6u);

    ::psynder::u64 frame_index = 0;
    ::psynder::f32 cpu_ms = 0.0f;
    ::psynder::f32 render_ms = 0.0f;
    ::psynder::u32 draw_calls = 0;
    ::psynder::u32 entities = 0;
    ::psynder::u32 section_count = 0;

    for (::psynder::u32 i = 0; i < field_count; ++i) {
        std::string key;
        REQUIRE(r.str(key));
        if (key == "frame") {
            REQUIRE(r.u64_(frame_index));
        } else if (key == "cpu_ms") {
            REQUIRE(r.f32_(cpu_ms));
        } else if (key == "render_ms") {
            REQUIRE(r.f32_(render_ms));
        } else if (key == "draw_calls") {
            REQUIRE(r.u32_(draw_calls));
        } else if (key == "entities") {
            REQUIRE(r.u32_(entities));
        } else if (key == "sections") {
            REQUIRE(r.array_header(section_count));
            REQUIRE(section_count == 2u);
            for (::psynder::u32 j = 0; j < section_count; ++j) {
                ::psynder::u32 section_fields = 0;
                REQUIRE(r.map_header(section_fields));
                REQUIRE(section_fields == 2u);
                std::string section_name;
                ::psynder::f32 section_ms = 0.0f;
                for (::psynder::u32 k = 0; k < section_fields; ++k) {
                    std::string section_key;
                    REQUIRE(r.str(section_key));
                    if (section_key == "name") {
                        REQUIRE(r.str(section_name));
                    } else if (section_key == "ms") {
                        REQUIRE(r.f32_(section_ms));
                    } else {
                        REQUIRE(r.skip());
                    }
                }
                if (j == 1u) {
                    REQUIRE(section_name == "render");
                    REQUIRE(section_ms == 2.5f);
                }
            }
        } else {
            REQUIRE(r.skip());
        }
    }

    REQUIRE(frame_index == 77u);
    REQUIRE(cpu_ms == 12.5f);
    REQUIRE(render_ms == 2.5f);
    REQUIRE(draw_calls == 9u);
    REQUIRE(entities == 3u);
    REQUIRE(r.eof());

    sock_close(s);
}

TEST_CASE("ipc: profiler stat broadcast tolerates workbench subscriptions", "[ipc][stats][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string tok = srv.session_token();

    proto::Welcome welcome;
    std::vector<::psynder::u8> tail;
    sock_t s = open_panel(port, tok, welcome, tail);
    REQUIRE(sock_valid(s));

    client_subscribe(s, "profiler");
    client_subscribe(s, "stats");
    client_subscribe(s, "perf");
    wait_for_subscriptions();

    const StatsSection sections[] = {
        {"host/frame", 16.0f},
        {"render", 3.0f},
    };
    for (::psynder::u32 i = 0; i < 300u; ++i) {
        srv.broadcast_stats_tick(StatsTick{
            i,
            16.0f,
            3.0f,
            2u,
            1u,
            std::span<const StatsSection>{sections},
        });
    }

    auto pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kStatsFrame);

    sock_close(s);
}

TEST_CASE("ipc: stats broadcast reaps disconnected subscribers safely", "[ipc][stats][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string tok = srv.session_token();

    proto::Welcome welcome;
    std::vector<::psynder::u8> tail;
    sock_t s = open_panel(port, tok, welcome, tail);
    REQUIRE(sock_valid(s));

    client_subscribe(s, "profiler");
    wait_for_subscriptions();
    sock_close(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const StatsSection sections[] = {
        {"host/frame", 11.0f},
        {"render", 1.0f},
    };
    for (::psynder::u32 i = 0; i < 8u; ++i) {
        srv.broadcast_stats_tick(StatsTick{
            i,
            11.0f,
            1.0f,
            1u,
            1u,
            std::span<const StatsSection>{sections},
        });
    }
    SUCCEED("dead profiler subscriber was reaped without terminating");
}

TEST_CASE("ipc: schema bump stays back-compat", "[ipc][schema]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string tok = srv.session_token();

    // The Welcome struct shape is stable across protocol bumps. An older panel
    // decoder sees the newer server_ver and can branch on the value — our
    // assertion here is that the value flows through and is the bumped
    // version, not the old 1.
    proto::Welcome welcome;
    std::vector<::psynder::u8> tail;
    sock_t s = open_panel(port, tok, welcome, tail);
    REQUIRE(sock_valid(s));
    REQUIRE(welcome.accepted);
    REQUIRE(welcome.server_ver == 4u);
    REQUIRE(proto::kProtocolVersion == 4u);

    // Forward-compat: ship the server a frame with an unknown opcode and
    // verify the connection stays alive (we can still drive normal traffic
    // afterwards). The "any unknown opcode treated as no-op rather than
    // disconnect" contract is what lets an older client speak to a v4 server
    // five waves from now without an upgrade flag-day.
    {
        msgpack::Writer w;
        w.u16_(static_cast<::psynder::u16>(0xFEFE));  // reserved-for-future
        w.boolean(true);
        send_ws_client_binary(s, w.data(), w.size());
    }
    // After the no-op, a Subscribe should still register and drive a
    // broadcast back. We use the "schemas" slice — a v2-only channel.
    client_subscribe(s, proto::channels::kschemas);
    wait_for_subscriptions();
    msgpack::Writer dpayload;
    dpayload.str("schema-list-v2");
    internal::Server::Get().push_scene_delta(proto::channels::kschemas,
                                             std::span<const ::psynder::u8>(dpayload.data(),
                                                                            dpayload.size()));

    auto pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kSceneDeltaFrame);
    proto::SceneDeltaSlice sd;
    REQUIRE(proto::SceneDeltaSlice_decode(r, sd));
    REQUIRE(sd.slice == proto::channels::kschemas);

    sock_close(s);
}

TEST_CASE("ipc: console frame separates engine console and lua", "[ipc][repl]") {
    auto& console = ::psynder::console::Console::Get();
    console.RegisterCommand("test_ipc_echo",
                            "unit-test echo command",
                            [](std::span<const std::string_view> args,
                               ::psynder::console::Output& out) {
                                out.Print("engine");
                                for (std::string_view arg : args) {
                                    out.Print(" ");
                                    out.Print(arg);
                                }
                            });

    // Install our stub backend BEFORE starting the server so the script
    // fallback is live. The previous test's backend (if any) is overwritten —
    // set_repl_backend takes the most recent install.
    g_stub.calls.store(0);
    {
        std::lock_guard<std::mutex> lk(g_stub.mu);
        g_stub.last_line.clear();
    }
    g_external_mirror.calls.store(0);
    {
        std::lock_guard<std::mutex> lk(g_external_mirror.mu);
        g_external_mirror.line.clear();
        g_external_mirror.output.clear();
        g_external_mirror.error.clear();
    }
    console.ClearExternalExecutionSinks();
    console.AddExternalExecutionSink(&external_mirror_sink);
    struct SinkRestore {
        ~SinkRestore() { ::psynder::console::Console::Get().ClearExternalExecutionSinks(); }
    } sink_restore;
    ::psynder::script::set_repl_backend(&stub_repl_backend);
    struct BackendRestore {
        ~BackendRestore() { ::psynder::script::set_repl_backend(nullptr); }
    } restore;

    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string tok = srv.session_token();

    proto::Welcome welcome;
    std::vector<::psynder::u8> tail;
    sock_t s = open_panel(port, tok, welcome, tail);
    REQUIRE(sock_valid(s));

    // Send a real engine-console command. The REPL stub should not run.
    {
        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleFrame);
        proto::ConsoleCmd cmd;
        cmd.text = "test_ipc_echo hello web";
        cmd.mode = "console";
        proto::ConsoleCmd_encode(w, cmd);
        send_ws_client_binary(s, w.data(), w.size());
    }
    // Give the server thread time to land the InboundCmd before we pump.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    srv.pump();

    // Read back the ConsoleReplyFrame.
    auto pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kConsoleReplyFrame);
    proto::ConsoleReply rep;
    REQUIRE(proto::ConsoleReply_decode(r, rep));
    REQUIRE(rep.ok);
    REQUIRE(rep.text == "engine hello web");
    REQUIRE(g_stub.calls.load() == 0);
    REQUIRE(g_external_mirror.calls.load() == 1);
    {
        std::lock_guard<std::mutex> lk(g_external_mirror.mu);
        REQUIRE(g_external_mirror.line == "test_ipc_echo hello web");
        REQUIRE(g_external_mirror.output == "engine hello web");
        REQUIRE(g_external_mirror.error.empty());
    }

    // Unknown engine-console input stays a console error and does not fall
    // through to Lua.
    {
        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleFrame);
        proto::ConsoleCmd cmd;
        cmd.text = "1+1";
        cmd.mode = "console";
        proto::ConsoleCmd_encode(w, cmd);
        send_ws_client_binary(s, w.data(), w.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    srv.pump();

    pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r2(pl.data(), pl.size());
    op = 0;
    REQUIRE(r2.u16_(op));
    REQUIRE(op == proto::opcodes::kConsoleReplyFrame);
    proto::ConsoleReply rep2;
    REQUIRE(proto::ConsoleReply_decode(r2, rep2));
    REQUIRE_FALSE(rep2.ok);
    REQUIRE(rep2.text.find("unknown command or cvar") != std::string::npos);
    REQUIRE(g_stub.calls.load() == 0);

    // Lua mode routes explicitly to the script REPL.
    {
        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleFrame);
        proto::ConsoleCmd cmd;
        cmd.text = "1+1";
        cmd.mode = "lua";
        proto::ConsoleCmd_encode(w, cmd);
        send_ws_client_binary(s, w.data(), w.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    srv.pump();

    pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r3(pl.data(), pl.size());
    op = 0;
    REQUIRE(r3.u16_(op));
    REQUIRE(op == proto::opcodes::kConsoleReplyFrame);
    proto::ConsoleReply rep3;
    REQUIRE(proto::ConsoleReply_decode(r3, rep3));
    REQUIRE(rep3.ok);
    REQUIRE(rep3.text == "2");

    // Sanity: the stub was actually invoked once with the right line.
    REQUIRE(g_stub.calls.load() == 1);
    {
        std::lock_guard<std::mutex> lk(g_stub.mu);
        REQUIRE(g_stub.last_line == "1+1");
    }

    // Completion requests are served by core/console/Completion, not by a
    // separate web-only table.
    {
        msgpack::Writer w;
        w.u16_(proto::opcodes::kConsoleCompletionQueryFrame);
        proto::ConsoleCompletionQuery query;
        query.id = 77;
        query.input = "test_ip";
        query.cursor = static_cast<::psynder::u32>(query.input.size());
        proto::ConsoleCompletionQuery_encode(w, query);
        send_ws_client_binary(s, w.data(), w.size());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    srv.pump();

    pl = recv_ws_binary(s, tail, std::chrono::milliseconds(2000));
    REQUIRE_FALSE(pl.empty());
    msgpack::Reader r4(pl.data(), pl.size());
    op = 0;
    REQUIRE(r4.u16_(op));
    REQUIRE(op == proto::opcodes::kConsoleCompletionReplyFrame);
    proto::ConsoleCompletionReply comp;
    REQUIRE(proto::ConsoleCompletionReply_decode(r4, comp));
    REQUIRE(comp.id == 77u);
    REQUIRE(comp.start == 0u);
    REQUIRE(comp.end == 7u);
    REQUIRE(std::find(comp.names.begin(), comp.names.end(), "test_ipc_echo") != comp.names.end());

    sock_close(s);
}
