// SPDX-License-Identifier: MIT
// Lane 24 tests — lm_cook pipelines.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_cook/Cook.h"
#include "../../tools/lm_cook/CookFormats.h"
#include "../../tools/lm_cook/SourceParsers.h"

#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::tools::cook;

TEST_CASE("lm_cook classifies extensions", "[tools][lm_cook]") {
    REQUIRE(classify_extension("foo.obj")  == CookKind::kMeshObj);
    REQUIRE(classify_extension("X.OBJ")    == CookKind::kMeshObj);
    REQUIRE(classify_extension("a.gltf")   == CookKind::kMeshGltf);
    REQUIRE(classify_extension("img.png")  == CookKind::kTexturePng);
    REQUIRE(classify_extension("snd.wav")  == CookKind::kAudioWav);
    REQUIRE(classify_extension("nope.txt") == CookKind::kUnknown);
}

TEST_CASE("lm_cook cooks an OBJ tetrahedron round-trip", "[tools][lm_cook]") {
    constexpr const char* kObj =
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "v 0 0 1\n"
        "vn 0 0 1\n"
        "vn 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0 1\n"
        "usemtl wall\n"
        "f 1/1/1 2/2/1 3/3/1\n"
        "f 1/1/2 4/2/2 2/3/2\n";

    std::vector<u8> lmm;
    std::string err;
    REQUIRE(cook_obj_blob(kObj, lmm, &err));
    REQUIRE_FALSE(lmm.empty());

    LmmMesh round;
    REQUIRE(read_lmm(lmm, round, &err));
    REQUIRE(round.vertices.size() == 6);     // two tris × 3 corners
    REQUIRE(round.indices.size() == 6);
    REQUIRE(round.materials.size() >= 1);
    REQUIRE(round.submeshes.size() >= 1);
}

TEST_CASE("lm_cook builds RGBA8 mipchain to single pixel", "[tools][lm_cook]") {
    // Build a 4x4 RGBA "red plus blue" gradient.
    std::vector<u8> rgba(4 * 4 * 4);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            usize i = (y * 4 + x) * 4;
            rgba[i + 0] = static_cast<u8>(x * 60);
            rgba[i + 1] = 0;
            rgba[i + 2] = static_cast<u8>(y * 60);
            rgba[i + 3] = 255;
        }
    }
    auto tex = build_mipchain_rgba8(rgba.data(), 4, 4);
    REQUIRE(tex.width == 4);
    REQUIRE(tex.height == 4);
    REQUIRE(tex.mips.size() == 3);    // 4 -> 2 -> 1
    REQUIRE(tex.mips.back().width == 1);
    REQUIRE(tex.mips.back().height == 1);
    REQUIRE(tex.pixels.size() == (4*4 + 2*2 + 1) * 4u);

    // Round-trip the binary blob.
    std::vector<u8> bytes;
    write_lmt(tex, bytes);
    LmtTexture back;
    std::string err;
    REQUIRE(read_lmt(bytes, back, &err));
    REQUIRE(back.width == 4);
    REQUIRE(back.height == 4);
    REQUIRE(back.mips.size() == 3);
    REQUIRE(back.pixels == tex.pixels);
}

TEST_CASE("lm_cook PNG stored round-trips", "[tools][lm_cook]") {
    // Build a 2x2 RGBA buffer, encode as PNG, then decode + cook.
    u8 rgba[2 * 2 * 4] = {
        255,   0,   0, 255,    0, 255,   0, 255,
          0,   0, 255, 255,  255, 255, 255, 255,
    };
    std::vector<u8> png;
    encode_png_stored(rgba, 2, 2, png);

    std::vector<u8> back_rgba;
    u32 w = 0, h = 0;
    std::string err;
    REQUIRE(decode_png_stored(png, back_rgba, w, h, &err));
    REQUIRE(w == 2);
    REQUIRE(h == 2);
    REQUIRE(back_rgba.size() == sizeof(rgba));
    REQUIRE(std::memcmp(back_rgba.data(), rgba, sizeof(rgba)) == 0);

    std::vector<u8> lmt;
    REQUIRE(cook_png_blob(png, lmt, &err));
    LmtTexture tex;
    REQUIRE(read_lmt(lmt, tex, &err));
    REQUIRE(tex.width == 2);
    REQUIRE(tex.height == 2);
    REQUIRE(tex.mips.size() == 2);
}

