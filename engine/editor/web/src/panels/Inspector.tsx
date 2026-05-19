// SPDX-License-Identifier: MIT
// Psynder editor — Inspector panel. Subscribes to the `schemas` channel for
// the PSYNDER_COMPONENT registry plus `selection` for the currently-selected
// entity. Each component on the selection is rendered as a SchemaForm; user
// edits are pushed back to the engine on the same channel as `set` envelopes.

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    ComponentSchema,
    Envelope,
    SchemaCatalog,
    SchemaDelta,
    SelectionPatch,
    SelectionState,
} from '../ipc/protocol';
import { SchemaForm } from '../schema/Form';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

export function Inspector() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    const [schemas, set_schemas] = React.useState<Map<string, ComponentSchema>>(
        () => new Map(),
    );
    const [selection, set_selection] = React.useState<SelectionState | null>(null);

    // ── Schemas subscription ────────────────────────────────────────────
    React.useEffect(() => {
        const unsub = client.subscribe('schemas', (env: Envelope) => {
            if (env.type === 'catalog') {
                const cat = env.payload as SchemaCatalog;
                set_schemas(() => {
                    const m = new Map<string, ComponentSchema>();
                    for (const c of cat.components) m.set(c.name, c);
                    return m;
                });
            } else if (env.type === 'delta') {
                const d = env.payload as SchemaDelta;
                set_schemas((prev) => {
                    const m = new Map(prev);
                    for (const c of d.added ?? []) m.set(c.name, c);
                    for (const n of d.removed ?? []) m.delete(n);
                    return m;
                });
            }
        });
        return unsub;
    }, [client]);

    // ── Selection subscription ──────────────────────────────────────────
    React.useEffect(() => {
        const unsub = client.subscribe('selection', (env: Envelope) => {
            if (env.type === 'state') {
                set_selection(env.payload as SelectionState);
            } else if (env.type === 'patch') {
                const p = env.payload as SelectionPatch;
                set_selection((prev) => {
                    if (!prev || prev.entity_id !== p.entity_id) return prev;
                    const next: SelectionState = {
                        ...prev,
                        components: {
                            ...prev.components,
                            [p.component]: {
                                ...(prev.components[p.component] ?? {}),
                                [p.field]: p.value,
                            },
                        },
                    };
                    return next;
                });
            } else if (env.type === 'cleared') {
                set_selection(null);
            }
        });
        return unsub;
    }, [client]);

    // ── Subscribe-request hint ──────────────────────────────────────────
    //
    // The first time the socket opens we send a `subscribe` envelope so the
    // engine knows this panel wants schema + selection streams. Resilient to
    // multiple opens (auto-reconnect): the engine treats it as idempotent.
    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') {
                client.send('schemas',   'subscribe', {});
                client.send('selection', 'subscribe', {});
            }
        });
        return unsub;
    }, [client]);

    const on_field_change = React.useCallback(
        (component: string, field: string, value: unknown) => {
            if (!selection) return;
            // Optimistic local update so the input stays in sync.
            set_selection({
                ...selection,
                components: {
                    ...selection.components,
                    [component]: {
                        ...(selection.components[component] ?? {}),
                        [field]: value,
                    },
                },
            });
            client.send<SelectionPatch>('selection', 'set', {
                entity_id: selection.entity_id,
                component,
                field,
                value,
            });
        },
        [client, selection],
    );

    return (
        <div className="psy-panel psy-inspector">
            <header className="psy-panel-header">
                <h2>Inspector</h2>
                <ConnectionBadge />
            </header>

            {!selection && (
                <div className="psy-empty">
                    Nothing selected. Click an entity in the viewport.
                </div>
            )}

            {selection && (
                <div className="psy-inspector-body">
                    <div className="psy-entity-banner">
                        <span className="psy-entity-id">#{selection.entity_id}</span>
                        {selection.entity_label && (
                            <span className="psy-entity-label">{selection.entity_label}</span>
                        )}
                    </div>

                    {Object.entries(selection.components).map(([name, values]) => {
                        const schema = schemas.get(name);
                        return (
                            <ComponentBlock
                                key={name}
                                name={name}
                                schema={schema}
                                values={values}
                                on_change={(field, v) => on_field_change(name, field, v)}
                            />
                        );
                    })}

                    {schemas.size === 0 && (
                        <div className="psy-empty psy-empty-warning">
                            No component schemas received yet.
                        </div>
                    )}
                </div>
            )}
        </div>
    );
}

interface ComponentBlockProps {
    name: string;
    schema?: ComponentSchema;
    values: Record<string, unknown>;
    on_change: (field: string, value: unknown) => void;
}

function ComponentBlock({ name, schema, values, on_change }: ComponentBlockProps) {
    const [collapsed, set_collapsed] = React.useState(false);
    return (
        <section className="psy-component">
            <button
                type="button"
                className="psy-component-header"
                onClick={() => set_collapsed((c) => !c)}
                aria-expanded={!collapsed}
            >
                <span className={`psy-disclosure ${collapsed ? 'is-collapsed' : ''}`}>
                    {collapsed ? '▸' : '▾'}
                </span>
                <span className="psy-component-name">{name}</span>
            </button>

            {!collapsed && (
                schema
                    ? <SchemaForm schema={schema} values={values} on_change={on_change} />
                    : (
                        <div className="psy-empty psy-empty-warning">
                            No schema registered for <code>{name}</code> — values shown raw.
                            <pre className="psy-raw">{JSON.stringify(values, null, 2)}</pre>
                        </div>
                    )
            )}
        </section>
    );
}
