// SPDX-License-Identifier: MIT
// Psynder editor - entity hierarchy panel for Arcade sessions.

import React from 'react';

import { get_client } from '../ipc/client';
import type { ConnectionState } from '../ipc/client';
import type { ConsoleEval, ConsoleResult, Envelope, SelectionState } from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

type CommandStatus = 'idle' | 'busy' | 'ok' | 'error';

interface HierarchyNode {
    id: number;
    label: string;
    kind: 'root' | 'group' | 'camera' | 'light' | 'entity' | 'spawn';
    visible?: boolean;
    children?: HierarchyNode[];
}

interface SceneCommand {
    id: number;
    source: string;
    label: string;
}

const EXAMPLE_SCENES = [
    'assets/main.psyscene',
    'assets/crate_room.psyscene',
];

const BASE_TREE: HierarchyNode[] = [
    {
        id: 1,
        label: 'Arcade Scene',
        kind: 'root',
        children: [
            { id: 2, label: 'Camera', kind: 'camera', visible: true },
            {
                id: 10,
                label: 'Lighting',
                kind: 'group',
                children: [
                    { id: 11, label: 'key_light', kind: 'light', visible: true },
                    { id: 12, label: 'ambient_probe', kind: 'light', visible: true },
                ],
            },
            {
                id: 100,
                label: 'Props',
                kind: 'group',
                children: [
                    { id: 0x427, label: 'crate_red.01', kind: 'entity', visible: true },
                    { id: 0x428, label: 'crate_blue.01', kind: 'entity', visible: true },
                    { id: 0x429, label: 'barrel_metal.01', kind: 'entity', visible: true },
                ],
            },
            {
                id: 200,
                label: 'Behaviors',
                kind: 'group',
                children: [
                    { id: 201, label: 'crate_spin', kind: 'spawn', visible: true },
                ],
            },
        ],
    },
];

function node_icon(kind: HierarchyNode['kind']): string {
    switch (kind) {
        case 'root': return '@';
        case 'group': return '>';
        case 'camera': return 'C';
        case 'light': return 'L';
        case 'spawn': return '*';
        case 'entity': return '#';
    }
}

function flatten_ids(nodes: readonly HierarchyNode[], out = new Set<number>()): Set<number> {
    for (const node of nodes) {
        out.add(node.id);
        if (node.children) flatten_ids(node.children, out);
    }
    return out;
}

