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

type PanelName = 'inspector' | 'console' | 'profiler' | 'assets' | 'props';

const PANEL_NAMES: readonly PanelName[] = [
    'inspector', 'console', 'profiler', 'assets', 'props',
];

// Engine route paths map onto panel names; "assets" / "props" land on the
// `/panels/assets` and `/panels/props` URLs that the engine launches Chrome
// against — see DESIGN.md §10.6 / §10.8.
const PATH_TO_PANEL: Record<string, PanelName> = {
    inspector: 'inspector',
    console:   'console',
    profiler:  'profiler',
    assets:    'assets',
    props:     'props',
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

    return (
        <div className="psy-app" data-panel={panel}>
            <nav className="psy-app-nav" aria-label="panel switcher">
                {PANEL_NAMES.map((name) => (
                    <button
                        key={name}
                        type="button"
                        className={`psy-nav-btn ${panel === name ? 'is-active' : ''}`}
                        onClick={() => switch_panel(name)}
                    >
                        {name}
                    </button>
                ))}
            </nav>

            <main className="psy-app-main">
                {panel === 'inspector' && <Inspector />}
                {panel === 'console'   && <Console />}
                {panel === 'profiler'  && <Profiler />}
                {panel === 'assets'    && <AssetBrowser />}
                {panel === 'props'     && <PropSpawn />}
            </main>
        </div>
    );
}
