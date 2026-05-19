// SPDX-License-Identifier: MIT
// Psynder — lm_cook driver. Lane 24 / tools.

#include "Cook.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace psynder::tools::cook {

namespace fs = std::filesystem;

namespace {

std::string lower_ext(std::string_view filename) {
    usize dot = filename.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(filename.substr(dot + 1));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<u8>(c)));
    return ext;
}

bool read_file(const fs::path& p, std::vector<u8>& out, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { err = "cannot open " + p.string(); return false; }
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz < 0) { err = "cannot stat " + p.string(); return false; }
    out.resize(static_cast<usize>(sz));
    in.seekg(0, std::ios::beg);
    if (!out.empty()) in.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(in);
}

bool write_file(const fs::path& p, std::span<const u8> data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write " + p.string(); return false; }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

}  // anon namespace

CookKind classify_extension(std::string_view filename) noexcept {
    std::string ext = lower_ext(filename);
    if (ext == "obj")  return CookKind::kMeshObj;
    if (ext == "gltf") return CookKind::kMeshGltf;
    if (ext == "png")  return CookKind::kTexturePng;
    if (ext == "wav")  return CookKind::kAudioWav;
    return CookKind::kUnknown;
}

bool cook_obj_blob(std::string_view obj_text, std::vector<u8>& lmm_out, std::string* err) {
    LmmMesh mesh;
    if (!parse_obj(obj_text, mesh, err)) return false;
    write_lmm(mesh, lmm_out);
    return true;
}

bool cook_gltf_blob(std::string_view gltf_text,
                    std::span<const u8> external_buffer,
                    std::vector<u8>& lmm_out,
                    std::string* err) {
    LmmMesh mesh;
    if (!parse_gltf(gltf_text, external_buffer, mesh, err)) return false;
    write_lmm(mesh, lmm_out);
    return true;
}

bool cook_png_blob(std::span<const u8> png_bytes, std::vector<u8>& lmt_out, std::string* err) {
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    if (!decode_png_stored(png_bytes, rgba, w, h, err)) return false;
    LmtTexture tex = build_mipchain_rgba8(rgba.data(), w, h);
    write_lmt(tex, lmt_out);
    return true;
}

bool cook_wav_blob(std::span<const u8> wav_bytes, std::vector<u8>& lma_out, std::string* err) {
    LmaAudio audio;
    if (!parse_wav(wav_bytes, audio, err)) return false;
    write_lma(audio, lma_out);
    return true;
}

CookResult cook_file(std::string_view source_path, std::string_view dest_dir) {
    CookResult res;
    res.kind = classify_extension(source_path);
    if (res.kind == CookKind::kUnknown) {
        res.error = "unrecognised extension";
        return res;
    }
    fs::path source(source_path);
    std::vector<u8> bytes;
    std::string err;
    if (!read_file(source, bytes, err)) {
        res.error = err;
        return res;
    }
    std::vector<u8> output;
    std::string sub_err;
    const char* out_ext = nullptr;
    switch (res.kind) {
        case CookKind::kMeshObj: {
            std::string_view obj(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!cook_obj_blob(obj, output, &sub_err)) { res.error = sub_err; return res; }
            out_ext = ".lmm";
        } break;
        case CookKind::kMeshGltf: {
            std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!cook_gltf_blob(text, {}, output, &sub_err)) { res.error = sub_err; return res; }
            out_ext = ".lmm";
        } break;
        case CookKind::kTexturePng: {
            if (!cook_png_blob(bytes, output, &sub_err)) { res.error = sub_err; return res; }
            out_ext = ".lmt";
        } break;
        case CookKind::kAudioWav: {
            if (!cook_wav_blob(bytes, output, &sub_err)) { res.error = sub_err; return res; }
            out_ext = ".lma";
        } break;
        default: break;
    }
    fs::path out_path = fs::path(dest_dir) / source.stem();
    out_path += out_ext;
    if (!write_file(out_path, output, err)) {
        res.error = err;
        return res;
    }
    res.output_path = out_path.string();
    res.ok = true;
    return res;
}

void print_help() {
    std::fprintf(stdout,
        "lm_cook — Psynder asset cooker (PNG/WAV/OBJ/glTF → .lmt/.lma/.lmm)\n"
        "\n"
        "Usage:\n"
        "  lm_cook <source_file> <dest_dir>\n"
        "  lm_cook --help\n"
        "\n"
        "Outputs are written as <dest_dir>/<stem>.<lmm|lmt|lma>.\n"
        "\n"
        "Wave-A note: PNG decoder accepts uncompressed-stored DEFLATE streams\n"
        "(what stb_image_write and Z_NO_COMPRESSION emit); arbitrary PNGs need\n"
        "the Wave-B stb_image vendor pass.\n");
}

int cli_main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }
    std::string_view a = argv[1];
    if (a == "--help" || a == "-h" || a == "help") { print_help(); return 0; }
    if (argc < 3) { print_help(); return 1; }
    auto r = cook_file(argv[1], argv[2]);
    if (!r.ok) {
        std::fprintf(stderr, "lm_cook: %s\n", r.error.c_str());
        return 1;
    }
    std::fprintf(stdout, "lm_cook: wrote %s\n", r.output_path.c_str());
    return 0;
}

}  // namespace psynder::tools::cook
