// SPDX-License-Identifier: MIT
// Psynder — lane 14 / net frame-header encode/decode tests.

#include <catch2/catch_test_macros.hpp>

#include "net/Frame.h"

#include <array>

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net: frame header round-trips", "[net][frame]") {
    FrameHeader h{};
    h.channel      = kChannelSnapshot;
    h.flags        = kFlagReliable | kFlagFragment;
    h.seq          = 0xDEADBEEFu;
    h.ack_base     = 0x12345678u;
    h.ack_bits     = 0xCAFEBABEu;
    h.payload_len  = 123;
    h.cipher_suite = 0;   // Wave-A: reserved-slot must round-trip even if 0.
    h.crc16        = 0;

    std::array<u8, kFrameHeaderBytes> buf{};
    REQUIRE(encode_header(h, std::span<u8>(buf.data(), buf.size())));

    FrameHeader g{};
    REQUIRE(decode_header(std::span<const u8>(buf.data(), buf.size()), g));
    CHECK(g.magic        == kFrameMagic);
    CHECK(g.version      == kFrameVersion);
    CHECK(g.channel      == kChannelSnapshot);
    CHECK((g.flags & kFlagReliable) != 0);
    CHECK((g.flags & kFlagFragment) != 0);
    CHECK(g.seq          == 0xDEADBEEFu);
    CHECK(g.ack_base     == 0x12345678u);
    CHECK(g.ack_bits     == 0xCAFEBABEu);
    CHECK(g.payload_len  == 123);
    CHECK(g.cipher_suite == 0);
}

TEST_CASE("net: frame header rejects wrong magic", "[net][frame]") {
    std::array<u8, kFrameHeaderBytes> buf{};
    // All zeroes => magic 0 != kFrameMagic.
    FrameHeader g{};
    REQUIRE_FALSE(decode_header(std::span<const u8>(buf.data(), buf.size()), g));
}

TEST_CASE("net: frame header reserves the cipher_suite slot", "[net][frame]") {
    FrameHeader h{};
    h.cipher_suite = 0x7F;  // arbitrary non-zero suite id (Wave-B will define)
    std::array<u8, kFrameHeaderBytes> buf{};
    REQUIRE(encode_header(h, std::span<u8>(buf.data(), buf.size())));
    FrameHeader g{};
    REQUIRE(decode_header(std::span<const u8>(buf.data(), buf.size()), g));
    // The slot exists in the wire layout and round-trips; the byte at
    // offset 5 must carry the value.
    CHECK(g.cipher_suite == 0x7F);
    CHECK(buf[5] == 0x7F);
}