TEST_CASE("lm_cook WAV round-trip preserves PCM samples", "[tools][lm_cook]") {
    i16 samples[8] = { 0, 32767, -32768, 1234, -567, 100, -100, 7 };
    std::vector<u8> wav;
    encode_wav_pcm16(samples, 4 /* sample_count per channel */, 2 /* stereo */,
                     48000, wav);
    LmaAudio audio;
    std::string err;
    REQUIRE(parse_wav(wav, audio, &err));
    REQUIRE(audio.channels == 2);
    REQUIRE(audio.sample_rate == 48000);
    REQUIRE(audio.bits_per_sample == 16);
    REQUIRE(audio.sample_count == 4);
    REQUIRE(audio.samples.size() == sizeof(samples));
    REQUIRE(std::memcmp(audio.samples.data(), samples, sizeof(samples)) == 0);

    std::vector<u8> lma;
    REQUIRE(cook_wav_blob(wav, lma, &err));
    LmaAudio back;
    REQUIRE(read_lma(lma, back, &err));
    REQUIRE(back.sample_count == 4);
}

// ─── Wave-B: stb_image integration ───────────────────────────────────────

TEST_CASE("lm_cook stb PNG round-trip preserves arbitrary pixel data", "[tools][lm_cook][wave-b]") {
    // A 16×16 RGBA image that exercises every channel + a transparency
    // gradient. Wave-A's stored-deflate codec round-tripped this, but the
    // emitted bytes were uncompressed (every PNG decoder accepts them, the
    // file size was just larger than necessary). Wave-B routes through
    // stb_image_write which DEFLATE-compresses the IDAT.
    constexpr u32 W = 16, H = 16;
    std::vector<u8> rgba(W * H * 4u);
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            usize i = (y * W + x) * 4;
            rgba[i + 0] = static_cast<u8>(x * 16);
            rgba[i + 1] = static_cast<u8>(y * 16);
            rgba[i + 2] = static_cast<u8>((x ^ y) * 16);
            rgba[i + 3] = static_cast<u8>(255 - x * 8);
        }
    }
    std::vector<u8> png;
    encode_png_stored(rgba.data(), W, H, png);
    REQUIRE_FALSE(png.empty());
    // PNG signature must be present in the encoded output.
    REQUIRE(png.size() >= 8);
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 0x50);
    REQUIRE(png[2] == 0x4E);
    REQUIRE(png[3] == 0x47);

    std::vector<u8> back;
    u32 w = 0, h = 0;
    std::string err;
    REQUIRE(decode_png_stored(png, back, w, h, &err));
    REQUIRE(w == W);
    REQUIRE(h == H);
    REQUIRE(back.size() == rgba.size());
    REQUIRE(std::memcmp(back.data(), rgba.data(), rgba.size()) == 0);
}

TEST_CASE("lm_cook stb PNG compresses output below stored-deflate baseline", "[tools][lm_cook][wave-b]") {
    // The Wave-A codec produced uncompressed IDAT (one byte filter + raw
    // RGBA scanlines, framed in stored-DEFLATE blocks). Wave-B's stb
    // encoder uses real DEFLATE so a flat image should compress well.
    constexpr u32 W = 64, H = 64;
    std::vector<u8> rgba(W * H * 4u);
    // Solid-colour fill — maximally compressible.
    for (usize i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = 200;
        rgba[i + 1] = 100;
        rgba[i + 2] = 50;
        rgba[i + 3] = 255;
    }
    std::vector<u8> png;
    encode_png_stored(rgba.data(), W, H, png);
    // Round-trip safety check.
    std::vector<u8> back;
    u32 w = 0, h = 0;
    std::string err;
    REQUIRE(decode_png_stored(png, back, w, h, &err));
    REQUIRE(back == rgba);
    // The raw scanline-deflated payload would be > W*H*4 = 16384 bytes plus
    // chunk overhead. With real DEFLATE on a constant image we expect well
    // under that.
    REQUIRE(png.size() < W * H * 4u / 2u);
}

