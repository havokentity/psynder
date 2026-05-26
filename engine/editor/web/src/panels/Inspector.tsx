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
    ConsoleResult,
    EditorCommandAck,
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
import { console_arg } from '../state/sceneCommands';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';
import {
    MATERIAL_PRESETS,
    material_preset_apply_command,
    material_preset_component_values,
    type MaterialPresetId,
} from './PrimitiveMaterialPalette';

type EditAckStatus = 'pending' | 'applied' | 'error';

interface PendingEdit {
    entity_id: number;
    component: string;
    field: string;
    value: unknown;
    sent_at: number;
    fallback_request_id?: number;
}

interface InspectorEditAck {
    status: EditAckStatus;
    text: string;
    sent_at: number;
}

export function Inspector() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const edit_console_seq = React.useRef(42000);

    const [schemas, set_schemas] = React.useState<Map<string, ComponentSchema>>(
        () => new Map(),
    );
    const [selection, set_selection] = React.useState<SelectionState | null>(null);
    const [dirty, set_dirty] = React.useState(() => editor_scene_dirty());
    const [edit_ack, set_edit_ack] = React.useState<InspectorEditAck | null>(null);
    const pending_edit = React.useRef<PendingEdit | null>(null);
    const fallback_timer = React.useRef<number | null>(null);

    React.useEffect(() => subscribe_editor_scene_dirty(set_dirty), []);

    const clear_fallback_timer = React.useCallback(() => {
        if (fallback_timer.current === null) return;
        window.clearTimeout(fallback_timer.current);
        fallback_timer.current = null;
    }, []);

    React.useEffect(() => () => clear_fallback_timer(), [clear_fallback_timer]);

    const settle_edit_ack = React.useCallback((status: EditAckStatus, text: string) => {
        clear_fallback_timer();
        pending_edit.current = null;
        set_edit_ack({ status, text, sent_at: Date.now() });
    }, [clear_fallback_timer]);

    const pending_matches = React.useCallback((
        entity_id: number | undefined,
        component: string | undefined,
        field: string | undefined,
    ) => {
        const edit = pending_edit.current;
        if (!edit) return false;
        return (entity_id === undefined || edit.entity_id === entity_id)
            && (component === undefined || edit.component === component)
            && (field === undefined || edit.field === field);
    }, []);

    const settle_from_selection_value = React.useCallback((
        entity_id: number,
        component: string,
        field: string,
        value: unknown,
    ) => {
        const edit = pending_edit.current;
        if (!edit) return;
        if (!pending_matches(entity_id, component, field)) return;
        if (!values_equal(edit.value, value)) return;
        settle_edit_ack('applied', `applied ${component}.${field}`);
    }, [pending_matches, settle_edit_ack]);

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
                const next = env.payload as SelectionState;
                set_selection(next);
                const edit = pending_edit.current;
                if (edit && edit.entity_id === next.entity_id) {
                    const value = next.components[edit.component]?.[edit.field];
                    settle_from_selection_value(edit.entity_id, edit.component, edit.field, value);
                }
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
                settle_from_selection_value(p.entity_id, p.component, p.field, p.value);
            } else if (env.type === 'command_ack') {
                const ack = env.payload as EditorCommandAck & {
                    entity_id?: number;
                    component?: string;
                    field?: string;
                };
                if (
                    (ack.command === COMPONENT_EDIT_TYPE || ack.command === 'component_field_edit.v1') &&
                    pending_matches(ack.entity_id, ack.component, ack.field)
                ) {
                    settle_edit_ack(
                        ack.ok ? 'applied' : 'error',
                        ack.text || (ack.ok ? 'edit applied' : 'edit failed'),
                    );
                }
            } else if (env.type === 'cleared') {
                set_selection(null);
            }
        });
        return unsub;
    }, [client, pending_matches, settle_edit_ack, settle_from_selection_value]);

    React.useEffect(() => {
        const unsub = client.subscribe('scene', (env: Envelope) => {
            if (env.type !== 'dirty_state') return;
            const state = env.payload as SceneDirtyState;
            set_editor_scene_dirty(!!state.dirty);
        });
        return unsub;
    }, [client]);

    React.useEffect(() => {
        const unsub = client.subscribe('console', (env: Envelope) => {
            if (env.type !== 'result') return;
            const result = env.payload as ConsoleResult;
            const edit = pending_edit.current;
            if (!edit || edit.fallback_request_id !== result.id) return;
            settle_edit_ack(
                result.ok ? 'applied' : 'error',
                result.text || (result.ok ? `applied ${edit.component}.${edit.field}` : 'edit failed'),
            );
        });
        return unsub;
    }, [client, settle_edit_ack]);

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
                client.send('console',   'subscribe', {});
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
            clear_fallback_timer();
            const sent_at = Date.now();
            pending_edit.current = {
                entity_id: selection.entity_id,
                component,
                field: field.name,
                value,
                sent_at,
            };
            set_edit_ack({
                status: 'pending',
                text: `sent ${component}.${field.name}`,
                sent_at,
            });
            fallback_timer.current = window.setTimeout(() => {
                const edit = pending_edit.current;
                if (!edit || edit.sent_at !== sent_at || edit.fallback_request_id !== undefined) return;
                const request_id = ++edit_console_seq.current;
                pending_edit.current = { ...edit, fallback_request_id: request_id };
                set_edit_ack({
                    status: 'pending',
                    text: `waiting for ${component}.${field.name} echo`,
                    sent_at,
                });
                client.send<ConsoleEval>('console', 'eval', {
                    id: request_id,
                    source: component_set_command(edit.entity_id, edit.component, edit.field, edit.value),
                    mode: 'console',
                });
            }, 650);
            mark_editor_scene_dirty();
        },
        [clear_fallback_timer, client, selection],
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
                {edit_ack && (
                    <span className={`psy-edit-ack is-${edit_ack.status}`} aria-live="polite">
                        {edit_ack.text}
                    </span>
                )}
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
        console_arg(component),
        console_arg(field),
        encode_component_value(value),
    ].join(' ');
}

function encode_component_value(value: unknown): string {
    if (Array.isArray(value)) return `[${value.map((v) => Number(v) || 0).join(',')}]`;
    if (typeof value === 'boolean') return value ? 'true' : 'false';
    if (typeof value === 'number') return String(value);
    return JSON.stringify(String(value ?? ''));
}

function values_equal(a: unknown, b: unknown): boolean {
    if (Object.is(a, b)) return true;
    try {
        return JSON.stringify(a) === JSON.stringify(b);
    } catch {
        return false;
    }
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
