// SPDX-License-Identifier: MIT
// Psynder — PsyGraph ECS binding template bodies. Lane 15 owns.
//
// `tick_psygraphs` is intentionally a serial, deterministic sweep: it snapshots
// the live entities once, then for each entity that carries a PsyGraphComponent
// it runs OnStart (lazily, once) then OnTick. A serial sweep (rather than the
// parallel `registry.query<...>` path) is chosen on purpose — graph actions
// fire host side effects whose ordering must be deterministic and which may
// touch shared runtime state, so we never run two instances concurrently.

#pragma once

#include <vector>

namespace psynder::script::psygraph {

template <class MakeHost>
void GraphRuntime::tick_psygraphs(scene::EcsRegistry& registry, MakeHost&& make_host, f64 dt) {
    // Snapshot live entities into a reused buffer. (This is the binding's only
    // bookkeeping allocation; it amortizes and is not part of the VM hot path.)
    const u32 total = registry.entity_count();
    if (total == 0)
        return;

    static thread_local std::vector<Entity> s_entities;
    s_entities.resize(total);
    const u32 got = registry.snapshot_live_entities(s_entities);
    const u32 count = got < total ? got : total;

    for (u32 i = 0; i < count; ++i) {
        const Entity e = s_entities[i];
        PsyGraphComponent* comp = registry.get<PsyGraphComponent>(e);
        if (!comp || comp->graph_id == kInvalidGraphId)
            continue;

        HostContext host = make_host(e, dt);
        host.self_entity = e.raw;
        host.delta_time = dt;

        if (!comp->started) {
            run_instance(*comp, EventKind::OnStart, host);
            comp->started = 1;
        }
        run_instance(*comp, EventKind::OnTick, host);
    }
}

}  // namespace psynder::script::psygraph
