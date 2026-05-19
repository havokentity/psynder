// SPDX-License-Identifier: MIT
// Psynder — Wave-B / lane 14: configurable selective-ACK window depth +
// per-channel AOI priorities (DESIGN.md §10.4).
//
// Three cases:
//   1. A 64-bit window absorbs a 50-frame burst with 10% packet drop and
//      every survivor + every retransmit gets through, end-to-end.
//   2. AOI per-channel priorities scale a peer's per-tick byte budget:
//      setting a channel to priority 1 (half of the default 2) halves
//      its allowed bytes.
//   3. The Wave-A 32-bit window keeps its pre-Wave-B behaviour bit-for-bit:
//      same retire count for the same synthetic ack pattern.

#include <catch2/catch_test_macros.hpp>

#include "net/Aoi.h"
#include "net/Reliability.h"

#include <array>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

// Deterministic 10% drop pattern: every 10th frame starting at offset 5
// (i.e. seqs 5, 15, 25, 35, 45). Picked off-boundary so the *last* frame
// (seq 50) is always delivered — keeps `highest_seq()` predictable.
constexpr bool is_dropped(u32 i) {
    return (i % 10) == 5;
}

}  // namespace

TEST_CASE("net/wave-b: 64-bit window survives 50-frame burst with 10% drop, all 50 delivered",
          "[net][window][wave-b]") {
    Reliability sender(WindowSize::Bits64);
    AckTracker  receiver(WindowSize::Bits64);

    REQUIRE(sender.window_bits() == 64u);
    REQUIRE(receiver.window_bits() == 64u);

    constexpr u32 kFrames = 50;
    // Drops at seqs 5, 15, 25, 35, 45 — exactly 5 frames, 10% loss.
    constexpr u32 kDropped   = 5;
    constexpr u32 kDelivered = kFrames - kDropped;

    std::array<u8, 4> payload{0xA1, 0xB2, 0xC3, 0xD4};

    // First pass: enqueue all 50 frames, receiver observes only the
    // non-dropped ones.
    for (u32 i = 1; i <= kFrames; ++i) {
        u32 s = sender.enqueue(payload, /*tick=*/i);
        REQUIRE(s == i);
        if (!is_dropped(i)) {
            receiver.observe(s);
        }
    }

    // Sender hasn't seen any acks yet: 50 in flight.
    CHECK(sender.inflight_count() == kFrames);
    REQUIRE(receiver.highest_seq() == kFrames);  // seq 50 was delivered.

    // Receiver pumps back a single 64-bit ACK reply. The 32-bit form would
    // only cover seqs 49..18 (32 sequences below the base); the 64-bit
    // form reaches all the way down to seq 1 — that's the point of
    // widening the window.
    u32 base = 0, lo = 0, hi = 0, hi2 = 0, hi3 = 0;
    receiver.snapshot_wide(base, lo, hi, hi2, hi3);
    REQUIRE(base == kFrames);   // highest observed seq.
    REQUIRE(hi2 == 0u);         // Bits64 mode: upper words must be zero.
    REQUIRE(hi3 == 0u);

    // Sender applies the wide ack. Every non-dropped frame must retire
    // in this one pass; the five dropped frames stay in flight.
    u32 retired = sender.apply_ack_wide(base, lo, hi, hi2, hi3);
    CHECK(retired == kDelivered);
    CHECK(sender.inflight_count() == kDropped);

    // Second pass: the sender retransmits the five dropped frames; the
    // receiver picks them up (its 64-bit bitmap has room) and pumps a
    // fresh ack that clears the rest.
    for (u32 i = 1; i <= kFrames; ++i) {
        if (!is_dropped(i)) continue;
        receiver.observe(i);  // simulate the retransmit being delivered.
    }
    receiver.snapshot_wide(base, lo, hi, hi2, hi3);
    retired = sender.apply_ack_wide(base, lo, hi, hi2, hi3);
    CHECK(retired == kDropped);            // the missing 5.
    CHECK(sender.inflight_count() == 0u);  // every burst frame retired.
}

