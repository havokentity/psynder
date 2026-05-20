// SPDX-License-Identifier: MIT
// Psynder — reliable-UDP networking. Lockstep racing + snapshot/AOI FPS.
// Lane 14 owns.

#pragma once

#include "core/Types.h"

#include <span>

namespace psynder::net {

struct PeerTag {};
using PeerId = Handle<PeerTag>;

enum class Mode : u8 { Lockstep, ClientServer };

struct HostDesc {
    Mode mode = Mode::ClientServer;
    u16 port = 7878;
    u16 max_peers = 32;
};

class Host {
   public:
    static Host& Get();
    bool start(const HostDesc& desc);
    void stop();

    PeerId connect(const char* host, u16 port);

    // Send / receive raw bytes; reliability and ordering handled by the
    // transport.
    void send(PeerId peer, std::span<const u8> bytes, bool reliable);
    void poll(void (*on_message)(PeerId, std::span<const u8>, void*) noexcept, void* user);
};

}  // namespace psynder::net
