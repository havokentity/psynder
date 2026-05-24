// SPDX-License-Identifier: MIT
// Psynder editor - entity hierarchy panel for Arcade sessions.

import React from 'react';

import { get_client } from '../ipc/client';
import type { ConnectionState } from '../ipc/client';
import type { ConsoleEval, ConsoleResult, Envelope, SceneDirtyState, SelectionState } from '../ipc/protocol';
import {
    DEFAULT_PRIMITIVE_MATERIAL_SELECTION,
    primitive_label,
    primitive_material_add_command,
    type PrimitiveMaterialSelection,
} from './PrimitiveMaterialPalette';
import { AddSceneMenu } from './AddSceneMenu';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';
import {
    editor_scene_dirty,
    last_scene_path,
    mark_editor_scene_dirty,
    recent_scene_paths,
    remember_scene_path,
    set_editor_scene_dirty,
    subscribe_editor_scene_dirty,
    subscribe_recent_scene_paths,
} from '../state/editorPersistence';

type CommandStatus = 'idle' | 'busy' | 'ok' | 'error';
type SceneCommandAction =
    | 'delete'
    | 'duplicate'
    | 'rename'
    | 'load'
    | 'save'
    | 'new'
    | 'add'
    | 'add-light'
    | 'light-kind'
    | 'reparent'
    | 'undo'
    | 'redo';
type LightKindValue = 0 | 1 | 2;

interface LightKindOption {
    value: LightKindValue;
    label: string;
    command_label: string;
}

interface HierarchyNode {
    id: number;
    parent?: number | null;
    label: string;
    kind: 'root' | 'group' | 'camera' | 'light' | 'entity' | 'spawn';
    visible?: boolean;
    component_count?: number;
    children?: HierarchyNode[];
}

interface SceneHierarchyPayload {
    entity_count?: number;
    nodes?: Array<{
        id?: number;
        parent?: number | null;
        label?: string;
        kind?: HierarchyNode['kind'];
        visible?: boolean;
        component_count?: number;
    }>;
}

interface SceneCommand {
    id: number;
    source: string;
    label: string;
    action?: SceneCommandAction;
    target_id?: number;
    path?: string;
    light_kind?: LightKindValue;
    quiet?: boolean;
}

interface PsySaveFilePickerOptions {
    suggestedName?: string;
    types?: Array<{
        description?: string;
        accept: Record<string, string[]>;
    }>;
    excludeAcceptAllOption?: boolean;
}

type PsySavePickerWindow = Window & {
    showSaveFilePicker?: (options?: PsySaveFilePickerOptions) => Promise<{ name?: string }>;
};

const EXAMPLE_SCENES = [
    'assets/main.psyscene',
    'assets/crate_room.psyscene',
];

const LIGHT_KIND_OPTIONS: LightKindOption[] = [
    { value: 0, label: 'Point', command_label: 'point light' },
    { value: 1, label: 'Directional', command_label: 'directional light' },
    { value: 2, label: 'Spot', command_label: 'spot light' },
];

