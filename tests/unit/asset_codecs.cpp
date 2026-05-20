// SPDX-License-Identifier: MIT
// Psynder — Lane 05 asset codec tests: .lmm / .lmt / .lma writer ↔ reader
// byte-exact round-trip, plus the Wave-B zstd `write_entry()` cooker path.

#include "asset/Codecs.h"
#include "asset/Formats.h"
#include "asset/LmpakFormat.h"
#include "asset/LmpakWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <random>
#include <vector>

using namespace psynder;
namespace fmt = psynder::asset::formats;

namespace {

// Build a deterministic mesh blob the codec can write + read back. The
// vertex stream is opaque bytes (the codec stores it verbatim) so we just
// pack synthetic data.
asset::lmm::Mesh make_test_mesh(u32 vertex_count, u32 index_count, u16 submesh_count) {
    asset::lmm::Mesh mesh;
    mesh.vertex_fmt = fmt::LmmVertexFmt::Pos3N3UV2;
    mesh.vertex_stride = asset::lmm::stride_for(mesh.vertex_fmt);
    mesh.vertex_count = vertex_count;
    mesh.index_count = index_count;
    mesh.bbox_min[0] = -1.0f;
    mesh.bbox_min[1] = -1.5f;
    mesh.bbox_min[2] = -2.0f;
    mesh.bbox_max[0] = 1.0f;
    mesh.bbox_max[1] = 1.5f;
    mesh.bbox_max[2] = 2.0f;

    mesh.vertex_data.assign(static_cast<usize>(vertex_count) * mesh.vertex_stride, 0);
    for (usize i = 0; i < mesh.vertex_data.size(); ++i) {
        mesh.vertex_data[i] = static_cast<u8>((i * 0x9Eu + 0x11u) & 0xFFu);
    }
    const u32 ix_bytes = asset::lmm::index_byte_size(vertex_count);
    mesh.index_data.assign(static_cast<usize>(index_count) * ix_bytes, 0);
    for (u32 i = 0; i < index_count; ++i) {
        const u32 ix = i % vertex_count;
        if (ix_bytes == 2) {
            u16 v = static_cast<u16>(ix);
            std::memcpy(mesh.index_data.data() + i * 2, &v, 2);
        } else {
            std::memcpy(mesh.index_data.data() + i * 4, &ix, 4);
        }
    }

    // Carve `submesh_count` ranges across the index buffer.
    const u32 chunk = index_count / submesh_count;
    for (u16 s = 0; s < submesh_count; ++s) {
        asset::lmm::Submesh sm;
        sm.index_start = s * chunk;
        sm.index_count = (s == submesh_count - 1u) ? (index_count - sm.index_start) : chunk;
        sm.material_hash = 0xABCD0000u | s;
        sm.reserved = 0;
        mesh.submeshes.push_back(sm);
    }
    return mesh;
}

bool meshes_equal(const asset::lmm::Mesh& a, const asset::lmm::Mesh& b) {
    if (a.vertex_fmt != b.vertex_fmt)
        return false;
    if (a.vertex_stride != b.vertex_stride)
        return false;
    if (a.vertex_count != b.vertex_count)
        return false;
    if (a.index_count != b.index_count)
        return false;
    if (std::memcmp(a.bbox_min, b.bbox_min, sizeof(a.bbox_min)) != 0)
        return false;
    if (std::memcmp(a.bbox_max, b.bbox_max, sizeof(a.bbox_max)) != 0)
        return false;
    if (a.submeshes.size() != b.submeshes.size())
        return false;
    for (usize i = 0; i < a.submeshes.size(); ++i) {
        const auto& l = a.submeshes[i];
        const auto& r = b.submeshes[i];
        if (l.index_start != r.index_start)
            return false;
        if (l.index_count != r.index_count)
            return false;
        if (l.material_hash != r.material_hash)
            return false;
        if (l.reserved != r.reserved)
            return false;
    }
    return a.vertex_data == b.vertex_data && a.index_data == b.index_data;
}

}  // namespace

TEST_CASE("asset/codecs: .lmm writer ↔ reader round-trip (u16 indices)", "[asset][codecs][lmm]") {
    // 8 vertices, 6 indices (1 quad as 2 triangles), 2 submeshes.
    auto mesh = make_test_mesh(/*vertex_count=*/8, /*index_count=*/6, /*submesh_count=*/2);
    std::vector<u8> blob;
    REQUIRE(asset::lmm::write_mesh(blob, mesh));
    REQUIRE(blob.size() >= sizeof(fmt::LmmHeader));

    // Header sanity: matches what the cooker promised.
    fmt::LmmHeader hdr{};
    std::memcpy(&hdr, blob.data(), sizeof(hdr));
    REQUIRE(hdr.file.magic == fmt::kLmmMagic);
    REQUIRE(hdr.file.version == fmt::kLmmVersion);
    REQUIRE(hdr.vertex_count == 8);
    REQUIRE(hdr.index_count == 6);
    REQUIRE(hdr.submesh_count == 2);
    REQUIRE(hdr.vertex_stride == 32);

    asset::lmm::Mesh decoded;
    REQUIRE(asset::lmm::read_mesh(std::span<const u8>(blob), decoded));
    REQUIRE(meshes_equal(mesh, decoded));
}