TEST_CASE("lm_cook glTF minimal JSON path", "[tools][lm_cook]") {
    // Construct a buffer that holds:
    //   - 3 positions (VEC3 f32) = 36 bytes
    //   - 3 indices (UNSIGNED_INT) = 12 bytes
    std::vector<u8> buf(36 + 12);
    auto put_f32 = [&](usize off, f32 v) {
        u32 bits; std::memcpy(&bits, &v, sizeof(bits));
        for (usize i = 0; i < 4; ++i) buf[off + i] = static_cast<u8>(bits >> (8 * i));
    };
    auto put_u32 = [&](usize off, u32 v) {
        for (usize i = 0; i < 4; ++i) buf[off + i] = static_cast<u8>(v >> (8 * i));
    };
    put_f32( 0, 0); put_f32( 4, 0); put_f32( 8, 0);
    put_f32(12, 1); put_f32(16, 0); put_f32(20, 0);
    put_f32(24, 0); put_f32(28, 1); put_f32(32, 0);
    put_u32(36, 0); put_u32(40, 1); put_u32(44, 2);

    // Build a small data URI.
    auto b64encode = [](std::span<const u8> in) {
        static const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        usize i = 0;
        while (i + 3 <= in.size()) {
            u32 v = (u32(in[i]) << 16) | (u32(in[i+1]) << 8) | u32(in[i+2]);
            out.push_back(alpha[(v >> 18) & 0x3F]);
            out.push_back(alpha[(v >> 12) & 0x3F]);
            out.push_back(alpha[(v >>  6) & 0x3F]);
            out.push_back(alpha[v & 0x3F]);
            i += 3;
        }
        if (i + 1 == in.size()) {
            u32 v = u32(in[i]) << 16;
            out.push_back(alpha[(v >> 18) & 0x3F]);
            out.push_back(alpha[(v >> 12) & 0x3F]);
            out.append("==");
        } else if (i + 2 == in.size()) {
            u32 v = (u32(in[i]) << 16) | (u32(in[i+1]) << 8);
            out.push_back(alpha[(v >> 18) & 0x3F]);
            out.push_back(alpha[(v >> 12) & 0x3F]);
            out.push_back(alpha[(v >>  6) & 0x3F]);
            out.push_back('=');
        }
        return out;
    };
    std::string b64 = b64encode(buf);
    std::string json = R"({"buffers":[{"uri":"data:application/octet-stream;base64,)"
                       + b64 + R"("}],"bufferViews":[{"byteOffset":0,"byteLength":36},)"
                       + R"({"byteOffset":36,"byteLength":12}],"accessors":[)"
                       + R"({"bufferView":0,"count":3,"componentType":5126,"type":"VEC3"},)"
                       + R"({"bufferView":1,"count":3,"componentType":5125,"type":"SCALAR"}],)"
                       + R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";

    std::vector<u8> lmm;
    std::string err;
    REQUIRE(cook_gltf_blob(json, {}, lmm, &err));
    LmmMesh mesh;
    REQUIRE(read_lmm(lmm, mesh, &err));
    REQUIRE(mesh.vertices.size() == 3);
    REQUIRE(mesh.indices.size() == 3);
    REQUIRE(mesh.indices[0] == 0);
    REQUIRE(mesh.indices[1] == 1);
    REQUIRE(mesh.indices[2] == 2);
}
