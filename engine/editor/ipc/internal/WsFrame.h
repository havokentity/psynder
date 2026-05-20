// SPDX-License-Identifier: MIT
// Psynder editor IPC — RFC 6455 WebSocket frame codec (minimal subset).
//
// Handles the framing rules we need for editor IPC: text + binary, ping/pong,
// close. No extensions, no continuation fragments (we always send single-frame
// messages from the server; client messages are reassembled with FIN check).

#pragma once

#include "core/Types.h"

#include <cstring>
#include <string_view>
#include <vector>

namespace psynder::editor::ipc::wsframe {

enum class Op : ::psynder::u8 {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    Op op = Op::Binary;
    bool fin = true;
    std::vector<::psynder::u8> payload;
};

// Encode a server→client frame. Server frames MUST NOT be masked (RFC 6455 §5.1).
inline std::vector<::psynder::u8> encode_server(Op op,
                                                const ::psynder::u8* data,
                                                ::psynder::usize n,
                                                bool fin = true) {
    std::vector<::psynder::u8> out;
    out.reserve(n + 16);
    ::psynder::u8 b0 = static_cast<::psynder::u8>(op) | (fin ? 0x80u : 0u);
    out.push_back(b0);
    if (n <= 125) {
        out.push_back(static_cast<::psynder::u8>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<::psynder::u8>(n >> 8));
        out.push_back(static_cast<::psynder::u8>(n));
    } else {
        out.push_back(127);
        ::psynder::u64 v = n;
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<::psynder::u8>(v >> (i * 8)));
    }
    if (n)
        out.insert(out.end(), data, data + n);
    return out;
}

inline std::vector<::psynder::u8> encode_server_text(std::string_view s) {
    return encode_server(Op::Text, reinterpret_cast<const ::psynder::u8*>(s.data()), s.size(), true);
}

inline std::vector<::psynder::u8> encode_server_binary(const ::psynder::u8* data, ::psynder::usize n) {
    return encode_server(Op::Binary, data, n, true);
}

inline std::vector<::psynder::u8> encode_close(::psynder::u16 code, std::string_view reason) {
    std::vector<::psynder::u8> body;
    body.reserve(2 + reason.size());
    body.push_back(static_cast<::psynder::u8>(code >> 8));
    body.push_back(static_cast<::psynder::u8>(code));
    for (char c : reason)
        body.push_back(static_cast<::psynder::u8>(c));
    return encode_server(Op::Close, body.data(), body.size(), true);
}

// Try to parse one frame from `in`. On success, returns frame size consumed
// (>0). On insufficient data, returns 0. On protocol error, returns SIZE_MAX.
// `frame.payload` is unmasked. Client frames MUST be masked (we enforce).
inline ::psynder::usize try_parse_client(const ::psynder::u8* in, ::psynder::usize n, Frame& frame) {
    if (n < 2)
        return 0;
    ::psynder::u8 b0 = in[0];
    ::psynder::u8 b1 = in[1];
    frame.fin = (b0 & 0x80) != 0;
    frame.op = static_cast<Op>(b0 & 0x0F);
    bool masked = (b1 & 0x80) != 0;
    if (!masked)
        return SIZE_MAX;  // client MUST mask
    ::psynder::u64 len = b1 & 0x7Fu;
    ::psynder::usize p = 2;
    if (len == 126) {
        if (n < p + 2)
            return 0;
        len = (::psynder::u64(in[p]) << 8) | in[p + 1];
        p += 2;
    } else if (len == 127) {
        if (n < p + 8)
            return 0;
        len = 0;
        for (::psynder::usize i = 0; i < 8; ++i)
            len = (len << 8) | in[p + i];
        p += 8;
    }
    if (n < p + 4)
        return 0;
    ::psynder::u8 mask[4] = {in[p], in[p + 1], in[p + 2], in[p + 3]};
    p += 4;
    if (n < p + len)
        return 0;
    // Cap payload size at 16 MiB to guard against DoS via a 64-bit length.
    if (len > (16ull * 1024 * 1024))
        return SIZE_MAX;
    frame.payload.resize(static_cast<::psynder::usize>(len));
    for (::psynder::usize i = 0; i < len; ++i) {
        frame.payload[i] = static_cast<::psynder::u8>(in[p + i] ^ mask[i & 3]);
    }
    return p + static_cast<::psynder::usize>(len);
}

}  // namespace psynder::editor::ipc::wsframe
