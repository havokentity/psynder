// SPDX-License-Identifier: MIT
// Psynder — in-process loopback transport impl. Lane 14 internal.

#include "Loopback.h"
#include "HostImpl.h"

#include <utility>
#include <vector>

namespace psynder::net {

LoopbackBus& LoopbackBus::Get() {
    static LoopbackBus s;
    return s;
}

bool LoopbackBus::bind(u16 port, HostImpl* host) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    auto [it, ok] = hosts_.try_emplace(port, host);
    return ok;
}

void LoopbackBus::unbind(u16 port) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    hosts_.erase(port);
}

HostImpl* LoopbackBus::lookup(u16 port) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = hosts_.find(port);
    return it == hosts_.end() ? nullptr : it->second;
}

bool LoopbackBus::send_to(u16 src_port, u16 dst_port, std::span<const u8> bytes) noexcept {
    HostImpl* dst = nullptr;
    LossPolicy policy;
    u32 attempt;
    {
        std::lock_guard<std::mutex> lk(mu_);
        attempt = ++attempts_;
        auto it = hosts_.find(dst_port);
        if (it == hosts_.end())
            return false;
        dst = it->second;
        policy = loss_;  // copy ref-counted std::function under the lock.
    }
    if (policy && !policy(src_port, dst_port, bytes, attempt)) {
        return false;  // intentionally dropped.
    }
    // Copy bytes into a stack buffer so the caller can re-use its src buf.
    std::vector<u8> copy(bytes.begin(), bytes.end());
    dst->on_datagram(src_port, std::span<const u8>(copy.data(), copy.size()));
    return true;
}

void LoopbackBus::set_loss_policy(LossPolicy p) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    loss_ = std::move(p);
}

void LoopbackBus::reset() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    hosts_.clear();
    loss_ = {};
    attempts_ = 0;
}

}  // namespace psynder::net
