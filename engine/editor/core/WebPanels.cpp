// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge.

#include "WebPanels.h"

#include "core/console/Console.h"
#include "editor/ipc/Ipc.h"
#include "platform/Platform.h"

#include <span>
#include <string>
#include <string_view>

namespace psynder::editor {
namespace {

std::string trimmed_editor_web_url() {
    constexpr std::string_view kFallback = "http://127.0.0.1:7654";
    constexpr std::string_view kOldDevDefault = "http://127.0.0.1:5173";
    auto* cvar = console::Console::Get().FindCVar("editor_web_url");
    auto* dev_mode = console::Console::Get().FindCVar("editor_web_dev_mode");
    std::string out = cvar ? cvar->value : std::string{kFallback};
    while (!out.empty() && out.back() == '/')
        out.pop_back();
    if ((!dev_mode || !dev_mode->GetBool()) && out == kOldDevDefault)
        out = std::string{kFallback};
    return out.empty() ? std::string{kFallback} : out;
}

std::string editor_panel_url(std::string_view panel) {
    std::string url = trimmed_editor_web_url();
    url += "/panels/";
    url.append(panel.data(), panel.size());
    url += "?token=";
    url += ipc::Server::Get().session_token();
    return url;
}

bool start_editor_ipc(console::Output& out) {
    ipc::ServerDesc desc{};
    if (!ipc::Server::Get().start(desc)) {
        out.PrintLine("editor-ipc: failed to start server on 127.0.0.1:7654");
        return false;
    }
    out.PrintLine("editor-ipc: listening on 127.0.0.1:7654");
    out.FormatLine("editor-ipc: token {}", ipc::Server::Get().session_token());
    out.FormatLine("editor workbench: {}", editor_panel_url("workbench"));
    return true;
}

bool valid_editor_panel(std::string_view panel) noexcept {
    return panel == "workbench" || panel == "scene" || panel == "hierarchy" || panel == "console" ||
           panel == "inspector" || panel == "profiler" || panel == "assets" || panel == "props" ||
           panel == "psygraph";
}

void open_editor_panel(std::string_view panel, bool open_browser, console::Output& out) {
    if (!valid_editor_panel(panel)) {
        out.PrintLine(
            "editor-panel: expected workbench, hierarchy, scene, console, inspector, profiler, assets, props, or psygraph");
        return;
    }
    if (!start_editor_ipc(out))
        return;
    const std::string url = editor_panel_url(panel);
    out.FormatLine("editor-panel: {}", url);
    if (!open_browser)
        return;
    if (platform::open_external_url(url)) {
        out.FormatLine("editor-panel: opened {}", panel);
    } else {
        out.PrintLine("editor-panel: could not open browser; paste the URL above");
    }
}

}  // namespace

void ensure_web_panel_commands_registered() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    auto& console_ref = console::Console::Get();
    console_ref.RegisterCVar("editor_web_url",
                             "http://127.0.0.1:7654",
                             "Base URL for editor panels; set to Vite only while developing the web UI.",
                             console::CVarFlags::Archive);
    console_ref.RegisterCVar("editor_web_dev_mode",
                             "0",
                             "Honor editor_web_url for an external web dev server instead of the bundled panel.",
                             console::CVarFlags::Archive);
    console_ref.RegisterCommand("editor_ipc_start",
                                "Start the local editor IPC server and print panel URLs.",
                                [](std::span<const std::string_view>, console::Output& out) {
                                    (void)start_editor_ipc(out);
                                });
    console_ref.RegisterCommand("editor_console",
                                "Start editor IPC and open the docked GUI console workspace.",
                                [](std::span<const std::string_view>, console::Output& out) {
                                    open_editor_panel("workbench", true, out);
                                });
    console_ref.RegisterCommand(
        "editor_panel",
        "Start editor IPC and open an editor panel: workbench, hierarchy, scene, console, inspector, profiler, assets, props, psygraph.",
        [](std::span<const std::string_view> args, console::Output& out) {
            const std::string_view panel = args.empty() ? std::string_view{"console"} : args[0];
            const bool open_browser = args.size() < 2u || args[1] != "noopen";
            open_editor_panel(panel, open_browser, out);
        });
    console_ref.RegisterCommand("editor_spawn_prop",
                                "Queue a prop spawn request from the web editor.",
                                [](std::span<const std::string_view> args, console::Output& out) {
                                    if (args.empty()) {
                                        out.PrintLine("editor_spawn_prop: expected prop id");
                                        return;
                                    }
                                    out.FormatLine("editor_spawn_prop: queued {}", args[0]);
                                });
}

void publish_web_profiler_frame(const WebProfilerFrame& frame) {
    auto& server = ipc::Server::Get();
    const bool has_profiler_subscribers = server.has_subscribers("profiler");
    const bool has_stats_subscribers = server.has_subscribers("stats");
    const bool has_perf_subscribers = server.has_subscribers("perf");
    if (!has_profiler_subscribers && !has_stats_subscribers && !has_perf_subscribers)
        return;

    server.broadcast_stats_tick(ipc::StatsTick{
        frame.frame_index,
        frame.cpu_ms,
        frame.render_ms,
        frame.draw_calls,
        frame.entities,
        frame.sections,
    });
}

void pump_web_panels() {
    ipc::Server::Get().pump();
}

}  // namespace psynder::editor
