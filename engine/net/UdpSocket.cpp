// SPDX-License-Identifier: MIT
// Psynder - localhost UDP datagram socket impl. Lane 14 internal (Wave B).

#include "UdpSocket.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using psy_socklen_t = int;
#define PSY_INVALID_FD (-1)  // we store INVALID_SOCKET-equivalent as -1 in int fd_.
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using psy_socklen_t = socklen_t;
#define PSY_INVALID_FD (-1)
#endif

namespace psynder::net {

namespace {

#if defined(_WIN32)
// On Win32 SOCKET is an unsigned handle; we tunnel it through `int fd_` using
// -1 as the sentinel. Helpers translate between the two consistently.
inline SOCKET to_sock(int fd) noexcept { return fd < 0 ? INVALID_SOCKET : static_cast<SOCKET>(fd); }
inline int from_sock(SOCKET s) noexcept { return s == INVALID_SOCKET ? -1 : static_cast<int>(s); }
bool g_wsa_ready = false;
#endif

}  // namespace

bool UdpSocket::platform_init() noexcept {
#if defined(_WIN32)
    if (g_wsa_ready)
        return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;
    g_wsa_ready = true;
    return true;
#else
    return true;  // POSIX needs no global init.
#endif
}

UdpSocket::~UdpSocket() noexcept {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_), port_(o.port_) {
    o.fd_ = -1;
    o.port_ = 0;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        port_ = o.port_;
        o.fd_ = -1;
        o.port_ = 0;
    }
    return *this;
}

bool UdpSocket::open(u16 port) noexcept {
    if (!platform_init())
        return false;
    close();

#if defined(_WIN32)
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return false;
#endif

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only.

#if defined(_WIN32)
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        return false;
    }
    // Non-blocking mode.
    u_long nb = 1;
    if (::ioctlsocket(s, FIONBIO, &nb) != 0) {
        ::closesocket(s);
        return false;
    }
#else
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return false;
    }
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0 || ::fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(s);
        return false;
    }
#endif

    // Resolve the actually-bound port (handles the ephemeral port-0 case).
    sockaddr_in bound;
    std::memset(&bound, 0, sizeof(bound));
    psy_socklen_t blen = sizeof(bound);
#if defined(_WIN32)
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        port_ = ntohs(bound.sin_port);
    else
        port_ = port;
    fd_ = from_sock(s);
#else
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        port_ = ntohs(bound.sin_port);
    else
        port_ = port;
    fd_ = s;
#endif
    return true;
}

void UdpSocket::close() noexcept {
    if (fd_ < 0)
        return;
#if defined(_WIN32)
    ::closesocket(to_sock(fd_));
#else
    ::close(fd_);
#endif
    fd_ = -1;
    port_ = 0;
}

bool UdpSocket::send_to(u16 dst_port, std::span<const u8> bytes) const noexcept {
    if (fd_ < 0)
        return false;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#if defined(_WIN32)
    int n = ::sendto(to_sock(fd_), reinterpret_cast<const char*>(bytes.data()),
                     static_cast<int>(bytes.size()), 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<int>(bytes.size());
#else
    ssize_t n = ::sendto(fd_, bytes.data(), bytes.size(), 0,
                         reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<ssize_t>(bytes.size());
#endif
}

usize UdpSocket::recv_from(std::span<u8> out, u16& out_src_port) const noexcept {
    if (fd_ < 0)
        return 0;
    sockaddr_in src;
    std::memset(&src, 0, sizeof(src));
    psy_socklen_t slen = sizeof(src);
#if defined(_WIN32)
    int n = ::recvfrom(to_sock(fd_), reinterpret_cast<char*>(out.data()),
                       static_cast<int>(out.size()), 0,
                       reinterpret_cast<sockaddr*>(&src), &slen);
    if (n <= 0)
        return 0;  // would-block / error -> no datagram this call.
    out_src_port = ntohs(src.sin_port);
    return static_cast<usize>(n);
#else
    ssize_t n = ::recvfrom(fd_, out.data(), out.size(), 0,
                           reinterpret_cast<sockaddr*>(&src), &slen);
    if (n <= 0)
        return 0;  // EAGAIN/EWOULDBLOCK -> empty; or recoverable error.
    out_src_port = ntohs(src.sin_port);
    return static_cast<usize>(n);
#endif
}

}  // namespace psynder::net
