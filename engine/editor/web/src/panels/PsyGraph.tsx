// SPDX-License-Identifier: MIT
// Psynder editor — PsyGraph NO-CODE visual-scripting authoring panel.
//
// This panel authors a REAL psygraph::Graph (the VM node model in
// engine/script/psygraph): place nodes from the catalog, wire exec + data ports,
// edit inline params. On "bind to entity" it serializes the graph to the engine
// blob format (psyGraphModel.serialize_graph — byte-identical to the C++
// Serialize.cpp) and ships it over the main-thread console-command IPC
// ("psygraph_set <entity> <hex>"); the engine validates + compiles it and stores
// it in the scene, so the authored graph is exactly what runs in Play.
//
// Wiring model (mirrors Blueprints / the C++ Edge model): click an OUTPUT pin
// then an INPUT pin of a compatible kind to create an edge. Exec pins connect to
// exec pins; data pins to data pins. Click a node to select + edit its param;
// Delete removes the selected node (and its edges).

import React from 'react';

import { get_client } from '../ipc/client';
import type { Envelope, SelectionState } from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';
import {
    EDGE_DATA,
    EDGE_EXEC,
    NODE_CATALOG,
    NodeTypeId,
    PinType,
    empty_graph,
    node_info,
    serialize_graph,
    to_hex,
} from './psyGraphModel';
import type { GraphDoc, GraphEdge, GraphNode, NodeTypeInfo } from './psyGraphModel';

const NODE_W = 168;
const HEADER_H = 26;
const PIN_H = 18;

type PinRef = {
    node_uid: number;
    side: 'out' | 'in';
    wire: 'exec' | 'data';
    index: number;     // pin index within that node's exec/data list of this side
    type: PinType;
};

// A "starter" graph so a freshly-opened panel shows a valid, runnable example:
// OnTick -> Branch(DeltaTime > 0.5) -> ApplyDamage(self, 5).
function starter_graph(): GraphDoc {
    const doc = empty_graph();
    const add = (type: NodeTypeId, x: number, y: number, param: number | string | boolean = 0): number => {
        const uid = doc.next_uid++;
        doc.nodes.push({ uid, type, x, y, param });
        return uid;
    };
    const edge = (kind: 0 | 1, fn: number, fp: number, tn: number, tp: number): void => {
        doc.edges.push({ uid: doc.next_uid++, kind, from_node: fn, from_pin: fp, to_node: tn, to_pin: tp });
    };
    const tick = add(NodeTypeId.OnTick, 24, 40);
    const half = add(NodeTypeId.LiteralFloat, 24, 200, 0.5);
    const cmp = add(NodeTypeId.Greater, 240, 120);
    const branch = add(NodeTypeId.Branch, 440, 60);
    const amount = add(NodeTypeId.LiteralFloat, 440, 240, 5);
    const dmg = add(NodeTypeId.ApplyDamage, 640, 80);
    edge(EDGE_DATA, tick, 0, cmp, 0);     // DeltaTime -> A
    edge(EDGE_DATA, half, 0, cmp, 1);     // 0.5 -> B
    edge(EDGE_DATA, cmp, 0, branch, 0);   // Greater -> Condition
    edge(EDGE_DATA, amount, 0, dmg, 1);   // 5 -> Amount (Target unconnected => self)
    edge(EDGE_EXEC, tick, 0, branch, 0);
    edge(EDGE_EXEC, branch, 0, dmg, 0);   // True -> ApplyDamage
    return doc;
}

function pin_type_compatible(producer: PinType, consumer: PinType): boolean {
    if (producer === consumer) return true;
    if (producer === PinType.Any || consumer === PinType.Any) return true;
    const num = (t: PinType) => t === PinType.Int || t === PinType.Float;
    return num(producer) && num(consumer);
}

function pin_color(type: PinType): string {
    switch (type) {
        case PinType.Exec: return '#e8e8e8';
        case PinType.Bool: return '#e05050';
        case PinType.Int: return '#50d0a0';
        case PinType.Float: return '#50a0e0';
        case PinType.Entity: return '#d0a050';
        case PinType.String: return '#c060d0';
        default: return '#999';
    }
}

