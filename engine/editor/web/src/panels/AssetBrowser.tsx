// SPDX-License-Identifier: MIT
// Psynder editor — Asset Browser panel. Surfaces the catalog of entries in
// the loaded `.lmpak` archives (DESIGN.md §10.7), grouped by category as
// collapsible disclosure groups. A search box does a case-insensitive
// substring match against the canonical VFS path, the pack name, and the
// content hash so users can locate an asset by any of those handles.
//
// Wave B scope: read-only display. Wave C wires real-data filters, drag-and-
// drop into the viewport, and a thumbnail preview pane.

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    AssetCatalog,
    AssetCategory,
    AssetDelta,
    AssetEntry,
    ConsoleEval,
    Envelope,
} from '../ipc/protocol';
import { console_command } from '../state/sceneCommands';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

const CATEGORY_LABELS: Record<AssetCategory, string> = {
    mesh:     'Meshes',
    texture:  'Textures',
    audio:    'Audio',
    level:    'Levels',
    script:   'Scripts',
    prefab:   'Prefabs',
    material: 'Materials',
    other:    'Other',
};

// Category display order — most level-design-relevant first, debug last.
const CATEGORY_ORDER: AssetCategory[] = [
    'mesh', 'texture', 'material', 'prefab', 'level', 'audio', 'script', 'other',
];

function format_bytes(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function matches(entry: AssetEntry, needle: string): boolean {
    if (!needle) return true;
    const n = needle.toLowerCase();
    return entry.path.toLowerCase().includes(n)
        || entry.pack.toLowerCase().includes(n)
        || entry.content_hash.toLowerCase().includes(n);
}

export function AssetBrowser() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    const [entries, set_entries] = React.useState<Map<string, AssetEntry>>(
        () => new Map(),
    );
    const [search, set_search] = React.useState('');
    const [collapsed, set_collapsed] = React.useState<Set<AssetCategory>>(
        () => new Set(),
    );
    const [status, set_status] = React.useState('');

    // ── Catalog + delta subscription ─────────────────────────────────────
    React.useEffect(() => {
        const unsub = client.subscribe('assets', (env: Envelope) => {
            if (env.type === 'catalog') {
                const cat = env.payload as AssetCatalog;
                set_entries(() => {
                    const m = new Map<string, AssetEntry>();
                    for (const e of cat.entries) m.set(e.path, e);
                    return m;
                });
            } else if (env.type === 'delta') {
                const d = env.payload as AssetDelta;
                set_entries((prev) => {
                    const m = new Map(prev);
                    for (const e of d.added ?? []) m.set(e.path, e);
                    for (const p of d.removed ?? []) m.delete(p);
                    return m;
                });
            }
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') client.send('assets', 'subscribe', {});
        });
        return unsub;
    }, [client]);

    // ── Group by category, applying the search filter ───────────────────
    const groups = React.useMemo(() => {
        const by_cat = new Map<AssetCategory, AssetEntry[]>();
        for (const e of entries.values()) {
            if (!matches(e, search)) continue;
            const bucket = by_cat.get(e.category) ?? [];
            bucket.push(e);
            by_cat.set(e.category, bucket);
        }
        // Stable ordering inside each group by path so the same asset always
        // sits on the same row across refreshes.
        for (const list of by_cat.values()) {
            list.sort((a, b) => a.path.localeCompare(b.path));
        }
        return by_cat;
    }, [entries, search]);

    const total_visible = React.useMemo(() => {
        let n = 0;
        for (const list of groups.values()) n += list.length;
        return n;
    }, [groups]);

    const toggle = (cat: AssetCategory) => {
        set_collapsed((prev) => {
            const next = new Set(prev);
            if (next.has(cat)) next.delete(cat);
            else next.add(cat);
            return next;
        });
    };

    const apply_texture_to_selection = React.useCallback((path: string) => {
        const id = Date.now() & 0x7fffffff;
        client.send<ConsoleEval>('console', 'eval', {
            id,
            source: console_command('material_texture_apply_selected', path),
            mode: 'console',
            quiet: true,
        });
        set_status(path === 'none' ? 'texture cleared' : 'texture applied');
    }, [client]);

    return (
        <div className="psy-panel psy-assets">
            <header className="psy-panel-header">
                <h2>Assets</h2>
                <ConnectionBadge />
                <input
                    type="search"
                    className="psy-input psy-assets-search"
                    placeholder="filter (path / pack / hash)"
                    value={search}
                    onChange={(e) => set_search(e.target.value)}
                    spellCheck={false}
                />
                <span className="psy-assets-count" title="visible / total">
                    {total_visible}/{entries.size}
                </span>
                {status && (
                    <span className="psy-assets-status" title={status}>
                        {status}
                    </span>
                )}
            </header>

            <div className="psy-assets-body">
                {entries.size === 0 && (
                    <div className="psy-empty">
                        No .lmpak archives mounted yet — waiting for catalog…
                    </div>
                )}

                {CATEGORY_ORDER.map((cat) => {
                    const list = groups.get(cat);
                    if (!list || list.length === 0) return null;
                    const is_collapsed = collapsed.has(cat);
                    return (
                        <section key={cat} className="psy-assets-group">
                            <button
                                type="button"
                                className="psy-assets-group-header"
                                onClick={() => toggle(cat)}
                                aria-expanded={!is_collapsed}
                            >
                                <span className={`psy-disclosure ${is_collapsed ? 'is-collapsed' : ''}`}>
                                    {is_collapsed ? '▸' : '▾'}
                                </span>
                                <span className="psy-assets-group-name">
                                    {CATEGORY_LABELS[cat]}
                                </span>
                                <span className="psy-assets-group-count">
                                    {list.length}
                                </span>
                            </button>

                            {!is_collapsed && (
                                <table className="psy-assets-table">
                                    <thead>
                                        <tr>
                                            <th>Path</th>
                                            <th>Pack</th>
                                            <th className="psy-cell-num">Size</th>
                                            <th className="psy-cell-action">Use</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        {list.map((e) => (
                                            <tr
                                                key={e.path}
                                                title={`${e.path}\n${e.pack}\n${e.content_hash}`}
                                            >
                                                <td className="psy-assets-path">{e.path}</td>
                                                <td className="psy-assets-pack">{e.pack}</td>
                                                <td className="psy-cell-num">
                                                    {format_bytes(e.size_bytes)}
                                                </td>
                                                <td className="psy-cell-action">
                                                    {e.category === 'texture' && (
                                                        <button
                                                            type="button"
                                                            className="psy-mini-button"
                                                            title="Apply texture to selected renderable"
                                                            onClick={() => apply_texture_to_selection(e.path)}
                                                        >
                                                            Apply
                                                        </button>
                                                    )}
                                                </td>
                                            </tr>
                                        ))}
                                        {cat === 'texture' && (
                                            <tr>
                                                <td className="psy-assets-path">none</td>
                                                <td className="psy-assets-pack">material</td>
                                                <td className="psy-cell-num">0 B</td>
                                                <td className="psy-cell-action">
                                                    <button
                                                        type="button"
                                                        className="psy-mini-button"
                                                        title="Clear selected renderable texture"
                                                        onClick={() => apply_texture_to_selection('none')}
                                                    >
                                                        Clear
                                                    </button>
                                                </td>
                                            </tr>
                                        )}
                                    </tbody>
                                </table>
                            )}
                        </section>
                    );
                })}
            </div>
        </div>
    );
}
