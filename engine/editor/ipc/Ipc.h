// SPDX-License-Identifier: MIT
// Psynder — editor IPC server. Local WebSocket + HTTP on 127.0.0.1:7654;
// msgpack frame format. Lane 19 owns.

#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::editor::ipc {

struct ServerDesc {
    const char* bind_host = "127.0.0.1";
    u16 port = 7654;
    bool require_session_token = true;
};

struct StatsSection {
    std::string_view name;
    f32 ms = 0.0f;
};

struct StatsTick {
    u64 frame_index = 0;
    f32 cpu_ms = 0.0f;
    f32 render_ms = 0.0f;
    u32 draw_calls = 0;
    u32 entities = 0;
    std::span<const StatsSection> sections;
};

using SelectionSelectHandler = void (*)(u32 entity_id);

enum class SelectionComponentEditValueKind : u8 {
    Null,
    Bool,
    I64,
    U64,
    F64,
    String,
    BoolArray,
    F64Array,
    StringArray,
};

struct SelectionComponentEditValue {
    SelectionComponentEditValueKind kind = SelectionComponentEditValueKind::Null;
    bool bool_value = false;
    i64 i64_value = 0;
    u64 u64_value = 0;
    f64 f64_value = 0.0;
    std::string string_value;
    std::vector<u8> bool_values;
    std::vector<f64> f64_values;
    std::vector<std::string> string_values;
};

struct SelectionComponentEdit {
    u32 entity_id = 0;
    std::string component;
    std::string field;
    std::string field_kind;
    SelectionComponentEditValue value;
};

using SelectionComponentEditHandler = void (*)(const SelectionComponentEdit& edit);

class Server {
   public:
    static Server& Get();

    bool start(const ServerDesc& desc);
    void stop();

    // Push a state delta to all connected panels (one-directional engine→UI).
    void broadcast(std::string_view channel, std::span<const u8> msgpack_payload);
    void broadcast_stats_tick(const StatsTick& tick);
    void set_selection_select_handler(SelectionSelectHandler handler);
    void set_selection_component_edit_handler(SelectionComponentEditHandler handler);

    [[nodiscard]] bool has_subscribers(std::string_view channel) const;

    // Pump once per frame to dispatch any incoming command RPCs onto the
    // console queue.
    void pump();

    const std::string& session_token() const noexcept;
};

}  // namespace psynder::editor::ipc