// Geometry: where a given pin sits in panel coordinates.
function node_height(info: NodeTypeInfo): number {
    const left = info.execIn + info.dataIn.length;
    const right = info.execOut.length + info.dataOut.length;
    return HEADER_H + Math.max(left, right, 1) * PIN_H + 8;
}

function pin_y(info: NodeTypeInfo, side: 'in' | 'out', wire: 'exec' | 'data', index: number): number {
    // Exec pins listed first, then data pins, top-down.
    const row = wire === 'exec' ? index : (side === 'in' ? info.execIn : info.execOut.length) + index;
    return HEADER_H + row * PIN_H + PIN_H / 2;
}

function pin_x(node: GraphNode, side: 'in' | 'out'): number {
    return side === 'in' ? node.x : node.x + NODE_W;
}

export function PsyGraph() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const [doc, set_doc] = React.useState<GraphDoc>(() => starter_graph());
    const [selected_node, set_selected_node] = React.useState<number | null>(null);
    const [pending_pin, set_pending_pin] = React.useState<PinRef | null>(null);
    const [entity_id, set_entity_id] = React.useState<number | null>(null);
    const [status, set_status] = React.useState<string>('');
    const [palette_open, set_palette_open] = React.useState(false);
    const drag = React.useRef<{ uid: number; dx: number; dy: number } | null>(null);

    // Track the editor selection so we know which entity to bind the graph onto.
    React.useEffect(() => {
        const unsub = client.subscribe('selection', (env: Envelope) => {
            if (env.type === 'state') {
                const sel = env.payload as SelectionState;
                set_entity_id(typeof sel.entity_id === 'number' && sel.entity_id > 0 ? sel.entity_id : null);
            } else if (env.type === 'cleared') {
                set_entity_id(null);
            }
        });
        return unsub;
    }, [client]);

    // Echo console replies so the user sees the engine's psygraph_set ack/error.
    React.useEffect(() => {
        const unsub = client.subscribe('console', (env: Envelope) => {
            if (env.type !== 'result') return;
            const p = env.payload as { ok?: boolean; text?: string };
            if (typeof p.text === 'string' && p.text.includes('psygraph'))
                set_status(p.text);
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') client.send('selection', 'subscribe', {});
        });
        return unsub;
    }, [client]);

    const add_node = React.useCallback((type: NodeTypeId) => {
        set_doc((prev) => {
            const uid = prev.next_uid;
            const info = node_info(type);
            const param: number | string | boolean =
                info?.param === 'string' ? '' : info?.param === 'bool' ? false : 0;
            return {
                ...prev,
                nodes: [...prev.nodes, { uid, type, x: 60, y: 60, param }],
                next_uid: prev.next_uid + 1,
            };
        });
        set_palette_open(false);
    }, []);

    const delete_selected = React.useCallback(() => {
        if (selected_node == null) return;
        set_doc((prev) => ({
            ...prev,
            nodes: prev.nodes.filter((n) => n.uid !== selected_node),
            edges: prev.edges.filter((e) => e.from_node !== selected_node && e.to_node !== selected_node),
        }));
        set_selected_node(null);
    }, [selected_node]);

    const set_node_param = React.useCallback((uid: number, param: number | string | boolean) => {
        set_doc((prev) => ({
            ...prev,
            nodes: prev.nodes.map((n) => (n.uid === uid ? { ...n, param } : n)),
        }));
    }, []);

    const on_pin_click = React.useCallback((pin: PinRef) => {
        set_pending_pin((prev) => {
            if (!prev) {
                // Begin a wire only from an OUTPUT pin (matches the directed model).
                return pin.side === 'out' ? pin : null;
            }
            // Complete: prev is an output; pin must be an input of the same wire
            // kind and a compatible type, and not the same node.
            if (pin.side !== 'in' || pin.wire !== prev.wire || pin.node_uid === prev.node_uid) {
                return pin.side === 'out' ? pin : null;
            }
            if (prev.wire === 'data' && !pin_type_compatible(prev.type, pin.type)) {
                set_status('incompatible pin types');
                return null;
            }
            const new_edge: GraphEdge = {
                uid: -1,
                kind: prev.wire === 'exec' ? EDGE_EXEC : EDGE_DATA,
                from_node: prev.node_uid,
                from_pin: prev.index,
                to_node: pin.node_uid,
                to_pin: pin.index,
            };
            set_doc((d) => {
                // A data input takes a single producer; replace any existing edge
                // into the same input pin. Exec outputs likewise fan to one.
                const filtered = d.edges.filter((e) => {
                    if (prev.wire === 'data') return !(e.kind === EDGE_DATA && e.to_node === pin.node_uid && e.to_pin === pin.index);
                    return !(e.kind === EDGE_EXEC && e.from_node === prev.node_uid && e.from_pin === prev.index);
                });
                return { ...d, edges: [...filtered, { ...new_edge, uid: d.next_uid }], next_uid: d.next_uid + 1 };
            });
            return null;
        });
    }, []);

    const set_variable_count = React.useCallback((n: number) => {
        set_doc((prev) => ({ ...prev, variable_count: Math.max(0, Math.trunc(n) || 0) }));
    }, []);

    const bind_to_entity = React.useCallback(() => {
        if (entity_id == null) {
            set_status('select an entity in the Hierarchy first');
            return;
        }
        const { bytes } = serialize_graph(doc);
        const hex = to_hex(bytes);
        client.send('console', 'eval', { source: `psygraph_set ${entity_id} ${hex}`, mode: 'console', id: Date.now() & 0x7fffffff });
        set_status(`sent ${bytes.length}-byte graph to entity ${entity_id}…`);
    }, [client, doc, entity_id]);

    const clear_from_entity = React.useCallback(() => {
        if (entity_id == null) return;
        client.send('console', 'eval', { source: `psygraph_clear ${entity_id}`, mode: 'console', id: Date.now() & 0x7fffffff });
        set_status(`cleared graph from entity ${entity_id}…`);
    }, [client, entity_id]);

    // Node drag.
    const on_node_pointer_down = (ev: React.PointerEvent, node: GraphNode) => {
        if ((ev.target as HTMLElement).dataset.pin) return; // pin clicks handle wiring
        set_selected_node(node.uid);
        drag.current = { uid: node.uid, dx: ev.clientX - node.x, dy: ev.clientY - node.y };
        (ev.currentTarget as HTMLElement).setPointerCapture(ev.pointerId);
    };
    const on_node_pointer_move = (ev: React.PointerEvent) => {
        const d = drag.current;
        if (!d) return;
        const x = ev.clientX - d.dx;
        const y = ev.clientY - d.dy;
        set_doc((prev) => ({ ...prev, nodes: prev.nodes.map((n) => (n.uid === d.uid ? { ...n, x, y } : n)) }));
    };
    const on_node_pointer_up = () => { drag.current = null; };

    const selected = doc.nodes.find((n) => n.uid === selected_node) ?? null;

    return (
        <div className="psy-panel psy-graph" onKeyDown={(e) => { if (e.key === 'Delete' || e.key === 'Backspace') delete_selected(); }} tabIndex={0}>
            <header className="psy-panel-header">
                <h2>PsyGraph</h2>
                <ConnectionBadge />
                <button type="button" className="psy-btn psy-btn-ghost" onClick={() => set_palette_open((o) => !o)}>add node</button>
                <button type="button" className="psy-btn psy-btn-ghost" disabled={selected_node == null} onClick={delete_selected}>delete</button>
                <span className="psy-graph-target">
                    {entity_id != null ? `entity ${entity_id}` : 'no entity selected'}
                </span>
                <button type="button" className="psy-btn psy-btn-primary" disabled={entity_id == null} onClick={bind_to_entity}>bind to entity</button>
                <button type="button" className="psy-btn psy-btn-ghost" disabled={entity_id == null} onClick={clear_from_entity}>clear</button>
            </header>

            {palette_open && (
                <NodePalette on_pick={add_node} on_close={() => set_palette_open(false)} />
            )}

            <div className="psy-graph-body">
                <GraphCanvas
                    doc={doc}
                    selected_node={selected_node}
                    pending_pin={pending_pin}
                    on_pin_click={on_pin_click}
                    on_node_pointer_down={on_node_pointer_down}
                    on_node_pointer_move={on_node_pointer_move}
                    on_node_pointer_up={on_node_pointer_up}
                />
                <aside className="psy-graph-inspector">
                    <div className="psy-graph-doc">
                        <span>{doc.nodes.length} nodes · {doc.edges.length} edges</span>
                        <label className="psy-graph-field">
                            <span>variables</span>
                            <input className="psy-input psy-input-number" type="number" min={0} value={doc.variable_count}
                                onChange={(e) => set_variable_count(Number(e.target.value))} />
                        </label>
                    </div>
                    {selected ? (
                        <NodeInspector node={selected} on_change={(p) => set_node_param(selected.uid, p)} />
                    ) : (
                        <div className="psy-empty">Select a node to edit its value. Click an output pin then an input pin to wire.</div>
                    )}
                    {status && <div className="psy-graph-diagnostics"><div className="psy-diag is-info">{status}</div></div>}
                </aside>
            </div>
        </div>
    );
}

