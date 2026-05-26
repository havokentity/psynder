// SPDX-License-Identifier: MIT
// Psynder editor — top-level router. Each panel is a separate Chrome window
// that hits one of `/panels/<name>` per DESIGN.md §10.6. In dev (vite) we
// use the `?panel=` query parameter to flip between them in one bundle.

import React from 'react';

import { AssetBrowser } from './panels/AssetBrowser';
import { Console } from './panels/Console';
import { Hierarchy } from './panels/Hierarchy';
import { Inspector } from './panels/Inspector';
import { Profiler } from './panels/Profiler';
import { PropSpawn } from './panels/PropSpawn';
import { PsyGraph } from './panels/PsyGraph';
import { get_client } from './ipc/client';
import type { ConnectionState } from './ipc/client';
import type { ConsoleEval, ConsoleResult, Envelope, SceneDirtyState } from './ipc/protocol';
import {
    editor_scene_dirty,
    last_scene_path,
    mark_editor_scene_dirty,
    remember_scene_path,
    set_editor_scene_dirty,
    subscribe_editor_scene_dirty,
    subscribe_recent_scene_paths,
} from './state/editorPersistence';
import {
    confirm_dirty_scene_navigation,
    DEFAULT_SCENE_PATH,
    ENGINE_SCENE_PATH_HELP,
    is_noop_history_result,
    load_scene_command,
    prompt_save_scene_path,
    save_scene_command,
    scene_load_failure_from_event,
} from './state/sceneCommands';

type PanelName = 'scene' | 'inspector' | 'console' | 'profiler' | 'assets' | 'props' | 'psygraph';
type RouteName = PanelName | 'workbench';
type ThemeName = 'forge' | 'field' | 'mono';
type DensityName = 'comfortable' | 'compact';
type SkinName = 'modern' | 'tactical';
type LayoutPreset = 'split' | 'stack' | 'quad' | 'single';
type LayoutName = LayoutPreset | 'custom';
type DockSlot = 'primary' | 'secondary' | 'tertiary' | 'quaternary';
type DockAxis = 'row' | 'column';
type DockDropZone = 'center' | 'left' | 'right' | 'top' | 'bottom';
type DockPath = readonly number[];
type DockLeaf = { kind: 'leaf'; panel: PanelName };
type DockSplit = {
    kind: 'split';
    axis: DockAxis;
    ratio: number;
    first: DockNode;
    second: DockNode;
};
type DockNode = DockLeaf | DockSplit;
type DockLayoutSnapshot = {
    layout: LayoutName;
    docks: Record<DockSlot, PanelName>;
    split: number;
    tree: DockNode;
    custom_tree: DockNode;
};
type DockUndo = { id: number; snapshot: DockLayoutSnapshot; message: string };
type SceneToolbarStatus = 'idle' | 'busy' | 'ok' | 'error';
type SceneToolbarAction = 'new' | 'load' | 'save' | 'undo' | 'redo';
type SceneToolbarCommand = {
    id: number;
    label: string;
    action: SceneToolbarAction;
    path?: string;
    acknowledged?: boolean;
};

const PANEL_NAMES: readonly PanelName[] = [
    'scene', 'inspector', 'console', 'profiler', 'assets', 'props', 'psygraph',
];
const LAYOUT_PRESETS: readonly LayoutPreset[] = ['split', 'stack', 'quad', 'single'];
const LAYOUT_NAMES: readonly LayoutName[] = [...LAYOUT_PRESETS, 'custom'];
const DOCK_SLOTS: readonly DockSlot[] = ['primary', 'secondary', 'tertiary', 'quaternary'];

const PANEL_META: Record<PanelName, { icon: string; label: string; hot: string }> = {
    scene:     { icon: 'H', label: 'Hierarchy', hot: 'tree' },
    inspector: { icon: 'I', label: 'Inspector', hot: 'sel' },
    console:   { icon: '>', label: 'Console', hot: 'repl' },
    profiler:  { icon: '~', label: 'Profiler', hot: 'fps' },
    assets:    { icon: '#', label: 'Assets', hot: 'vfs' },
    props:     { icon: '+', label: 'Props', hot: 'spawn' },
    psygraph:  { icon: '*', label: 'PsyGraph', hot: 'logic' },
};
const WORKBENCH_META = { icon: '=', label: 'Workbench', hot: 'dock' };
const DEFAULT_DOCKS: Record<DockSlot, PanelName> = {
    primary: 'scene',
    secondary: 'console',
    tertiary: 'inspector',
    quaternary: 'props',
};

function take_scene_toolbar_command(
    pending: Map<number, SceneToolbarCommand>,
    actions: readonly SceneToolbarAction[],
    require_acknowledged = true,
    request_id?: number,
): SceneToolbarCommand | null {
    if (request_id !== undefined) {
        const command = pending.get(request_id);
        if (command && actions.includes(command.action)) {
            pending.delete(request_id);
            return command;
        }
    }
    for (const [id, command] of pending) {
        if (!actions.includes(command.action)) continue;
        if (require_acknowledged && !command.acknowledged) continue;
        pending.delete(id);
        return command;
    }
    return null;
}

// Engine route paths map onto panel names; "assets" / "props" land on the
// `/panels/assets` and `/panels/props` URLs that the engine launches Chrome
// against — see DESIGN.md §10.6 / §10.8.
const PATH_TO_ROUTE: Record<string, RouteName> = {
    workbench: 'workbench',
    scene:     'scene',
    hierarchy: 'scene',
    inspector: 'inspector',
    console:   'console',
    profiler:  'profiler',
    assets:    'assets',
    props:     'props',
    psygraph:  'psygraph',
};

function pick_route(): RouteName {
    if (typeof window === 'undefined') return 'inspector';
    // Path-based first (engine routes via /panels/<name>), then query string.
    const m = window.location.pathname.match(/\/panels\/([a-z]+)/);
    if (m && m[1] in PATH_TO_ROUTE) return PATH_TO_ROUTE[m[1]];
    const qp = new URLSearchParams(window.location.search).get('panel');
    return safe_route(qp) ?? safe_route(window.localStorage.getItem('psy_last_route')) ?? 'inspector';
}

function safe_layout(value: string | null): LayoutName {
    return value && (LAYOUT_NAMES as readonly string[]).includes(value)
        ? value as LayoutName
        : 'split';
}

function safe_panel(value: string | null, fallback: PanelName): PanelName {
    return value && (PANEL_NAMES as readonly string[]).includes(value)
        ? value as PanelName
        : fallback;
}

function safe_route(value: string | null): RouteName | null {
    if (value === 'hierarchy') return 'scene';
    if (value === 'workbench' || (value && (PANEL_NAMES as readonly string[]).includes(value))) {
        return value as RouteName;
    }
    return null;
}

function is_text_edit_target(target: EventTarget | null): boolean {
    if (!(target instanceof HTMLElement)) return false;
    if (target.isContentEditable) return true;
    const tag = target.tagName.toLowerCase();
    return tag === 'input' || tag === 'textarea' || tag === 'select';
}

