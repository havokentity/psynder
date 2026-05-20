// SPDX-License-Identifier: MIT
// Psynder editor IPC — msgpack encode/decode roundtrip tests. Lane 19.
//
// Exercises both the raw Writer/Reader API and the generated struct codecs
// from protocol.psy → Protocol.gen.h. The roundtrip is the load-bearing
// invariant: panels and the engine MUST agree on the byte stream produced
// by the IDL.

#include "editor/ipc/internal/Msgpack.h"
#include "editor/ipc/proto/Protocol.gen.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::editor::ipc;

namespace {

// Convenience: round-trip one value through the codec and assert equality.
template <class T, class EncodeFn, class DecodeFn>
T roundtrip_one(const T& in, EncodeFn enc, DecodeFn dec) {
    msgpack::Writer w;
    enc(w, in);
    msgpack::Reader r(w.data(), w.size());
    T out{};
    REQUIRE(dec(r, out));
    REQUIRE(r.eof());
    return out;
}

}  // namespace

TEST_CASE("msgpack: primitive roundtrip", "[ipc][msgpack]") {
    SECTION("bool") {
        for (bool v : {true, false}) {
            msgpack::Writer w;
            w.boolean(v);
            msgpack::Reader r(w.data(), w.size());
            bool out = !v;
            REQUIRE(r.boolean(out));
            REQUIRE(out == v);
        }
    }
    SECTION("unsigned int extremes") {
        const u64 samples[] = {
            0ull,
            1ull,
            127ull,
            128ull,
            255ull,
            256ull,
            65535ull,
            65536ull,
            static_cast<u64>(std::numeric_limits<u32>::max()),
            static_cast<u64>(std::numeric_limits<u32>::max()) + 1ull,
            std::numeric_limits<u64>::max(),
        };
        for (u64 v : samples) {
            msgpack::Writer w;
            w.u64_(v);
            msgpack::Reader r(w.data(), w.size());
            u64 out = 0;
            REQUIRE(r.u64_(out));
            REQUIRE(out == v);
        }
    }
    SECTION("signed int extremes") {
        const i64 samples[] = {
            0,
            -1,
            -32,
            -33,
            127,
            -128,
            -129,
            32767,
            -32768,
            -32769,
            std::numeric_limits<i32>::min(),
            std::numeric_limits<i32>::max(),
            std::numeric_limits<i64>::min(),
            std::numeric_limits<i64>::max(),
        };
        for (i64 v : samples) {
            msgpack::Writer w;
            w.i64_(v);
            msgpack::Reader r(w.data(), w.size());
            i64 out = 0;
            REQUIRE(r.i64_(out));
            REQUIRE(out == v);
        }
    }
    SECTION("f32 / f64 bit-exact") {
        const f32 a = 3.14159265f;
        msgpack::Writer w;
        w.f32_(a);
        msgpack::Reader r(w.data(), w.size());
        f32 out = 0;
        REQUIRE(r.f32_(out));
        REQUIRE(out == a);
        msgpack::Writer w2;
        w2.f64_(2.718281828459045);
        msgpack::Reader r2(w2.data(), w2.size());
        f64 out64 = 0;
        REQUIRE(r2.f64_(out64));
        REQUIRE(out64 == 2.718281828459045);
    }
    SECTION("string sizes — fixstr / str8 / str16 / str32") {
        const std::string sizes[] = {
            "",
            "a",
            std::string(31, 'x'),
            std::string(32, 'x'),
            std::string(255, 'x'),
            std::string(256, 'x'),
            std::string(65535, 'x'),
            std::string(65536, 'x'),
        };
        for (const auto& s : sizes) {
            msgpack::Writer w;
            w.str(s);
            msgpack::Reader r(w.data(), w.size());
            std::string out;
            REQUIRE(r.str(out));
            REQUIRE(out == s);
        }
    }
    SECTION("binary blobs") {
        std::vector<u8> blob(300);
        for (size_t i = 0; i < blob.size(); ++i)
            blob[i] = static_cast<u8>(i & 0xFF);
        msgpack::Writer w;
        w.bin(blob.data(), blob.size());
        msgpack::Reader r(w.data(), w.size());
        std::vector<u8> out;
        REQUIRE(r.bin(out));
        REQUIRE(out == blob);
    }
    SECTION("array of strings") {
        std::vector<std::string> arr = {"hello", "world", "psynder"};
        msgpack::Writer w;
        w.array_header(arr.size());
        for (const auto& s : arr)
            w.str(s);
        msgpack::Reader r(w.data(), w.size());
        u32 n = 0;
        REQUIRE(r.array_header(n));
        REQUIRE(n == arr.size());
        for (const auto& expected : arr) {
            std::string got;
            REQUIRE(r.str(got));
            REQUIRE(got == expected);
        }
        REQUIRE(r.eof());
    }
}

