// SPDX-License-Identifier: MIT
// Psynder editor — PsyGraph visual behavior authoring panel. Wave C keeps this
// offline/mock-friendly while the backend protocol catches up.

import React from 'react';

import { get_client } from '../ipc/client';
import type { Envelope, PsyGraphDocument, PsyGraphNode, PsyGraphValue } from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

function value_text(value: PsyGraphValue): string {
    if (Array.isArray(value)) return `[${value.map((x) => x.toFixed(2)).join(', ')}]`;
    if (typeof value === 'object') {
        if (value.type === 'linearIndex') return `linear_index(${value.base}, ${value.step})`;
        return `${value.value}`;
    }
    return String(value);
}

export function PsyGraph() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const [doc, set_doc] = React.useState<PsyGraphDocument | null>(null);
    const [selected_id, set_selected_id] = React.useState<string>('spin_crates');

    React.useEffect(() => {
        const unsub = client.subscribe('psygraph', (env: Envelope) => {
            if (env.type === 'document') {
                const next = env.payload as PsyGraphDocument;
                set_doc(next);
                set_selected_id((id) => next.nodes.some((n) => n.id === id) ? id : next.nodes[0]?.id ?? '');
            }
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') client.send('psygraph', 'subscribe', {});
        });
        return unsub;
    }, [client]);

    const selected = doc?.nodes.find((n) => n.id === selected_id) ?? null;

    return (
        <div className="psy-panel psy-graph">
            <header className="psy-panel-header">
                <h2>PsyGraph</h2>
                <ConnectionBadge />
                <button type="button" className="psy-btn psy-btn-primary" disabled>
                    compile
                </button>
                <button type="button" className="psy-btn psy-btn-ghost" disabled>
                    save
                </button>
            </header>

            {!doc ? (
                <div className="psy-empty">Waiting for graph document…</div>
            ) : (
                <div className="psy-graph-body">
                    <GraphCanvas doc={doc} selected_id={selected_id} on_select={set_selected_id} />
                    <Inspector doc={doc} node={selected} />
                </div>
            )}
        </div>
    );
}

function GraphCanvas({
    doc,
    selected_id,
    on_select,
}: {
    doc: PsyGraphDocument;
    selected_id: string;
    on_select(id: string): void;
}) {
    return (
        <div className="psy-graph-canvas" role="application" aria-label="PsyGraph canvas">
            <svg className="psy-graph-links" width="100%" height="100%">
                {doc.links.map((link) => {
                    const a = doc.nodes.find((n) => n.id === link.from_node);
                    const b = doc.nodes.find((n) => n.id === link.to_node);
                    if (!a || !b) return null;
                    const x0 = a.x + 180;
                    const y0 = a.y + 42;
                    const x1 = b.x;
                    const y1 = b.y + 42;
                    const mid = (x0 + x1) * 0.5;
                    return (
                        <path
                            key={link.id}
                            d={`M ${x0} ${y0} C ${mid} ${y0}, ${mid} ${y1}, ${x1} ${y1}`}
                            className="psy-graph-link"
                        />
                    );
                })}
            </svg>
            {doc.nodes.map((node) => (
                <button
                    key={node.id}
                    type="button"
                    className={`psy-graph-node ${selected_id === node.id ? 'is-selected' : ''}`}
                    style={{ left: node.x, top: node.y }}
                    onClick={() => on_select(node.id)}
                >
                    <span className="psy-graph-node-title">{node.title}</span>
                    <span className="psy-graph-node-op">{node.op}</span>
                    <PinList node={node} />
                </button>
            ))}
        </div>
    );
}

function PinList({ node }: { node: PsyGraphNode }) {
    const pins = [...node.inputs, ...node.outputs];
    if (pins.length === 0) return null;
    return (
        <span className="psy-graph-pins">
            {pins.slice(0, 4).map((pin) => (
                <span key={`${pin.id}-${pin.kind}`} className={`psy-graph-pin is-${pin.kind}`}>
                    {pin.label}
                </span>
            ))}
        </span>
    );
}

function Inspector({ doc, node }: { doc: PsyGraphDocument; node: PsyGraphNode | null }) {
    return (
        <aside className="psy-graph-inspector">
            <div className="psy-graph-doc">
                <span>{doc.name}</span>
                <code>{doc.source_path}</code>
                <code>target: {doc.target_group}</code>
            </div>
            {node ? (
                <>
                    <h3>{node.title}</h3>
                    <code className="psy-graph-op">{node.op}</code>
                    <dl className="psy-graph-values">
                        {Object.entries(node.values).map(([key, value]) => (
                            <React.Fragment key={key}>
                                <dt>{key}</dt>
                                <dd>{value_text(value)}</dd>
                            </React.Fragment>
                        ))}
                    </dl>
                </>
            ) : (
                <div className="psy-empty">No node selected.</div>
            )}
            <div className="psy-graph-diagnostics">
                {doc.diagnostics.map((d, i) => (
                    <div key={`${d.level}-${i}`} className={`psy-diag is-${d.level}`}>
                        {d.text}
                    </div>
                ))}
            </div>
        </aside>
    );
}