function safe_split(value: string | null): number {
    const parsed = Number(value);
    return Number.isFinite(parsed) ? Math.min(76, Math.max(24, parsed)) : 50;
}

function safe_ratio(value: number): number {
    return Number.isFinite(value) ? Math.min(82, Math.max(18, value)) : 50;
}

function dock_leaf(panel: PanelName): DockLeaf {
    return { kind: 'leaf', panel };
}

function is_dock_node(value: unknown): value is DockNode {
    if (!value || typeof value !== 'object') return false;
    const node = value as Record<string, unknown>;
    if (node.kind === 'leaf') {
        return typeof node.panel === 'string' &&
            (PANEL_NAMES as readonly string[]).includes(node.panel);
    }
    if (node.kind === 'split') {
        return (node.axis === 'row' || node.axis === 'column') &&
            typeof node.ratio === 'number' &&
            is_dock_node(node.first) &&
            is_dock_node(node.second);
    }
    return false;
}

function normalize_dock_node(node: DockNode): DockNode {
    if (node.kind === 'leaf') return dock_leaf(node.panel);
    return {
        kind: 'split',
        axis: node.axis,
        ratio: safe_ratio(node.ratio),
        first: normalize_dock_node(node.first),
        second: normalize_dock_node(node.second),
    };
}

function load_dock_tree(fallback: DockNode): DockNode {
    const stored = window.localStorage.getItem('psy_dock_tree_v1');
    if (!stored) return fallback;
    try {
        const parsed = JSON.parse(stored) as unknown;
        return is_dock_node(parsed) ? normalize_dock_node(parsed) : fallback;
    } catch {
        return fallback;
    }
}

function load_custom_dock_tree(fallback: DockNode): DockNode {
    const stored = window.localStorage.getItem('psy_dock_custom_tree_v1');
    if (!stored) return fallback;
    try {
        const parsed = JSON.parse(stored) as unknown;
        return is_dock_node(parsed) ? normalize_dock_node(parsed) : fallback;
    } catch {
        return fallback;
    }
}

function preset_tree(
    layout: LayoutPreset,
    docks: Record<DockSlot, PanelName>,
    split: number,
): DockNode {
    if (layout === 'single') return dock_leaf(docks.primary);
    if (layout === 'stack') {
        return {
            kind: 'split',
            axis: 'column',
            ratio: safe_ratio(split),
            first: dock_leaf(docks.primary),
            second: dock_leaf(docks.secondary),
        };
    }
    if (layout === 'quad') {
        return {
            kind: 'split',
            axis: 'column',
            ratio: 50,
            first: {
                kind: 'split',
                axis: 'row',
                ratio: 50,
                first: dock_leaf(docks.primary),
                second: dock_leaf(docks.secondary),
            },
            second: {
                kind: 'split',
                axis: 'row',
                ratio: 50,
                first: dock_leaf(docks.tertiary),
                second: dock_leaf(docks.quaternary),
            },
        };
    }
    return {
        kind: 'split',
        axis: 'row',
        ratio: safe_ratio(split),
        first: dock_leaf(docks.primary),
        second: dock_leaf(docks.secondary),
    };
}

function is_preset_ratio(value: number): boolean {
    return Math.abs(safe_ratio(value) - 50) < 0.5;
}

function layout_shape(node: DockNode): LayoutName {
    if (node.kind === 'leaf') return 'single';
    if (
        node.axis === 'row' &&
        is_preset_ratio(node.ratio) &&
        node.first.kind === 'leaf' &&
        node.second.kind === 'leaf'
    ) {
        return 'split';
    }
    if (
        node.axis === 'column' &&
        is_preset_ratio(node.ratio) &&
        node.first.kind === 'leaf' &&
        node.second.kind === 'leaf'
    ) {
        return 'stack';
    }
    if (
        node.axis === 'column' &&
        is_preset_ratio(node.ratio) &&
        node.first.kind === 'split' &&
        node.second.kind === 'split' &&
        node.first.axis === 'row' &&
        node.second.axis === 'row' &&
        is_preset_ratio(node.first.ratio) &&
        is_preset_ratio(node.second.ratio) &&
        node.first.first.kind === 'leaf' &&
        node.first.second.kind === 'leaf' &&
        node.second.first.kind === 'leaf' &&
        node.second.second.kind === 'leaf'
    ) {
        return 'quad';
    }
    return 'custom';
}

function update_dock_at(
    node: DockNode,
    path: DockPath,
    fn: (target: DockNode) => DockNode,
): DockNode {
    if (path.length === 0) return fn(node);
    if (node.kind !== 'split') return node;
    const [head, ...rest] = path;
    return {
        ...node,
        first: head === 0 ? update_dock_at(node.first, rest, fn) : node.first,
        second: head === 1 ? update_dock_at(node.second, rest, fn) : node.second,
    };
}

function get_dock_at(node: DockNode, path: DockPath): DockNode | null {
    if (path.length === 0) return node;
    if (node.kind !== 'split') return null;
    const [head, ...rest] = path;
    return get_dock_at(head === 0 ? node.first : node.second, rest);
}

function remove_dock_at(node: DockNode, path: DockPath): DockNode {
    if (path.length === 0 || node.kind !== 'split') return node;
    const [head, ...rest] = path;
    if (rest.length === 0) {
        return normalize_dock_node(head === 0 ? node.second : node.first);
    }
    if (head === 0) {
        return {
            ...node,
            first: remove_dock_at(node.first, rest),
        };
    }
    return {
        ...node,
        second: remove_dock_at(node.second, rest),
    };
}

function append_dock_panel(node: DockNode, panel: PanelName): DockNode {
    if (node.kind === 'leaf') {
        return {
            kind: 'split',
            axis: 'row',
            ratio: 50,
            first: normalize_dock_node(node),
            second: dock_leaf(panel),
        };
    }
    return {
        kind: 'split',
        axis: 'row',
        ratio: 68,
        first: normalize_dock_node(node),
        second: dock_leaf(panel),
    };
}

function count_dock_panels(node: DockNode, counts = new Map<PanelName, number>()): Map<PanelName, number> {
    if (node.kind === 'leaf') {
        counts.set(node.panel, (counts.get(node.panel) ?? 0) + 1);
        return counts;
    }
    count_dock_panels(node.first, counts);
    count_dock_panels(node.second, counts);
    return counts;
}

function collect_dock_leaf_paths(node: DockNode, prefix: DockPath = []): DockPath[] {
    if (node.kind === 'leaf') return [prefix];
    return [
        ...collect_dock_leaf_paths(node.first, [...prefix, 0]),
        ...collect_dock_leaf_paths(node.second, [...prefix, 1]),
    ];
}

function dock_path_key(path: DockPath): string {
    return path.length === 0 ? 'root' : path.join('-');
}

function same_path(a: DockPath, b: DockPath): boolean {
    return a.length === b.length && a.every((part, index) => part === b[index]);
}