TEST_CASE("net/wave-b: AOI per-channel priority halves bytes when set to 1 of 2",
          "[net][aoi][priority][wave-b]") {
    AoiFilter f;
    PeerId p{1};
    f.set_peer(p, math::Vec3{0, 0, 0}, /*radius=*/100.f);

    constexpr u32 kBaseBudget = 4096;

    // Default priorities — every channel gets the full share.
    REQUIRE(f.channel_priority(kChannelDefault)  == kDefaultChannelPriority);
    REQUIRE(f.channel_priority(kChannelSnapshot) == kDefaultChannelPriority);
    REQUIRE(f.bytes_for_channel(kChannelDefault,  kBaseBudget) == kBaseBudget);
    REQUIRE(f.bytes_for_channel(kChannelSnapshot, kBaseBudget) == kBaseBudget);

    // Drop the snapshot channel to half priority. Default is 2; we set it
    // to 1 → half the bytes.
    f.set_channel_priority(kChannelSnapshot, 1);
    CHECK(f.channel_priority(kChannelSnapshot) == 1u);

    const u32 lockstep_bytes = f.bytes_for_channel(kChannelLockstep, kBaseBudget);
    const u32 snapshot_bytes = f.bytes_for_channel(kChannelSnapshot, kBaseBudget);
    CHECK(lockstep_bytes == kBaseBudget);
    CHECK(snapshot_bytes == kBaseBudget / 2);
    CHECK(snapshot_bytes * 2 == lockstep_bytes);   // exact 1:2 ratio.

    // Doubling pushes the channel above the default share — proves the
    // setter accepts more than just the halving direction.
    f.set_channel_priority(kChannelLockstep, 4);
    CHECK(f.bytes_for_channel(kChannelLockstep, kBaseBudget) == kBaseBudget * 2);

    // Sphere visibility is unaffected by priority — the AOI gate runs
    // before the budget split.
    CHECK(f.visible(p, math::Vec3{50.f, 0.f, 0.f}));
    CHECK_FALSE(f.visible(p, math::Vec3{200.f, 0.f, 0.f}));
}

TEST_CASE("net/wave-b: existing 32-bit window matches Wave-A behaviour bit-for-bit",
          "[net][window][wave-b][back-compat]") {
    // Same scenario as the Wave-A "selective ACK retires arbitrary subsets"
    // case (tests/unit/net_reliability.cpp): default-constructed
    // Reliability must still retire exactly (base + the marked bits)
    // when fed the narrow 32-bit ack form.
    Reliability r;
    REQUIRE(r.window_bits() == 32u);

    std::array<u8, 1> payload{0x77};
    for (u32 i = 0; i < 10; ++i) (void)r.enqueue(payload, 0);

    // ack_base=10 with bit i=1 set → seq 8 acked; seq 9 missing.
    const u32 ack_base = 10;
    const u32 ack_bits = (1u << 1);
    const u32 retired  = r.apply_ack(ack_base, ack_bits);
    CHECK(retired == 2);              // 10 and 8.
    CHECK(r.inflight_count() == 8);   // 9, 7, 6, 5, 4, 3, 2, 1 still in flight.

    // AckTracker default ctor must also remain at 32-bit width.
    AckTracker a;
    CHECK(a.window_bits() == 32u);

    // Observe + snapshot in the narrow form: the low 32 bits of history
    // are emitted, and bits beyond bit 31 are not. (Bits 32..63 of an
    // internally-tracked 128-bit history must not leak through.)
    for (u32 i = 1; i <= 40; ++i) a.observe(i);   // 40 in-order observes.
    u32 base = 0, bits = 0;
    a.snapshot(base, bits);
    CHECK(base == 40u);
    // Narrow snapshot must fit in 32 bits — the value reflects seqs
    // 39..8 (32 prior sequences); all 32 bits should be set.
    CHECK(bits == 0xFFFFFFFFu);
}
