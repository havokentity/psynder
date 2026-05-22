// SPDX-License-Identifier: MIT
// Psynder — async texture asset loading.

#pragma once

#include "core/Types.h"

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace psynder::asset {

enum class TexturePixelFormat : u16 {
    RGBA8 = 0,
};

enum class TextureColorSpace : u8 {
    Linear = 0,
    SRGB = 1,
};

enum class TextureSourceFormat : u8 {
    Auto = 0,
    PpmP6,
    Lmt,
};

enum class TextureLoadStatus : u8 {
    Pending = 0,
    Ready,
    Failed,
};

struct TextureMip {
    u32 width = 0;
    u32 height = 0;
    u32 offset = 0;
    u32 byte_size = 0;
};

struct TextureData {
    u32 width = 0;
    u32 height = 0;
    TexturePixelFormat pixel_format = TexturePixelFormat::RGBA8;
    TextureColorSpace color_space = TextureColorSpace::SRGB;
    std::vector<TextureMip> mips;  // mip 0 is largest
    std::vector<u8> pixels;        // tightly packed mip chain

    [[nodiscard]] bool valid() const noexcept {
        return width != 0 && height != 0 && !mips.empty() && !pixels.empty();
    }

    [[nodiscard]] const TextureMip* mip(u32 index) const noexcept {
        return index < mips.size() ? &mips[index] : nullptr;
    }

    [[nodiscard]] std::span<const u8> mip_bytes(u32 index) const noexcept {
        const TextureMip* m = mip(index);
        if (!m || static_cast<usize>(m->offset) + m->byte_size > pixels.size())
            return {};
        return {pixels.data() + m->offset, m->byte_size};
    }
};

struct TextureLoadDesc {
    std::string virtual_path;
    TextureSourceFormat source_format = TextureSourceFormat::Auto;
};

using TextureLoadCallback = void (*)(TextureLoadStatus status,
                                     const TextureData& texture,
                                     void* user) noexcept;

namespace texture_detail {
struct TextureLoadState;
}  // namespace texture_detail

class TextureLoad {
   public:
    TextureLoad() = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] TextureLoadStatus status() const noexcept;
    [[nodiscard]] bool ok() const noexcept { return status() == TextureLoadStatus::Ready; }
    [[nodiscard]] const TextureData* texture() const noexcept;

    // Moves the decoded texture out exactly once. Returns false while pending,
    // on failure, or after another caller has already consumed the payload.
    bool take_if_ready(TextureData& out) noexcept;

   private:
    friend TextureLoad load_texture_async(TextureLoadDesc desc,
                                          TextureLoadCallback callback,
                                          void* user);

    explicit TextureLoad(std::shared_ptr<texture_detail::TextureLoadState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<texture_detail::TextureLoadState> state_;
};

TextureLoad load_texture_async(TextureLoadDesc desc,
                               TextureLoadCallback callback = nullptr,
                               void* user = nullptr);

inline TextureLoad load_texture_async(std::string virtual_path,
                                      TextureLoadCallback callback = nullptr,
                                      void* user = nullptr) {
    TextureLoadDesc desc{};
    desc.virtual_path = std::move(virtual_path);
    return load_texture_async(std::move(desc), callback, user);
}

inline TextureLoad load_ppm_texture_async(std::string virtual_path,
                                          TextureLoadCallback callback = nullptr,
                                          void* user = nullptr) {
    TextureLoadDesc desc{};
    desc.virtual_path = std::move(virtual_path);
    desc.source_format = TextureSourceFormat::PpmP6;
    return load_texture_async(std::move(desc), callback, user);
}

// CPU decode helpers for worker-side code and tests. They do not perform I/O.
bool decode_ppm_rgba8(std::span<const u8> bytes, TextureData& out) noexcept;
bool decode_lmt_rgba8(std::span<const u8> bytes, TextureData& out) noexcept;
bool decode_texture_rgba8(std::span<const u8> bytes,
                          TextureSourceFormat source_format,
                          TextureData& out) noexcept;

}  // namespace psynder::asset
