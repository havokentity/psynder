// SPDX-License-Identifier: MIT
// Psynder script lane -- visual graph compiler.
//
// This is deliberately lane-internal. The public script VM contract remains
// Script.h; editor WebSocket clients can feed graph documents through the
// existing console REPL path using `:graph <json>`.

#pragma once

#include "core/Types.h"

#include <string>
#include <string_view>

namespace psynder::script::detail {

struct VisualCompileResult {
    bool ok = false;
    std::string lua_source;
    std::string diagnostic;
};

// Accepts:
//   :graph { "nodes": [...] }
//   :vscript { "nodes": [...] }
//   :visual { "nodes": [...] }
bool is_visual_graph_repl_command(std::string_view line) noexcept;

// Removes the leading REPL command and compiles the remaining graph payload.
VisualCompileResult compile_visual_graph_repl(std::string_view line);

// Compiles a graph document directly. Current wire shape:
// {
//   "nodes": [
//     {"id":"a", "op":"const", "value":1},
//     {"id":"b", "op":"const", "value":2},
//     {"id":"sum", "op":"add", "inputs":["a","b"]},
//     {"id":"out", "op":"log", "input":"sum"}
//   ],
//   "return": "sum"
// }
//
// Supported ops: const, add, sub, mul, div, neg, component, spawn, log,
// return. The output is ordinary Lua source and runs in the existing safe
// Lua VM with the current `world` bindings. Node inputs are resolved in
// document order, so a node may only reference ids from earlier nodes; the
// top-level "return" field is evaluated after all nodes have been compiled.
VisualCompileResult compile_visual_graph(std::string_view graph_json);

// File/source routing helpers. `.vsg` and `.psygraph` are visual graph
// documents; `.psy` is reserved for the native Psynder VM.
bool is_visual_graph_name(std::string_view name) noexcept;
bool has_visual_graph_marker(std::string_view source) noexcept;
std::string_view strip_visual_graph_marker(std::string_view source) noexcept;

}  // namespace psynder::script::detail