function parse_dock_path(value: string): DockPath | null {
    if (!value) return null;
    try {
        const parsed = JSON.parse(value) as unknown;
        return Array.isArray(parsed) &&
            parsed.every((part) => part === 0 || part === 1)
            ? parsed
            : null;
    } catch {
        return null;
    }
}

function transparent_drag_image(): HTMLElement {
    const existing = document.getElementById('psy-transparent-drag-image');
    if (existing instanceof HTMLElement) return existing;
    const el = document.createElement('div');
    el.id = 'psy-transparent-drag-image';
    el.style.position = 'fixed';
    el.style.left = '-1000px';
    el.style.top = '-1000px';
    el.style.width = '1px';
    el.style.height = '1px';
    el.style.opacity = '0';
    el.style.pointerEvents = 'none';
    document.body.appendChild(el);
    return el;
}

function split_dock_leaf(target: DockNode, panel: PanelName, zone: DockDropZone): DockNode {
    if (zone === 'center' || target.kind !== 'leaf') return dock_leaf(panel);
    const axis: DockAxis = zone === 'left' || zone === 'right' ? 'row' : 'column';
    const incoming = dock_leaf(panel);
    const existing = dock_leaf(target.panel);
    const incoming_first = zone === 'left' || zone === 'top';
    return {
        kind: 'split',
        axis,
        ratio: 50,
        first: incoming_first ? incoming : existing,
        second: incoming_first ? existing : incoming,
    };
}

