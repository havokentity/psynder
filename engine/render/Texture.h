// SPDX-License-Identifier: MIT
// Psynder — lightweight CPU texture ownership + async load helpers.

#pragma once

#include "jobs/JobSystem.h"
#include "render/Image.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace psynder::render {

// Hot-path view: plain pointer + dimensions, cheap to copy into DrawItem.
struct TextureView {
    const u32* texels = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 pitch = 0;  // texels per row

    [[nodiscard]] constexpr bool valid() const noexcept {
        return texels != nullptr && width != 0 && height != 0 && pitch >= width;
    }
};

class Texture2D {
   public:
    Texture2D() = default;
    explicit Texture2D(Rgba8Image image) noexcept : image_(std::move(image)) {}

    [[nodiscard]] static Texture2D from_rgba8(u32 width, u32 height, std::vector<u32> pixels) {
        Rgba8Image image{};
        image.width = width;
        image.height = height;
        image.pixels = std::move(pixels);
        if (image.width == 0 || image.height == 0 ||
            image.pixels.size() != static_cast<usize>(image.width) * image.height) {
            image = {};
        }
        return Texture2D{std::move(image)};
    }

    [[nodiscard]] TextureView view() const noexcept {
        return {image_.pixels.empty() ? nullptr : image_.pixels.data(),
                image_.width,
                image_.height,
                image_.width};
    }

    [[nodiscard]] bool valid() const noexcept { return view().valid(); }
    [[nodiscard]] u32 width() const noexcept { return image_.width; }
    [[nodiscard]] u32 height() const noexcept { return image_.height; }
    [[nodiscard]] const u32* data() const noexcept {
        return image_.pixels.empty() ? nullptr : image_.pixels.data();
    }
    [[nodiscard]] std::vector<u32>& pixels() noexcept { return image_.pixels; }
    [[nodiscard]] const std::vector<u32>& pixels() const noexcept { return image_.pixels; }

   private:
    Rgba8Image image_{};
};

enum class TextureLoadStatus : u8 {
    Pending,
    Ready,
    Failed,
};

namespace texture_detail {

struct TextureLoadState {
    std::atomic<bool> done{false};
    bool ok = false;
    Texture2D texture;
    std::atomic<bool> consumed{false};
};

struct TextureLoadPayload {
    std::shared_ptr<TextureLoadState> state;
    std::string path;
};

inline void load_ppm_texture_job(void* user) noexcept {
    auto* payload = static_cast<TextureLoadPayload*>(user);
    Rgba8Image image{};
    const bool ok = image_detail::load_ppm_rgba8_from_file_blocking(payload->path.c_str(), image);
    payload->state->texture = ok ? Texture2D{std::move(image)} : Texture2D{};
    payload->state->ok = ok;
    payload->state->done.store(true, std::memory_order_release);
    delete payload;
}

}  // namespace texture_detail

class TextureLoad {
   public:
    TextureLoad() = default;

    [[nodiscard]] bool ready() const noexcept {
        return state_ && state_->done.load(std::memory_order_acquire);
    }

    [[nodiscard]] TextureLoadStatus status() const noexcept {
        if (!ready())
            return TextureLoadStatus::Pending;
        return state_->ok ? TextureLoadStatus::Ready : TextureLoadStatus::Failed;
    }

    [[nodiscard]] bool ok() const noexcept { return status() == TextureLoadStatus::Ready; }

    [[nodiscard]] const Texture2D* texture() const noexcept {
        return ok() ? &state_->texture : nullptr;
    }

    bool take_if_ready(Texture2D& out) {
        if (!state_ || !state_->done.load(std::memory_order_acquire) || !state_->ok)
            return false;
        bool expected = false;
        if (!state_->consumed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return false;
        out = std::move(state_->texture);
        return true;
    }

   private:
    friend TextureLoad load_ppm_texture_async(std::string path);

    TextureLoad(std::shared_ptr<texture_detail::TextureLoadState> state, jobs::JobHandle handle) noexcept
        : state_(std::move(state)), handle_(handle) {}

    std::shared_ptr<texture_detail::TextureLoadState> state_;
    // Kept for diagnostics/future cancellation hooks; callers never block on it.
    jobs::JobHandle handle_{};
};

inline TextureLoad load_ppm_texture_async(std::string path) {
    auto state = std::make_shared<texture_detail::TextureLoadState>();
    auto* payload = new texture_detail::TextureLoadPayload{state, std::move(path)};

    jobs::JobDesc desc{};
    desc.fn = &texture_detail::load_ppm_texture_job;
    desc.user = payload;
    desc.name = "load_ppm_texture";
    const jobs::JobHandle handle = jobs::JobSystem::Get().submit(desc);
    return TextureLoad{std::move(state), handle};
}

}  // namespace psynder::render
