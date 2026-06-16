// SPDX-License-Identifier: MIT
// Psynder editor IPC — session-token auth + HTTP/WS handshake tests. Lane 19.
//
// Covers:
//   1. The token generator produces fresh, non-empty tokens.
//   2. The constant_time_equal check accepts the real token and rejects bad
//      ones (timing-attack defence is also unit-coverable for correctness).
//   3. A live server bound to an ephemeral port answers HTTP probes.
//   4. WebSocket upgrade with a bad token is rejected with HTTP 401.
//   5. WebSocket upgrade with the real token succeeds and we receive the
//      WelcomeFrame.

#include "editor/ipc/Ipc.h"
#include "editor/ipc/internal/Crypto.h"
#include "editor/ipc/internal/Msgpack.h"
#include "editor/ipc/internal/Server.h"
#include "editor/ipc/internal/WsFrame.h"
#include "editor/ipc/proto/Protocol.gen.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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
#else
using sock_t = int;
inline int sock_close(sock_t s) {
    return ::close(s);
}
inline bool sock_valid(sock_t s) {
    return s >= 0;
}
#endif

// Pick an ephemeral port to avoid colliding with concurrent test runs / the
// real engine. We bind a temporary listener to port 0, read back what the
// OS gave us, then immediately close it so the IPC server can grab the same
// port. This is racy in principle but reliable in practice on a quiet box.
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
#if defined(_WIN32)
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }
    return s;
}

bool send_all(sock_t s, std::string_view data) {
    const char* p = data.data();
    size_t n = data.size();
    while (n) {
#if defined(_WIN32)
        auto sent = ::send(s, p, static_cast<int>(n), 0);
#else
        auto sent = ::send(s, p, n, 0);
#endif
        if (sent <= 0)
            return false;
        p += sent;
        n -= static_cast<size_t>(sent);
    }
    return true;
}

// Apply a recv timeout to a socket so blocking reads don't hang the test.
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

// Read until we have a CRLF CRLF (HTTP head boundary) or the socket closes.
// `tail_after_head` (out): any bytes already received past the boundary; the
// caller must consume these before issuing further recv() calls.
std::string read_http_head(sock_t s,
                           std::vector<::psynder::u8>& tail_after_head,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    set_recv_timeout(s, timeout);
    tail_after_head.clear();
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
                tail_after_head.assign(reinterpret_cast<const ::psynder::u8*>(buf.data() + head_end),
                                       reinterpret_cast<const ::psynder::u8*>(buf.data() + buf.size()));
                buf.resize(head_end);
            }
            break;
        }
    }
    return buf;
}

// Read N bytes, drawing first from an in-memory tail then from the socket.
// Bytes received past `want` are pushed back into `tail` so the next call
// can consume them — without that spill the helper drops anything that
// arrives in the same recv() chunk as the wanted prefix. (Hit when the
// server flushes WS header + payload as a single packet on localhost.)
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

}  // namespace