export function App() {
    const ipc_client = React.useMemo(() => get_client(), []);
    const command_seq = React.useRef(90000);
    const scene_pending = React.useRef(new Map<number, SceneToolbarCommand>());
    const saw_engine_connection = React.useRef(false);
    const [route, set_route] = React.useState<RouteName>(pick_route);
    const [connection, set_connection] = React.useState<ConnectionState>(ipc_client.current_state());
    const [scene_dirty, set_scene_dirty] = React.useState(() => editor_scene_dirty());
    const [scene_path, set_scene_path] = React.useState(() => last_scene_path() ?? DEFAULT_SCENE_PATH);
    const [scene_status, set_scene_status] = React.useState<SceneToolbarStatus>('idle');
    const [scene_message, set_scene_message] = React.useState('scene ready');
    const [settings_open, set_settings_open] = React.useState(false);
    const [theme, set_theme] = React.useState<ThemeName>(() => (
        (window.localStorage.getItem('psy_theme') as ThemeName | null) ?? 'forge'
    ));
    const [density, set_density] = React.useState<DensityName>(() => (
        (window.localStorage.getItem('psy_density') as DensityName | null) ?? 'comfortable'
    ));
    const [skin, set_skin] = React.useState<SkinName>(() => (
        (window.localStorage.getItem('psy_skin') as SkinName | null) ?? 'modern'
    ));
    const [layout, set_layout] = React.useState<LayoutName>(() => (
        safe_layout(window.localStorage.getItem('psy_dock_layout'))
    ));
    const [docks, set_docks] = React.useState<Record<DockSlot, PanelName>>(() => ({
        primary: safe_panel(window.localStorage.getItem('psy_dock_primary'), DEFAULT_DOCKS.primary),
        secondary: safe_panel(window.localStorage.getItem('psy_dock_secondary'), DEFAULT_DOCKS.secondary),
        tertiary: safe_panel(window.localStorage.getItem('psy_dock_tertiary'), DEFAULT_DOCKS.tertiary),
        quaternary: safe_panel(window.localStorage.getItem('psy_dock_quaternary'), DEFAULT_DOCKS.quaternary),
    }));
    const [dock_split, set_dock_split] = React.useState(() => (
        safe_split(window.localStorage.getItem('psy_dock_split'))
    ));
    const [dock_tree, set_dock_tree] = React.useState<DockNode>(() => (
        load_dock_tree(preset_tree(layout === 'custom' ? 'split' : layout, docks, dock_split))
    ));
    const [custom_dock_tree, set_custom_dock_tree] = React.useState<DockNode>(() => (
        load_custom_dock_tree(dock_tree)
    ));
    const dock_snapshot = React.useMemo<DockLayoutSnapshot>(() => ({
        layout,
        docks,
        split: dock_split,
        tree: dock_tree,
        custom_tree: custom_dock_tree,
    }), [custom_dock_tree, dock_split, dock_tree, docks, layout]);

    // In dev mode there's only a single bundle; let the user flip between
    // panels via the header without reloading. The engine doesn't drive
    // this — each Chrome window only ever shows one panel.
    React.useEffect(() => {
        const on_pop = () => set_route(pick_route());
        window.addEventListener('popstate', on_pop);
        return () => window.removeEventListener('popstate', on_pop);
    }, []);

    const switch_route = React.useCallback((name: RouteName) => {
        if (route === name) return;
        const url = new URL(window.location.href);
        url.searchParams.set('panel', name);
        // Keep history sane in dev — engine-launched windows already pin
        // a panel and won't see this code path.
        window.history.pushState({}, '', url.toString());
        set_route(name);
    }, [route]);

    React.useEffect(() => {
        const on_key = (e: KeyboardEvent) => {
            if (!(e.metaKey || e.ctrlKey) || e.altKey || is_text_edit_target(e.target)) return;
            if (e.key === '0') {
                e.preventDefault();
                switch_route('workbench');
                return;
            }
            const digit = Number(e.key);
            if (Number.isInteger(digit) && digit >= 1 && digit <= PANEL_NAMES.length) {
                e.preventDefault();
                switch_route(PANEL_NAMES[digit - 1]);
            }
        };
        window.addEventListener('keydown', on_key);
        return () => window.removeEventListener('keydown', on_key);
    }, [switch_route]);

    React.useEffect(() => {
        const unsub = ipc_client.on_state((state) => {
            set_connection(state);
            if (state === 'open') {
                saw_engine_connection.current = true;
                return;
            }
            if (!saw_engine_connection.current) return;
            if (state === 'reconnecting' || state === 'closed') {
                window.setTimeout(() => {
                    window.close();
                }, 120);
            }
        });
        return unsub;
    }, [ipc_client]);

    React.useEffect(() => subscribe_editor_scene_dirty(set_scene_dirty), []);

    React.useEffect(() => {
        const on_before_unload = (event: BeforeUnloadEvent) => {
            if (!editor_scene_dirty()) return;
            event.preventDefault();
            event.returnValue = '';
        };
        window.addEventListener('beforeunload', on_before_unload);
        return () => window.removeEventListener('beforeunload', on_before_unload);
    }, []);

    React.useEffect(() => subscribe_recent_scene_paths((paths) => {
        const latest = last_scene_path() ?? paths[0];
        if (latest) set_scene_path(latest);
    }), []);

    React.useEffect(() => {
        const unsub_state = ipc_client.on_state((state) => {
            if (state === 'open') {
                ipc_client.send('console', 'subscribe', {});
                ipc_client.send('scene', 'subscribe', {});
            }
        });
        const unsub_console = ipc_client.subscribe('console', (env: Envelope) => {
            if (env.type !== 'result') return;
            const result = env.payload as ConsoleResult;
            const command = scene_pending.current.get(result.id);
            if (!command) return;

            const result_text = result.text || command.label;
            if (!result.ok) {
                scene_pending.current.delete(result.id);
                set_scene_status('error');
                set_scene_message(result_text);
                return;
            }

            if (command.action === 'save') {
                command.acknowledged = true;
                set_scene_status('busy');
                set_scene_message(command.path ? `save accepted; waiting for clean scene ${command.path}` : result_text);
                return;
            }
            if (command.action === 'load') {
                command.acknowledged = true;
                set_scene_status('busy');
                set_scene_message(command.path ? `load accepted; waiting for scene ${command.path}` : result_text);
                return;
            }
            if (command.action === 'new') {
                command.acknowledged = true;
                set_scene_status('busy');
                set_scene_message(result.text || 'new scene accepted; waiting for scene');
                return;
            }

            scene_pending.current.delete(result.id);
            set_scene_status('ok');
            if (command.action === 'undo' || command.action === 'redo') {
                if (!is_noop_history_result(command.action, result_text)) mark_editor_scene_dirty();
                set_scene_message(result_text);
                return;
            }
            set_scene_message(result_text);
        });
        const unsub_scene = ipc_client.subscribe('scene', (env: Envelope) => {
            const load_failure = scene_load_failure_from_event(env.type, env.payload);
            if (load_failure) {
                const command = take_scene_toolbar_command(
                    scene_pending.current,
                    ['load'],
                    false,
                    load_failure.request_id,
                );
                if (command || load_failure.request_id === undefined) {
                    set_scene_status('error');
                    set_scene_message(load_failure.text);
                }
                return;
            }
            if (env.type === 'dirty_state') {
                const payload = env.payload as SceneDirtyState;
                const dirty = !!payload.dirty;
                set_editor_scene_dirty(dirty);
                if (!dirty) {
                    const command = take_scene_toolbar_command(scene_pending.current, ['save']);
                    if (command) {
                        if (command.path) {
                            remember_scene_path(command.path);
                            set_scene_path(command.path);
                        }
                        set_scene_status('ok');
                        set_scene_message(command.path ? `saved ${command.path}` : 'scene saved');
                    }
                }
                return;
            }
            if (env.type === 'hierarchy') {
                const command = take_scene_toolbar_command(scene_pending.current, ['load', 'new']);
                if (!command) return;
                if (command.path) {
                    remember_scene_path(command.path);
                    set_scene_path(command.path);
                }
                set_editor_scene_dirty(false);
                set_scene_status('ok');
                set_scene_message(command.action === 'new'
                    ? 'new scene ready'
                    : `opened ${command.path ?? 'scene'}`);
            }
        });
        return () => {
            unsub_state();
            unsub_console();
            unsub_scene();
        };
    }, [ipc_client]);

    React.useEffect(() => {
        window.localStorage.setItem('psy_theme', theme);
        window.localStorage.setItem('psy_density', density);
        window.localStorage.setItem('psy_skin', skin);
    }, [theme, density, skin]);

    React.useEffect(() => {
        window.localStorage.setItem('psy_last_route', route);
    }, [route]);

    React.useEffect(() => {
        window.localStorage.setItem('psy_dock_layout', layout);
    }, [layout]);

    React.useEffect(() => {
        for (const slot of DOCK_SLOTS) {
            window.localStorage.setItem(`psy_dock_${slot}`, docks[slot]);
        }
    }, [docks]);

    React.useEffect(() => {
        window.localStorage.setItem('psy_dock_split', String(dock_split));
    }, [dock_split]);

    React.useEffect(() => {
        window.localStorage.setItem('psy_dock_tree_v1', JSON.stringify(dock_tree));
    }, [dock_tree]);

    React.useEffect(() => {
        window.localStorage.setItem('psy_dock_custom_tree_v1', JSON.stringify(custom_dock_tree));
    }, [custom_dock_tree]);

    React.useEffect(() => {
        const shaped = layout_shape(dock_tree);
        if (shaped !== layout) {
            set_layout(shaped);
        }
        if (shaped === 'custom') {
            set_custom_dock_tree(dock_tree);
        }
    }, [dock_tree, layout]);

    const apply_layout_preset = (next_layout: LayoutName) => {
        if (next_layout === 'custom') {
            set_layout('custom');
            set_dock_tree(custom_dock_tree);
            return;
        }
        const next_split = 50;
        set_dock_split(next_split);
        set_layout(next_layout);
        set_dock_tree(preset_tree(next_layout, docks, next_split));
    };

    const reset_layout = () => {
        const next_split = 50;
        set_docks(DEFAULT_DOCKS);
        set_dock_split(next_split);
        set_layout('split');
        set_dock_tree(preset_tree('split', DEFAULT_DOCKS, next_split));
        set_custom_dock_tree(preset_tree('split', DEFAULT_DOCKS, next_split));
    };

    const restore_dock_snapshot = (snapshot: DockLayoutSnapshot) => {
        set_docks(snapshot.docks);
        set_dock_split(snapshot.split);
        set_layout(snapshot.layout);
        set_dock_tree(snapshot.tree);
        set_custom_dock_tree(snapshot.custom_tree);
    };

    const request_quit = React.useCallback(() => {
        if (!confirm_dirty_scene_navigation(scene_dirty, 'quit Psynder Arcade')) return;
        if (!window.confirm('Quit Psynder Arcade?')) return;
        ipc_client.send<ConsoleEval>('console', 'eval', {
            id: ++command_seq.current,
            source: 'quit',
            mode: 'console',
        });
        window.setTimeout(() => {
            window.close();
        }, 120);
    }, [ipc_client, scene_dirty]);

    const close_editor_window = React.useCallback(() => {
        if (!confirm_dirty_scene_navigation(scene_dirty, 'close the editor window')) return;
        window.close();
    }, [scene_dirty]);

    const toggle_fullscreen = React.useCallback(() => {
        if (document.fullscreenElement) {
            void document.exitFullscreen();
        } else {
            void document.documentElement.requestFullscreen?.();
        }
    }, []);

    const send_scene_command = React.useCallback((
        source: string,
        label: string,
        action: SceneToolbarAction,
        path?: string,
    ) => {
        const id = ++command_seq.current;
        scene_pending.current.set(id, { id, label, action, path });
        set_scene_status('busy');
        set_scene_message(label);
        if (connection === 'open') {
            ipc_client.send<ConsoleEval>('console', 'eval', { id, source, mode: 'console' });
        } else {
            scene_pending.current.delete(id);
            set_scene_status('error');
            set_scene_message(`connection is ${connection}`);
        }
    }, [connection, ipc_client]);

    const request_new_scene = React.useCallback(() => {
        if (!confirm_dirty_scene_navigation(scene_dirty, 'create a new scene')) return;
        send_scene_command('arcade_new_scene', 'creating blank scene', 'new');
    }, [scene_dirty, send_scene_command]);

    const request_open_scene = React.useCallback(() => {
        if (!confirm_dirty_scene_navigation(scene_dirty, 'open another scene')) return;
        const next_path = window.prompt(`Open ${ENGINE_SCENE_PATH_HELP}`, scene_path.trim() || DEFAULT_SCENE_PATH);
        if (next_path === null) return;
        const path = next_path.trim();
        if (!path) {
            set_scene_status('error');
            set_scene_message('scene path required');
            return;
        }
        send_scene_command(load_scene_command(path), `opening ${path}`, 'load', path);
    }, [scene_dirty, scene_path, send_scene_command]);

    const request_save_scene = React.useCallback(() => {
        const path = scene_path.trim();
        if (!path) {
            set_scene_status('error');
            set_scene_message('scene save path required');
            return;
        }
        send_scene_command(save_scene_command(path), `saving ${path}`, 'save', path);
    }, [scene_path, send_scene_command]);

    const request_save_scene_as = React.useCallback(async () => {
        const next_path = await prompt_save_scene_path(scene_path);
        if (next_path === null) return;
        const path = next_path.trim();
        if (!path) {
            set_scene_status('error');
            set_scene_message('scene save path required');
            return;
        }
        send_scene_command(save_scene_command(path), `saving ${path}`, 'save', path);
    }, [scene_path, send_scene_command]);

    const request_scene_undo = React.useCallback(() => {
        send_scene_command('editor_undo', 'undoing scene edit', 'undo');
    }, [send_scene_command]);

    const request_scene_redo = React.useCallback(() => {
        send_scene_command('editor_redo', 'redoing scene edit', 'redo');
    }, [send_scene_command]);

    const current = route === 'workbench' ? WORKBENCH_META : PANEL_META[route];
    const scene_pending_ui = scene_status === 'busy';
    const scene_state_label = scene_pending_ui ? 'pending' : scene_dirty ? 'dirty' : 'clean';
    const scene_dirty_label = scene_pending_ui ? 'pending' : scene_dirty ? 'modified' : 'saved';
    const scene_toolbar_disabled = connection !== 'open' || scene_pending_ui;

    return (
        <div
            className="psy-app"
            data-panel={route}
            data-theme={theme}
            data-density={density}
            data-skin={skin}
        >
            <div className="psy-fx" aria-hidden="true" />
            <header className="psy-topbar">
                <div className="psy-window-controls" aria-label="Editor window controls">
                    <button
                        type="button"
                        className="psy-window-control is-close"
                        onClick={close_editor_window}
                        aria-label="Close editor window"
                        title="Close editor window"
                    />
                    <span className="psy-window-control is-minimize" aria-hidden="true" />
                    <button
                        type="button"
                        className="psy-window-control is-fullscreen"
                        onClick={toggle_fullscreen}
                        aria-label="Toggle fullscreen"
                        title="Toggle fullscreen"
                    />
                </div>

                <div className="psy-brand" aria-label="Psynder editor">
                    <span className="psy-brand-mark">P</span>
                    <span className="psy-brand-name">Psynder</span>
                    <span className="psy-brand-sub">editor</span>
                </div>

                <div className="psy-status-rail" aria-label="editor status">
                    <span className="psy-status-tile is-hot">
                        <b>{current.hot}</b>
                        <small>{current.label}</small>
                    </span>
                    <span className="psy-status-tile">
                        <b>{connection}</b>
                        <small>ipc</small>
                    </span>
                    <span className="psy-status-tile">
                        <b>{scene_state_label}</b>
                        <small>scene</small>
                    </span>
                </div>

                <div
                    className="psy-scene-chrome"
                    aria-label="scene toolbar"
                    data-status={scene_status}
                    title={scene_message}
                >
                    <span className={`psy-dirty-pill ${scene_dirty ? 'is-dirty' : ''} ${scene_pending_ui ? 'is-pending' : ''}`}>
                        {scene_dirty_label}
                    </span>
                    <span className="psy-scene-chrome-message" aria-live="polite">{scene_message}</span>
                    <input
                        className="psy-scene-chrome-path"
                        value={scene_path}
                        onChange={(e) => set_scene_path(e.target.value)}
                        spellCheck={false}
                        aria-label="Scene path"
                        title={`${ENGINE_SCENE_PATH_HELP}; used by Open and Save`}
                    />
                    <button
                        type="button"
                        className="psy-scene-tool"
                        onClick={request_new_scene}
                        disabled={scene_toolbar_disabled}
                        title="New scene"
                    >
                        <span aria-hidden="true">+</span>
                        <b>New</b>
                    </button>
                    <button
                        type="button"
                        className="psy-scene-tool"
                        onClick={request_open_scene}
                        disabled={scene_toolbar_disabled}
                        title="Open scene path"
                    >
                        <span aria-hidden="true">^</span>
                        <b>Open</b>
                    </button>
                    <button
                        type="button"
                        className="psy-scene-tool"
                        onClick={request_save_scene}
                        disabled={scene_toolbar_disabled}
                        title="Save scene"
                    >
                        <span aria-hidden="true">v</span>
                        <b>Save</b>
                    </button>
                    <button
                        type="button"
                        className="psy-scene-tool"
                        onClick={request_save_scene_as}
                        disabled={scene_toolbar_disabled}
                        title={`Save scene as a ${ENGINE_SCENE_PATH_HELP}`}
                    >
                        <span aria-hidden="true">...</span>
                        <b>Save As</b>
                    </button>
                    <span className="psy-toolbar-divider" aria-hidden="true" />
                    <button
                        type="button"
                        className="psy-scene-tool is-icon-only"
                        onClick={request_scene_undo}
                        disabled={scene_toolbar_disabled}
                        aria-label="Undo scene edit"
                        title="Undo scene edit"
                    >
                        <span aria-hidden="true">U</span>
                    </button>
                    <button
                        type="button"
                        className="psy-scene-tool is-icon-only"
                        onClick={request_scene_redo}
                        disabled={scene_toolbar_disabled}
                        aria-label="Redo scene edit"
                        title="Redo scene edit"
                    >
                        <span aria-hidden="true">R</span>
                    </button>
                </div>

                <div className="psy-topbar-actions">
                    <button
                        type="button"
                        className="psy-toolbar-btn"
                        onClick={() => switch_route('workbench')}
                        aria-label="Open docked workbench"
                        title="Docked workbench"
                    >
                        =
                    </button>
                    <button
                        type="button"
                        className="psy-toolbar-btn"
                        onClick={() => set_settings_open((open) => !open)}
                        aria-label="Open editor panel settings"
                        aria-expanded={settings_open}
                        title="Panel settings"
                    >
                        *
                    </button>
                    <button
                        type="button"
                        className="psy-toolbar-btn psy-toolbar-btn-danger"
                        onClick={request_quit}
                        aria-label="Quit Psynder Arcade"
                        title="Quit Psynder Arcade"
                    >
                        <span className="psy-power-icon" aria-hidden="true" />
                    </button>
                </div>

                {settings_open && (
                    <div className="psy-settings-pop" role="dialog" aria-label="Panel settings">
                        <SettingRow label="skin">
                            <Segmented<SkinName>
                                values={['modern', 'tactical']}
                                value={skin}
                                on_change={set_skin}
                            />
                        </SettingRow>
                        <SettingRow label="theme">
                            <div className="psy-swatches">
                                {(['forge', 'field', 'mono'] as ThemeName[]).map((name) => (
                                    <button
                                        key={name}
                                        type="button"
                                        className={`psy-swatch is-${name}${theme === name ? ' is-selected' : ''}`}
                                        onClick={() => set_theme(name)}
                                        aria-label={`Use ${name} theme`}
                                        title={name}
                                    />
                                ))}
                            </div>
                        </SettingRow>
                        <SettingRow label="density">
                            <Segmented<DensityName>
                                values={['comfortable', 'compact']}
                                value={density}
                                on_change={set_density}
                            />
                        </SettingRow>
                        <SettingRow label="layout">
                            <Segmented<LayoutName>
                                values={LAYOUT_NAMES}
                                value={layout}
                                on_change={apply_layout_preset}
                            />
                        </SettingRow>
                        <button
                            type="button"
                            className="psy-reset-btn"
                            onClick={reset_layout}
                        >
                            reset layout
                        </button>
                    </div>
                )}
            </header>

            <div className="psy-workbench">
                <nav className="psy-app-nav" aria-label="panel switcher">
                    <button
                        type="button"
                        className={`psy-nav-btn ${route === 'workbench' ? 'is-active' : ''}`}
                        onClick={() => switch_route('workbench')}
                    >
                        <span className="psy-nav-icon">{WORKBENCH_META.icon}</span>
                        <span className="psy-nav-label">{WORKBENCH_META.label}</span>
                        <span className="psy-nav-hot">{WORKBENCH_META.hot}</span>
                    </button>
                    {PANEL_NAMES.map((name) => (
                        <button
                            key={name}
                            type="button"
                            draggable
                            className={`psy-nav-btn ${route === name ? 'is-active' : ''}`}
                            onClick={() => switch_route(name)}
                            onDragStart={(e) => {
                                e.dataTransfer.setData('application/x-psy-panel', name);
                                e.dataTransfer.effectAllowed = 'copyMove';
                            }}
                        >
                            <span className="psy-nav-icon">{PANEL_META[name].icon}</span>
                            <span className="psy-nav-label">{PANEL_META[name].label}</span>
                            <span className="psy-nav-hot">{PANEL_META[name].hot}</span>
                        </button>
                    ))}
                </nav>

                <main className="psy-app-main">
                    {route === 'workbench' ? (
                        <DockWorkspace
                            layout={layout}
                            tree={dock_tree}
                            split={dock_split}
                            snapshot={dock_snapshot}
                            on_layout={apply_layout_preset}
                            on_reset={reset_layout}
                            on_restore={restore_dock_snapshot}
                            on_tree={set_dock_tree}
                            on_split={set_dock_split}
                        />
                    ) : (
                        <PanelView name={route} />
                    )}
                </main>
            </div>
        </div>
    );
}

