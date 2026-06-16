// SPDX-License-Identifier: MIT
// Psynder editor IPC — mock data. When the WS to the engine is unreachable
// (e.g. `npm run dev` without a running engine, or storybook), we drive the
// React panels with a representative trickle of frames so the UI is still
// exercisable in isolation. The mock attaches *only* to the client's listener
// fan-out — it never holds a real socket — so the moment the engine comes up
// the real frames take over.

import type {
    AssetCatalog,
    AssetEntry,
    ComponentSchema,
    ConsoleLog,
    ConsoleResult,
    Envelope,
    ProfilerFrame,
    PsyGraphDocument,
    PropCatalog,
    PropEntry,
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
            { name: 'rotation', kind: 'vec3', numeric: { step: 0.1, unit: 'deg' } },
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
            rotation: [0, 0, 0],
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

// Representative `.lmpak` entries — Wave C will replace with live data.
const DEMO_ASSETS: AssetEntry[] = [
    { path: 'meshes/crate_red.lmm',     category: 'mesh',    pack: 'sandbox.lmpak', size_bytes:  18_240, content_hash: 'fnv-mesh-crate-red' },
    { path: 'meshes/crate_blue.lmm',    category: 'mesh',    pack: 'sandbox.lmpak', size_bytes:  18_240, content_hash: 'fnv-mesh-crate-blue' },
    { path: 'meshes/barrel_metal.lmm',  category: 'mesh',    pack: 'sandbox.lmpak', size_bytes:  24_512, content_hash: 'fnv-mesh-barrel' },
    { path: 'meshes/lamp_post.lmm',     category: 'mesh',    pack: 'sandbox.lmpak', size_bytes:  31_104, content_hash: 'fnv-mesh-lamp' },
    { path: 'textures/crate_red_diff.lmt',   category: 'texture', pack: 'sandbox.lmpak', size_bytes: 262_144, content_hash: 'fnv-tex-crate-red-d' },
    { path: 'textures/crate_red_norm.lmt',   category: 'texture', pack: 'sandbox.lmpak', size_bytes: 262_144, content_hash: 'fnv-tex-crate-red-n' },
    { path: 'textures/concrete_01.lmt', category: 'texture', pack: 'core.lmpak',    size_bytes: 524_288, content_hash: 'fnv-tex-concrete' },
    { path: 'audio/impact_wood.lma',    category: 'audio',   pack: 'core.lmpak',    size_bytes:  48_000, content_hash: 'fnv-aud-impact-w' },
    { path: 'audio/impact_metal.lma',   category: 'audio',   pack: 'core.lmpak',    size_bytes:  52_000, content_hash: 'fnv-aud-impact-m' },
    { path: 'audio/ambient_room.lma',   category: 'audio',   pack: 'core.lmpak',    size_bytes: 192_000, content_hash: 'fnv-aud-ambient' },
    { path: 'levels/crate_room.psylevel',    category: 'level',   pack: 'sandbox.lmpak', size_bytes:  72_192, content_hash: 'fnv-lvl-crate-room' },
    { path: 'levels/test_arena.psylevel',    category: 'level',   pack: 'sandbox.lmpak', size_bytes: 124_864, content_hash: 'fnv-lvl-arena' },
    { path: 'scripts/sandbox_init.lua', category: 'script',  pack: 'sandbox.lmpak', size_bytes:   2_048, content_hash: 'fnv-scr-init' },
    { path: 'scripts/physgun.lua',      category: 'script',  pack: 'sandbox.lmpak', size_bytes:   8_192, content_hash: 'fnv-scr-physgun' },
    { path: 'prefabs/crate_stack.lmprefab',  category: 'prefab',  pack: 'sandbox.lmpak', size_bytes:   1_312, content_hash: 'fnv-pfb-stack' },
    { path: 'materials/metal_rough.lmmat',   category: 'material', pack: 'core.lmpak', size_bytes:     512, content_hash: 'fnv-mat-metal' },
];

// Representative props — paired with a tiny inline-svg thumbnail so the
// menu grid is non-empty even without the engine's HTTP image side.
function tile_thumb(label: string, fill: string): string {
    const svg =
        `<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">`
        + `<rect width="96" height="96" fill="${fill}"/>`
        + `<text x="48" y="54" text-anchor="middle" font-family="monospace"`
        + ` font-size="12" fill="#000">${label}</text>`
        + `</svg>`;
    return `data:image/svg+xml;utf8,${encodeURIComponent(svg)}`;
}

const DEMO_PROPS: PropEntry[] = [
    { id: 'crate_red',    name: 'Crate (red)',    category: 'crates',    thumbnail_url: tile_thumb('crate', '#b8533a'), tags: ['wood', 'storage', 'sandbox'] },
    { id: 'crate_blue',   name: 'Crate (blue)',   category: 'crates',    thumbnail_url: tile_thumb('crate', '#3a78b8'), tags: ['wood', 'storage'] },
    { id: 'barrel_metal', name: 'Barrel (metal)', category: 'barrels',   thumbnail_url: tile_thumb('barrel', '#9aa0a6'), tags: ['metal', 'explosive'] },
    { id: 'barrel_wood',  name: 'Barrel (wood)',  category: 'barrels',   thumbnail_url: tile_thumb('barrel', '#8c5a2a'), tags: ['wood'] },
    { id: 'lamp_post',    name: 'Lamp post',      category: 'lighting',  thumbnail_url: tile_thumb('lamp', '#d8c46a'), tags: ['light', 'outdoor'] },
    { id: 'chair_wood',   name: 'Chair (wood)',   category: 'furniture', thumbnail_url: tile_thumb('chair', '#a87340'), tags: ['wood', 'interior'] },
    { id: 'desk',         name: 'Desk',           category: 'furniture', thumbnail_url: tile_thumb('desk', '#7a5034'), tags: ['wood', 'interior'] },
    { id: 'cone_traffic', name: 'Traffic cone',   category: 'misc',      thumbnail_url: tile_thumb('cone', '#e08a30'), tags: ['outdoor'] },
    { id: 'tire',         name: 'Tire',           category: 'misc',      thumbnail_url: tile_thumb('tire', '#222'),     tags: ['rubber', 'physics'] },
    { id: 'rocket',       name: 'Rocket',         category: 'physics',   thumbnail_url: tile_thumb('rocket', '#d04040'), tags: ['weld', 'thruster'] },
    { id: 'ball_beach',   name: 'Beach ball',     category: 'physics',   thumbnail_url: tile_thumb('ball', '#f2dc6a'),  tags: ['bouncy'] },
    { id: 'computer',     name: 'Computer',       category: 'furniture', thumbnail_url: tile_thumb('pc', '#9a9a9a'),    tags: ['interior', 'electronics'] },
];

const DEMO_PSYGRAPH: PsyGraphDocument = {
    id: 'crate-spin',
    name: 'CrateSpin',
    source_path: 'samples/02_textured_quad/assets/behaviors/crate_spin.psyscript',
    target_group: 'crates',
    nodes: [
        {
            id: 'on_update',
            op: 'on_update',
            title: 'On Update',
            x: 56,
            y: 96,
            inputs: [],
            outputs: [{ id: 'flow', label: 'flow', kind: 'flow' }],
            values: {},
        },
        {
            id: 'spin_crates',
            op: 'spin',
            title: 'Transform Spin',
            x: 324,
            y: 76,
            inputs: [
                { id: 'flow', label: 'flow', kind: 'flow' },
                { id: 'axis', label: 'axis', kind: 'vec3' },
                { id: 'speed', label: 'speed', kind: 'float' },
            ],
            outputs: [{ id: 'flow', label: 'flow', kind: 'flow' }],
            values: {
                axis: [0, 1, 0],
                speed: { type: 'linearIndex', base: 0.35, step: 0.12 },
                phase: { type: 'constant', value: 0 },
                targetGroup: 'crates',
            },
        },
        {
            id: 'compiled_op',
            op: 'compiled_behavior_op',
            title: 'Cooked SoA Op',
            x: 640,
            y: 104,
            inputs: [{ id: 'flow', label: 'flow', kind: 'flow' }],
            outputs: [],
            values: {
                chunk: 'BehaviorSpinOps',
                runtime: 'Scene::SpinBehaviorSoA',
            },
        },
    ],
    links: [
        {
            id: 'update_to_spin',
            from_node: 'on_update',
            from_pin: 'flow',
            to_node: 'spin_crates',
            to_pin: 'flow',
        },
        {
            id: 'spin_to_compiled',
            from_node: 'spin_crates',
            from_pin: 'flow',
            to_node: 'compiled_op',
            to_pin: 'flow',
        },
    ],
    diagnostics: [
        { level: 'info', text: 'PsyGraph mock: compiles to one packed spin op.' },
    ],
};

export interface MockDriver {
    stop(): void;
}

type Deliver = (env: Envelope) => void;

export interface MockConsoleReply {
    logs: ConsoleLog[];
    result: ConsoleResult;
}

function summarize_command(source: string): string {
    const one_line = source.replace(/\s+/g, ' ').trim();
    return one_line.length > 80 ? `${one_line.slice(0, 77)}...` : one_line;
}

/** Deterministic offline reply for console commands sent while IPC is mocked. */
export function mock_console_eval(id: number, source: string): MockConsoleReply {
    const now = Date.now();
    const command = source.trim();
    const normalized = command.toLowerCase();
    const logs: ConsoleLog[] = [
        {
            level: 'debug',
            ts: now,
            tag: 'mock-repl',
            text: `accepted #${id}: ${summarize_command(command)}`,
        },
    ];

    if (normalized === 'help' || normalized === '?') {
        return {
            logs,
            result: {
                id,
                ok: true,
                duration_ms: 1.2,
                value_kind: 'text',
                text: [
                    'mock console commands:',
                    '  help',
                    '  getpos',
                    '  selection',
                    '  spawn <prop_id>',
                    '  echo <text>',
                ].join('\n'),
            },
        };
    }

    if (normalized === 'getpos') {
        return {
            logs,
            result: {
                id,
                ok: true,
                duration_ms: 0.8,
                value_kind: 'table',
                text: '{ x = 1.500, y = 0.000, z = -3.250 }',
            },
        };
    }

    if (normalized === 'selection') {
        return {
            logs,
            result: {
                id,
                ok: true,
                duration_ms: 0.9,
                value_kind: 'table',
                text: 'entity 0x427: crate_red.01 (Transform, Visible, RigidBody)',
            },
        };
    }

    if (normalized.startsWith('spawn ')) {
        const prop_id = command.slice(6).trim() || '<missing>';
        logs.push({
            level: prop_id === '<missing>' ? 'warn' : 'info',
            ts: now + 1,
            tag: 'mock-scene',
            text: prop_id === '<missing>'
                ? 'spawn requested without a prop id'
                : `queued spawn for prop '${prop_id}'`,
        });
        return {
            logs,
            result: {
                id,
                ok: prop_id !== '<missing>',
                duration_ms: 2.4,
                value_kind: prop_id === '<missing>' ? 'error' : 'text',
                text: prop_id === '<missing>'
                    ? 'usage: spawn <prop_id>'
                    : `mock entity spawned from '${prop_id}'`,
            },
        };
    }

    if (normalized.startsWith('echo ')) {
        return {
            logs,
            result: {
                id,
                ok: true,
                duration_ms: 0.5,
                value_kind: 'string',
                text: command.slice(5),
            },
        };
    }

    if (normalized.includes('error') || normalized.includes('throw')) {
        logs.push({
            level: 'error',
            ts: now + 1,
            tag: 'mock-lua',
            text: 'mock evaluator raised a scripted error',
        });
        return {
            logs,
            result: {
                id,
                ok: false,
                duration_ms: 1.6,
                value_kind: 'error',
                text: `mock error while evaluating: ${summarize_command(command)}`,
            },
        };
    }

    return {
        logs,
        result: {
            id,
            ok: true,
            duration_ms: 1.0,
            value_kind: 'text',
            text: `mock ok: ${summarize_command(command)}`,
        },
    };
}

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

    // ── Asset + prop catalogs ────────────────────────────────────────────
    const asset_catalog: AssetCatalog = { entries: DEMO_ASSETS };
    send({ v: PROTOCOL_VERSION, ch: 'assets', type: 'catalog', payload: asset_catalog });
    const prop_catalog: PropCatalog = { props: DEMO_PROPS };
    send({ v: PROTOCOL_VERSION, ch: 'props', type: 'catalog', payload: prop_catalog });
    send({ v: PROTOCOL_VERSION, ch: 'psygraph', type: 'document', payload: DEMO_PSYGRAPH });

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
        const render = 1.1 + wiggle(0.5, 0.4);
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
            render_ms: render,
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

/** Synchronously generate the mock asset catalog. */
export function mock_assets(): AssetCatalog {
    return { entries: DEMO_ASSETS };
}

/** Synchronously generate the mock prop catalog. */
export function mock_props(): PropCatalog {
    return { props: DEMO_PROPS };
}

/** Synchronously generate the mock PsyGraph behavior document. */
export function mock_psygraph(): PsyGraphDocument {
    return DEMO_PSYGRAPH;
}
