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
    const [docks, set_docks] = React.useState<Record<DockSlot, PanelName>>(() => ({
        primary: safe_panel(window.localStorage.getItem('psy_dock_primary'), DEFAULT_DOCKS.primary),
        secondary: safe_panel(window.localStorage.getItem('psy_dock_secondary'), DEFAULT_DOCKS.secondary),
        tertiary: safe_panel(window.localStorage.getItem('psy_dock_tertiary'), DEFAULT_DOCKS.tertiary),
        quaternary: safe_panel(window.localStorage.getItem('psy_dock_quaternary'), DEFAULT_DOCKS.quaternary),
    }));
    const [dock_split, set_dock_split] = React.useState(() => (
        safe_split(window.localStorage.getItem('psy_dock_split'))
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
                                on_change={set_layout}
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
                            docks={docks}
                            split={dock_split}
                            on_layout={set_layout}
                            on_split={set_dock_split}
                            on_dock={(slot, panel) => set_docks((prev) => ({ ...prev, [slot]: panel }))}
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
    docks,
    split,
    on_layout,
    on_split,
    on_dock,
}: {
    layout: LayoutName;
    docks: Record<DockSlot, PanelName>;
    split: number;
    on_layout(layout: LayoutName): void;
    on_split(split: number): void;
    on_dock(slot: DockSlot, panel: PanelName): void;
}) {
    const slots: DockSlot[] = layout === 'quad'
        ? ['primary', 'secondary', 'tertiary', 'quaternary']
        : layout === 'single'
            ? ['primary']
            : ['primary', 'secondary'];

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
                className="psy-dock-grid"
                data-layout={layout}
                style={{ '--psy-dock-split': `${split}%` } as React.CSSProperties}
            >
                {slots.map((slot, index) => (
                    <React.Fragment key={slot}>
                        <DockSlotView
                            slot={slot}
                            panel={docks[slot]}
                            on_panel={(panel) => on_dock(slot, panel)}
                        />
                        {index === 0 && (layout === 'split' || layout === 'stack') && (
                            <DockDivider layout={layout} split={split} on_split={on_split} />
                        )}
                    </React.Fragment>
                ))}
            </div>
        </section>
    );
}

function DockDivider({
    layout,
    split,
    on_split,
}: {
    layout: Extract<LayoutName, 'split' | 'stack'>;
    split: number;
    on_split(split: number): void;
}) {
    const begin_resize = (e: React.PointerEvent<HTMLDivElement>) => {
        const grid = e.currentTarget.parentElement;
        if (!grid) return;
        e.preventDefault();
        const rect = grid.getBoundingClientRect();
        const move = (ev: PointerEvent) => {
            const raw = layout === 'split'
                ? ((ev.clientX - rect.left) / Math.max(rect.width, 1)) * 100
                : ((ev.clientY - rect.top) / Math.max(rect.height, 1)) * 100;
            on_split(Math.min(76, Math.max(24, raw)));
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
            data-layout={layout}
            onPointerDown={begin_resize}
            role="separator"
            aria-orientation={layout === 'split' ? 'vertical' : 'horizontal'}
            aria-valuemin={24}
            aria-valuemax={76}
            aria-valuenow={Math.round(split)}
        />
    );
}

function DockSlotView({
    slot,
    panel,
    on_panel,
}: {
    slot: DockSlot;
    panel: PanelName;
    on_panel(panel: PanelName): void;
}) {
    return (
        <section
            className="psy-dock-slot"
            data-slot={slot}
            onDragOver={(e) => {
                if (Array.from(e.dataTransfer.types).includes('application/x-psy-panel')) {
                    e.preventDefault();
                    e.dataTransfer.dropEffect = 'copy';
                }
            }}
            onDrop={(e) => {
                const dropped = e.dataTransfer.getData('application/x-psy-panel');
                if ((PANEL_NAMES as readonly string[]).includes(dropped)) {
                    e.preventDefault();
                    on_panel(dropped as PanelName);
                }
            }}
        >
            <div className="psy-dock-tabbar">
                <span className="psy-dock-slot-name">{slot}</span>
                <select
                    className="psy-dock-select"
                    value={panel}
                    aria-label={`${slot} dock panel`}
                    onChange={(e) => on_panel(e.target.value as PanelName)}
                >
                    {PANEL_NAMES.map((name) => (
                        <option key={name} value={name}>{PANEL_META[name].label}</option>
                    ))}
                </select>
            </div>
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
