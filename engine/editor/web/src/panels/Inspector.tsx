// SPDX-License-Identifier: MIT
// Psynder editor — Inspector panel. Subscribes to the `schemas` channel for
// the PSYNDER_COMPONENT registry plus `selection` for the currently-selected
// entity. Each component on the selection is rendered as a SchemaForm; user
// edits are pushed back to the engine as explicit component-edit intents.

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    ComponentSchema,
    ConsoleEval,
    Envelope,
    FieldSchema,
    SchemaCatalog,
    SchemaDelta,
    SceneDirtyState,
    SelectionPatch,
    SelectionState,
} from '../ipc/protocol';
import {
    COMPONENT_EDIT_CHANNEL,
    COMPONENT_EDIT_TYPE,
    build_component_edit_intent,
    type ComponentEditIntent,
} from '../schema/editIntent';
import { SchemaForm } from '../schema/Form';
import {
    editor_scene_dirty,
    mark_editor_scene_dirty,
    set_editor_scene_dirty,
    subscribe_editor_scene_dirty,
} from '../state/editorPersistence';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';
import {
    MATERIAL_PRESETS,
    material_preset_apply_command,
    material_preset_component_values,
    type MaterialPresetId,
} from './PrimitiveMaterialPalette';

export function Inspector() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const edit_console_seq = React.useRef(42000);

    const [schemas, set_schemas] = React.useState<Map<string, ComponentSchema>>(
        () => new Map(),
    );
    const [selection, set_selection] = React.useState<SelectionState | null>(null);
    const [dirty, set_dirty] = React.useState(() => editor_scene_dirty());

    React.useEffect(() => subscribe_editor_scene_dirty(set_dirty), []);

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

    React.useEffect(() => {
        const unsub = client.subscribe('scene', (env: Envelope) => {
            if (env.type !== 'dirty_state') return;
            const state = env.payload as SceneDirtyState;
            set_editor_scene_dirty(!!state.dirty);
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
                client.send('scene',     'subscribe', {});
            }
        });
        return unsub;
    }, [client]);

    const on_field_change = React.useCallback(
        (component: string, schema: ComponentSchema, field: FieldSchema, value: unknown) => {
            if (!selection) return;
            const previous_value = selection.components[component]?.[field.name];
            // Optimistic local update so the input stays in sync.
            set_selection({
                ...selection,
                components: {
                    ...selection.components,
                    [component]: {
                        ...(selection.components[component] ?? {}),
                        [field.name]: value,
                    },
                },
            });
            client.send<ComponentEditIntent>(
                COMPONENT_EDIT_CHANNEL,
                COMPONENT_EDIT_TYPE,
                build_component_edit_intent({
                    selection,
                    component,
                    schema,
                    field,
                    value,
                    previous_value,
                }),
            );
            client.send<ConsoleEval>('console', 'eval', {
                id: ++edit_console_seq.current,
                source: component_set_command(selection.entity_id, component, field.name, value),
                mode: 'console',
                quiet: true,
            });
            mark_editor_scene_dirty();
        },
        [client, selection],
    );

    const apply_material_preset = React.useCallback((preset_id: MaterialPresetId) => {
        if (!selection) return;
        set_selection({
            ...selection,
            components: {
                ...selection.components,
                MaterialComponent: {
                    ...(selection.components.MaterialComponent ?? {}),
                    ...material_preset_component_values(preset_id, selection.entity_id),
                },
            },
        });
        client.send<ConsoleEval>('console', 'eval', {
            id: ++edit_console_seq.current,
            source: material_preset_apply_command(selection.entity_id, preset_id),
            mode: 'console',
            quiet: true,
        });
        mark_editor_scene_dirty();
    }, [client, selection]);

    return (
        <div className="psy-panel psy-inspector">
            <header className="psy-panel-header">
                <h2>Inspector</h2>
                <ConnectionBadge />
                <span className={`psy-dirty-pill ${dirty ? 'is-dirty' : ''}`}>
                    {dirty ? 'modified' : 'saved'}
                </span>
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
                                on_apply_material_preset={
                                    name === 'MaterialComponent' ? apply_material_preset : undefined
                                }
                                on_change={(field, v) => {
                                    if (schema) on_field_change(name, schema, field, v);
                                }}
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

function component_set_command(
    entity_id: number,
    component: string,
    field: string,
    value: unknown,
): string {
    return [
        'component_set',
        String(entity_id),
        component,
        field,
        encode_component_value(value),
    ].join(' ');
}

function encode_component_value(value: unknown): string {
    if (Array.isArray(value)) return `[${value.map((v) => Number(v) || 0).join(',')}]`;
    if (typeof value === 'boolean') return value ? 'true' : 'false';
    if (typeof value === 'number') return String(value);
    return JSON.stringify(String(value ?? ''));
}

interface ComponentBlockProps {
    name: string;
    schema?: ComponentSchema;
    values: Record<string, unknown>;
    on_apply_material_preset?: (preset: MaterialPresetId) => void;
    on_change: (field: FieldSchema, value: unknown) => void;
}

function ComponentBlock({
    name,
    schema,
    values,
    on_apply_material_preset,
    on_change,
}: ComponentBlockProps) {
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
                    ? (
                        <>
                            {on_apply_material_preset && (
                                <div className="psy-material-preset-strip" aria-label="Apply material preset">
                                    {MATERIAL_PRESETS.map((preset) => (
                                        <button
                                            key={preset.id}
                                            type="button"
                                            className="psy-material-swatch"
                                            style={{ '--psy-swatch': preset.metadata.albedo } as React.CSSProperties}
                                            onClick={() => on_apply_material_preset(preset.id)}
                                            title={`Apply ${preset.label} material`}
                                            aria-label={`Apply ${preset.label} material`}
                                        >
                                            <span />
                                        </button>
                                    ))}
                                </div>
                            )}
                            <SchemaForm schema={schema} values={values} on_change={on_change} />
                        </>
                    )
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
