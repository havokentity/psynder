// SPDX-License-Identifier: MIT
// Psynder — in-process loopback transport. Lane 14 internal.
//
// A single process-wide bus carries datagrams between Hosts started on
// distinct logical "ports". The bus is the substrate the unit-test harness
// uses to exercise rUDP without a real socket (and the substrate the editor
// uses for multi-user co-op editing in the same process, per DESIGN.md
// §10.4 + §10.8).
//
// The bus is intentionally simple:
//   - Each Host registers itself under a unique 16-bit port.
//   - send_to() copies the datagram into the destination Host's mailbox.
//   - poll() drains the mailbox.
//   - A "loss policy" hook lets tests inject deterministic packet loss
//     without touching the rUDP code.

#pragma once

#include "core/Types.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace psynder::net {

class HostImpl;  // forward.

// LossPolicy decides whether a single datagram from src_port -> dst_port
// (carrying `bytes`) reaches the destination. Return true to deliver, false
// to drop. The default policy (nullptr) delivers everything.
using LossPolicy =
    std::function<bool(u16 src_port, u16 dst_port, std::span<const u8> bytes, u32 deliver_attempt)>;

class LoopbackBus {
   public:
    static LoopbackBus& Get();

    bool bind(u16 port, HostImpl* host) noexcept;
    void unbind(u16 port) noexcept;
    HostImpl* lookup(u16 port) const noexcept;

    // Deliver `bytes` from src_port -> dst_port. Returns true if the dest
    // Host accepted the datagram (no loss policy or policy said yes).
    bool send_to(u16 src_port, u16 dst_port, std::span<const u8> bytes) noexcept;

    // Override the loss policy. Pass {} to restore "deliver all".
    void set_loss_policy(LossPolicy p) noexcept;

    // Reset state — used by tests in between cases.
    void reset() noexcept;

   private:
    LoopbackBus() = default;
    mutable std::mutex mu_;
    std::unordered_map<u16, HostImpl*> hosts_;
    LossPolicy loss_;
    u32 attempts_ = 0;
};

}  // namespace psynder::net
