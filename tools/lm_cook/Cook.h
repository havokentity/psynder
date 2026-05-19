// SPDX-License-Identifier: MIT
// Psynder — lm_cook library entrypoint.

#pragma once

#include "core/Types.h"
#include "CookFormats.h"
#include "SourceParsers.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::cook {

enum class CookKind {
    kUnknown,
    kMeshObj,
    kMeshGltf,
    kTexturePng,
    kAudioWav,
};

CookKind classify_extension(std::string_view filename) noexcept;

struct CookResult {
    bool ok = false;
    CookKind kind = CookKind::kUnknown;
    std::string output_path;
    std::string error;
};

// Cook a single source file. The output extension is selected from `kind`
// (.lmm / .lmt / .lma); `dest_dir` is mkdir-p'd if missing.
CookResult cook_file(std::string_view source_path,
                     std::string_view dest_dir);

// Library entrypoints used by tests to drive each pipeline with synthetic
// in-memory inputs:
bool cook_obj_blob(std::string_view obj_text,
                   std::vector<u8>& lmm_out,
                   std::string* err = nullptr);
bool cook_gltf_blob(std::string_view gltf_text,
                    std::span<const u8> external_buffer,
                    std::vector<u8>& lmm_out,
                    std::string* err = nullptr);
bool cook_png_blob(std::span<const u8> png_bytes,
                   std::vector<u8>& lmt_out,
                   std::string* err = nullptr);
bool cook_wav_blob(std::span<const u8> wav_bytes,
                   std::vector<u8>& lma_out,
                   std::string* err = nullptr);

int  cli_main(int argc, char** argv);
void print_help();

}  // namespace psynder::tools::cook
