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
type ThemeName = 'forge' | 'field' | 'mono';
type DensityName = 'comfortable' | 'compact';
type SkinName = 'modern' | 'tactical';

const PANEL_NAMES: readonly PanelName[] = [
    'inspector', 'console', 'profiler', 'assets', 'props', 'psygraph',
];

const PANEL_META: Record<PanelName, { icon: string; label: string; hot: string }> = {
    inspector: { icon: 'I', label: 'Inspector', hot: 'sel' },
    console:   { icon: '>', label: 'Console', hot: 'repl' },
    profiler:  { icon: '~', label: 'Profiler', hot: 'fps' },
    assets:    { icon: '#', label: 'Assets', hot: 'vfs' },
    props:     { icon: '+', label: 'Props', hot: 'spawn' },
    psygraph:  { icon: '*', label: 'PsyGraph', hot: 'logic' },
};

// Engine route paths map onto panel names; "assets" / "props" land on the
// `/panels/assets` and `/panels/props` URLs that the engine launches Chrome
// against — see DESIGN.md §10.6 / §10.8.
const PATH_TO_PANEL: Record<string, PanelName> = {
    inspector: 'inspector',
    console:   'console',
    profiler:  'profiler',
    assets:    'assets',
    props:     'props',
    psygraph:  'psygraph',
};

function pick_panel(): PanelName {
    if (typeof window === 'undefined') return 'inspector';
    // Path-based first (engine routes via /panels/<name>), then query string.
    const m = window.location.pathname.match(/\/panels\/([a-z]+)/);
    if (m && m[1] in PATH_TO_PANEL) return PATH_TO_PANEL[m[1]];
    const qp = new URLSearchParams(window.location.search).get('panel');
    if (qp && (PANEL_NAMES as readonly string[]).includes(qp)) {
        return qp as PanelName;
    }
    return 'inspector';
}

export function App() {
    const [panel, set_panel] = React.useState<PanelName>(pick_panel);
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

    // In dev mode there's only a single bundle; let the user flip between
    // panels via the header without reloading. The engine doesn't drive
    // this — each Chrome window only ever shows one panel.
    React.useEffect(() => {
        const on_pop = () => set_panel(pick_panel());
        window.addEventListener('popstate', on_pop);
        return () => window.removeEventListener('popstate', on_pop);
    }, []);

    const switch_panel = (name: PanelName) => {
        if (panel === name) return;
        const url = new URL(window.location.href);
        url.searchParams.set('panel', name);
        // Keep history sane in dev — engine-launched windows already pin
        // a panel and won't see this code path.
        window.history.pushState({}, '', url.toString());
        set_panel(name);
    };

    React.useEffect(() => {
        window.localStorage.setItem('psy_theme', theme);
        window.localStorage.setItem('psy_density', density);
        window.localStorage.setItem('psy_skin', skin);
    }, [theme, density, skin]);

    const current = PANEL_META[panel];

    return (
        <div
            className="psy-app"
            data-panel={panel}
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
                    </div>
                )}
            </header>

            <div className="psy-workbench">
                <nav className="psy-app-nav" aria-label="panel switcher">
                    {PANEL_NAMES.map((name) => (
                        <button
                            key={name}
                            type="button"
                            className={`psy-nav-btn ${panel === name ? 'is-active' : ''}`}
                            onClick={() => switch_panel(name)}
                        >
                            <span className="psy-nav-icon">{PANEL_META[name].icon}</span>
                            <span className="psy-nav-label">{PANEL_META[name].label}</span>
                            <span className="psy-nav-hot">{PANEL_META[name].hot}</span>
                        </button>
                    ))}
                </nav>

                <main className="psy-app-main">
                    {panel === 'inspector' && <Inspector />}
                    {panel === 'console'   && <Console />}
                    {panel === 'profiler'  && <Profiler />}
                    {panel === 'assets'    && <AssetBrowser />}
                    {panel === 'props'     && <PropSpawn />}
                    {panel === 'psygraph'  && <PsyGraph />}
                </main>
            </div>
        </div>
    );
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