TEST_CASE("msgpack: skip() walks past values", "[ipc][msgpack]") {
    msgpack::Writer w;
    w.array_header(3);
    w.u32_(42);
    w.str("ignore me");
    w.boolean(true);
    msgpack::Reader r(w.data(), w.size());
    u32 n = 0;
    REQUIRE(r.array_header(n));
    REQUIRE(n == 3);
    REQUIRE(r.skip());  // u32
    REQUIRE(r.skip());  // str
    bool b = false;
    REQUIRE(r.boolean(b));
    REQUIRE(b);
}

TEST_CASE("msgpack: malformed input is rejected", "[ipc][msgpack]") {
    // Truncated str header (claims 200 bytes but only has 4).
    std::vector<u8> bad = {0xD9, 200, 'x', 'y', 'z'};
    msgpack::Reader r(bad.data(), bad.size());
    std::string out;
    REQUIRE_FALSE(r.str(out));

    // Truncated u32: tag 0xCE then only 2 bytes.
    std::vector<u8> bad2 = {0xCE, 1, 2};
    msgpack::Reader r2(bad2.data(), bad2.size());
    u32 v = 0;
    REQUIRE_FALSE(r2.u32_(v));

    // Empty buffer.
    msgpack::Reader r3(nullptr, 0);
    bool b = false;
    REQUIRE_FALSE(r3.boolean(b));
}

TEST_CASE("ipc: generated Hello struct roundtrips", "[ipc][protocol]") {
    proto::Hello h;
    h.protocol_version = proto::kProtocolVersion;
    h.engine_version = "0.1.0-wave-a";
    h.session_token = "deadbeef00112233";

    msgpack::Writer w;
    proto::Hello_encode(w, h);
    msgpack::Reader r(w.data(), w.size());
    proto::Hello out;
    REQUIRE(proto::Hello_decode(r, out));
    REQUIRE(r.eof());
    REQUIRE(out.protocol_version == h.protocol_version);
    REQUIRE(out.engine_version == h.engine_version);
    REQUIRE(out.session_token == h.session_token);
}

TEST_CASE("ipc: generated Welcome / StatsTick / SceneDelta roundtrip", "[ipc][protocol]") {
    SECTION("Welcome") {
        proto::Welcome w;
        w.accepted = true;
        w.server_ver = 42;
        w.server_build = "psynder-test";
        w.reason = "";
        msgpack::Writer mw;
        proto::Welcome_encode(mw, w);
        msgpack::Reader mr(mw.data(), mw.size());
        proto::Welcome out;
        REQUIRE(proto::Welcome_decode(mr, out));
        REQUIRE(out.accepted == w.accepted);
        REQUIRE(out.server_ver == w.server_ver);
        REQUIRE(out.server_build == w.server_build);
        REQUIRE(out.reason == w.reason);
    }
    SECTION("StatsTick") {
        proto::StatsTick s;
        s.frame_index = 12345;
        s.cpu_ms = 4.5f;
        s.gpu_ms = 0.0f;
        s.draw_calls = 1024;
        s.entities = 9876;
        msgpack::Writer mw;
        proto::StatsTick_encode(mw, s);
        msgpack::Reader mr(mw.data(), mw.size());
        proto::StatsTick out;
        REQUIRE(proto::StatsTick_decode(mr, out));
        REQUIRE(out.frame_index == s.frame_index);
        REQUIRE(out.cpu_ms == s.cpu_ms);
        REQUIRE(out.gpu_ms == s.gpu_ms);
        REQUIRE(out.draw_calls == s.draw_calls);
        REQUIRE(out.entities == s.entities);
    }
    SECTION("SceneDelta with binary payload") {
        proto::SceneDelta d;
        d.frame_index = 0xDEADBEEFCAFEBABEull;
        d.entity_id = 0x01020304;
        d.op = 7;
        d.payload = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
        msgpack::Writer mw;
        proto::SceneDelta_encode(mw, d);
        msgpack::Reader mr(mw.data(), mw.size());
        proto::SceneDelta out;
        REQUIRE(proto::SceneDelta_decode(mr, out));
        REQUIRE(out.frame_index == d.frame_index);
        REQUIRE(out.entity_id == d.entity_id);
        REQUIRE(out.op == d.op);
        REQUIRE(out.payload == d.payload);
    }
}

TEST_CASE("ipc: opcode constants are stable", "[ipc][protocol]") {
    // The numeric opcodes are the wire contract — pin them so a thoughtless
    // edit to protocol.psy can't silently shift the wire format.
    REQUIRE(proto::opcodes::kHelloFrame == 1);
    REQUIRE(proto::opcodes::kWelcomeFrame == 2);
    REQUIRE(proto::opcodes::kSubscribeFrame == 3);
    REQUIRE(proto::opcodes::kUnsubscribeFrame == 4);
    REQUIRE(proto::opcodes::kLogFrame == 16);
    REQUIRE(proto::opcodes::kSceneFrame == 17);
    REQUIRE(proto::opcodes::kStatsFrame == 18);
    REQUIRE(proto::opcodes::kConsoleFrame == 19);
}