function NodePalette({ on_pick, on_close }: { on_pick(type: NodeTypeId): void; on_close(): void }) {
    const categories = Array.from(new Set(NODE_CATALOG.map((n) => n.category)));
    return (
        <section className="psy-graph-export" role="dialog" aria-label="Node palette">
            <header>
                <span>Add node</span>
                <button type="button" className="psy-btn psy-btn-ghost" onClick={on_close}>close</button>
            </header>
            <div className="psy-graph-palette">
                {categories.map((cat) => (
                    <div key={cat} className="psy-graph-palette-cat">
                        <strong>{cat}</strong>
                        {NODE_CATALOG.filter((n) => n.category === cat).map((n) => (
                            <button key={n.id} type="button" className="psy-btn psy-btn-ghost" onClick={() => on_pick(n.id)}>{n.name}</button>
                        ))}
                    </div>
                ))}
            </div>
        </section>
    );
}

function GraphCanvas(props: {
    doc: GraphDoc;
    selected_node: number | null;
    pending_pin: PinRef | null;
    on_pin_click(pin: PinRef): void;
    on_node_pointer_down(ev: React.PointerEvent, node: GraphNode): void;
    on_node_pointer_move(ev: React.PointerEvent): void;
    on_node_pointer_up(): void;
}) {
    const { doc } = props;
    const node_by_uid = new Map(doc.nodes.map((n) => [n.uid, n]));

    return (
        <div className="psy-graph-canvas" role="application" aria-label="PsyGraph canvas"
            onPointerMove={props.on_node_pointer_move} onPointerUp={props.on_node_pointer_up}>
            <svg className="psy-graph-links" width="100%" height="100%">
                {doc.edges.map((e) => {
                    const a = node_by_uid.get(e.from_node);
                    const b = node_by_uid.get(e.to_node);
                    if (!a || !b) return null;
                    const ai = node_info(a.type);
                    const bi = node_info(b.type);
                    if (!ai || !bi) return null;
                    const wire = e.kind === EDGE_EXEC ? 'exec' : 'data';
                    const x0 = pin_x(a, 'out');
                    const y0 = a.y + pin_y(ai, 'out', wire, e.from_pin);
                    const x1 = pin_x(b, 'in');
                    const y1 = b.y + pin_y(bi, 'in', wire, e.to_pin);
                    const mid = (x0 + x1) * 0.5;
                    const color = wire === 'exec' ? '#e8e8e8' : '#7090c0';
                    return (
                        <path key={e.uid} d={`M ${x0} ${y0} C ${mid} ${y0}, ${mid} ${y1}, ${x1} ${y1}`}
                            className="psy-graph-link" style={{ stroke: color }} fill="none" />
                    );
                })}
            </svg>
            {doc.nodes.map((node) => {
                const info = node_info(node.type);
                if (!info) return null;
                return (
                    <NodeBox key={node.uid} node={node} info={info}
                        selected={props.selected_node === node.uid}
                        pending={props.pending_pin}
                        on_pin_click={props.on_pin_click}
                        on_pointer_down={props.on_node_pointer_down} />
                );
            })}
        </div>
    );
}

