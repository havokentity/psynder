// SPDX-License-Identifier: MIT
// Psynder — HostImpl. Lane 14 internal core.

#include "HostImpl.h"
#include "Loopback.h"

#include <algorithm>
#include <utility>

namespace psynder::net {

HostImpl::~HostImpl() { stop(); }

bool HostImpl::start(const HostDesc& desc) noexcept {
    if (started_) return false;
    port_      = desc.port;
    max_peers_ = desc.max_peers;
    mode_      = desc.mode;
    tick_      = 0;
    next_peer_handle_ = 1;
    next_peer_index_  = 0;
    peers_.clear();
    inbox_.clear();
    pending_ack_peers_.clear();

    if (!LoopbackBus::Get().bind(port_, this)) {
        port_ = 0;
        return false;
    }
    started_ = true;
    return true;
}

void HostImpl::stop() noexcept {
    if (!started_) return;
    LoopbackBus::Get().unbind(port_);
    started_  = false;
    port_     = 0;
    peers_.clear();
    inbox_.clear();
    pending_ack_peers_.clear();
}

PeerState* HostImpl::find_peer_(PeerId id) noexcept {
    auto it = peers_.find(id.raw);
    return it == peers_.end() ? nullptr : &it->second;
}

PeerState* HostImpl::find_peer_by_port_(u16 port) noexcept {
    for (auto& [k, v] : peers_) {
        if (v.remote_port == port) return &v;
    }
    return nullptr;
}

PeerId HostImpl::register_peer_(u16 remote_port) noexcept {
    if (peers_.size() >= max_peers_) return PeerId{};
    PeerId h{ next_peer_handle_++ };
    PeerState ps;
    ps.id          = h;
    ps.remote_port = remote_port;
    ps.peer_index  = next_peer_index_++;
    peers_.emplace(h.raw, std::move(ps));
    return h;
}

PeerId HostImpl::connect(u16 dest_port) noexcept {
    if (!started_) return PeerId{};
    // Reuse existing peer state if we already know about that port.
    if (PeerState* existing = find_peer_by_port_(dest_port)) {
        return existing->id;
    }
    return register_peer_(dest_port);
}

void HostImpl::send_raw_(u16 dst_port, const FrameHeader& h,
                         std::span<const u8> payload) noexcept {
    u8 buf[kFrameHeaderBytes + 1500];
    if (payload.size() > sizeof(buf) - kFrameHeaderBytes) return;
    if (!encode_header(h, std::span<u8>(buf, kFrameHeaderBytes))) return;
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), buf + kFrameHeaderBytes);
    }
    LoopbackBus::Get().send_to(port_, dst_port,
                               std::span<const u8>(buf, kFrameHeaderBytes + payload.size()));
}

void HostImpl::send(PeerId peer, std::span<const u8> bytes,
                    bool reliable, u8 channel) noexcept {
    if (!started_) return;
    PeerState* ps = find_peer_(peer);
    if (!ps) return;

    FrameHeader h;
    h.channel     = channel;
    h.payload_len = u16(bytes.size());
    ps->recv.snapshot(h.ack_base, h.ack_bits);

    if (reliable) {
        u32 seq = ps->send.enqueue(bytes, tick_);
        if (seq == 0) return;  // window full; caller backs off
        h.seq    = seq;
        h.flags |= kFlagReliable;
        ps->send.mark_transmitted(seq, tick_);
    } else {
        h.seq = 0;
    }
    send_raw_(ps->remote_port, h, bytes);
}

void HostImpl::on_datagram(u16 src_port, std::span<const u8> bytes) noexcept {
    if (!started_) return;
    FrameHeader h;
    if (!decode_header(bytes, h)) return;
    if (bytes.size() < kFrameHeaderBytes + h.payload_len) return;
    std::span<const u8> payload(bytes.data() + kFrameHeaderBytes, h.payload_len);

    // Locate / auto-register sender. Loopback datagrams from an unknown port
    // imply the peer is initiating contact; we accept and register, so the
    // test harness doesn't need an explicit "accept" step on each side.
    PeerState* ps = find_peer_by_port_(src_port);
    if (!ps) {
        PeerId id = register_peer_(src_port);
        if (!id.valid()) return;  // peer table full; drop
        ps = find_peer_(id);
        if (!ps) return;
    }

    // Apply piggy-backed ack from the peer.
    if (h.ack_base != 0) {
        ps->send.apply_ack(h.ack_base, h.ack_bits);
    }

    if (h.flags & kFlagAckOnly) {
        return;  // no payload to deliver.
    }

    if (h.flags & kFlagReliable) {
        handle_reliable_in_(*ps, h, payload);
    } else {
        handle_unreliable_in_(*ps, h, payload);
    }
    // Owe this peer an ack on the next flush.
    if (std::find(pending_ack_peers_.begin(), pending_ack_peers_.end(), ps->id)
        == pending_ack_peers_.end()) {
        pending_ack_peers_.push_back(ps->id);
    }
}