TEST_CASE("asset/codecs: .lmm writer ↔ reader round-trip (u32 indices, wide mesh)",
          "[asset][codecs][lmm]") {
    // 70k vertices forces 32-bit indices.
    auto mesh = make_test_mesh(/*vertex_count=*/70'000,
                               /*index_count=*/180,
                               /*submesh_count=*/3);
    REQUIRE(asset::lmm::index_byte_size(mesh.vertex_count) == 4);

    std::vector<u8> blob;
    REQUIRE(asset::lmm::write_mesh(blob, mesh));

    asset::lmm::Mesh decoded;
    REQUIRE(asset::lmm::read_mesh(std::span<const u8>(blob), decoded));
    REQUIRE(meshes_equal(mesh, decoded));
}

TEST_CASE("asset/codecs: .lmm reader rejects bad magic / version", "[asset][codecs][lmm]") {
    auto mesh = make_test_mesh(4, 6, 1);
    std::vector<u8> blob;
    REQUIRE(asset::lmm::write_mesh(blob, mesh));

    SECTION("magic mismatch") {
        blob[0] ^= 0xFFu;
        asset::lmm::Mesh decoded;
        REQUIRE_FALSE(asset::lmm::read_mesh(std::span<const u8>(blob), decoded));
    }
    SECTION("version mismatch") {
        // Bump version to 999.
        u16 bad_ver = 999;
        std::memcpy(blob.data() + 4, &bad_ver, sizeof(bad_ver));
        asset::lmm::Mesh decoded;
        REQUIRE_FALSE(asset::lmm::read_mesh(std::span<const u8>(blob), decoded));
    }
    SECTION("truncated payload") {
        blob.resize(blob.size() / 2);
        asset::lmm::Mesh decoded;
        REQUIRE_FALSE(asset::lmm::read_mesh(std::span<const u8>(blob), decoded));
    }
}

TEST_CASE("asset/codecs: .lmt writer ↔ reader round-trip (RGBA8 + mip pyramid)",
          "[asset][codecs][lmt]") {
    asset::lmt::Texture tex;
    tex.width = 4;
    tex.height = 4;
    tex.pixel_fmt = fmt::LmtPixelFmt::RGBA8;
    tex.flags = fmt::kLmtFlagSRGB;

    // 3 mips: 4x4 (64 bytes), 2x2 (16 bytes), 1x1 (4 bytes).
    const u32 mip0_bytes = 4u * 4u * 4u;
    const u32 mip1_bytes = 2u * 2u * 4u;
    const u32 mip2_bytes = 1u * 1u * 4u;
    tex.pixel_data.resize(mip0_bytes + mip1_bytes + mip2_bytes);
    for (usize i = 0; i < tex.pixel_data.size(); ++i) {
        tex.pixel_data[i] = static_cast<u8>(i * 7u + 3u);
    }
    tex.mips.push_back({4u, 4u, 0u, mip0_bytes});
    tex.mips.push_back({2u, 2u, mip0_bytes, mip1_bytes});
    tex.mips.push_back({1u, 1u, mip0_bytes + mip1_bytes, mip2_bytes});

    std::vector<u8> blob;
    REQUIRE(asset::lmt::write_texture(blob, tex));

    asset::lmt::Texture decoded;
    REQUIRE(asset::lmt::read_texture(std::span<const u8>(blob), decoded));
    REQUIRE(decoded.width == tex.width);
    REQUIRE(decoded.height == tex.height);
    REQUIRE(decoded.pixel_fmt == tex.pixel_fmt);
    REQUIRE(decoded.flags == tex.flags);
    REQUIRE(decoded.mips.size() == tex.mips.size());
    for (usize i = 0; i < tex.mips.size(); ++i) {
        REQUIRE(decoded.mips[i].width == tex.mips[i].width);
        REQUIRE(decoded.mips[i].height == tex.mips[i].height);
        REQUIRE(decoded.mips[i].offset == tex.mips[i].offset);
        REQUIRE(decoded.mips[i].byte_size == tex.mips[i].byte_size);
    }
    REQUIRE(decoded.pixel_data == tex.pixel_data);
}

