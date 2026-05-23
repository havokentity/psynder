// SPDX-License-Identifier: MIT
// Psynder editor — top-level router. Each panel is a separate Chrome window
// that hits one of `/panels/<name>` per DESIGN.md §10.6. In dev (vite) we
// use the `?panel=` query parameter to flip between them in one bundle.

import React from 'react';

import { AssetBrowser } from './panels/AssetBrowser';
import { Console } from './panels/Console';
import { Inspector } from './panels/Inspector';
import { Profiler } from './panels/Profiler';
import { PropSpawn } from './panels/PropSpawn';
import { PsyGraph } from './panels/PsyGraph';

type PanelName = 'inspector' | 'console' | 'profiler' | 'assets' | 'props' | 'psygraph';
type RouteName = PanelName | 'workbench';
type ThemeName = 'forge' | 'field' | 'mono';
type DensityName = 'comfortable' | 'compact';
type SkinName = 'modern' | 'tactical';
type LayoutName = 'split' | 'stack' | 'quad' | 'single';
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

const PANEL_NAMES: readonly PanelName[] = [
    'inspector', 'console', 'profiler', 'assets', 'props', 'psygraph',
];
const LAYOUT_NAMES: readonly LayoutName[] = ['split', 'stack', 'quad', 'single'];
const DOCK_SLOTS: readonly DockSlot[] = ['primary', 'secondary', 'tertiary', 'quaternary'];

const PANEL_META: Record<PanelName, { icon: string; label: string; hot: string }> = {
    inspector: { icon: 'I', label: 'Inspector', hot: 'sel' },
    console:   { icon: '>', label: 'Console', hot: 'repl' },
    profiler:  { icon: '~', label: 'Profiler', hot: 'fps' },
    assets:    { icon: '#', label: 'Assets', hot: 'vfs' },
    props:     { icon: '+', label: 'Props', hot: 'spawn' },
    psygraph:  { icon: '*', label: 'PsyGraph', hot: 'logic' },
};
const WORKBENCH_META = { icon: '=', label: 'Workbench', hot: 'dock' };
const DEFAULT_DOCKS: Record<DockSlot, PanelName> = {
    primary: 'console',
    secondary: 'profiler',
    tertiary: 'inspector',
    quaternary: 'assets',
};

