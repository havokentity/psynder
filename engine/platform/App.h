// SPDX-License-Identifier: MIT
// Psynder — shared executable app host helpers.

#pragma once

#include "asset/Vault.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/FrameStats.h"
#include "render/Framebuffer.h"
#include "render/GeometryTools.h"
#include "render/PngWriter.h"
#include "render/RenderingSystem.h"
#include "scene/SceneEcs.h"
#include "ui/imm/Imm.h"

#include <array>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psynder::app {

struct WindowAppOptions {
    bool depth_buffer = false;
    bool has_default_camera = true;
};

struct FrameClear {
    bool color = false;
    bool depth = false;
    u32 color_rgba8 = 0xFF000000u;

    [[nodiscard]] static constexpr FrameClear none() noexcept { return {}; }
    [[nodiscard]] static constexpr FrameClear color_only(u32 rgba8) noexcept {
        return {true, false, rgba8};
    }
    [[nodiscard]] static constexpr FrameClear depth_only() noexcept {
        return {false, true, 0xFF000000u};
    }
    [[nodiscard]] static constexpr FrameClear color_depth(u32 rgba8) noexcept {
        return {true, true, rgba8};
    }
};

inline platform::WindowDesc default_window_desc(std::string_view title = "Psynder") {
    platform::WindowDesc desc{};
    desc.title.assign(title.data(), title.size());
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;
    return desc;
}

// Owns the standard app/sample process boilerplate: common CLI args, one
// platform window, a CPU RGBA8 framebuffer backing store, optional depth, PNG
// capture, and deterministic cleanup. Samples keep only their scene/update code.
class WindowApp {
   public:
    WindowApp(const AppArgs& args, const platform::WindowDesc& desc, WindowAppOptions options = {})
        : args_(args)
        , width_(desc.render_width)
        , height_(desc.render_height)
        , has_default_camera_(options.has_default_camera) {
        render::reset_frame_stats();
        window_ = platform::create_window(desc);
        if (!window_)
            return;

        pixels_.resize(static_cast<usize>(width_) * height_, 0u);
        if (options.depth_buffer)
            depth_.resize(static_cast<usize>(width_) * height_, 0u);

        framebuffer_.width = width_;
        framebuffer_.height = height_;
        framebuffer_.pitch = width_ * 4u;
        framebuffer_.format = render::PixelFormat::RGBA8;
        framebuffer_.pixels = reinterpret_cast<u8*>(pixels_.data());
        framebuffer_.depth = depth_.empty() ? nullptr : depth_.data();
    }

    WindowApp(const WindowApp&) = delete;
    WindowApp& operator=(const WindowApp&) = delete;

