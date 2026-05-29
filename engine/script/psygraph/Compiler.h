// SPDX-License-Identifier: MIT
// Psynder — PsyGraph compiler: Graph -> Program (flat bytecode). Lane 15 owns.
//
// Compilation is two-phase per event:
//   1. Validate the graph (structural + type checks). Any failure produces a
//      diagnostic and aborts — no partial program is emitted.
//   2. For each event node, walk the exec edges in execution order; for each
//      action/flow node encountered, recursively materialize its data inputs
//      (pure node sub-trees) into registers, then emit the node's op(s).
//
// Pure (data-only) nodes are evaluated on demand and memoized to a register so
// a value feeding two consumers is computed once. Exec flow is linearized;
// Branch emits a JumpIfFalse, Sequence emits its successors back to back.

#pragma once

#include "Bytecode.h"
#include "Graph.h"

#include <string>

namespace psynder::script::psygraph {

struct CompileResult {
    bool ok = false;
    Program program;
    std::string diagnostic;  // human-readable; empty on success
};

// Compile a graph into a runnable program. On validation failure, `ok` is
// false and `diagnostic` explains the first error (dangling exec pin, type
// mismatch, unknown node type, bad variable slot, cyclic pure data, ...).
CompileResult compile_graph(const Graph& graph);

// Validation only (no codegen). Returns empty string on success, otherwise the
// first error message. Exposed so the editor can lint a graph live.
std::string validate_graph(const Graph& graph);

}  // namespace psynder::script::psygraph
