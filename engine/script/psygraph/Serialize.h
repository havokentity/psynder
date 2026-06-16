// SPDX-License-Identifier: MIT
// Psynder — PsyGraph graph serialization. Lane 15 owns.
//
// A graph round-trips to a compact little-endian binary blob so the editor can
// persist authored logic. The layout mirrors the engine's existing chunked
// binary style (see asset/LmpakFormat.h): a fixed header with a FourCC magic +
// version, then length-prefixed sections for nodes, edges, and the string
// pool. All multi-byte integers are stored little-endian; the writer/reader
// agree byte-for-byte so a save/load/save cycle is byte-stable.
//
//   +------------------+
//   | GraphBlobHeader  |  magic 'PSYG', version, variable_count,
//   |                  |  node_count, edge_count, string_count
//   +------------------+
//   | Nodes section    |  per node: type(u16), param_count(u16), params(u64*)
//   +------------------+
//   | Edges section    |  per edge: kind(u8), from_node(u32), to_node(u32),
//   |                  |            from_pin(u16), to_pin(u16)
//   +------------------+
//   | String pool      |  per string: len(u32) + raw bytes (no NUL)
//   +------------------+

#pragma once

#include "Graph.h"

#include "core/Types.h"

#include <span>
#include <string>
#include <vector>

namespace psynder::script::psygraph {

inline constexpr u32 kGraphBlobMagic = 0x47595350u;  // 'P','S','Y','G' little-endian
inline constexpr u32 kGraphBlobVersion = 1u;

// Serialize `graph` into `out` (appended). Deterministic and byte-stable.
void serialize_graph(const Graph& graph, std::vector<u8>& out);

// Parse a graph blob. Returns false (and leaves `out` unspecified) on a
// malformed / truncated / wrong-magic blob; `error` carries the reason.
bool deserialize_graph(std::span<const u8> blob, Graph& out, std::string& error);

}  // namespace psynder::script::psygraph