    WindowApp(WindowApp&& other) noexcept { move_from(other); }
    WindowApp& operator=(WindowApp&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(other);
        }
        return *this;
    }

    ~WindowApp() { destroy(); }

    [[nodiscard]] bool valid() const noexcept { return window_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] const AppArgs& args() const noexcept { return args_; }
    [[nodiscard]] u32 smoke_frames() const noexcept { return args_.smoke_frames; }
    [[nodiscard]] bool capture_requested() const noexcept { return !args_.capture_out.empty(); }

    [[nodiscard]] platform::Window& window() noexcept { return *window_; }
    [[nodiscard]] const platform::Window& window() const noexcept { return *window_; }
    [[nodiscard]] render::Framebuffer& framebuffer() noexcept { return framebuffer_; }
    [[nodiscard]] const render::Framebuffer& framebuffer() const noexcept { return framebuffer_; }
    [[nodiscard]] std::vector<u32>& pixels() noexcept { return pixels_; }
    [[nodiscard]] const std::vector<u32>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::vector<u32>& depth() noexcept { return depth_; }
    [[nodiscard]] const std::vector<u32>& depth() const noexcept { return depth_; }
    [[nodiscard]] render::RenderingSystem& rendering_system() noexcept { return rendering_system_; }
    [[nodiscard]] const render::RenderingSystem& rendering_system() const noexcept {
        return rendering_system_;
    }

    [[nodiscard]] render::raster::ViewState default_raster_view() noexcept {
        render::raster::ViewState view{};
        view.target = framebuffer_;
        view.view = math::identity4();
        view.projection = math::identity4();
        view.tile_w = 64;
        view.tile_h = 64;
        return view;
    }

    void set_scene(scene::Scene& scene) noexcept {
        active_scene_ = &scene;
        if (has_default_camera_)
            (void)scene.ensure_default_camera(render_target_aspect());
    }

    void clear_scene() noexcept {
        active_scene_ = nullptr;
        active_scene_rendered_ = false;
    }

    void reserve_scene_capacity(u32 renderables, u32 meshes = 0) {
        rendering_system_.reserve_scene_capacity(renderables, meshes);
    }

    [[nodiscard]] render::MeshId create_mesh(const render::MeshDesc& mesh_desc) {
        return rendering_system_.meshes().create_mesh(mesh_desc);
    }

    [[nodiscard]] scene::RenderableComponent make_mesh_renderable(
        render::MeshId mesh,
        render::MaterialId material,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        math::Aabb local_bounds = math::aabb_empty(),
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) const {
        return rendering_system_.make_mesh_renderable(mesh, material, flags, local_bounds, mobility);
    }

    [[nodiscard]] render::SceneMeshEntity create_mesh_entity(
        scene::Scene& scene,
        const render::MeshDesc& mesh_desc,
        render::MaterialId material,
        const scene::LocalTransform& local = {},
        scene::SceneNode parent = scene::kInvalidSceneNode,
        scene::RenderableFlags flags = scene::RenderableFlags::Visible,
        scene::ObjectMobility mobility = scene::ObjectMobility::Dynamic) {
        return rendering_system_.create_mesh_entity(scene,
                                                    mesh_desc,
                                                    material,
                                                    local,
                                                    parent,
                                                    flags,
                                                    mobility);
    }

    render::SceneRenderStats render_scene(scene::Scene& scene) {
        return render_scene(scene, default_raster_view());
    }

    render::SceneRenderStats render_scene(scene::Scene& scene, const render::raster::ViewState& view) {
        render::SceneRenderStats stats = rendering_system_.render_raster(scene, view);
        if (&scene == active_scene_)
            active_scene_rendered_ = true;
        return stats;
    }

    render::SceneRenderStats engine_frame_render() {
        if (active_scene_ == nullptr || active_scene_rendered_)
            return {};
        scene::SceneCameraView camera{};
        if (!active_scene_->active_camera_view(render_target_aspect(), camera)) {
            draw_no_camera_notice();
            active_scene_rendered_ = true;
            return {};
        }
        render::raster::ViewState view{};
        view.target = framebuffer_;
        view.view = camera.view;
        view.projection = camera.projection;
        view.tile_w = camera.tile_w;
        view.tile_h = camera.tile_h;
        return render_scene(*active_scene_, view);
    }

    void engine_frame_begin(FrameClear clear) noexcept {
        active_scene_rendered_ = false;
        if (active_scene_ && active_scene_->environment().clear_enabled())
            apply_environment_clear(active_scene_->environment());
        if (clear.color)
            render::clear_framebuffer_color(framebuffer_, clear.color_rgba8);
        if (clear.depth)
            render::clear_framebuffer_depth(framebuffer_);
    }

    editor::Mode engine_frame_update(f32 dt) noexcept {
        engine_frame_update_ran_ = true;
        engine_frame_ms_ = dt > 0.0f ? dt * 1000.0f : 1000.0f / 60.0f;
        record_engine_frame_ms(engine_frame_ms_);
        if (auto* input = platform::input()) {
            return editor::sample_update(*input, dt);
        }
        return editor::current_mode();
    }

    void engine_frame_post() noexcept {
        engine_frame_render();
        const render::FrameStats render_stats = render::frame_stats_snapshot();
        if (auto* input = platform::input()) {
            if (engine_frame_update_ran_) {
                editor::draw_frame_overlays(framebuffer_, make_engine_debug_hud(render_stats));
            } else {
                editor::frame_overlays(*input, framebuffer_, make_engine_overlay_stats(render_stats));
            }
        }
        render::reset_frame_stats();
        engine_frame_update_ran_ = false;
    }

    void engine_frame_post(const editor::FrameOverlayStats& stats) noexcept {
        engine_frame_render();
        editor::FrameOverlayStats reported = stats;
        reported.render_stats_valid = true;
        if (auto* input = platform::input()) {
            if (engine_frame_update_ran_) {
                editor::draw_frame_overlays(framebuffer_,
                                            editor::make_debug_hud_stats_with_render(
                                                engine_frame_ms_,
                                                engine_frame_ms_,
                                                reported));
            } else {
                editor::frame_overlays(*input, framebuffer_, reported);
            }
        }
        render::reset_frame_stats();
        engine_frame_update_ran_ = false;
    }

    void engine_frame_post(const ui::imm::DebugHudStats& hud) noexcept {
        engine_frame_render();
        if (!engine_frame_update_ran_) {
            if (auto* input = platform::input()) {
                editor::sample_update(*input, hud.frame_ms > 0.0f ? hud.frame_ms * 0.001f
                                                                  : 1.0f / 60.0f);
            }
        }
        editor::draw_frame_overlays(framebuffer_, hud);
        render::reset_frame_stats();
        engine_frame_update_ran_ = false;
    }

    void present() { window().present(framebuffer_); }

    [[nodiscard]] bool write_capture_if_requested(std::string_view log_name) const {
        if (args_.capture_out.empty())
            return true;
        const bool ok = render::write_png_rgba8_framebuffer(args_.capture_out.c_str(),
                                                            pixels_.data(),
                                                            framebuffer_.width,
                                                            framebuffer_.height);
        if (!ok) {
            PSY_LOG_ERROR("{}: failed to write capture to {}", log_name, args_.capture_out);
            return false;
        }
        PSY_LOG_INFO("{}: wrote capture to {}", log_name, args_.capture_out);
        return true;
    }

   private:
    void destroy() noexcept {
        if (window_) {
            platform::destroy_window(window_);
            window_ = nullptr;
        }
    }

    void move_from(WindowApp& other) noexcept {
        args_ = other.args_;
        window_ = other.window_;
        width_ = other.width_;
        height_ = other.height_;
        engine_frame_update_ran_ = other.engine_frame_update_ran_;
        engine_frame_ms_ = other.engine_frame_ms_;
        engine_frame_ms_ring_ = other.engine_frame_ms_ring_;
        engine_frame_ms_head_ = other.engine_frame_ms_head_;
        engine_frame_ms_count_ = other.engine_frame_ms_count_;
        active_scene_ = other.active_scene_;
        active_scene_rendered_ = other.active_scene_rendered_;
        has_default_camera_ = other.has_default_camera_;
        pixels_ = std::move(other.pixels_);
        depth_ = std::move(other.depth_);
        rendering_system_ = std::move(other.rendering_system_);
        framebuffer_ = other.framebuffer_;
        framebuffer_.pixels = reinterpret_cast<u8*>(pixels_.data());
        framebuffer_.depth = depth_.empty() ? nullptr : depth_.data();
        other.window_ = nullptr;
        other.engine_frame_update_ran_ = false;
        other.engine_frame_ms_ = 1000.0f / 60.0f;
        other.engine_frame_ms_ring_ = {};
        other.engine_frame_ms_head_ = 0;
        other.engine_frame_ms_count_ = 0;
        other.active_scene_ = nullptr;
        other.active_scene_rendered_ = false;
        other.has_default_camera_ = true;
        other.framebuffer_ = {};
    }

    AppArgs args_{};
    platform::Window* window_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool engine_frame_update_ran_ = false;
    f32 engine_frame_ms_ = 1000.0f / 60.0f;
    std::array<f32, 120> engine_frame_ms_ring_{};
    u32 engine_frame_ms_head_ = 0;
    u32 engine_frame_ms_count_ = 0;
    scene::Scene* active_scene_ = nullptr;
    bool active_scene_rendered_ = false;
    bool has_default_camera_ = true;
    std::vector<u32> pixels_;
    std::vector<u32> depth_;
    render::Framebuffer framebuffer_{};
    render::RenderingSystem rendering_system_{};

    void record_engine_frame_ms(f32 frame_ms) noexcept {
        engine_frame_ms_ring_[engine_frame_ms_head_] = frame_ms;
        engine_frame_ms_head_ =
            (engine_frame_ms_head_ + 1u) % static_cast<u32>(engine_frame_ms_ring_.size());
        if (engine_frame_ms_count_ < static_cast<u32>(engine_frame_ms_ring_.size()))
            ++engine_frame_ms_count_;
    }

    [[nodiscard]] f32 average_engine_frame_ms() const noexcept {
        if (engine_frame_ms_count_ == 0u)
            return engine_frame_ms_;
        f32 sum = 0.0f;
        for (u32 i = 0; i < engine_frame_ms_count_; ++i)
            sum += engine_frame_ms_ring_[i];
        return sum / static_cast<f32>(engine_frame_ms_count_);
    }

    [[nodiscard]] f32 render_target_aspect() const noexcept {
        return height_ == 0u ? 1.0f : static_cast<f32>(width_) / static_cast<f32>(height_);
    }

    void apply_environment_clear(const scene::Environment& environment) noexcept {
        const scene::EnvironmentSettings& settings = environment.settings();
        if (settings.clear_color)
            render::clear_framebuffer_color(framebuffer_, settings.clear_color_rgba8);
        if (settings.clear_depth)
            render::clear_framebuffer_depth(framebuffer_);
    }

    void draw_no_camera_notice() noexcept {
        if (framebuffer_.width == 0u || framebuffer_.height == 0u || framebuffer_.pixels == nullptr)
            return;

        constexpr std::string_view kText = "No Camera Rendering";
        constexpr f32 kCellW = 6.0f;
        constexpr f32 kCellH = 8.0f;
        const f32 text_w = static_cast<f32>(kText.size()) * kCellW;
        const f32 panel_w = text_w + 16.0f;
        const f32 panel_h = kCellH + 12.0f;
        const f32 x = (static_cast<f32>(framebuffer_.width) - panel_w) * 0.5f;
        const f32 y = (static_cast<f32>(framebuffer_.height) - panel_h) * 0.5f;

        ui::imm::begin_frame(framebuffer_);
        ui::imm::filled_rect(math::Vec2{x, y},
                             math::Vec2{panel_w, panel_h},
                             ui::imm::rgba(0x0B, 0x10, 0x18));
        ui::imm::rect_outline(math::Vec2{x, y},
                              math::Vec2{panel_w, panel_h},
                              ui::imm::rgba(0x71, 0x82, 0x99));
        ui::imm::label(math::Vec2{x + 8.0f, y + 6.0f},
                       kText,
                       ui::imm::rgba(0xFF, 0xD2, 0x66));
        ui::imm::end_frame();
    }

    [[nodiscard]] static editor::FrameOverlayStats make_engine_overlay_stats(
        const render::FrameStats& stats) noexcept {
        editor::FrameOverlayStats out{};
        out.draw_calls = stats.raster_draws;
        out.triangles = stats.raster_triangles;
        out.active_voices = 0u;
        out.rt_tiles = stats.rt_tiles;
        out.rt_jobs = stats.rt_jobs;
        out.raster_stats_valid = stats.raster_reported;
        out.rt_stats_valid = stats.rt_reported;
        out.render_stats_valid = stats.has_render_report();
        return out;
    }

    [[nodiscard]] ui::imm::DebugHudStats make_engine_debug_hud(
        const render::FrameStats& stats) const noexcept {
        return editor::make_debug_hud_stats(engine_frame_ms_,
                                            average_engine_frame_ms(),
                                            make_engine_overlay_stats(stats));
    }
};

