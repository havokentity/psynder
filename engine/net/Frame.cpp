// SPDX-License-Identifier: MIT
// Psynder — frame header serializer. Lane 14 internal.

#include "Frame.h"

#include <cstring>

namespace psynder::net {

namespace {

PSY_FORCEINLINE void write_u32_le(u8* p, u32 v) noexcept {
    p[0] = u8(v       & 0xFFu);
    p[1] = u8((v >> 8)  & 0xFFu);
    p[2] = u8((v >> 16) & 0xFFu);
    p[3] = u8((v >> 24) & 0xFFu);
}

PSY_FORCEINLINE void write_u16_le(u8* p, u16 v) noexcept {
    p[0] = u8(v       & 0xFFu);
    p[1] = u8((v >> 8) & 0xFFu);
}

PSY_FORCEINLINE u32 read_u32_le(const u8* p) noexcept {
    return u32(p[0])
         | (u32(p[1]) << 8)
         | (u32(p[2]) << 16)
         | (u32(p[3]) << 24);
}

PSY_FORCEINLINE u16 read_u16_le(const u8* p) noexcept {
    return u16(u16(p[0]) | u16(u16(p[1]) << 8));
}

}  // namespace

bool encode_header(const FrameHeader& h, std::span<u8> out) noexcept {
    if (out.size() < kFrameHeaderBytes) return false;
    u8* p = out.data();
    write_u32_le(p + 0,  h.magic);
    p[4] = h.version;
    p[5] = h.cipher_suite;
    p[6] = h.channel;
    p[7] = h.flags;
    write_u32_le(p + 8,  h.seq);
    write_u32_le(p + 12, h.ack_base);
    write_u32_le(p + 16, h.ack_bits);
    write_u16_le(p + 20, h.payload_len);
    write_u16_le(p + 22, h.crc16);
    return true;
}

bool decode_header(std::span<const u8> in, FrameHeader& h) noexcept {
    if (in.size() < kFrameHeaderBytes) return false;
    const u8* p = in.data();
    h.magic        = read_u32_le(p + 0);
    if (h.magic != kFrameMagic) return false;
    h.version      = p[4];
    if (h.version != kFrameVersion) return false;
    h.cipher_suite = p[5];
    h.channel      = p[6];
    h.flags        = p[7];
    h.seq          = read_u32_le(p + 8);
    h.ack_base     = read_u32_le(p + 12);
    h.ack_bits     = read_u32_le(p + 16);
    h.payload_len  = read_u16_le(p + 20);
    h.crc16        = read_u16_le(p + 22);
    // Sanity: payload_len must fit inside the (caller-bounded) datagram. The
    // caller checks total size vs. header+payload_len. We don't have the
    // bound here.
    return true;
}

}  // namespace psynder::net
