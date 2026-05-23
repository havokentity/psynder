// SPDX-License-Identifier: MIT
// Psynder editor web-panel bridge.

#include "WebPanels.h"

#include "core/console/Console.h"
#include "editor/ipc/Ipc.h"
#include "platform/Platform.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::editor {
namespace {

class MsgpackWriter {
   public:
    void map_header(usize n) {
        if (n <= 15u) {
            put1(static_cast<u8>(0x80u | n));
        } else if (n <= 0xFFFFu) {
            put1(0xDEu);
            put_be16(static_cast<u16>(n));
        } else {
            put1(0xDFu);
            put_be32(static_cast<u32>(n));
        }
    }

    void array_header(usize n) {
        if (n <= 15u) {
            put1(static_cast<u8>(0x90u | n));
        } else if (n <= 0xFFFFu) {
            put1(0xDCu);
            put_be16(static_cast<u16>(n));
        } else {
            put1(0xDDu);
            put_be32(static_cast<u32>(n));
        }
    }

    void u32_(u32 v) {
        if (v <= 0x7Fu) {
            put1(static_cast<u8>(v));
        } else if (v <= 0xFFu) {
            put1(0xCCu);
            put1(static_cast<u8>(v));
        } else if (v <= 0xFFFFu) {
            put1(0xCDu);
            put_be16(static_cast<u16>(v));
        } else {
            put1(0xCEu);
            put_be32(v);
        }
    }

    void u64_(u64 v) {
        if (v <= 0xFFFFFFFFull) {
            u32_(static_cast<u32>(v));
        } else {
            put1(0xCFu);
            put_be64(v);
        }
    }

    void f32_(f32 v) {
        put1(0xCAu);
        u32 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        put_be32(bits);
    }

    void str(std::string_view s) {
        const usize n = s.size();
        if (n <= 31u) {
            put1(static_cast<u8>(0xA0u | n));
        } else if (n <= 0xFFu) {
            put1(0xD9u);
            put1(static_cast<u8>(n));
        } else if (n <= 0xFFFFu) {
            put1(0xDAu);
            put_be16(static_cast<u16>(n));
        } else {
            put1(0xDBu);
            put_be32(static_cast<u32>(n));
        }
        if (n > 0u) {
            const auto* bytes = reinterpret_cast<const u8*>(s.data());
            bytes_.insert(bytes_.end(), bytes, bytes + n);
        }
    }

    [[nodiscard]] const std::vector<u8>& bytes() const noexcept { return bytes_; }

   private:
    std::vector<u8> bytes_;

    void put1(u8 v) { bytes_.push_back(v); }
    void put_be16(u16 v) {
        bytes_.push_back(static_cast<u8>(v >> 8));
        bytes_.push_back(static_cast<u8>(v));
    }
    void put_be32(u32 v) {
        bytes_.push_back(static_cast<u8>(v >> 24));
        bytes_.push_back(static_cast<u8>(v >> 16));
        bytes_.push_back(static_cast<u8>(v >> 8));
        bytes_.push_back(static_cast<u8>(v));
    }
    void put_be64(u64 v) {
        put_be32(static_cast<u32>(v >> 32));
        put_be32(static_cast<u32>(v));
    }
};

std::vector<u8> encode_profiler_envelope(const WebProfilerFrame& frame) {
    constexpr u32 kEnvelopeVersion = 4;
    MsgpackWriter w;

    w.map_header(4);
    w.str("v");
    w.u32_(kEnvelopeVersion);
    w.str("ch");
    w.str("profiler");
    w.str("type");
    w.str("frame");
    w.str("payload");

    w.map_header(6);
    w.str("frame");
    w.u64_(frame.frame_index);
    w.str("cpu_ms");
    w.f32_(frame.cpu_ms);
    w.str("gpu_ms");
    w.f32_(0.0f);
    w.str("draw_calls");
    w.u32_(frame.draw_calls);
    w.str("entities");
    w.u32_(frame.entities);
    w.str("sections");
    w.array_header(frame.sections.size());
    for (const WebProfilerSection& section : frame.sections) {
        w.map_header(2);
        w.str("name");
        w.str(section.name);
        w.str("ms");
        w.f32_(section.ms);
    }

    return w.bytes();
}

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
    return panel == "workbench" || panel == "console" || panel == "inspector" || panel == "profiler" ||
           panel == "assets" || panel == "props" || panel == "psygraph";
}

void open_editor_panel(std::string_view panel, bool open_browser, console::Output& out) {
    if (!valid_editor_panel(panel)) {
        out.PrintLine("editor-panel: expected workbench, console, inspector, profiler, assets, props, or psygraph");
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
        "Start editor IPC and open an editor panel: workbench, console, inspector, profiler, assets, props, psygraph.",
        [](std::span<const std::string_view> args, console::Output& out) {
            const std::string_view panel = args.empty() ? std::string_view{"console"} : args[0];
            const bool open_browser = args.size() < 2u || args[1] != "noopen";
            open_editor_panel(panel, open_browser, out);
        });
}

void publish_web_profiler_frame(const WebProfilerFrame& frame) {
    auto& server = ipc::Server::Get();
    const bool has_profiler_subscribers = server.has_subscribers("profiler");
    const bool has_stats_subscribers = server.has_subscribers("stats");
    if (!has_profiler_subscribers && !has_stats_subscribers)
        return;

    const std::vector<u8> payload = encode_profiler_envelope(frame);
    const std::span<const u8> bytes(payload.data(), payload.size());
    server.broadcast(has_profiler_subscribers ? "profiler" : "stats", bytes);
}

void pump_web_panels() {
    ipc::Server::Get().pump();
}

}  // namespace psynder::editor