export function Hierarchy() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const seq = React.useRef(26000);
    const pending = React.useRef(new Map<number, SceneCommand>());
    const [connection, set_connection] = React.useState<ConnectionState>(client.current_state());
    const [status, set_status] = React.useState<CommandStatus>('idle');
    const [message, set_message] = React.useState('ready');
    const [scene_path, set_scene_path] = React.useState(EXAMPLE_SCENES[0]);
    const [selection, set_selection] = React.useState<SelectionState | null>(null);
    const [local_selected, set_local_selected] = React.useState<number | null>(0x427);
    const [collapsed, set_collapsed] = React.useState<Set<number>>(() => new Set());

    React.useEffect(() => {
        const unsub_state = client.on_state((state) => {
            set_connection(state);
            if (state === 'open') {
                client.send('console', 'subscribe', {});
                client.send('selection', 'subscribe', {});
            }
        });
        const unsub_console = client.subscribe('console', (env: Envelope) => {
            if (env.type !== 'result') return;
            const result = env.payload as ConsoleResult;
            const command = pending.current.get(result.id);
            if (!command) return;
            pending.current.delete(result.id);
            set_status(result.ok ? 'ok' : 'error');
            set_message(result.text || command.label);
        });
        const unsub_selection = client.subscribe('selection', (env: Envelope) => {
            if (env.type === 'state') {
                const next = env.payload as SelectionState;
                set_selection(next);
                set_local_selected(next.entity_id);
            } else if (env.type === 'cleared') {
                set_selection(null);
                set_local_selected(null);
            }
        });
        return () => {
            unsub_state();
            unsub_console();
            unsub_selection();
        };
    }, [client]);

    const send_command = React.useCallback((source: string, label: string) => {
        const id = ++seq.current;
        pending.current.set(id, { id, source, label });
        set_status('busy');
        set_message(label);
        if (connection === 'open') {
            client.send<ConsoleEval>('console', 'eval', { id, source, mode: 'console' });
        } else {
            pending.current.delete(id);
            set_status('error');
            set_message(`connection is ${connection}`);
        }
    }, [client, connection]);

    const tree = React.useMemo(() => {
        if (!selection || flatten_ids(BASE_TREE).has(selection.entity_id)) return BASE_TREE;
        return [
            ...BASE_TREE,
            {
                id: selection.entity_id,
                label: selection.entity_label || `entity_${selection.entity_id.toString(16)}`,
                kind: 'entity' as const,
                visible: true,
            },
        ];
    }, [selection]);

    const select_node = React.useCallback((node: HierarchyNode) => {
        set_local_selected(node.id);
        client.send('selection', 'select', { entity_id: node.id });
    }, [client]);

    const toggle_node = React.useCallback((id: number) => {
        set_collapsed((prev) => {
            const next = new Set(prev);
            if (next.has(id)) next.delete(id);
            else next.add(id);
            return next;
        });
    }, []);

    const load_scene = React.useCallback(() => {
        const path = scene_path.trim();
        if (!path) {
            set_status('error');
            set_message('scene path required');
            return;
        }
        send_command(`arcade_load_scene ${path}`, `loading ${path}`);
    }, [scene_path, send_command]);

    return (
        <div className="psy-panel psy-hierarchy">
            <header className="psy-panel-header">
                <h2>Hierarchy</h2>
                <ConnectionBadge />
                <button
                    type="button"
                    className="psy-btn psy-btn-primary"
                    onClick={() => send_command('arcade_new_scene', 'creating blank scene')}
                >
                    new
                </button>
            </header>

            <div className="psy-hierarchy-toolbar">
                <input
                    className="psy-input"
                    value={scene_path}
                    onChange={(e) => set_scene_path(e.target.value)}
                    spellCheck={false}
                    aria-label="Cooked scene path"
                />
                <button type="button" className="psy-btn" onClick={load_scene}>
                    load
                </button>
            </div>

            <div className={`psy-hierarchy-status is-${status}`} role="status">
                <span className="psy-scene-status-dot" />
                <span>{message}</span>
            </div>

            <div className="psy-hierarchy-body" role="tree" aria-label="Scene hierarchy">
                {tree.map((node) => (
                    <HierarchyRow
                        key={node.id}
                        node={node}
                        depth={0}
                        selected={local_selected}
                        collapsed={collapsed}
                        on_select={select_node}
                        on_toggle={toggle_node}
                    />
                ))}
            </div>
        </div>
    );
}

function HierarchyRow({
    node,
    depth,
    selected,
    collapsed,
    on_select,
    on_toggle,
}: {
    node: HierarchyNode;
    depth: number;
    selected: number | null;
    collapsed: Set<number>;
    on_select(node: HierarchyNode): void;
    on_toggle(id: number): void;
}) {
    const has_children = !!node.children?.length;
    const is_collapsed = collapsed.has(node.id);
    const is_selected = selected === node.id;

    return (
        <React.Fragment>
            <div
                className={`psy-hierarchy-row ${is_selected ? 'is-selected' : ''}`}
                role="treeitem"
                aria-selected={is_selected}
                aria-expanded={has_children ? !is_collapsed : undefined}
                style={{ '--psy-depth': depth } as React.CSSProperties}
                onClick={() => on_select(node)}
            >
                <button
                    type="button"
                    className="psy-hierarchy-disclosure"
                    aria-label={is_collapsed ? `Expand ${node.label}` : `Collapse ${node.label}`}
                    disabled={!has_children}
                    onClick={(e) => {
                        e.stopPropagation();
                        if (has_children) on_toggle(node.id);
                    }}
                >
                    {has_children ? (is_collapsed ? '+' : '-') : ''}
                </button>
                <span className={`psy-hierarchy-icon is-${node.kind}`}>{node_icon(node.kind)}</span>
                <span className="psy-hierarchy-label">{node.label}</span>
                <span className="psy-hierarchy-id">{node.id.toString(16).toUpperCase()}</span>
                {node.visible !== undefined && (
                    <span className={`psy-hierarchy-vis ${node.visible ? 'is-on' : ''}`} />
                )}
            </div>
            {has_children && !is_collapsed && node.children!.map((child) => (
                <HierarchyRow
                    key={child.id}
                    node={child}
                    depth={depth + 1}
                    selected={selected}
                    collapsed={collapsed}
                    on_select={on_select}
                    on_toggle={on_toggle}
                />
            ))}
        </React.Fragment>
    );
}