TEST_CASE("ipc: session token generation", "[ipc][session]") {
    auto a = crypto::make_session_token();
    auto b = crypto::make_session_token();
    REQUIRE(a.size() == 32);
    REQUIRE(b.size() == 32);
    // 128 bits of entropy collision is astronomically unlikely.
    REQUIRE(a != b);
    // Hex chars only.
    for (char c : a) {
        REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}

TEST_CASE("ipc: constant-time token compare", "[ipc][session]") {
    auto tok = crypto::make_session_token();
    REQUIRE(crypto::constant_time_equal(tok, tok));
    REQUIRE_FALSE(crypto::constant_time_equal(tok, ""));
    REQUIRE_FALSE(crypto::constant_time_equal(tok, tok + "x"));
    // One-character flip — still rejected.
    std::string flipped = tok;
    flipped[0] = (flipped[0] == '0') ? '1' : '0';
    REQUIRE_FALSE(crypto::constant_time_equal(tok, flipped));
}

// RAII guard so Catch2 REQUIRE-throw early-exits still stop() the singleton.
struct ServerGuard {
    Server* srv = &Server::Get();
    ServerGuard() = default;
    ~ServerGuard() { srv->stop(); }
};

TEST_CASE("ipc: live server rejects bad WebSocket token", "[ipc][session][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));

    sock_t s = connect_local(port);
    REQUIRE(sock_valid(s));

    // Build a WebSocket upgrade with a deliberately wrong token.
    std::string upgrade =
        "GET /ws?token=BOGUS HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(send_all(s, upgrade));

    std::vector<::psynder::u8> tail;
    auto head = read_http_head(s, tail);
    sock_close(s);

    REQUIRE_FALSE(head.empty());
    // First line MUST be 401 — anything 1xx/2xx/3xx is a vuln.
    REQUIRE(head.rfind("HTTP/1.1 401", 0) == 0);
}

TEST_CASE("ipc: live server accepts good WebSocket token", "[ipc][session][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));
    const std::string good_token = srv.session_token();
    REQUIRE(good_token.size() == 32);

    sock_t s = connect_local(port);
    REQUIRE(sock_valid(s));

    std::string upgrade = "GET /ws?token=" + good_token +
                          " HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                          "Sec-WebSocket-Version: 13\r\n\r\n";
    REQUIRE(send_all(s, upgrade));

    std::vector<::psynder::u8> tail;
    auto head = read_http_head(s, tail);
    REQUIRE_FALSE(head.empty());
    REQUIRE(head.rfind("HTTP/1.1 101", 0) == 0);
    // RFC 6455 §4.2.2 says the accept value for the sample key
    // "dGhlIHNhbXBsZSBub25jZQ==" is "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=".
    REQUIRE(head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

    // After the handshake, the server enqueues a WelcomeFrame. The first byte
    // is a WS frame header — for a small binary frame this is 0x82, followed
    // by length byte and the msgpack payload. The bytes may already be in
    // `tail` if recv() returned them with the HTTP response.
    auto fhead = read_exact(s, 2, tail);
    REQUIRE(fhead.size() == 2);
    REQUIRE(fhead[0] == 0x82);  // FIN + binary
    size_t plen = fhead[1] & 0x7F;
    // Welcome msgpack is short (under 126 bytes); 2-byte header is enough.
    REQUIRE(plen > 0);
    REQUIRE((fhead[1] & 0x80) == 0);  // server frames are unmasked
    auto pl = read_exact(s, plen, tail);
    REQUIRE(pl.size() == plen);

    // Parse: opcode (u16) + Welcome body.
    msgpack::Reader r(pl.data(), pl.size());
    ::psynder::u16 op = 0;
    REQUIRE(r.u16_(op));
    REQUIRE(op == proto::opcodes::kWelcomeFrame);
    proto::Welcome w;
    REQUIRE(proto::Welcome_decode(r, w));
    REQUIRE(w.accepted);
    REQUIRE(w.server_ver == proto::kProtocolVersion);

    sock_close(s);
}

TEST_CASE("ipc: server destructor stops live websocket workers", "[ipc][session][server]") {
    auto port = pick_port();
    sock_t s;
    {
        psynder::editor::ipc::internal::Server srv;
        REQUIRE(srv.start("127.0.0.1", port, true));
        const std::string good_token = srv.session_token();

        s = connect_local(port);
        REQUIRE(sock_valid(s));
        std::string upgrade = "GET /ws?token=" + good_token +
                              " HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\n\r\n";
        REQUIRE(send_all(s, upgrade));

        std::vector<::psynder::u8> tail;
        auto head = read_http_head(s, tail);
        REQUIRE(head.rfind("HTTP/1.1 101", 0) == 0);
    }
    sock_close(s);
}

TEST_CASE("ipc: HTTP healthz route works without auth", "[ipc][http][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));

    sock_t s = connect_local(port);
    REQUIRE(sock_valid(s));
    REQUIRE(send_all(s, "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    std::vector<::psynder::u8> tail;
    auto head = read_http_head(s, tail);
    // The body ("ok\n") may arrive in a TCP segment *after* the header
    // boundary — macOS usually coalesces head+body into one recv(), but
    // Linux frequently splits them, leaving `tail` empty. Drain whatever
    // remains so the body assertion is timing-independent.
    {
        set_recv_timeout(s, std::chrono::milliseconds(500));
        char tmp[256];
        for (;;) {
            auto got = ::recv(s, tmp, sizeof(tmp), 0);
            if (got <= 0)
                break;
            tail.insert(tail.end(),
                        reinterpret_cast<const ::psynder::u8*>(tmp),
                        reinterpret_cast<const ::psynder::u8*>(tmp) + got);
        }
    }
    sock_close(s);
    REQUIRE(head.rfind("HTTP/1.1 200", 0) == 0);
    // Content-Length: 3 then body "ok\n" appears either after the head boundary
    // (in `tail`) or already merged into `head` — accept either form.
    std::string body(reinterpret_cast<const char*>(tail.data()), tail.size());
    REQUIRE((body == "ok\n" || head.find("ok") != std::string::npos));
}

TEST_CASE("ipc: HTTP panel route serves an editor page without auth", "[ipc][http][server]") {
    ServerGuard guard;
    auto port = pick_port();
    ServerDesc desc;
    desc.bind_host = "127.0.0.1";
    desc.port = port;
    desc.require_session_token = true;
    auto& srv = *guard.srv;
    REQUIRE(srv.start(desc));

    sock_t s = connect_local(port);
    REQUIRE(sock_valid(s));
    REQUIRE(send_all(s, "GET /panels/console?token=test HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
    std::vector<::psynder::u8> tail;
    auto head = read_http_head(s, tail);
    {
        set_recv_timeout(s, std::chrono::milliseconds(500));
        char tmp[1024];
        for (;;) {
            auto got = ::recv(s, tmp, sizeof(tmp), 0);
            if (got <= 0)
                break;
            tail.insert(tail.end(),
                        reinterpret_cast<const ::psynder::u8*>(tmp),
                        reinterpret_cast<const ::psynder::u8*>(tmp) + got);
        }
    }
    sock_close(s);

    REQUIRE(head.rfind("HTTP/1.1 200", 0) == 0);
    REQUIRE(head.find("Content-Type: text/html") != std::string::npos);
    std::string body(reinterpret_cast<const char*>(tail.data()), tail.size());
    REQUIRE((body.find("<div id=\"root\"></div>") != std::string::npos ||
             body.find("Psynder Editor") != std::string::npos));
}