function DockWorkspace({
    layout,
    tree,
    split,
    snapshot,
    on_layout,
    on_reset,
    on_restore,
    on_tree,
    on_split,
}: {
    layout: LayoutName;
    tree: DockNode;
    split: number;
    snapshot: DockLayoutSnapshot;
    on_layout(layout: LayoutName): void;
    on_reset(): void;
    on_restore(snapshot: DockLayoutSnapshot): void;
    on_tree(tree: DockNode): void;
    on_split(split: number): void;
}) {
    const shell_ref = React.useRef<HTMLElement | null>(null);
    const drag_source_ref = React.useRef<DockPath | null>(null);
    const drop_handled_ref = React.useRef(false);
    const undo_timer_ref = React.useRef<ReturnType<typeof window.setTimeout> | null>(null);
    const undo_seq = React.useRef(0);
    const [active_path, set_active_path] = React.useState<DockPath>([]);
    const [undo, set_undo] = React.useState<DockUndo | null>(null);
    const panel_counts = React.useMemo(() => count_dock_panels(tree), [tree]);
    const hidden_panels = React.useMemo(() => (
        PANEL_NAMES.filter((name) => !panel_counts.has(name))
    ), [panel_counts]);
    const leaf_paths = React.useMemo(() => collect_dock_leaf_paths(tree), [tree]);

    React.useEffect(() => () => {
        if (undo_timer_ref.current !== null) {
            window.clearTimeout(undo_timer_ref.current);
        }
    }, []);

    React.useEffect(() => {
        if (!get_dock_at(tree, active_path)) {
            set_active_path(leaf_paths[0] ?? []);
        }
    }, [active_path, leaf_paths, tree]);

    const push_undo = React.useCallback((message: string) => {
        undo_seq.current += 1;
        if (undo_timer_ref.current !== null) {
            window.clearTimeout(undo_timer_ref.current);
        }
        set_undo({
            id: undo_seq.current,
            snapshot,
            message,
        });
        undo_timer_ref.current = window.setTimeout(() => {
            set_undo(null);
            undo_timer_ref.current = null;
        }, 4200);
    }, [snapshot]);

    const replace_at = React.useCallback((path: DockPath, panel: PanelName, zone: DockDropZone) => {
        on_tree(update_dock_at(tree, path, (target) => split_dock_leaf(target, panel, zone)));
    }, [on_tree, tree]);

    const move_or_drop_at = React.useCallback((
        path: DockPath,
        panel: PanelName,
        zone: DockDropZone,
        source_path: DockPath | null,
    ) => {
        if (!source_path) {
            replace_at(path, panel, zone);
            return;
        }
        drop_handled_ref.current = true;
        if (same_path(path, source_path)) {
            if (zone !== 'center') {
                on_tree(update_dock_at(tree, path, (target) => split_dock_leaf(target, panel, zone)));
            }
            return;
        }
        const inserted = update_dock_at(tree, path, (target) => split_dock_leaf(target, panel, zone));
        on_tree(remove_dock_at(inserted, source_path));
    }, [on_tree, replace_at, tree]);

    const resize_at = React.useCallback((path: DockPath, ratio: number) => {
        on_tree(update_dock_at(tree, path, (target) => (
            target.kind === 'split' ? { ...target, ratio: safe_ratio(ratio) } : target
        )));
        if (path.length === 0) on_split(ratio);
    }, [on_split, on_tree, tree]);

    const remove_at = React.useCallback((path: DockPath) => {
        push_undo('panel removed');
        on_tree(remove_dock_at(tree, path));
    }, [on_tree, push_undo, tree]);

    const add_panel = React.useCallback((panel: PanelName) => {
        const target = get_dock_at(tree, active_path);
        push_undo('panel added');
        on_tree(target?.kind === 'leaf'
            ? update_dock_at(tree, active_path, (node) => split_dock_leaf(node, panel, 'right'))
            : append_dock_panel(tree, panel));
    }, [active_path, on_tree, push_undo, tree]);

    const reset_with_undo = React.useCallback(() => {
        push_undo('layout reset');
        on_reset();
    }, [on_reset, push_undo]);

    const begin_drag = React.useCallback((path: DockPath) => {
        drag_source_ref.current = path;
        drop_handled_ref.current = false;
    }, []);

    const finish_drag = React.useCallback(() => {
        drag_source_ref.current = null;
        drop_handled_ref.current = false;
    }, []);

    const focus_path = React.useCallback((path: DockPath) => {
        set_active_path(path);
        window.requestAnimationFrame(() => {
            const target = shell_ref.current?.querySelector<HTMLElement>(
                `[data-dock-path="${dock_path_key(path)}"]`,
            );
            target?.focus();
        });
    }, []);

    const focus_relative_path = React.useCallback((direction: 1 | -1) => {
        if (leaf_paths.length === 0) return;
        const index = leaf_paths.findIndex((path) => same_path(path, active_path));
        const next_index = index < 0
            ? 0
            : (index + direction + leaf_paths.length) % leaf_paths.length;
        focus_path(leaf_paths[next_index]);
    }, [active_path, focus_path, leaf_paths]);

    const undo_close = React.useCallback(() => {
        if (!undo) return;
        if (undo_timer_ref.current !== null) {
            window.clearTimeout(undo_timer_ref.current);
            undo_timer_ref.current = null;
        }
        on_restore(undo.snapshot);
        set_undo(null);
    }, [on_restore, undo]);

    const handle_key_down = React.useCallback((e: React.KeyboardEvent<HTMLElement>) => {
        if (!(e.metaKey || e.ctrlKey) || e.altKey || is_text_edit_target(e.target)) return;
        if (e.key === 'z') {
            if (!undo) return;
            e.preventDefault();
            undo_close();
        } else if (e.key === 'Backspace' || e.key === 'Delete') {
            e.preventDefault();
            reset_with_undo();
        } else if (e.key === 'Enter') {
            const next_panel = hidden_panels[0];
            if (!next_panel) return;
            e.preventDefault();
            add_panel(next_panel);
        } else if (e.key === 'ArrowRight' || e.key === 'ArrowDown') {
            e.preventDefault();
            focus_relative_path(1);
        } else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') {
            e.preventDefault();
            focus_relative_path(-1);
        }
    }, [add_panel, focus_relative_path, hidden_panels, reset_with_undo, undo, undo_close]);

    return (
        <section
            ref={shell_ref}
            className="psy-dock-shell"
            data-layout={layout}
            onKeyDown={handle_key_down}
        >
            <div className="psy-dock-toolbar">
                <div className="psy-dock-title">
                    <span className="psy-dock-glyph">=</span>
                    <span>Dock Workspace</span>
                </div>
                <Segmented<LayoutName>
                    values={LAYOUT_NAMES}
                    value={layout}
                    on_change={on_layout}
                />
                <div className="psy-dock-tray" aria-label="Hidden dock panels">
                    <span className="psy-dock-tray-label">panels</span>
                    <div className="psy-dock-tray-items">
                        {hidden_panels.length === 0 ? (
                            <span className="psy-dock-tray-empty">all visible</span>
                        ) : hidden_panels.map((name) => (
                            <button
                                key={name}
                                type="button"
                                className="psy-dock-tray-btn"
                                onClick={() => add_panel(name)}
                                aria-label={`Add ${PANEL_META[name].label} to dock`}
                                title={`Add ${PANEL_META[name].label}`}
                            >
                                <span>{PANEL_META[name].icon}</span>
                                <small>{PANEL_META[name].label}</small>
                            </button>
                        ))}
                    </div>
                </div>
                <button
                    type="button"
                    className="psy-reset-btn"
                    onClick={reset_with_undo}
                >
                    reset layout
                </button>
            </div>
            <div
                className="psy-dock-tree"
                style={{ '--psy-dock-split': `${split}%` } as React.CSSProperties}
            >
                <DockNodeView
                    node={tree}
                    path={[]}
                    label="root"
                    on_begin_drag={begin_drag}
                    on_drop_panel={move_or_drop_at}
                    on_finish_drag={finish_drag}
                    on_remove={remove_at}
                    on_resize={resize_at}
                    active_path={active_path}
                    panel_counts={panel_counts}
                    on_activate={set_active_path}
                />
            </div>
            {undo && (
                <div className="psy-dock-undo" role="status">
                    <span>{undo.message}</span>
                    <button type="button" onClick={undo_close}>undo</button>
                </div>
            )}
        </section>
    );
}

