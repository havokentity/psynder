// SPDX-License-Identifier: MIT
// Psynder — PsyGraph ECS binding (thin). Lane 15 owns.
//
// This is the ONLY file in the module that touches the ECS. It is deliberately
// thin and sits behind the public API so the core (Graph/Compiler/Vm) stays
// testable with no ECS at all. The component is a POD `{graph_id, instance}`;
// the runtime owns the compiled programs + the per-instance VmState pool; the
// system entry runs the requested event for every entity carrying the
// component.
//
// Dependency note: this binding depends only on scene/EcsRegistry.h (core +
// math), NOT on the render-heavy scene/SceneEcs.h, so psygraph stays light.

#pragma once

#include "Bytecode.h"
#include "Compiler.h"
#include "Graph.h"
#include "Host.h"
#include "Vm.h"

#include "core/Types.h"
#include "scene/EcsRegistry.h"

#include <unordered_map>
#include <vector>

namespace psynder::script::psygraph {

// Stable id for a compiled graph asset, handed out by GraphRuntime::register_graph.
using GraphId = u32;
inline constexpr GraphId kInvalidGraphId = 0u;

// POD component: which compiled graph an entity runs, plus its own VmState
// slot index (into GraphRuntime's pool) so each entity has private variables.
PSYNDER_COMPONENT(PsyGraphComponent) {
    GraphId graph_id = kInvalidGraphId;
    u32 instance = 0xFFFFFFFFu;  // index into GraphRuntime instance pool; 0xFFFFFFFF = unbound
    u8 started = 0;              // 0 until OnStart has fired
};

// Owns compiled programs and the pooled per-instance VmStates. The runtime is
// constructed once by the embedding (editor play-runtime / arcade / test) and
// reused; binding an entity reserves a VmState slot up front so ticking is
// alloc-free.
class GraphRuntime {
   public:
    // Compile + register a graph. Returns kInvalidGraphId on compile failure
    // (the diagnostic is written to `out_error` when provided).
    GraphId register_graph(const Graph& graph, std::string* out_error = nullptr);

    const Program* program(GraphId id) const noexcept;

    // Reserve a VmState instance bound to a graph. Returns the instance index
    // to store in the component. This is where the (one-time) allocation for
    // an entity happens — never during tick.
    u32 create_instance(GraphId id);

    // Run a single event for one bound instance. Fires OnStart lazily.
    void run_instance(PsyGraphComponent& comp, EventKind event, HostContext& host);

    // ─── System entries (thin, optional) ───────────────────────────────────
    // Runs OnStart (once) + OnTick for every entity with a PsyGraphComponent.
    // `make_host` lets the caller build a per-entity HostContext (set self +
    // delta_time + hooks). Kept as a callback so this module never hard-wires
    // to gameplay; the editor/game supplies the host wiring.
    template <class MakeHost>
    void tick_psygraphs(scene::EcsRegistry & registry, MakeHost && make_host, f64 dt);

   private:
    struct Instance {
        GraphId graph = kInvalidGraphId;
        VmState state;
    };
    std::vector<Program> programs_;       // index = GraphId - 1
    std::vector<Instance> instances_;     // index = component.instance
    Vm vm_;
};

}  // namespace psynder::script::psygraph

#include "EcsBinding_Internal.h"
