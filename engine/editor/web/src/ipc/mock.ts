// SPDX-License-Identifier: MIT
// Psynder editor IPC — mock data. When the WS to the engine is unreachable
// (e.g. `npm run dev` without a running engine, or storybook), we drive the
// React panels with a representative trickle of frames so the UI is still
// exercisable in isolation. The mock attaches *only* to the client's listener
// fan-out — it never holds a real socket — so the moment the engine comes up
// the real frames take over.

import type {
    ComponentSchema,
    ConsoleLog,
    Envelope,
    ProfilerFrame,
    SchemaCatalog,
    SelectionState,
} from './protocol';
import { PROTOCOL_VERSION } from './protocol';

const DEMO_SCHEMAS: ComponentSchema[] = [
    {
        name: 'Transform',
        layout_hash: 'mock-transform-v1',
        fields: [
            { name: 'position', kind: 'vec3', numeric: { step: 0.01, unit: 'm' } },
            { name: 'rotation', kind: 'quat' },
            { name: 'scale',    kind: 'vec3', numeric: { step: 0.01, min: 0.001 } },
        ],
    },
    {
        name: 'Visible',
        layout_hash: 'mock-visible-v1',
        fields: [
            { name: 'enabled',  kind: 'bool', help: 'Skip render when false.' },
            { name: 'tint',     kind: 'color' },
            { name: 'cast_shadows', kind: 'bool' },
            {
                name: 'pass',
                kind: 'enum',
                enum: {
                    options: [
                        { label: 'Opaque',      value: 0 },
                        { label: 'Transparent', value: 1 },
                        { label: 'Decal',       value: 2 },
                    ],
                },
            },
        ],
    },
    {
        name: 'RigidBody',
        layout_hash: 'mock-rb-v1',
        fields: [
            { name: 'mass',    kind: 'f32', numeric: { min: 0, step: 0.1, unit: 'kg' } },
            { name: 'kinematic', kind: 'bool' },
            { name: 'linear_damping',  kind: 'f32', numeric: { min: 0, max: 1, step: 0.01 } },
            { name: 'angular_damping', kind: 'f32', numeric: { min: 0, max: 1, step: 0.01 } },
            { name: 'name',    kind: 'string', readonly: true },
        ],
    },
];

const DEMO_SELECTION: SelectionState = {
    entity_id: 0x4_2_7,
    entity_label: 'crate_red.01 (mock)',
    components: {
        Transform: {
            position: [1.5, 0.0, -3.25],
            rotation: [0, 0, 0, 1],
            scale: [1, 1, 1],
        },
        Visible: {
            enabled: true,
            tint: 0xFFB04020,
            cast_shadows: true,
            pass: 0,
        },
        RigidBody: {
            mass: 12.5,
            kinematic: false,
            linear_damping: 0.05,
            angular_damping: 0.10,
            name: 'crate_red.01',
        },
    },
};

export interface MockDriver {
    stop(): void;
}

type Deliver = (env: Envelope) => void;

/** Start emitting demo frames into a listener. Caller owns the lifetime. */
export function start_mock(deliver: Deliver): MockDriver {
    let alive = true;

    const send = (env: Envelope) => {
        if (!alive) return;
        try { deliver(env); }
        catch (err) {
            // eslint-disable-next-line no-console
            console.error('[psynder mock] listener threw', err);
        }
    };

    // ── Initial schema catalog + selection ───────────────────────────────
    const catalog: SchemaCatalog = { components: DEMO_SCHEMAS };
    send({ v: PROTOCOL_VERSION, ch: 'schemas',   type: 'catalog', payload: catalog });
    send({ v: PROTOCOL_VERSION, ch: 'selection', type: 'state',   payload: DEMO_SELECTION });

    // ── Profiler — emulate a 60 Hz frame stream ─────────────────────────
    let frame_idx = 0;
    const profiler_handle = setInterval(() => {
        if (!alive) return;
        frame_idx += 1;
        const t = frame_idx * 0.016;
        // Deterministic-ish wiggle so the strip-chart shows life without RNG.
        const wiggle = (a: number, b: number) =>
            a + b * Math.sin(t * (1 + a * 0.13));
        const cpu = 5.5 + wiggle(0.3, 1.6);
        const gpu = 1.1 + wiggle(0.5, 0.4);
        const sections = [
            { name: 'scene',    ms: 0.4 + wiggle(0.1, 0.2) },
            { name: 'render',   ms: cpu * 0.55 },
            { name: 'physics',  ms: cpu * 0.20 },
            { name: 'audio',    ms: cpu * 0.05 },
            { name: 'ui',       ms: cpu * 0.07 },
        ];
        const frame: ProfilerFrame = {
            frame: frame_idx,
            cpu_ms: cpu,
            gpu_ms: gpu,
            sections,
        };
        send({ v: PROTOCOL_VERSION, ch: 'profiler', type: 'frame', payload: frame });
    }, 60);

    // ── Console — periodic info line ─────────────────────────────────────
    let log_idx = 0;
    const log_handle = setInterval(() => {
        if (!alive) return;
        log_idx += 1;
        const line: ConsoleLog = {
            level: 'info',
            ts: Date.now(),
            tag: 'mock',
            text: `engine offline — emitting demo frame ${log_idx}`,
        };
        send({ v: PROTOCOL_VERSION, ch: 'console', type: 'log', payload: line });
    }, 2500);

    return {
        stop() {
            alive = false;
            clearInterval(profiler_handle);
            clearInterval(log_handle);
        },
    };
}

/** Synchronously generate a single mock catalog — used by tests/storybook. */
export function mock_catalog(): SchemaCatalog {
    return { components: DEMO_SCHEMAS };
}

/** Synchronously generate a single mock selection — used by tests/storybook. */
export function mock_selection(): SelectionState {
    return DEMO_SELECTION;
}