function DockNodeView({
    node,
    path,
    label,
    active_path,
    panel_counts,
    on_begin_drag,
    on_drop_panel,
    on_finish_drag,
    on_remove,
    on_resize,
    on_activate,
}: {
    node: DockNode;
    path: DockPath;
    label: string;
    active_path: DockPath;
    panel_counts: Map<PanelName, number>;
    on_begin_drag(path: DockPath): void;
    on_drop_panel(
        path: DockPath,
        panel: PanelName,
        zone: DockDropZone,
        source_path: DockPath | null,
    ): void;
    on_finish_drag(): void;
    on_remove(path: DockPath): void;
    on_resize(path: DockPath, ratio: number): void;
    on_activate(path: DockPath): void;
}) {
    if (node.kind === 'leaf') {
        return (
            <DockSlotView
                slot={label}
                panel={node.panel}
                path={path}
                duplicate_count={panel_counts.get(node.panel) ?? 0}
                active={same_path(path, active_path)}
                on_begin_drag={() => on_begin_drag(path)}
                on_drop_panel={(panel, zone, source_path) => (
                    on_drop_panel(path, panel, zone, source_path)
                )}
                on_finish_drag={on_finish_drag}
                on_remove={path.length > 0 ? () => on_remove(path) : undefined}
                on_activate={() => on_activate(path)}
            />
        );
    }

    return (
        <div
            className="psy-dock-node is-split"
            data-axis={node.axis}
            style={{ '--psy-dock-node-ratio': `${node.ratio}%` } as React.CSSProperties}
        >
            <DockNodeView
                node={node.first}
                path={[...path, 0]}
                label={`${label}.a`}
                active_path={active_path}
                panel_counts={panel_counts}
                on_begin_drag={on_begin_drag}
                on_drop_panel={on_drop_panel}
                on_finish_drag={on_finish_drag}
                on_remove={on_remove}
                on_resize={on_resize}
                on_activate={on_activate}
            />
            <DockDivider
                axis={node.axis}
                ratio={node.ratio}
                on_ratio={(ratio) => on_resize(path, ratio)}
            />
            <DockNodeView
                node={node.second}
                path={[...path, 1]}
                label={`${label}.b`}
                active_path={active_path}
                panel_counts={panel_counts}
                on_begin_drag={on_begin_drag}
                on_drop_panel={on_drop_panel}
                on_finish_drag={on_finish_drag}
                on_remove={on_remove}
                on_resize={on_resize}
                on_activate={on_activate}
            />
        </div>
    );
}

