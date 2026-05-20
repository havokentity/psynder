// SPDX-License-Identifier: MIT
// Psynder editor IPC — minimal crypto helpers.
//
// Self-contained SHA-1 (for WebSocket handshake) and Base64 encoder; plus
// a cryptographically-random session-token generator backed by std::random_device.
//
// SHA-1 is *only* used for the RFC 6455 Sec-WebSocket-Accept handshake — it is
// not used for any security guarantee. The session-token check (constant-time
// string compare against a per-startup token) is what gates access.

#pragma once

#include "core/Types.h"

#include <array>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

namespace psynder::editor::ipc::crypto {

// ─── SHA-1 ──────────────────────────────────────────────────────────────────
struct Sha1 {
    ::psynder::u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    ::psynder::u64 len_bits = 0;
    ::psynder::u8 buf[64]{};
    ::psynder::usize buf_n = 0;

    static ::psynder::u32 rol(::psynder::u32 x, int n) noexcept {
        return (x << n) | (x >> (32 - n));
    }

    void block(const ::psynder::u8* p) noexcept {
        ::psynder::u32 w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (::psynder::u32(p[i * 4]) << 24) | (::psynder::u32(p[i * 4 + 1]) << 16) |
                   (::psynder::u32(p[i * 4 + 2]) << 8) | ::psynder::u32(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        ::psynder::u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            ::psynder::u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            ::psynder::u32 t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const ::psynder::u8* p, ::psynder::usize n) noexcept {
        len_bits += static_cast<::psynder::u64>(n) * 8u;
        if (buf_n) {
            ::psynder::usize take = 64 - buf_n;
            if (take > n)
                take = n;
            std::memcpy(buf + buf_n, p, take);
            buf_n += take;
            p += take;
            n -= take;
            if (buf_n == 64) {
                block(buf);
                buf_n = 0;
            }
        }
        while (n >= 64) {
            block(p);
            p += 64;
            n -= 64;
        }
        if (n) {
            std::memcpy(buf, p, n);
            buf_n = n;
        }
    }

    void finalize(::psynder::u8 out[20]) noexcept {
        // pad: 0x80, zeros, then 8 bytes big-endian length in bits.
        ::psynder::u64 final_bits = len_bits;
        ::psynder::u8 one = 0x80;
        update(&one, 1);
        ::psynder::u8 zero = 0x00;
        while (buf_n != 56)
            update(&zero, 1);
        ::psynder::u8 lb[8];
        for (int i = 0; i < 8; ++i)
            lb[i] = static_cast<::psynder::u8>(final_bits >> (56 - i * 8));
        update(lb, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<::psynder::u8>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<::psynder::u8>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<::psynder::u8>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<::psynder::u8>(h[i]);
        }
    }
};

inline std::array<::psynder::u8, 20> sha1(std::string_view data) noexcept {
    Sha1 ctx;
    ctx.update(reinterpret_cast<const ::psynder::u8*>(data.data()), data.size());
    std::array<::psynder::u8, 20> out{};
    ctx.finalize(out.data());
    return out;
}

// ─── Base64 ────────────────────────────────────────────────────────────────
inline std::string base64_encode(const ::psynder::u8* data, ::psynder::usize n) {
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    ::psynder::usize i = 0;
    while (i + 3 <= n) {
        ::psynder::u32 v = (::psynder::u32(data[i]) << 16) | (::psynder::u32(data[i + 1]) << 8) |
                           ::psynder::u32(data[i + 2]);
        out.push_back(alpha[(v >> 18) & 0x3F]);
        out.push_back(alpha[(v >> 12) & 0x3F]);
        out.push_back(alpha[(v >> 6) & 0x3F]);
        out.push_back(alpha[v & 0x3F]);
        i += 3;
    }
    if (i < n) {
        ::psynder::u32 v = ::psynder::u32(data[i]) << 16;
        if (i + 1 < n)
            v |= ::psynder::u32(data[i + 1]) << 8;
        out.push_back(alpha[(v >> 18) & 0x3F]);
        out.push_back(alpha[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < n) ? alpha[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

inline std::string base64_encode(std::string_view s) {
    return base64_encode(reinterpret_cast<const ::psynder::u8*>(s.data()), s.size());
}

// ─── URL-safe random token ─────────────────────────────────────────────────
// Returns a 128-bit token hex-encoded (32 chars). Uses std::random_device for
// entropy; on macOS / Linux this maps to /dev/urandom + getrandom().
inline std::string make_session_token() {
    std::random_device rd;
    std::uniform_int_distribution<::psynder::u32> dist(0u, 0xFFFFFFFFu);
    ::psynder::u32 words[4] = {dist(rd), dist(rd), dist(rd), dist(rd)};
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(32);
    for (::psynder::usize w = 0; w < 4; ++w) {
        ::psynder::u32 v = words[w];
        for (::psynder::usize b = 0; b < 4; ++b) {
            ::psynder::u8 byte = static_cast<::psynder::u8>(v >> (24 - b * 8));
            s[w * 8 + b * 2] = hex[byte >> 4];
            s[w * 8 + b * 2 + 1] = hex[byte & 0xF];
        }
    }
    return s;
}

// Constant-time string compare — defends against timing side-channel on the
// token check. Both inputs must be the same length to compare equal.
inline bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    ::psynder::u8 diff = 0;
    for (::psynder::usize i = 0; i < a.size(); ++i) {
        diff |= static_cast<::psynder::u8>(a[i]) ^ static_cast<::psynder::u8>(b[i]);
    }
    return diff == 0;
}

}  // namespace psynder::editor::ipc::crypto
