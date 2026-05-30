// SPDX-License-Identifier: MIT
// Psynder - localhost UDP datagram socket. Lane 14 internal (Wave B).
//
// A thin, non-blocking BSD/Winsock datagram socket bound to 127.0.0.1. This
// is the real-wire substrate behind HostImpl when UDP is enabled; the
// in-process LoopbackBus remains the deterministic test/offline path. The
// socket carries the SAME channel-framed packets (Frame.h) the loopback path
// does - the framing/reliability layers above do not know which transport
// they ride.
//
// Design constraints (mission HARD constraints):
//   - Bind 127.0.0.1 only (localhost; no exposure).
//   - Non-blocking: recv() returns 0 when the kernel buffer is empty rather
//     than stalling the tick.
//   - Alloc-free on the hot path: send() / recv() take caller-owned buffers;
//     this type allocates nothing per call. The recv buffer is pooled by the
//     caller (HostImpl).
//   - Endian-stable: we move raw bytes; the 16-bit port is the only numeric
//     field and is handled by the OS sockaddr in network byte order. The
//     application payload is already little-endian (Frame.cpp).
//
// If sockets are unavailable in a headless CI sandbox, open() simply fails
// and the caller falls back to LoopbackBus; nothing aborts.

#pragma once

#include "core/Types.h"

#include <span>

namespace psynder::net {

// A non-blocking IPv4/UDP socket bound to the loopback interface. Holds an OS
// socket file descriptor (or INVALID); move-only so ownership is clear.
class UdpSocket {
   public:
    UdpSocket() noexcept = default;
    ~UdpSocket() noexcept;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& o) noexcept;
    UdpSocket& operator=(UdpSocket&& o) noexcept;

    // One-time process init for platforms that need it (Winsock WSAStartup).
    // Idempotent; safe to call from every open(). Returns false if the socket
    // subsystem could not be initialised (sockets unavailable).
    static bool platform_init() noexcept;

    // Bind a non-blocking UDP socket to 127.0.0.1:`port`. Pass port 0 to let
    // the OS pick an ephemeral port (read back via local_port()). Returns
    // false if the subsystem is unavailable or the bind fails (e.g. port in
    // use) - the caller then falls back to the loopback bus.
    bool open(u16 port) noexcept;
    void close() noexcept;

    bool is_open() const noexcept { return fd_ >= 0; }

    // The port we are actually bound to (resolves an ephemeral 0 to the real
    // assigned port). 0 if not open.
    u16 local_port() const noexcept { return port_; }

    // Send `bytes` to 127.0.0.1:`dst_port`. Returns true if the datagram was
    // handed to the kernel. Never blocks; a full send buffer drops the packet
    // (UDP semantics - the reliability layer retransmits).
    bool send_to(u16 dst_port, std::span<const u8> bytes) const noexcept;

    // Non-blocking receive of one datagram into `out`. On success writes the
    // source port to `out_src_port` and returns the byte count (>0). Returns 0
    // when no datagram is queued (would-block) or on a recoverable error.
    usize recv_from(std::span<u8> out, u16& out_src_port) const noexcept;

   private:
    int fd_ = -1;   // OS socket descriptor; -1 == closed. (SOCKET on Win32.)
    u16 port_ = 0;  // resolved bound port.
};

}  // namespace psynder::net