function DockDivider({
    axis,
    ratio,
    on_ratio,
}: {
    axis: DockAxis;
    ratio: number;
    on_ratio(ratio: number): void;
}) {
    const begin_resize = (e: React.PointerEvent<HTMLDivElement>) => {
        const grid = e.currentTarget.parentElement;
        if (!grid) return;
        e.preventDefault();
        const rect = grid.getBoundingClientRect();
        const move = (ev: PointerEvent) => {
            const raw = axis === 'row'
                ? ((ev.clientX - rect.left) / Math.max(rect.width, 1)) * 100
                : ((ev.clientY - rect.top) / Math.max(rect.height, 1)) * 100;
            on_ratio(safe_ratio(raw));
        };
        const up = () => {
            window.removeEventListener('pointermove', move);
            window.removeEventListener('pointerup', up);
        };
        window.addEventListener('pointermove', move);
        window.addEventListener('pointerup', up);
    };

    return (
        <div
            className="psy-dock-divider"
            data-axis={axis}
            onPointerDown={begin_resize}
            role="separator"
            aria-orientation={axis === 'row' ? 'vertical' : 'horizontal'}
            aria-valuemin={18}
            aria-valuemax={82}
            aria-valuenow={Math.round(ratio)}
        />
    );
}

function DockSlotView({
    slot,
    panel,
    path,
    duplicate_count,
    active,
    on_begin_drag,
    on_drop_panel,
    on_finish_drag,
    on_remove,
    on_activate,
}: {
    slot: string;
    panel: PanelName;
    path: DockPath;
    duplicate_count: number;
    active: boolean;
    on_begin_drag(): void;
    on_drop_panel(panel: PanelName, zone: DockDropZone, source_path: DockPath | null): void;
    on_finish_drag(): void;
    on_remove?: () => void;
    on_activate(): void;
}) {
    const [drop_zone, set_drop_zone] = React.useState<DockDropZone | null>(null);

    const zone_from_event = (e: React.DragEvent<HTMLElement>): DockDropZone => {
        const rect = e.currentTarget.getBoundingClientRect();
        const x = (e.clientX - rect.left) / Math.max(rect.width, 1);
        const y = (e.clientY - rect.top) / Math.max(rect.height, 1);
        const edge = 0.26;
        if (x < edge) return 'left';
        if (x > 1 - edge) return 'right';
        if (y < edge) return 'top';
        if (y > 1 - edge) return 'bottom';
        return 'center';
    };

    return (
        <section
            className="psy-dock-slot"
            data-slot={slot}
            data-dock-path={dock_path_key(path)}
            data-drop-zone={drop_zone ?? undefined}
            data-active={active ? 'true' : undefined}
            tabIndex={0}
            onFocus={on_activate}
            onPointerDown={on_activate}
            onDragOver={(e) => {
                if (Array.from(e.dataTransfer.types).includes('application/x-psy-panel')) {
                    e.preventDefault();
                    e.dataTransfer.dropEffect = 'move';
                    set_drop_zone(zone_from_event(e));
                }
            }}
            onDragLeave={(e) => {
                if (!e.currentTarget.contains(e.relatedTarget as Node | null)) {
                    set_drop_zone(null);
                }
            }}
            onDrop={(e) => {
                const dropped = e.dataTransfer.getData('application/x-psy-panel');
                if ((PANEL_NAMES as readonly string[]).includes(dropped)) {
                    e.preventDefault();
                    const source_path = parse_dock_path(
                        e.dataTransfer.getData('application/x-psy-dock-path'),
                    );
                    on_drop_panel(dropped as PanelName, zone_from_event(e), source_path);
                    set_drop_zone(null);
                }
            }}
            onDragEnd={() => set_drop_zone(null)}
        >
            <div className="psy-dock-tabbar">
                <button
                    type="button"
                    className="psy-dock-drag"
                    draggable
                    aria-label={`Move ${PANEL_META[panel].label}`}
                    title="Move panel"
                    onDragStart={(e) => {
                        on_begin_drag();
                        e.dataTransfer.setData('application/x-psy-panel', panel);
                        e.dataTransfer.setData('application/x-psy-dock-path', JSON.stringify(path));
                        e.dataTransfer.effectAllowed = 'move';
                        e.dataTransfer.setDragImage(transparent_drag_image(), 0, 0);
                    }}
                    onDragEnd={on_finish_drag}
                >
                    ::
                </button>
                <span className="psy-dock-slot-name">{slot}</span>
                {duplicate_count > 1 && (
                    <span
                        className="psy-dock-dup-badge"
                        aria-label={`${PANEL_META[panel].label} appears ${duplicate_count} times`}
                    >
                        x{duplicate_count}
                    </span>
                )}
                <select
                    className="psy-dock-select"
                    value={panel}
                    aria-label={`${slot} dock panel`}
                    onChange={(e) => on_drop_panel(e.target.value as PanelName, 'center', null)}
                >
                    {PANEL_NAMES.map((name) => (
                        <option key={name} value={name}>{PANEL_META[name].label}</option>
                    ))}
                </select>
                {on_remove && (
                    <button
                        type="button"
                        className="psy-dock-close"
                        aria-label={`Remove ${PANEL_META[panel].label} from dock`}
                        title="Remove panel"
                        onClick={on_remove}
                    >
                        x
                    </button>
                )}
            </div>
            {drop_zone && (
                <div className="psy-dock-drop-ghost" data-zone={drop_zone}>
                    <span>{drop_zone === 'center' ? 'replace' : `split ${drop_zone}`}</span>
                </div>
            )}
            <div className="psy-dock-content">
                <PanelView name={panel} />
            </div>
        </section>
    );
}

function PanelView({ name }: { name: PanelName }) {
    if (name === 'scene') return <Hierarchy />;
    if (name === 'inspector') return <Inspector />;
    if (name === 'console') return <Console />;
    if (name === 'profiler') return <Profiler />;
    if (name === 'assets') return <AssetBrowser />;
    if (name === 'props') return <PropSpawn />;
    return <PsyGraph />;
}

function SettingRow({
    label,
    children,
}: {
    label: string;
    children: React.ReactNode;
}) {
    return (
        <div className="psy-setting-row">
            <span>{label}</span>
            {children}
        </div>
    );
}

function Segmented<T extends string>({
    values,
    value,
    on_change,
}: {
    values: readonly T[];
    value: T;
    on_change(value: T): void;
}) {
    return (
        <div className="psy-segmented">
            {values.map((item) => (
                <button
                    key={item}
                    type="button"
                    className={`psy-segmented-option${item === value ? ' is-selected' : ''}`}
                    onClick={() => on_change(item)}
                >
                    {item}
                </button>
            ))}
        </div>
    );
}