// Engine route paths map onto panel names; "assets" / "props" land on the
// `/panels/assets` and `/panels/props` URLs that the engine launches Chrome
// against — see DESIGN.md §10.6 / §10.8.
const PATH_TO_ROUTE: Record<string, RouteName> = {
    workbench: 'workbench',
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
    if (qp === 'workbench' || (qp && (PANEL_NAMES as readonly string[]).includes(qp))) {
        return qp as RouteName;
    }
    return 'inspector';
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

function preset_tree(
    layout: LayoutName,
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
    const [route, set_route] = React.useState<RouteName>(pick_route);
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
    const [docks] = React.useState<Record<DockSlot, PanelName>>(() => ({
        primary: safe_panel(window.localStorage.getItem('psy_dock_primary'), DEFAULT_DOCKS.primary),
        secondary: safe_panel(window.localStorage.getItem('psy_dock_secondary'), DEFAULT_DOCKS.secondary),
        tertiary: safe_panel(window.localStorage.getItem('psy_dock_tertiary'), DEFAULT_DOCKS.tertiary),
        quaternary: safe_panel(window.localStorage.getItem('psy_dock_quaternary'), DEFAULT_DOCKS.quaternary),
    }));
    const [dock_split, set_dock_split] = React.useState(() => (
        safe_split(window.localStorage.getItem('psy_dock_split'))
    ));
    const [dock_tree, set_dock_tree] = React.useState<DockNode>(() => (
        load_dock_tree(preset_tree(layout, docks, dock_split))
    ));

    // In dev mode there's only a single bundle; let the user flip between
    // panels via the header without reloading. The engine doesn't drive
    // this — each Chrome window only ever shows one panel.
    React.useEffect(() => {
        const on_pop = () => set_route(pick_route());
        window.addEventListener('popstate', on_pop);
        return () => window.removeEventListener('popstate', on_pop);
    }, []);

    const switch_route = (name: RouteName) => {
        if (route === name) return;
        const url = new URL(window.location.href);
        url.searchParams.set('panel', name);
        // Keep history sane in dev — engine-launched windows already pin
        // a panel and won't see this code path.
        window.history.pushState({}, '', url.toString());
        set_route(name);
    };

    React.useEffect(() => {
        window.localStorage.setItem('psy_theme', theme);
        window.localStorage.setItem('psy_density', density);
        window.localStorage.setItem('psy_skin', skin);
    }, [theme, density, skin]);

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

    const apply_layout_preset = (next_layout: LayoutName) => {
        set_layout(next_layout);
        set_dock_tree(preset_tree(next_layout, docks, dock_split));
    };

    const current = route === 'workbench' ? WORKBENCH_META : PANEL_META[route];

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
                        <b>7654</b>
                        <small>ipc</small>
                    </span>
                    <span className="psy-status-tile">
                        <b>{density === 'comfortable' ? 'touch' : 'dense'}</b>
                        <small>ui</small>
                    </span>
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
                            on_layout={apply_layout_preset}
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
    on_layout,
    on_tree,
    on_split,
}: {
    layout: LayoutName;
    tree: DockNode;
    split: number;
    on_layout(layout: LayoutName): void;
    on_tree(tree: DockNode): void;
    on_split(split: number): void;
}) {
    const replace_at = React.useCallback((path: DockPath, panel: PanelName, zone: DockDropZone) => {
        on_tree(update_dock_at(tree, path, (target) => split_dock_leaf(target, panel, zone)));
    }, [on_tree, tree]);

    const resize_at = React.useCallback((path: DockPath, ratio: number) => {
        on_tree(update_dock_at(tree, path, (target) => (
            target.kind === 'split' ? { ...target, ratio: safe_ratio(ratio) } : target
        )));
        if (path.length === 0) on_split(ratio);
    }, [on_split, on_tree, tree]);

    return (
        <section className="psy-dock-shell" data-layout={layout}>
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
            </div>
            <div
                className="psy-dock-tree"
                style={{ '--psy-dock-split': `${split}%` } as React.CSSProperties}
            >
                <DockNodeView
                    node={tree}
                    path={[]}
                    label="root"
                    on_drop_panel={replace_at}
                    on_resize={resize_at}
                />
            </div>
        </section>
    );
}

function DockNodeView({
    node,
    path,
    label,
    on_drop_panel,
    on_resize,
}: {
    node: DockNode;
    path: DockPath;
    label: string;
    on_drop_panel(path: DockPath, panel: PanelName, zone: DockDropZone): void;
    on_resize(path: DockPath, ratio: number): void;
}) {
    if (node.kind === 'leaf') {
        return (
            <DockSlotView
                slot={label}
                panel={node.panel}
                on_drop_panel={(panel, zone) => on_drop_panel(path, panel, zone)}
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
                on_drop_panel={on_drop_panel}
                on_resize={on_resize}
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
                on_drop_panel={on_drop_panel}
                on_resize={on_resize}
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
    on_drop_panel,
}: {
    slot: string;
    panel: PanelName;
    on_drop_panel(panel: PanelName, zone: DockDropZone): void;
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
            data-drop-zone={drop_zone ?? undefined}
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
                    on_drop_panel(dropped as PanelName, zone_from_event(e));
                    set_drop_zone(null);
                }
            }}
            onDragEnd={() => set_drop_zone(null)}
        >
            <div className="psy-dock-tabbar">
                <span className="psy-dock-slot-name">{slot}</span>
                <select
                    className="psy-dock-select"
                    value={panel}
                    aria-label={`${slot} dock panel`}
                    onChange={(e) => on_drop_panel(e.target.value as PanelName, 'center')}
                >
                    {PANEL_NAMES.map((name) => (
                        <option key={name} value={name}>{PANEL_META[name].label}</option>
                    ))}
                </select>
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