TEST_CASE("asset/codecs: .lmt writer ↔ reader round-trip (P8 with palette)",
          "[asset][codecs][lmt]") {
    asset::lmt::Texture tex;
    tex.width = 8;
    tex.height = 8;
    tex.pixel_fmt = fmt::LmtPixelFmt::P8;
    tex.flags = 0;

    // 1 mip, 8x8 P8 = 64 bytes of palette indices.
    tex.pixel_data.resize(64);
    for (usize i = 0; i < tex.pixel_data.size(); ++i) {
        tex.pixel_data[i] = static_cast<u8>(i);
    }
    tex.mips.push_back({8u, 8u, 0u, 64u});

    // Palette: 256 entries × RGBA8 = 1024 bytes.
    tex.palette.resize(256 * 4);
    for (usize i = 0; i < tex.palette.size(); ++i) {
        tex.palette[i] = static_cast<u8>(i ^ 0x5A);
    }

    std::vector<u8> blob;
    REQUIRE(asset::lmt::write_texture(blob, tex));

    asset::lmt::Texture decoded;
    REQUIRE(asset::lmt::read_texture(std::span<const u8>(blob), decoded));
    REQUIRE(decoded.pixel_fmt == fmt::LmtPixelFmt::P8);
    REQUIRE(decoded.palette == tex.palette);
    REQUIRE(decoded.pixel_data == tex.pixel_data);
}

TEST_CASE("asset/codecs: .lma writer ↔ reader round-trip (PCM_S16 mono)", "[asset][codecs][lma]") {
    asset::lma::Audio audio;
    audio.sample_rate = 48000;
    audio.frame_count = 1024;
    audio.channels = 1;
    audio.sample_fmt = fmt::LmaSampleFmt::PCM_S16;
    audio.flags = 0;
    audio.pcm_data.resize(static_cast<usize>(audio.frame_count) * audio.channels *
                          asset::lma::bytes_per_sample(audio.sample_fmt));
    for (usize i = 0; i < audio.pcm_data.size(); ++i) {
        audio.pcm_data[i] = static_cast<u8>((i * 13u + 17u) & 0xFFu);
    }

    std::vector<u8> blob;
    REQUIRE(asset::lma::write_audio(blob, audio));

    asset::lma::Audio decoded;
    REQUIRE(asset::lma::read_audio(std::span<const u8>(blob), decoded));
    REQUIRE(decoded.sample_rate == audio.sample_rate);
    REQUIRE(decoded.frame_count == audio.frame_count);
    REQUIRE(decoded.channels == audio.channels);
    REQUIRE(decoded.sample_fmt == audio.sample_fmt);
    REQUIRE(decoded.flags == audio.flags);
    REQUIRE(decoded.pcm_data == audio.pcm_data);
}

TEST_CASE("asset/codecs: .lma writer ↔ reader round-trip (PCM_F32 stereo + loop)",
          "[asset][codecs][lma]") {
    asset::lma::Audio audio;
    audio.sample_rate = 44100;
    audio.frame_count = 4096;
    audio.channels = 2;
    audio.sample_fmt = fmt::LmaSampleFmt::PCM_F32;
    audio.flags = fmt::kLmaFlagLoop;
    audio.loop_start = 1024;
    audio.loop_end = 3072;
    audio.pcm_data.resize(static_cast<usize>(audio.frame_count) * audio.channels *
                          asset::lma::bytes_per_sample(audio.sample_fmt));
    std::mt19937 rng(0xCAFEBABEu);
    for (usize i = 0; i < audio.pcm_data.size(); ++i) {
        audio.pcm_data[i] = static_cast<u8>(rng() & 0xFFu);
    }

    std::vector<u8> blob;
    REQUIRE(asset::lma::write_audio(blob, audio));

    asset::lma::Audio decoded;
    REQUIRE(asset::lma::read_audio(std::span<const u8>(blob), decoded));
    REQUIRE(decoded.sample_rate == audio.sample_rate);
    REQUIRE(decoded.frame_count == audio.frame_count);
    REQUIRE(decoded.channels == audio.channels);
    REQUIRE(decoded.sample_fmt == audio.sample_fmt);
    REQUIRE((decoded.flags & fmt::kLmaFlagLoop) != 0);
    REQUIRE(decoded.loop_start == audio.loop_start);
    REQUIRE(decoded.loop_end == audio.loop_end);
    REQUIRE(decoded.pcm_data == audio.pcm_data);
}

TEST_CASE("asset/codecs: writer rejects an inconsistent mesh", "[asset][codecs][lmm]") {
    auto mesh = make_test_mesh(8, 6, 2);
    // Truncate the vertex buffer so vertex_count * stride != vertex_data.size().
    mesh.vertex_data.pop_back();
    std::vector<u8> blob;
    REQUIRE_FALSE(asset::lmm::write_mesh(blob, mesh));
}