void HostImpl::handle_reliable_in_(PeerState& ps, const FrameHeader& h,
                                   std::span<const u8> payload) noexcept {
    // observe() handles the 32-bit selective-ACK window dedup; the
    // in-order delivery cursor + ooo_buffer handles dedup for older holes
    // that have fallen outside the bitmap.
    if (!ps.recv.observe(h.seq)) {
        return;  // duplicate within the bitmap window
    }
    // Hard dedup against the in-order cursor: anything ≤ what we've already
    // surfaced is a stale retransmit.
    if (h.seq < ps.next_deliver) {
        return;
    }
    if (h.seq == ps.next_deliver) {
        InboundMessage im{};
        im.from     = ps.id;
        im.channel  = h.channel;
        im.reliable = true;
        im.bytes.assign(payload.begin(), payload.end());
        inbox_.push_back(std::move(im));
        ++ps.next_deliver;
        drain_ooo_(ps);
        return;
    }
    // Future seq — buffer if not already present.
    auto it = ps.ooo_buffer.find(h.seq);
    if (it == ps.ooo_buffer.end()) {
        ps.ooo_buffer[h.seq].assign(payload.begin(), payload.end());
    }
}

void HostImpl::handle_unreliable_in_(PeerState& ps, const FrameHeader& /*h*/,
                                     std::span<const u8> payload) noexcept {
    InboundMessage im{};
    im.from     = ps.id;
    im.channel  = kChannelDefault;
    im.reliable = false;
    im.bytes.assign(payload.begin(), payload.end());
    inbox_.push_back(std::move(im));
}

void HostImpl::drain_ooo_(PeerState& ps) noexcept {
    for (;;) {
        auto it = ps.ooo_buffer.find(ps.next_deliver);
        if (it == ps.ooo_buffer.end()) break;
        InboundMessage im{};
        im.from     = ps.id;
        im.reliable = true;
        im.channel  = kChannelDefault;
        im.bytes    = std::move(it->second);
        inbox_.push_back(std::move(im));
        ps.ooo_buffer.erase(it);
        ++ps.next_deliver;
    }
}

void HostImpl::run_rto_(PeerState& ps) noexcept {
    std::vector<u32> rt;
    if (ps.send.collect_retransmits(tick_, kDefaultRtoTicks, rt) == 0) return;

    for (u32 slot : rt) {
        SendEntry& e = ps.send.entry(slot);
        if (!e.in_use || e.acked) continue;
        FrameHeader h;
        h.channel     = kChannelDefault;
        h.payload_len = u16(e.payload.size());
        h.flags       = kFlagReliable;
        h.seq         = e.seq;
        ps.recv.snapshot(h.ack_base, h.ack_bits);
        send_raw_(ps.remote_port, h, e.payload);
        ps.send.mark_transmitted(e.seq, tick_);
    }
}

void HostImpl::flush_acks_(PeerState& ps) noexcept {
    // Emit a tiny ack-only datagram for the peer.
    FrameHeader h;
    h.flags       = kFlagAckOnly;
    h.seq         = 0;
    h.payload_len = 0;
    ps.recv.snapshot(h.ack_base, h.ack_bits);
    if (h.ack_base == 0) return;  // nothing to ack yet.
    send_raw_(ps.remote_port, h, std::span<const u8>{});
}

u32 HostImpl::poll(std::vector<InboundMessage>& out) noexcept {
    if (!started_) return 0;
    u32 n = u32(inbox_.size());
    for (InboundMessage& m : inbox_) {
        out.push_back(std::move(m));
    }
    inbox_.clear();
    return n;
}

void HostImpl::tick() noexcept {
    if (!started_) return;
    ++tick_;
    for (auto& [k, v] : peers_) {
        run_rto_(v);
    }
    // Flush any outstanding acks for peers that delivered to us this tick.
    for (PeerId pid : pending_ack_peers_) {
        if (PeerState* ps = find_peer_(pid)) {
            flush_acks_(*ps);
        }
    }
    pending_ack_peers_.clear();
}

void HostImpl::lockstep_submit(u32 tick, std::span<const u8> input) noexcept {
    LockstepInput li{};
    li.peer_index = 0;  // owner is always peer_index 0 on its own host.
    li.tick       = tick;
    li.payload.assign(input.begin(), input.end());
    lockstep_.submit(std::move(li));
}

bool HostImpl::lockstep_ready(u32 tick) const noexcept {
    return lockstep_.is_ready(tick);
}

LockstepBundle HostImpl::lockstep_take(u32 tick) noexcept {
    return lockstep_.take_bundle(tick);
}

// ─── Test-harness factory ─────────────────────────────────────────────────

HostImpl* make_test_host(const HostDesc& desc) noexcept {
    auto* h = new (std::nothrow) HostImpl();
    if (!h) return nullptr;
    if (!h->start(desc)) {
        delete h;
        return nullptr;
    }
    return h;
}

void destroy_test_host(HostImpl* h) noexcept {
    if (!h) return;
    h->stop();
    delete h;
}

}  // namespace psynder::net