function NodeBox(props: {
    node: GraphNode;
    info: NodeTypeInfo;
    selected: boolean;
    pending: PinRef | null;
    on_pin_click(pin: PinRef): void;
    on_pointer_down(ev: React.PointerEvent, node: GraphNode): void;
}) {
    const { node, info } = props;
    const height = node_height(info);
    const pin = (side: 'in' | 'out', wire: 'exec' | 'data', index: number, type: PinType, label: string) => {
        const top = pin_y(info, side, wire, index) - PIN_H / 2;
        const is_pending = props.pending != null && props.pending.node_uid === node.uid &&
            props.pending.side === side && props.pending.wire === wire && props.pending.index === index;
        return (
            <button key={`${side}-${wire}-${index}`} type="button" data-pin="1"
                className={`psy-graph-pin is-${side} ${is_pending ? 'is-pending' : ''}`}
                style={{ top, [side === 'in' ? 'left' : 'right']: -6, borderColor: pin_color(type) } as React.CSSProperties}
                title={`${label} (${PinType[type]})`}
                onClick={(e) => { e.stopPropagation(); props.on_pin_click({ node_uid: node.uid, side, wire, index, type }); }}>
                <span className="psy-graph-pin-dot" style={{ background: pin_color(type) }} />
                <span className="psy-graph-pin-label">{label}</span>
            </button>
        );
    };
    return (
        <div className={`psy-graph-node ${props.selected ? 'is-selected' : ''}`}
            style={{ left: node.x, top: node.y, width: NODE_W, height }}
            onPointerDown={(e) => props.on_pointer_down(e, node)}>
            <span className="psy-graph-node-title">{info.name}</span>
            {info.execIn > 0 && pin('in', 'exec', 0, PinType.Exec, 'exec')}
            {info.dataIn.map((p, i) => pin('in', 'data', i, p.type, p.name))}
            {info.execOut.map((p, i) => pin('out', 'exec', i, PinType.Exec, p.name))}
            {info.dataOut.map((p, i) => pin('out', 'data', i, p.type, p.name))}
        </div>
    );
}

function NodeInspector({ node, on_change }: { node: GraphNode; on_change(p: number | string | boolean): void }) {
    const info = node_info(node.type);
    if (!info) return null;
    return (
        <>
            <h3>{info.name}</h3>
            <code className="psy-graph-op">{info.category}</code>
            {info.param === 'none' ? (
                <div className="psy-empty">No editable value.</div>
            ) : info.param === 'bool' ? (
                <label className="psy-graph-field">
                    <span>value</span>
                    <input type="checkbox" checked={!!node.param} onChange={(e) => on_change(e.target.checked)} />
                </label>
            ) : info.param === 'string' ? (
                <label className="psy-graph-field">
                    <span>value</span>
                    <input className="psy-input" type="text" value={String(node.param ?? '')} onChange={(e) => on_change(e.target.value)} />
                </label>
            ) : (
                <label className="psy-graph-field">
                    <span>{info.param === 'varSlot' ? 'var slot' : 'value'}</span>
                    <input className="psy-input psy-input-number" type="number"
                        step={info.param === 'float' ? '0.01' : '1'}
                        value={Number(node.param ?? 0)}
                        onChange={(e) => on_change(Number(e.target.value))} />
                </label>
            )}
        </>
    );
}
