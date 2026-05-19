// SPDX-License-Identifier: MIT
// Psynder editor — Prop Spawn menu. Sandbox-mode UI for the Garry's Mod-style
// prop browser described in DESIGN.md §10.8: a searchable grid of prop
// thumbnails, grouped (visually) by category, that emits `spawn_prop`
// commands back to the engine on the `selection` channel.
//
// Wave B: grid + search + click-to-spawn. Wave C wires drag-to-place,
// favorites, and the genre filter chips (indoor FPS / racing / tactical).

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    Envelope,
    PropCatalog,
    PropEntry,
    SpawnPropCommand,
} from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

function matches(p: PropEntry, needle: string): boolean {
    if (!needle) return true;
    const n = needle.toLowerCase();
    if (p.name.toLowerCase().includes(n)) return true;
    if (p.id.toLowerCase().includes(n))   return true;
    if (p.category.toLowerCase().includes(n)) return true;
    for (const t of p.tags ?? []) {
        if (t.toLowerCase().includes(n)) return true;
    }
    return false;
}

// Tiny visual fallback when the engine doesn't supply a thumbnail.
function placeholder_thumb(p: PropEntry): string {
    const hue = (() => {
        let h = 0;
        for (let i = 0; i < p.id.length; i++) h = (h * 31 + p.id.charCodeAt(i)) | 0;
        return Math.abs(h) % 360;
    })();
    const svg =
        `<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">`
        + `<rect width="96" height="96" fill="hsl(${hue} 35% 35%)"/>`
        + `<text x="48" y="54" text-anchor="middle" font-family="monospace"`
        + ` font-size="11" fill="#eee">${p.name.slice(0, 10)}</text>`
        + `</svg>`;
    return `data:image/svg+xml;utf8,${encodeURIComponent(svg)}`;
}

export function PropSpawn() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    const [catalog, set_catalog] = React.useState<PropEntry[]>([]);
    const [search, set_search] = React.useState('');
    const [last_spawned, set_last_spawned] = React.useState<string | null>(null);
    const [category, set_category] = React.useState<string>('all');

    // ── Subscription ────────────────────────────────────────────────────
    React.useEffect(() => {
        const unsub = client.subscribe('props', (env: Envelope) => {
            if (env.type === 'catalog') {
                const c = env.payload as PropCatalog;
                set_catalog(c.props);
            }
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') client.send('props', 'subscribe', {});
        });
        return unsub;
    }, [client]);

    // ── Category filter chips ───────────────────────────────────────────
    const categories = React.useMemo(() => {
        const s = new Set<string>();
        for (const p of catalog) s.add(p.category);
        const arr = Array.from(s);
        arr.sort();
        return arr;
    }, [catalog]);

    const filtered = React.useMemo(() => {
        return catalog.filter((p) => {
            if (category !== 'all' && p.category !== category) return false;
            return matches(p, search);
        });
    }, [catalog, search, category]);

    const on_spawn = React.useCallback((p: PropEntry) => {
        client.send<SpawnPropCommand>('selection', 'spawn_prop', {
            prop_id: p.id,
        });
        set_last_spawned(p.id);
    }, [client]);

    return (
        <div className="psy-panel psy-props">
            <header className="psy-panel-header">
                <h2>Prop spawn</h2>
                <ConnectionBadge />
                <input
                    type="search"
                    className="psy-input psy-props-search"
                    placeholder="search props"
                    value={search}
                    onChange={(e) => set_search(e.target.value)}
                    spellCheck={false}
                />
            </header>

            <div className="psy-props-chips">
                <button
                    type="button"
                    className={`psy-chip ${category === 'all' ? 'is-active' : ''}`}
                    onClick={() => set_category('all')}
                >
                    all <span className="psy-chip-count">{catalog.length}</span>
                </button>
                {categories.map((c) => {
                    const count = catalog.filter((p) => p.category === c).length;
                    return (
                        <button
                            key={c}
                            type="button"
                            className={`psy-chip ${category === c ? 'is-active' : ''}`}
                            onClick={() => set_category(c)}
                        >
                            {c} <span className="psy-chip-count">{count}</span>
                        </button>
                    );
                })}
            </div>

            <div className="psy-props-body">
                {catalog.length === 0 && (
                    <div className="psy-empty">
                        No props loaded yet — waiting for catalog…
                    </div>
                )}
                {catalog.length > 0 && filtered.length === 0 && (
                    <div className="psy-empty">
                        No props match the current filter.
                    </div>
                )}

                {filtered.length > 0 && (
                    <div className="psy-props-grid">
                        {filtered.map((p) => {
                            const src = p.thumbnail_url ?? placeholder_thumb(p);
                            return (
                                <button
                                    key={p.id}
                                    type="button"
                                    className={`psy-prop-tile ${last_spawned === p.id ? 'is-recent' : ''}`}
                                    onClick={() => on_spawn(p)}
                                    title={`${p.name} (${p.category})\nclick to spawn`}
                                >
                                    <img
                                        className="psy-prop-thumb"
                                        src={src}
                                        alt={p.name}
                                        draggable={false}
                                    />
                                    <span className="psy-prop-name">{p.name}</span>
                                    <span className="psy-prop-category">{p.category}</span>
                                </button>
                            );
                        })}
                    </div>
                )}
            </div>

            {last_spawned && (
                <footer className="psy-props-footer">
                    spawned <code>{last_spawned}</code> — engine will place at cursor.
                </footer>
            )}
        </div>
    );
}
