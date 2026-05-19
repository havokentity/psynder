// SPDX-License-Identifier: MIT
// Psynder — HostImpl: the engine behind the public `Host` surface. Lane 14
// internal.
//
// Wave A scope: drive everything over the in-process LoopbackBus so the
// unit tests, editor multi-user mode (DESIGN.md §10.8), and sample binaries
// all share one substrate. Wave B will swap in a real UDP socket transport
// behind the same HostImpl interface; the public Net.h surface doesn't
// change.

#pragma once

#include "Aoi.h"
#include "Frame.h"
#include "Lockstep.h"
#include "Net.h"           // PeerId, HostDesc, Mode
#include "Reliability.h"
#include "Snapshot.h"
#include "core/Types.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace psynder::net {

// Per-peer state on this host. Keyed by PeerId.raw.
struct PeerState {
    PeerId        id{};                   // self-id used by the OWNER side
    u16           remote_port = 0;
    u32           peer_index  = 0;        // stable index 0..max_peers-1
    Reliability   send{};
    AckTracker    recv{};
    // Buffered in-order delivery: payloads we received but with a hole in
    // front of them. Keyed by seq. Once `next_deliver` advances we drain.
    std::unordered_map<u32, std::vector<u8>> ooo_buffer;
    u32           next_deliver = 1;       // next reliable seq to surface
};

// Decoded message ready for the application.
struct InboundMessage {
    PeerId            from;
    std::vector<u8>   bytes;
    u8                channel = kChannelDefault;
    bool              reliable = false;
};

class HostImpl {
public:
    HostImpl() = default;
    ~HostImpl();

    HostImpl(const HostImpl&)            = delete;
    HostImpl& operator=(const HostImpl&) = delete;

    bool start(const HostDesc& desc) noexcept;
    void stop() noexcept;

    // Select the selective-ACK window depth for new peers. Must be called
    // before `start()` (or before any peer is registered) so the per-peer
    // Reliability + AckTracker pair builds with the matching width.
    //
    // Default is WindowSize::Bits32 — Wave-A wire layout, ABI-compatible
    // with HostDesc-only callers. Selecting Bits64 / Bits128 widens the
    // selective-ACK bitmap, the receive history, and the send-ring cap.
    //
    // Lane-14 internal API; the public Net.h surface remains frozen.
    void set_window_size(WindowSize sz) noexcept { window_size_ = sz; }
    WindowSize window_size() const noexcept { return window_size_; }

    // Returns the local port we're bound to.
    u16 local_port() const noexcept { return port_; }
    Mode mode() const noexcept { return mode_; }

    // Establish a peer relationship. In Wave A the "address" is just the
    // loopback port. Returns 0-handle on failure (peer table full / target
    // not in the bus).
    PeerId connect(u16 dest_port) noexcept;

    // Send `bytes` on `channel`. If `reliable`, allocates a seq from the
    // sliding window; otherwise sends unreliable (one-shot).
    void send(PeerId peer, std::span<const u8> bytes,
              bool reliable, u8 channel = kChannelDefault) noexcept;

    // Drain delivered application-level messages into out_buffer. Returns
    // the number of messages appended. Drives RTO bookkeeping internally.
    u32 poll(std::vector<InboundMessage>& out) noexcept;

    // Called by LoopbackBus when a datagram arrives. Internal; visibility is
    // public only so the bus can route into us without a friend declaration.
    void on_datagram(u16 src_port, std::span<const u8> bytes) noexcept;

    // Advance the logical tick. RTO and lockstep coordination tick on this.
    void tick() noexcept;

    // ── Lockstep helpers (Mode::Lockstep) ──────────────────────────────
    void lockstep_submit(u32 tick, std::span<const u8> input) noexcept;
    bool lockstep_ready(u32 tick) const noexcept;
    LockstepBundle lockstep_take(u32 tick) noexcept;

    // ── Snapshot / AOI helpers (Mode::ClientServer) ────────────────────
    AoiFilter&       aoi()       noexcept { return aoi_; }
    const AoiFilter& aoi() const noexcept { return aoi_; }

    // Iterate over current peers (read-only).
    template <class Fn>
    void for_each_peer(Fn fn) const {
        for (const auto& [k, v] : peers_) fn(v);
    }

    // Test introspection.
    u32 logical_tick() const noexcept { return tick_; }
    usize peer_count() const noexcept { return peers_.size(); }

private:
    // Internal helpers.
    PeerState* find_peer_(PeerId id) noexcept;
    PeerState* find_peer_by_port_(u16 port) noexcept;
    PeerId     register_peer_(u16 remote_port) noexcept;
    void       send_raw_(u16 dst_port, const FrameHeader& h,
                         std::span<const u8> payload) noexcept;

    void       handle_reliable_in_(PeerState& ps, const FrameHeader& h,
                                   std::span<const u8> payload) noexcept;
    void       handle_unreliable_in_(PeerState& ps, const FrameHeader& h,
                                     std::span<const u8> payload) noexcept;
    void       drain_ooo_(PeerState& ps) noexcept;
    void       run_rto_(PeerState& ps) noexcept;
    void       flush_acks_(PeerState& ps) noexcept;

    bool                                              started_ = false;
    u16                                               port_    = 0;
    u16                                               max_peers_ = 0;
    Mode                                              mode_    = Mode::ClientServer;
    u32                                               tick_    = 0;
    u32                                               next_peer_handle_ = 1;
    u32                                               next_peer_index_  = 0;
    WindowSize                                        window_size_ = WindowSize::Bits32;
    std::unordered_map<u32, PeerState>                peers_;        // by PeerId.raw
    std::vector<InboundMessage>                       inbox_;        // pending delivery
    AoiFilter                                         aoi_;
    LockstepCoordinator                               lockstep_{8};  // default 8 for racing
    // Pending acks: peers whose ack we owe on the next tick / flush.
    std::vector<PeerId>                               pending_ack_peers_;
};

// Internal API for the test harness: build a fresh HostImpl bound to a
// custom port. The default Host::Get() singleton is independent of these.
HostImpl* make_test_host(const HostDesc& desc) noexcept;
void      destroy_test_host(HostImpl* h) noexcept;

// RTO in ticks. Lane 14 ticks 60Hz; 6 ticks ≈ 100ms.
inline constexpr u32 kDefaultRtoTicks = 6;

}  // namespace psynder::net