const EMPTY_TREE: HierarchyNode[] = [
    {
        id: 0,
        label: 'No Scene Loaded',
        kind: 'root',
        visible: true,
        children: [],
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

function find_node_by_id(nodes: readonly HierarchyNode[], id: number): HierarchyNode | null {
    for (const node of nodes) {
        if (node.id === id) return node;
        const child = node.children ? find_node_by_id(node.children, id) : null;
        if (child) return child;
    }
    return null;
}

function is_placeholder_scene_root(node: HierarchyNode | null): boolean {
    return !!node && node.id === 0 && node.kind === 'root' && node.label === EMPTY_TREE[0].label;
}

function is_editable_scene_node(node: HierarchyNode | null): node is HierarchyNode {
    return !!node && node.kind !== 'root' && node.id !== 0 && !is_placeholder_scene_root(node);
}

function is_selectable_scene_node(node: HierarchyNode | null): node is HierarchyNode {
    return !!node && !is_placeholder_scene_root(node);
}

function console_arg(value: string): string {
    if (value && /^[A-Za-z0-9_./:@-]+$/.test(value)) return value;
    const escaped = value
        .replace(/\\/g, '\\\\')
        .replace(/"/g, '\\"')
        .replace(/\n/g, '\\n')
        .replace(/\t/g, '\\t')
        .replace(/\r/g, '\\n')
        .replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/g, ' ');
    return `"${escaped}"`;
}

function console_command(name: string, ...args: Array<string | number>): string {
    return [name, ...args.map((arg) => console_arg(String(arg)))].join(' ');
}

function scene_basename(path: string): string {
    const normalized = path.replace(/\\/g, '/');
    const slash = normalized.lastIndexOf('/');
    return slash >= 0 ? normalized.slice(slash + 1) : normalized;
}

function scene_dirname(path: string): string {
    const normalized = path.replace(/\\/g, '/');
    const slash = normalized.lastIndexOf('/');
    return slash >= 0 ? normalized.slice(0, slash + 1) : '';
}

function ensure_scene_extension(name: string): string {
    return name.toLowerCase().endsWith('.psyscene') ? name : `${name}.psyscene`;
}

function scene_path_with_basename(current_path: string, basename: string): string {
    return `${scene_dirname(current_path)}${ensure_scene_extension(basename)}`;
}

function added_entity_id(text: string): number | null {
    const match = /\badded\s+(\d+)\b/i.exec(text);
    if (!match) return null;
    const id = Number(match[1]);
    return Number.isFinite(id) ? id : null;
}

function light_kind_label(value: LightKindValue): LightKindOption {
    return LIGHT_KIND_OPTIONS.find((option) => option.value === value) ?? LIGHT_KIND_OPTIONS[0];
}

function is_noop_history_result(action: SceneCommandAction | undefined, text: string): boolean {
    if (action !== 'undo' && action !== 'redo') return false;
    return text.toLowerCase().includes(`nothing to ${action}`);
}

function scene_tree_from_payload(payload: SceneHierarchyPayload): HierarchyNode[] {
    const raw_nodes = Array.isArray(payload.nodes) ? payload.nodes : [];
    const nodes = raw_nodes
        .filter((node) => typeof node.id === 'number')
        .map((node): HierarchyNode => ({
            id: Number(node.id),
            parent: typeof node.parent === 'number' ? Number(node.parent) : null,
            label: node.label || (Number(node.id) === 0 ? 'Arcade Scene' : `Entity #${Number(node.id)}`),
            kind: node.kind || (Number(node.id) === 0 ? 'root' : 'entity'),
            visible: node.visible,
            component_count: typeof node.component_count === 'number' ? node.component_count : undefined,
            children: [],
        }));

    if (nodes.length === 0) return EMPTY_TREE;

    const by_id = new Map<number, HierarchyNode>();
    for (const node of nodes) by_id.set(node.id, node);

    const roots: HierarchyNode[] = [];
    for (const node of nodes) {
        if (node.id === 0 || node.parent == null || !by_id.has(node.parent)) {
            roots.push(node);
            continue;
        }
        const parent = by_id.get(node.parent);
        parent?.children?.push(node);
    }

    return roots.length > 0 ? roots : EMPTY_TREE;
}

export function Hierarchy() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const seq = React.useRef(26000);
    const pending = React.useRef(new Map<number, SceneCommand>());
    const [connection, set_connection] = React.useState<ConnectionState>(client.current_state());
    const [status, set_status] = React.useState<CommandStatus>('idle');
    const [message, set_message] = React.useState('ready');
    const [scene_path, set_scene_path] = React.useState(() => last_scene_path() ?? EXAMPLE_SCENES[0]);
    const [recent_paths, set_recent_paths] = React.useState<string[]>(() => recent_scene_paths());
    const [dirty, set_dirty] = React.useState(() => editor_scene_dirty());
    const [palette_selection, set_palette_selection] = React.useState<PrimitiveMaterialSelection>(
        DEFAULT_PRIMITIVE_MATERIAL_SELECTION,
    );
    const [light_kind, set_light_kind] = React.useState<LightKindValue>(0);
    const [selection, set_selection] = React.useState<SelectionState | null>(null);
    const [local_selected, set_local_selected] = React.useState<number | null>(null);
    const [live_tree, set_live_tree] = React.useState<HierarchyNode[] | null>(null);
    const [collapsed, set_collapsed] = React.useState<Set<number>>(() => new Set());
    const [renaming_id, set_renaming_id] = React.useState<number | null>(null);
    const [drag_over_id, set_drag_over_id] = React.useState<number | null>(null);

    React.useEffect(() => subscribe_editor_scene_dirty(set_dirty), []);
    React.useEffect(() => subscribe_recent_scene_paths(set_recent_paths), []);

    React.useEffect(() => {
        const unsub_state = client.on_state((state) => {
            set_connection(state);
            if (state === 'open') {
                client.send('console', 'subscribe', {});
                client.send('scene', 'subscribe', {});
                client.send('selection', 'subscribe', {});
            }
        });
        const unsub_console = client.subscribe('console', (env: Envelope) => {
            if (env.type !== 'result') return;
            const result = env.payload as ConsoleResult;
            const command = pending.current.get(result.id);
            if (!command) return;
            pending.current.delete(result.id);
            if (!command.quiet || !result.ok) set_status(result.ok ? 'ok' : 'error');
            const result_text = result.text || command.label;
            if (result.ok && command.action === 'save') {
                if (command.path) {
                    set_recent_paths(remember_scene_path(command.path));
                    set_editor_scene_dirty(false);
                }
                set_message(command.path ? `saved ${command.path}` : result_text);
            } else if (result.ok && command.action === 'load') {
                if (command.path) set_recent_paths(remember_scene_path(command.path));
                set_editor_scene_dirty(false);
                set_message(command.path ? `loaded ${command.path}` : result_text);
            } else if (result.ok && command.action === 'new') {
                set_editor_scene_dirty(false);
                set_message(result.text || 'blank scene ready');
            } else if (result.ok && command.action === 'add-light') {
                const light_option = light_kind_label(command.light_kind ?? 0);
                const entity_id = added_entity_id(result_text);
                if (entity_id !== null && command.light_kind) {
                    client.send<ConsoleEval>('console', 'eval', {
                        id: ++seq.current,
                        source: console_command('component_set', entity_id, 'LightComponent', 'kind', command.light_kind),
                        mode: 'console',
                        quiet: true,
                    });
                    set_message(`added ${light_option.command_label}`);
                } else if (entity_id === null && command.light_kind) {
                    set_status('error');
                    set_message(`added light, but could not resolve new entity id`);
                } else {
                    set_message(`added ${light_option.command_label}`);
                }
            } else if (!command.quiet || !result.ok) {
                set_message(result_text);
            }
            if (result.ok && command.action === 'delete' && command.target_id !== undefined) {
                set_local_selected((current) => current === command.target_id ? null : current);
                set_selection((current) => current?.entity_id === command.target_id ? null : current);
            }
            const history_changed = result.ok
                && (command.action === 'undo' || command.action === 'redo')
                && !is_noop_history_result(command.action, result_text);
            if (result.ok && (
                command.action === 'add'
                || command.action === 'add-light'
                || command.action === 'delete'
                || command.action === 'duplicate'
                || command.action === 'rename'
                || command.action === 'reparent'
                || history_changed
            )) {
                mark_editor_scene_dirty();
            }
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
        const unsub_scene = client.subscribe('scene', (env: Envelope) => {
            if (env.type === 'dirty_state') {
                const payload = env.payload as SceneDirtyState;
                set_editor_scene_dirty(!!payload.dirty);
                return;
            }
            if (env.type !== 'hierarchy') return;
            const payload = env.payload as SceneHierarchyPayload;
            const next_tree = scene_tree_from_payload(payload);
            const next_ids = flatten_ids(next_tree);
            set_live_tree(next_tree);
            set_local_selected((current) => {
                if (current == null || !next_ids.has(current)) return null;
                const node = find_node_by_id(next_tree, current);
                if (is_selectable_scene_node(node)) return current;
                return null;
            });
            set_status('ok');
            const count = Number(payload.entity_count ?? 0);
            set_message(count === 1 ? '1 entity' : `${count} entities`);
        });
        return () => {
            unsub_state();
            unsub_console();
            unsub_selection();
            unsub_scene();
        };
    }, [client]);

    const send_command = React.useCallback((
        source: string,
        label: string,
        meta: Pick<SceneCommand, 'action' | 'target_id' | 'path' | 'light_kind' | 'quiet'> = {},
    ) => {
        const id = ++seq.current;
        if (!meta.quiet) {
            pending.current.set(id, { id, source, label, ...meta });
            set_status('busy');
            set_message(label);
        }
        if (connection === 'open') {
            client.send<ConsoleEval>('console', 'eval', { id, source, mode: 'console', quiet: meta.quiet });
        } else {
            pending.current.delete(id);
            if (!meta.quiet) {
                set_status('error');
                set_message(`connection is ${connection}`);
            }
        }
    }, [client, connection]);

    const tree = React.useMemo(() => {
        const base = live_tree ?? EMPTY_TREE;
        if (!selection || flatten_ids(base).has(selection.entity_id)) return base;
        return [
            ...base,
            {
                id: selection.entity_id,
                label: selection.entity_label || `entity_${selection.entity_id.toString(16)}`,
                kind: 'entity' as const,
                visible: true,
            },
        ];
    }, [live_tree, selection]);

    const select_node = React.useCallback((node: HierarchyNode) => {
        if (!is_selectable_scene_node(node)) {
            set_local_selected(null);
            return;
        }
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

    const confirm_dirty_navigation = React.useCallback((verb: string) => {
        if (!dirty) return true;
        return window.confirm(`Discard unsaved scene changes and ${verb}?`);
    }, [dirty]);

    const load_scene = React.useCallback(() => {
        const path = scene_path.trim();
        if (!path) {
            set_status('error');
            set_message('scene path required');
            return;
        }
        if (!confirm_dirty_navigation('load another scene')) return;
        send_command(console_command('arcade_load_scene', path), `loading ${path}`, { action: 'load', path });
    }, [confirm_dirty_navigation, scene_path, send_command]);

    const save_scene_to_path = React.useCallback((next_path: string) => {
        const path = next_path.trim();
        if (!path) {
            set_status('error');
            set_message('scene save path required');
            return;
        }
        set_scene_path(path);
        send_command(console_command('scene_save', path), `saving ${path}`, { action: 'save', path });
    }, [send_command]);

    const save_scene = React.useCallback(() => {
        save_scene_to_path(scene_path);
    }, [save_scene_to_path, scene_path]);

    const save_scene_as = React.useCallback(async () => {
        const current_path = scene_path.trim() || EXAMPLE_SCENES[0];
        const current_name = ensure_scene_extension(scene_basename(current_path) || 'untitled.psyscene');
        let suggested_path = current_path;
        const picker = (window as PsySavePickerWindow).showSaveFilePicker;

        if (picker) {
            try {
                const handle = await picker({
                    suggestedName: current_name,
                    types: [
                        {
                            description: 'Psynder scene',
                            accept: { 'application/octet-stream': ['.psyscene'] },
                        },
                    ],
                    excludeAcceptAllOption: false,
                });
                if (handle.name) suggested_path = scene_path_with_basename(current_path, handle.name);
                set_message('picked file name; confirm engine path');
            } catch (err) {
                if (err instanceof DOMException && err.name === 'AbortError') return;
            }
        }

        const next_path = window.prompt('Save scene as engine path', suggested_path);
        if (next_path === null) return;
        save_scene_to_path(next_path);
    }, [save_scene_to_path, scene_path]);

    const add_primitive = React.useCallback((selection: PrimitiveMaterialSelection) => {
        const label = primitive_label(selection.primitive_kind).toLowerCase();
        send_command(
            primitive_material_add_command(selection, { include_material: true }),
            `adding ${label}`,
            { action: 'add' },
        );
    }, [send_command]);

    const add_empty_entity = React.useCallback(() => {
        send_command('entity_add_empty', 'adding empty entity', { action: 'add' });
    }, [send_command]);

    const add_camera = React.useCallback(() => {
        send_command('camera_add', 'adding camera', { action: 'add' });
    }, [send_command]);

    const add_light = React.useCallback(() => {
        const light_option = light_kind_label(light_kind);
        send_command('light_add', `adding ${light_option.command_label}`, {
            action: 'add-light',
            light_kind,
        });
    }, [light_kind, send_command]);

    const selected_node = React.useMemo(() => {
        return local_selected == null ? null : find_node_by_id(tree, local_selected);
    }, [local_selected, tree]);
    const scene_is_loaded = live_tree !== null && !is_placeholder_scene_root(live_tree[0] ?? null);
    const can_edit_selection = scene_is_loaded && is_editable_scene_node(selected_node);
    const action_title = can_edit_selection
        ? `Selected: ${selected_node.label}`
        : 'Select an entity to enable hierarchy actions';

    const create_new_scene = React.useCallback(() => {
        if (!confirm_dirty_navigation('create a new scene')) return;
        send_command('arcade_new_scene', 'creating blank scene', { action: 'new' });
    }, [confirm_dirty_navigation, send_command]);

    const begin_rename_selected = React.useCallback(() => {
        if (!selected_node || !can_edit_selection) return;
        set_renaming_id(selected_node.id);
    }, [can_edit_selection, selected_node]);

    const commit_rename = React.useCallback((node: HierarchyNode, next_name: string) => {
        const trimmed = next_name.trim();
        set_renaming_id(null);
        if (!is_editable_scene_node(node) || !trimmed || trimmed === node.label) return;
        send_command(
            console_command('entity_rename', node.id, trimmed),
            `renaming ${node.label}`,
            { action: 'rename', target_id: node.id },
        );
    }, [send_command]);

    const duplicate_selected = React.useCallback(() => {
        if (!selected_node || !can_edit_selection) return;
        send_command(
            console_command('entity_duplicate', selected_node.id),
            `duplicating ${selected_node.label}`,
            { action: 'duplicate', target_id: selected_node.id },
        );
    }, [can_edit_selection, selected_node, send_command]);

    const delete_selected = React.useCallback(() => {
        if (!selected_node || !can_edit_selection) return;
        send_command(
            console_command('entity_delete', selected_node.id),
            `deleting ${selected_node.label}`,
            { action: 'delete', target_id: selected_node.id },
        );
    }, [can_edit_selection, selected_node, send_command]);

    const reparent_entity = React.useCallback((child: HierarchyNode, parent: HierarchyNode) => {
        if (!is_editable_scene_node(child) || is_placeholder_scene_root(parent)) return;
        if (child.id === parent.id) return;
        const parent_id = parent.kind === 'root' ? 0 : parent.id;
        send_command(
            console_command('entity_reparent', child.id, parent_id),
            parent_id === 0 ? `moving ${child.label} to scene root` : `moving ${child.label} under ${parent.label}`,
            { action: 'reparent', target_id: child.id },
        );
    }, [send_command]);

    const undo_scene_edit = React.useCallback(() => {
        send_command('editor_undo', 'undoing scene edit', { action: 'undo' });
    }, [send_command]);

    const redo_scene_edit = React.useCallback(() => {
        send_command('editor_redo', 'redoing scene edit', { action: 'redo' });
    }, [send_command]);

    return (
        <div className="psy-panel psy-hierarchy">
            <header className="psy-panel-header">
                <h2>Hierarchy</h2>
                <ConnectionBadge />
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={undo_scene_edit}
                    disabled={connection !== 'open'}
                    title="Undo latest editor scene edit"
                >
                    undo
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={redo_scene_edit}
                    disabled={connection !== 'open'}
                    title="Redo latest editor scene edit"
                >
                    redo
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={duplicate_selected}
                    disabled={!can_edit_selection}
                    aria-label={selected_node ? `Duplicate ${selected_node.label}` : 'Duplicate selected entity'}
                    title={`Duplicate selected entity. ${action_title}`}
                >
                    dup
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={begin_rename_selected}
                    disabled={!can_edit_selection}
                    aria-label={selected_node ? `Rename ${selected_node.label}` : 'Rename selected entity'}
                    title={`Rename selected entity. ${action_title}`}
                >
                    rename
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-danger"
                    onClick={delete_selected}
                    disabled={!can_edit_selection}
                    aria-label={selected_node ? `Delete ${selected_node.label}` : 'Delete selected entity'}
                    title={`Delete selected entity. ${action_title}`}
                >
                    del
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-primary"
                    onClick={create_new_scene}
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
                <button type="button" className="psy-btn" onClick={save_scene}>
                    save
                </button>
                <button type="button" className="psy-btn" onClick={save_scene_as} title="Save scene under another engine path">
                    save as
                </button>
                <select
                    className="psy-input psy-light-kind-select"
                    value={String(light_kind)}
                    onChange={(e) => set_light_kind(Number(e.target.value) as LightKindValue)}
                    aria-label="Light variant for Add menu"
                    title="Light variant used by Add > Light"
                >
                    {LIGHT_KIND_OPTIONS.map((option) => (
                        <option key={option.value} value={String(option.value)}>
                            {option.label}
                        </option>
                    ))}
                </select>
                <AddSceneMenu
                    value={palette_selection}
                    on_change={set_palette_selection}
                    on_add_primitive={add_primitive}
                    on_add_empty_entity={add_empty_entity}
                    on_add_camera={add_camera}
                    on_add_light={add_light}
                />
            </div>

            <div className={`psy-hierarchy-status is-${status}`} role="status">
                <span className="psy-scene-status-dot" />
                <span>{message}</span>
                <span className={`psy-dirty-pill ${dirty ? 'is-dirty' : ''}`}>
                    {dirty ? 'modified' : 'saved'}
                </span>
            </div>

            {recent_paths.length > 0 && (
                <div className="psy-scene-recent psy-scene-recent-compact" aria-label="Recent scene paths">
                    {recent_paths.map((path) => (
                        <button
                            key={path}
                            type="button"
                            className="psy-scene-recent-row"
                            onClick={() => set_scene_path(path)}
                            title={path}
                        >
                            <span>recent</span>
                            <code>{path}</code>
                        </button>
                    ))}
                </div>
            )}

            <div className="psy-hierarchy-body" role="tree" aria-label="Scene hierarchy">
                {tree.map((node) => (
                    <HierarchyRow
                        key={node.id}
                        node={node}
                        depth={0}
                        selected={local_selected}
                        collapsed={collapsed}
                        renaming_id={renaming_id}
                        on_select={select_node}
                        on_toggle={toggle_node}
                        on_begin_rename={(target) => {
                            if (is_editable_scene_node(target)) set_renaming_id(target.id);
                        }}
                        on_commit_rename={commit_rename}
                        on_cancel_rename={() => set_renaming_id(null)}
                        drag_over_id={drag_over_id}
                        on_drag_over={set_drag_over_id}
                        on_reparent={reparent_entity}
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
    renaming_id,
    on_select,
    on_toggle,
    on_begin_rename,
    on_commit_rename,
    on_cancel_rename,
    drag_over_id,
    on_drag_over,
    on_reparent,
}: {
    node: HierarchyNode;
    depth: number;
    selected: number | null;
    collapsed: Set<number>;
    renaming_id: number | null;
    on_select(node: HierarchyNode): void;
    on_toggle(id: number): void;
    on_begin_rename(node: HierarchyNode): void;
    on_commit_rename(node: HierarchyNode, next_name: string): void;
    on_cancel_rename(): void;
    drag_over_id: number | null;
    on_drag_over(id: number | null): void;
    on_reparent(child: HierarchyNode, parent: HierarchyNode): void;
}) {
    const has_children = !!node.children?.length;
    const is_collapsed = collapsed.has(node.id);
    const is_selected = selected === node.id;
    const renaming = renaming_id === node.id;
    const draggable = is_editable_scene_node(node);
    const drop_target = is_selectable_scene_node(node);
    const is_drop_target = drag_over_id === node.id;

    return (
        <React.Fragment>
            <div
                className={`psy-hierarchy-row ${is_selected ? 'is-selected' : ''} ${is_drop_target ? 'is-drop-target' : ''}`}
                role="treeitem"
                aria-selected={is_selected}
                aria-expanded={has_children ? !is_collapsed : undefined}
                style={{ '--psy-depth': depth } as React.CSSProperties}
                draggable={draggable && !renaming}
                onDragStart={(e) => {
                    if (!draggable) return;
                    e.dataTransfer.setData('application/x-psynder-entity', String(node.id));
                    e.dataTransfer.effectAllowed = 'move';
                }}
                onDragOver={(e) => {
                    if (!drop_target) return;
                    e.preventDefault();
                    e.dataTransfer.dropEffect = 'move';
                    on_drag_over(node.id);
                }}
                onDragLeave={() => {
                    if (is_drop_target) on_drag_over(null);
                }}
                onDrop={(e) => {
                    if (!drop_target) return;
                    e.preventDefault();
                    on_drag_over(null);
                    const raw = e.dataTransfer.getData('application/x-psynder-entity');
                    const child_id = Number(raw);
                    if (!Number.isFinite(child_id)) return;
                    const child = find_node_by_id([node], child_id) ?? null;
                    if (child) {
                        on_reparent(child, node);
                        return;
                    }
                    on_reparent({ id: child_id, label: `Entity ${child_id}`, kind: 'entity' }, node);
                }}
                onClick={() => on_select(node)}
                onDoubleClick={() => on_begin_rename(node)}
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
                {renaming ? (
                    <InlineRenameInput
                        value={node.label}
                        on_commit={(next) => on_commit_rename(node, next)}
                        on_cancel={on_cancel_rename}
                    />
                ) : (
                    <span className="psy-hierarchy-label">{node.label}</span>
                )}
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
                    renaming_id={renaming_id}
                    on_select={on_select}
                    on_toggle={on_toggle}
                    on_begin_rename={on_begin_rename}
                    on_commit_rename={on_commit_rename}
                    on_cancel_rename={on_cancel_rename}
                    drag_over_id={drag_over_id}
                    on_drag_over={on_drag_over}
                    on_reparent={on_reparent}
                />
            ))}
        </React.Fragment>
    );
}

function InlineRenameInput({
    value,
    on_commit,
    on_cancel,
}: {
    value: string;
    on_commit(next: string): void;
    on_cancel(): void;
}) {
    const [draft, set_draft] = React.useState(value);
    const input_ref = React.useRef<HTMLInputElement | null>(null);
    const closed_ref = React.useRef(false);

    React.useEffect(() => {
        input_ref.current?.focus();
        input_ref.current?.select();
    }, []);

    return (
        <input
            ref={input_ref}
            className="psy-input psy-hierarchy-rename"
            value={draft}
            spellCheck={false}
            onChange={(e) => set_draft(e.target.value)}
            onBlur={() => {
                if (closed_ref.current) return;
                closed_ref.current = true;
                on_commit(draft);
            }}
            onClick={(e) => e.stopPropagation()}
            onDoubleClick={(e) => e.stopPropagation()}
            onKeyDown={(e) => {
                if (e.key === 'Enter') {
                    closed_ref.current = true;
                    on_commit(draft);
                }
                if (e.key === 'Escape') {
                    closed_ref.current = true;
                    on_cancel();
                }
            }}
            aria-label="Rename entity"
        />
    );
}