enum class FrameAction {
    Continue,
    Exit,
};

template <class ArgsT = AppArgs>
struct WindowFrameContextT {
    WindowApp& app;
    platform::Window& window;
    render::Framebuffer& framebuffer;
    const ArgsT& args;
    u32 frame_index = 0;
    f64 seconds = 0.0;
};

using WindowFrameContext = WindowFrameContextT<AppArgs>;

namespace detail {

template <class Sample>
auto parse_sample_args(int argc, char** argv) {
    if constexpr (requires { Sample::parse_args(argc, argv); }) {
        return Sample::parse_args(argc, argv);
    } else {
        return parse_common_args(argc, argv).args;
    }
}

template <class Sample>
std::string_view sample_log_name(const Sample& sample) noexcept {
    if constexpr (requires { sample.log_name(); }) {
        return sample.log_name();
    } else if constexpr (requires { Sample::log_name(); }) {
        return Sample::log_name();
    } else if constexpr (requires { std::string_view{Sample::log_name}; }) {
        return std::string_view{Sample::log_name};
    } else {
        return "app";
    }
}

template <class Sample>
std::string_view sample_display_name(const Sample& sample) noexcept {
    if constexpr (requires { sample.display_name(); }) {
        return sample.display_name();
    } else if constexpr (requires { Sample::display_name(); }) {
        return Sample::display_name();
    } else if constexpr (requires { std::string_view{Sample::display_name}; }) {
        return std::string_view{Sample::display_name};
    } else {
        return sample_log_name(sample);
    }
}

template <class Sample, class ArgsT>
WindowAppOptions sample_window_options(const Sample& sample, const ArgsT& args) noexcept {
    if constexpr (requires { sample.window_options(args); }) {
        return sample.window_options(args);
    } else if constexpr (requires { Sample::window_options(args); }) {
        return Sample::window_options(args);
    } else {
        return {};
    }
}

template <class Sample, class ArgsT>
platform::WindowDesc sample_window_desc(const Sample& sample, const ArgsT& args) {
    if constexpr (requires { sample.window_desc(args); }) {
        return sample.window_desc(args);
    } else if constexpr (requires { Sample::window_desc(args); }) {
        return Sample::window_desc(args);
    } else {
        return default_window_desc(sample_display_name(sample));
    }
}

template <class Sample>
void sample_started(Sample& sample, WindowApp& app) {
    if constexpr (requires { sample.started(app); }) {
        sample.started(app);
    }
}

template <class Sample>
void sample_stopped(Sample& sample, WindowApp& app) {
    if constexpr (requires { sample.stopped(app); }) {
        sample.stopped(app);
    }
}

template <class Sample, class ArgsT>
void run_sample_frame_begin(Sample& sample, WindowFrameContextT<ArgsT>& ctx) {
    if constexpr (requires { sample.frame_begin(ctx); }) {
        sample.frame_begin(ctx);
    }
}

template <class Sample, class ArgsT>
FrameAction run_sample_frame(Sample& sample, WindowFrameContextT<ArgsT>& ctx) {
    if constexpr (requires { sample.frame(ctx); }) {
        if constexpr (requires {
                          { sample.frame(ctx) } -> std::same_as<FrameAction>;
                      }) {
            return sample.frame(ctx);
        } else {
            sample.frame(ctx);
            return FrameAction::Continue;
        }
    } else {
        return FrameAction::Continue;
    }
}

template <class Sample, class ArgsT>
FrameClear sample_frame_clear(Sample& sample, const WindowFrameContextT<ArgsT>& ctx) noexcept {
    if constexpr (requires { sample.frame_clear(ctx); }) {
        return sample.frame_clear(ctx);
    } else if constexpr (requires { Sample::frame_clear(ctx); }) {
        return Sample::frame_clear(ctx);
    } else if constexpr (requires { sample.frame_clear(); }) {
        return sample.frame_clear();
    } else if constexpr (requires { Sample::frame_clear(); }) {
        return Sample::frame_clear();
    } else if constexpr (requires {
                             { Sample::frame_clear } -> std::convertible_to<FrameClear>;
                         }) {
        return Sample::frame_clear;
    } else {
        (void)sample;
        (void)ctx;
        return FrameClear::none();
    }
}

template <class Sample>
constexpr bool engine_frame_post_enabled() noexcept {
    if constexpr (requires {
                      { Sample::engine_frame_post_enabled() } -> std::convertible_to<bool>;
                  }) {
        return Sample::engine_frame_post_enabled();
    } else if constexpr (requires {
                             { Sample::engine_frame_post_enabled } -> std::convertible_to<bool>;
                         }) {
        return Sample::engine_frame_post_enabled;
    } else {
        return true;
    }
}

template <class Sample, class ArgsT>
void run_engine_frame_post(Sample& sample, WindowFrameContextT<ArgsT>& ctx) {
    if constexpr (requires { sample.frame_post(ctx); }) {
        sample.frame_post(ctx);
    } else if constexpr (engine_frame_post_enabled<Sample>()) {
        ctx.app.engine_frame_post();
    }
}

inline void mount_directory_if_present(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        asset::Vault::Get().mount_directory(path.string());
}

inline void mount_archive_if_present(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
        asset::Vault::Get().mount_vault(path.string());
}

template <class Sample>
std::string_view sample_asset_root(const Sample&) {
    if constexpr (requires {
                      { Sample::asset_root() } -> std::convertible_to<std::string_view>;
                  }) {
        return Sample::asset_root();
    } else if constexpr (requires {
                             { Sample::asset_root } -> std::convertible_to<std::string_view>;
                         }) {
        return Sample::asset_root;
    } else {
        return {};
    }
}

template <class Sample>
void mount_standard_asset_roots(const Sample& sample) {
    namespace fs = std::filesystem;

    const auto mount_archives = [](const fs::path& root) {
        if (root.empty())
            return;
        mount_archive_if_present(root / "psynder.psyvault");
        mount_archive_if_present(root / "assets.psyvault");
    };

    const fs::path exe_path{platform::executable_path()};
    const fs::path exe_dir = exe_path.parent_path();
    mount_archives(exe_dir);
    mount_archives(fs::path{platform::current_working_directory()});

    const std::string_view root = sample_asset_root(sample);
    if (!root.empty()) {
        mount_directory_if_present(exe_dir);
#if defined(PSYNDER_SOURCE_DIR)
        mount_directory_if_present(fs::path{PSYNDER_SOURCE_DIR} / fs::path{std::string(root)});
#else
        mount_directory_if_present(fs::path{std::string(root)});
#endif
    }
}

}  // namespace detail

