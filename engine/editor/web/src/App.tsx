// SPDX-License-Identifier: MIT
// Psynder editor — top-level router. Each panel is a separate Chrome window
// that hits one of `/panels/inspector`, `/panels/console`, `/panels/profiler`
// per DESIGN.md §10.6. In dev (vite) we use the `?panel=` query parameter.

import React from 'react';

import { Console } from './panels/Console';
import { Inspector } from './panels/Inspector';
import { Profiler } from './panels/Profiler';

type PanelName = 'inspector' | 'console' | 'profiler';

function pick_panel(): PanelName {
    if (typeof window === 'undefined') return 'inspector';
    // Path-based first (engine routes via /panels/<name>), then query string.
    const m = window.location.pathname.match(/\/panels\/(inspector|console|profiler)/);
    if (m) return m[1] as PanelName;
    const qp = new URLSearchParams(window.location.search).get('panel');
    if (qp === 'console' || qp === 'profiler') return qp;
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
                {(['inspector', 'console', 'profiler'] as const).map((name) => (
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
            </main>
        </div>
    );
}
