// SPDX-License-Identifier: MIT
// Psynder — PsyGraph authoring data model. Lane 15 owns.
//
// The graph is the editor-facing, serializable source of truth. It is a flat
// list of nodes + a flat list of edges, deliberately structured so it can be
// round-tripped to a compact binary blob with no pointer chasing (edges refer
// to nodes / pins by index). This mirrors the node-graph documents used by
// Unreal Blueprints and Unity Visual Scripting, kept POD-friendly for our
// chunked-binary serializer.
//
// Two edge kinds exist, matching the Blueprints model:
//   * EXEC edges  — sequence control flow between exec pins (the white wires)
//   * DATA edges  — feed a producer's data-output pin into a consumer's
//                   data-input pin (the typed coloured wires)

#pragma once

#include "NodeTypes.h"

#include "core/Types.h"

#include <string>
#include <vector>

namespace psynder::script::psygraph {

// Index of a node within Graph::nodes. kInvalidNode marks "no node".
using NodeIndex = u32;
inline constexpr NodeIndex kInvalidNode = 0xFFFFFFFFu;

// A node instance. `params` are interpreted per NodeType (var slot for
// Get/SetVar, raw bit payload for literals, etc.). Param semantics:
//   LiteralFloat  -> params[0] reinterpreted as f64 bits
//   LiteralInt    -> params[0] reinterpreted as i64 bits
//   LiteralBool   -> params[0] (0/1)
//   LiteralString -> params[0] = string-pool index
//   Get/SetVar    -> params[0] = variable slot index
struct Node {
    NodeTypeId type = NodeTypeId::OnStart;
    std::vector<u64> params;
};

enum class EdgeKind : u8 {
    Exec = 0,
    Data = 1,
};

// A directed edge. For EXEC edges, `from_pin` is the index into the source
// node type's exec_out array and `to_pin` is ignored (exec-in is implicit
// pin 0). For DATA edges, `from_pin` indexes the source's data_out array and
// `to_pin` indexes the destination's data_in array.
struct Edge {
    EdgeKind kind = EdgeKind::Exec;
    NodeIndex from_node = kInvalidNode;
    NodeIndex to_node = kInvalidNode;
    u16 from_pin = 0;
    u16 to_pin = 0;
};

// The full authoring document.
//   * `nodes` / `edges` are the topology.
//   * `strings` is the shared string pool; LiteralString params and the Log
//     action reference entries by index.
//   * `variable_count` reserves the per-instance variable bank size; Get/SetVar
//     slot params must be < variable_count.
struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<std::string> strings;
    u32 variable_count = 0;

    // Intern a string into the pool, returning its index (deduplicated).
    u32 intern_string(std::string_view s);
};

}  // namespace psynder::script::psygraph