template <class Sample>
int run_window_sample(int argc, char** argv) {
    const auto args = detail::parse_sample_args<Sample>(argc, argv);
    Sample sample{};
    const std::string_view log_name = detail::sample_log_name(sample);
    const std::string_view display_name = detail::sample_display_name(sample);

    WindowApp app{args, detail::sample_window_desc(sample, args), detail::sample_window_options(sample, args)};
    if (!app) {
        PSY_LOG_ERROR("{}: failed to create window", log_name);
        return EXIT_FAILURE;
    }

    detail::mount_standard_asset_roots(sample);
    detail::sample_started(sample, app);

    if (args.smoke_frames > 0) {
        PSY_LOG_INFO("{} — smoke mode, {} frames", display_name, args.smoke_frames);
    } else {
        PSY_LOG_INFO("{} running", display_name);
    }

    if constexpr (requires { sample.run(app, args); }) {
        const int result = sample.run(app, args);
        detail::sample_stopped(sample, app);
        return result;
    }

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;
    while (!app.window().should_close()) {
        app.window().poll_events();

        WindowFrameContextT<std::decay_t<decltype(args)>> ctx{
            app,
            app.window(),
            app.framebuffer(),
            args,
            frame,
            args.smoke_frames > 0 ? static_cast<f64>(frame) * (1.0 / 60.0)
                                  : platform::Clock::seconds(platform::Clock::ticks_now() - t0),
        };

        detail::run_sample_frame_begin(sample, ctx);
        app.engine_frame_begin(detail::sample_frame_clear(sample, ctx));
        const FrameAction action = detail::run_sample_frame(sample, ctx);
        detail::run_engine_frame_post(sample, ctx);
        app.present();

        ++frame;
        if (action == FrameAction::Exit) {
            break;
        }
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("{}: smoke target reached ({}); exiting", log_name, args.smoke_frames);
            break;
        }
    }

    const bool capture_ok = app.write_capture_if_requested(log_name);
    detail::sample_stopped(sample, app);
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace psynder::app

#define PSYNDER_WINDOW_SAMPLE_MAIN(SampleType)                            \
    int main(int argc, char** argv) {                                     \
        return ::psynder::app::run_window_sample<SampleType>(argc, argv); \
    }