TEST_CASE("asset/codecs: writer rejects a P8 texture without a 1KB palette",
          "[asset][codecs][lmt]") {
    asset::lmt::Texture tex;
    tex.width = 8;
    tex.height = 8;
    tex.pixel_fmt = fmt::LmtPixelFmt::P8;
    tex.flags = 0;
    tex.pixel_data.assign(64, 0);
    tex.mips.push_back({8, 8, 0, 64});
    // Missing palette → reject.
    std::vector<u8> blob;
    REQUIRE_FALSE(asset::lmt::write_texture(blob, tex));
}

TEST_CASE("asset/codecs: writer rejects an audio buffer with bad pcm size", "[asset][codecs][lma]") {
    asset::lma::Audio audio;
    audio.sample_rate = 48000;
    audio.frame_count = 128;
    audio.channels = 2;
    audio.sample_fmt = fmt::LmaSampleFmt::PCM_S16;
    // Should be 128 * 2 * 2 = 512 bytes; supply 511 to trip the check.
    audio.pcm_data.assign(511, 0);
    std::vector<u8> blob;
    REQUIRE_FALSE(asset::lma::write_audio(blob, audio));
}

// ─── zstd cooker `write_entry` ──────────────────────────────────────────
// The new `Writer::write_entry(path, bytes, size, compress)` is the
// Wave-B-named cooker entry point. It mirrors `add_file` with a
// cooker-friendlier default (level 6) and a span overload. The on-disk
// entry has the per-entry zstd flag set, with both compressed +
// uncompressed sizes recorded.

TEST_CASE("asset/codecs: Writer::write_entry stores compressed + uncompressed sizes",
          "[asset][codecs][lmpak][zstd]") {
    if (!asset::lmpak::zstd_available()) {
        SUCCEED("zstd not built into psynder_asset; cooker test skipped");
        return;
    }
    // Highly compressible payload: 64 KiB of a repeating cycle.
    std::vector<u8> payload(64 * 1024);
    for (usize i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>('A' + (i % 26));
    }

    asset::lmpak::Writer w;
    REQUIRE(w.write_entry("data/big.bin",
                          payload.data(),
                          payload.size(),
                          /*compress=*/true));
    // span overload exercises the alternative signature too.
    REQUIRE(w.write_entry("data/copy.bin",
                          std::span<const u8>(payload),
                          /*compress=*/true));

    std::vector<u8> archive = w.build_bytes();
    REQUIRE(archive.size() > sizeof(asset::lmpak::LmpakHeader));

    asset::lmpak::LmpakHeader hdr{};
    std::memcpy(&hdr, archive.data(), sizeof(hdr));
    REQUIRE(hdr.magic == asset::lmpak::kMagic);
    REQUIRE(hdr.version == asset::lmpak::kVersion);
    REQUIRE(hdr.entry_count == 2);
    // The archive-wide flag should signal "at least one zstd entry".
    REQUIRE((hdr.flags & asset::lmpak::kFlagHasCompr) != 0);

    // Walk the entry table and confirm each entry has compressed flag +
    // a `size` smaller than `uncompressed`.
    const auto* entries =
        reinterpret_cast<const asset::lmpak::LmpakEntry*>(archive.data() + hdr.entry_table_offset);
    for (u32 i = 0; i < hdr.entry_count; ++i) {
        REQUIRE((entries[i].flags & asset::lmpak::kEntryZstd) != 0);
        REQUIRE(entries[i].uncompressed == payload.size());
        REQUIRE(entries[i].size < entries[i].uncompressed);
    }
}

TEST_CASE("asset/codecs: Writer::write_entry uncompressed bypasses zstd", "[asset][codecs][lmpak]") {
    std::vector<u8> payload{0xDE, 0xAD, 0xBE, 0xEF};
    asset::lmpak::Writer w;
    REQUIRE(w.write_entry("misc/raw.bin",
                          payload.data(),
                          payload.size(),
                          /*compress=*/false));
    auto archive = w.build_bytes();

    asset::lmpak::LmpakHeader hdr{};
    std::memcpy(&hdr, archive.data(), sizeof(hdr));
    REQUIRE(hdr.entry_count == 1);
    const auto* entry =
        reinterpret_cast<const asset::lmpak::LmpakEntry*>(archive.data() + hdr.entry_table_offset);
    REQUIRE((entry->flags & asset::lmpak::kEntryZstd) == 0);
    REQUIRE(entry->size == payload.size());
    REQUIRE(entry->uncompressed == payload.size());
    // Raw payload bytes are present at the offset.
    REQUIRE(std::memcmp(archive.data() + entry->offset, payload.data(), payload.size()) == 0);
}
