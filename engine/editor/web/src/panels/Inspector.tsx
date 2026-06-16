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
    SelectionComponentAdd,
    SelectionComponentRemove,
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

// Components the Inspector is allowed to remove (inverse of Add Component).
// Only the optional authoring components are removable; structural/foundational
// ones (TransformComponent/SceneNodeComponent) and the render data
// (RenderableComponent/CameraComponent/MaterialComponent) are NOT — dropping
// them would corrupt the entity. Keyed by the full component name as it appears
// in `selection.components`. The value is the short label the host accepts.
const REMOVABLE_COMPONENTS: Readonly<Record<string, string>> = {
    RigidBodyComponent: 'RigidBody',
    VehicleComponent: 'Vehicle',
    HelicopterComponent: 'Helicopter',
    CharacterControllerComponent: 'CharacterController',
    LightComponent: 'Light',
    FactionComponent: 'Faction',
    HitboxComponent: 'Hitbox',
    WeaponModeComponent: 'WeaponMode',
    AiAgentComponent: 'AiAgent',
    PerceptionComponent: 'Perception',
    PatrolComponent: 'Patrol',
};

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
    const [add_status, set_add_status] = React.useState<InspectorEditAck | null>(null);
    const pending_edit = React.useRef<PendingEdit | null>(null);
    const fallback_timer = React.useRef<number | null>(null);
    const ack_dismiss_timer = React.useRef<number | null>(null);
    const add_dismiss_timer = React.useRef<number | null>(null);

    React.useEffect(() => subscribe_editor_scene_dirty(set_dirty), []);

    // Auto-dismiss the transient edit/add toasts a couple seconds after they
    // settle (#62b). A `pending` toast stays put until it resolves; only
    // terminal `applied` / `error` states schedule a clear.
    React.useEffect(() => {
        if (ack_dismiss_timer.current !== null) {
            window.clearTimeout(ack_dismiss_timer.current);
            ack_dismiss_timer.current = null;
        }
        if (!edit_ack || edit_ack.status === 'pending') return;
        ack_dismiss_timer.current = window.setTimeout(() => {
            ack_dismiss_timer.current = null;
            set_edit_ack(null);
        }, 2500);
        return () => {
            if (ack_dismiss_timer.current !== null) {
                window.clearTimeout(ack_dismiss_timer.current);
                ack_dismiss_timer.current = null;
            }
        };
    }, [edit_ack]);

    React.useEffect(() => {
        if (add_dismiss_timer.current !== null) {
            window.clearTimeout(add_dismiss_timer.current);
            add_dismiss_timer.current = null;
        }
        if (!add_status || add_status.status === 'pending') return;
        add_dismiss_timer.current = window.setTimeout(() => {
            add_dismiss_timer.current = null;
            set_add_status(null);
        }, 2500);
        return () => {
            if (add_dismiss_timer.current !== null) {
                window.clearTimeout(add_dismiss_timer.current);
                add_dismiss_timer.current = null;
            }
        };
    }, [add_status]);

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
                } else if (ack.command === 'add_component') {
                    set_add_status({
                        status: ack.ok ? 'applied' : 'error',
                        text: ack.text || (ack.ok ? 'component added' : 'add component failed'),
                        sent_at: Date.now(),
                    });
                } else if (ack.command === 'remove_component') {
                    set_add_status({
                        status: ack.ok ? 'applied' : 'error',
                        text: ack.text || (ack.ok ? 'component removed' : 'remove component failed'),
                        sent_at: Date.now(),
                    });
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

    const add_component = React.useCallback((component: string, variant?: string) => {
        if (!selection) return;
        const label = variant ? `${component} (${variant})` : component;
        set_add_status({ status: 'pending', text: `adding ${label}...`, sent_at: Date.now() });
        client.send<SelectionComponentAdd>('selection', 'add_component', {
            entity_id: selection.entity_id,
            component,
            ...(variant ? { variant } : {}),
        });
        mark_editor_scene_dirty();
    }, [client, selection]);

    // Inverse of add_component: strip a removable authoring component off the
    // selection. `component` is the host label (e.g. "RigidBody"); `display` is
    // the full name shown in the panel header. Targets the entity carried in the
    // payload (host validates liveness at apply time) — same discipline as add.
    const remove_component = React.useCallback((component: string, display: string) => {
        if (!selection) return;
        set_add_status({ status: 'pending', text: `removing ${display}...`, sent_at: Date.now() });
        client.send<SelectionComponentRemove>('selection', 'remove_component', {
            entity_id: selection.entity_id,
            component,
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

            {edit_ack && (
                <div
                    className={`psy-edit-toast is-${edit_ack.status}`}
                    role="status"
                    aria-live="polite"
                >
                    {edit_ack.text}
                </div>
            )}

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
                        const removable_label = REMOVABLE_COMPONENTS[name];
                        return (
                            <ComponentBlock
                                key={name}
                                name={name}
                                schema={schema}
                                values={values}
                                on_apply_material_preset={
                                    name === 'MaterialComponent' ? apply_material_preset : undefined
                                }
                                on_remove={
                                    removable_label
                                        ? () => remove_component(removable_label, name)
                                        : undefined
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

                    <section className="psy-add-component">
                        <div className="psy-add-component-title">Add component</div>
                        <div className="psy-add-component-row">
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'RigidBodyComponent' in selection.components}
                                onClick={() => add_component('RigidBody')}
                                title="Add a dynamic RigidBody so this object simulates in Play mode"
                            >
                                RigidBody (dynamic)
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'RigidBodyComponent' in selection.components}
                                onClick={() => add_component('RigidBody', 'static')}
                                title="Add a static (zero-mass) RigidBody - floors, walls, fixed props"
                            >
                                RigidBody (static)
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'VehicleComponent' in selection.components}
                                onClick={() => add_component('Vehicle')}
                                title="Add a drivable Vehicle so this object accepts WASD in Play mode"
                            >
                                Vehicle
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'HelicopterComponent' in selection.components}
                                onClick={() => add_component('Helicopter')}
                                title="Add a flyable Helicopter so this object accepts flight input in Play mode"
                            >
                                Helicopter
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'CharacterControllerComponent' in selection.components}
                                onClick={() => add_component('CharacterController')}
                                title="Add a kinematic CharacterController capsule for walk-around play"
                            >
                                CharacterController
                            </button>
                        </div>
                        <div className="psy-add-component-title">Gameplay &amp; AI</div>
                        <div className="psy-add-component-row">
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'FactionComponent' in selection.components}
                                onClick={() => add_component('Faction')}
                                title="Tag this entity's team id so combat knows friend from foe"
                            >
                                Faction
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'HitboxComponent' in selection.components}
                                onClick={() => add_component('Hitbox')}
                                title="Add a damage hitbox so weapons + projectiles can hit this entity in Play"
                            >
                                Hitbox
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'WeaponModeComponent' in selection.components}
                                onClick={() => add_component('WeaponMode')}
                                title="Set the weapon fire kind (hitscan / projectile) and projectile params"
                            >
                                WeaponMode
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'AiAgentComponent' in selection.components}
                                onClick={() => add_component('AiAgent')}
                                title="Make this entity an AI soldier that perceives, chases and fires in Play"
                            >
                                AI Agent
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'PerceptionComponent' in selection.components}
                                onClick={() => add_component('Perception')}
                                title="Add a perception sense (needed for an AI agent to acquire targets)"
                            >
                                Perception
                            </button>
                            <button
                                type="button"
                                className="psy-button"
                                disabled={'PatrolComponent' in selection.components}
                                onClick={() => add_component('Patrol')}
                                title="Add a patrol route so an idle AI agent walks waypoints in Play"
                            >
                                Patrol
                            </button>
                        </div>
                        {add_status && (
                            <div className={`psy-edit-toast is-${add_status.status}`} role="status" aria-live="polite">
                                {add_status.text}
                            </div>
                        )}
                    </section>
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
    on_remove?: () => void;
    on_change: (field: FieldSchema, value: unknown) => void;
}

function ComponentBlock({
    name,
    schema,
    values,
    on_apply_material_preset,
    on_remove,
    on_change,
}: ComponentBlockProps) {
    const [collapsed, set_collapsed] = React.useState(false);
    return (
        <section className="psy-component">
            <div className="psy-component-header-row">
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
                {on_remove && (
                    <button
                        type="button"
                        className="psy-component-remove"
                        onClick={on_remove}
                        title={`Remove ${name} from this entity`}
                        aria-label={`Remove ${name}`}
                    >
                        ✕
                    </button>
                )}
            </div>

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
