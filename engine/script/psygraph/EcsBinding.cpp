// SPDX-License-Identifier: MIT
// Psynder — PsyGraph ECS binding (runtime). Lane 15 owns.

#include "EcsBinding.h"

#include <string>
#include <utility>

namespace psynder::script::psygraph {

GraphId GraphRuntime::register_graph(const Graph& graph, std::string* out_error) {
    CompileResult cr = compile_graph(graph);
    if (!cr.ok) {
        if (out_error)
            *out_error = cr.diagnostic;
        return kInvalidGraphId;
    }
    programs_.push_back(std::move(cr.program));
    return static_cast<GraphId>(programs_.size());  // 1-based; 0 = invalid
}

const Program* GraphRuntime::program(GraphId id) const noexcept {
    if (id == kInvalidGraphId || id > programs_.size())
        return nullptr;
    return &programs_[id - 1];
}

u32 GraphRuntime::create_instance(GraphId id) {
    const Program* prog = program(id);
    if (!prog)
        return 0xFFFFFFFFu;
    Instance inst;
    inst.graph = id;
    inst.state.reset_for(*prog);  // one-time per-instance allocation
    instances_.push_back(std::move(inst));
    return static_cast<u32>(instances_.size() - 1);
}

void GraphRuntime::run_instance(PsyGraphComponent& comp, EventKind event, HostContext& host) {
    if (comp.instance >= instances_.size())
        return;
    Instance& inst = instances_[comp.instance];
    const Program* prog = program(inst.graph);
    if (!prog)
        return;
    vm_.run(event, *prog, inst.state, host);
}

}  // namespace psynder::script::psygraph
