// SPDX-License-Identifier: MIT
// Psynder editor IPC — TypeScript view of the msgpack frame envelope shared
// with `engine/editor/ipc/`. Until lane 19 ships `protocol.psy` codegen this
// file is the contract: every frame is an envelope { v, ch, type, payload },
// msgpack-encoded, framed as a single WebSocket binary message.
//
// Channels are one of:
//   - "schemas"   : engine pushes PSYNDER_COMPONENT schema catalog deltas.
//   - "selection" : engine pushes the currently-selected entity's component
//                   values; panel pushes back property edits. Also carries
//                   `spawn_prop` commands from the prop-spawn menu.
//   - "console"   : bi-directional engine-console / Lua command path — panel
//                   sends `eval`, engine streams `log` lines plus `result`.
//   - "profiler"  : engine pushes a `frame` sample per render frame (cpu_ms,
//                   render_ms, ms_per_section breakdown, fps).
//   - "assets"    : engine pushes the catalog of entries in the loaded
//                   `.lmpak` archives, plus deltas as packs mount/unmount.
//   - "props"     : engine pushes the searchable prop library used by the
//                   spawn menu (thumbnails are URLs served by the IPC HTTP
//                   side; in mock mode they're inline svg data URIs).
//   - "psygraph"  : graph-authoring documents for PsyScript/PsyGraph
//                   behavior assets. Wave C starts with mock/offline data.
//
// The version `v` is the protocol revision. Wave-A pegs it to 1. A drift
// detector in `client.ts` logs a warning if the engine reports a higher
// version than the bundle was built against; the React app degrades to
// best-effort rendering of channels it understands.

import { kProtocolVersion } from './protocol.gen';
import type { ConsoleCompletionQuery } from './protocol.gen';

export const PROTOCOL_VERSION = kProtocolVersion;

export type Channel =
    | 'stats'
    | 'perf'
    | 'schemas'
    | 'selection'
    | 'console'
    | 'profiler'
    | 'assets'
    | 'props'
    | 'psygraph';

export interface Envelope<T = unknown> {
    v: number;
    ch: Channel;
    type: string;
    payload: T;
}

// ─── Schemas channel ─────────────────────────────────────────────────────
//
// The engine emits one `catalog` frame on connect (full set) plus `delta`
// frames as PSYNDER_COMPONENT registrations come in at runtime (e.g. when a
// mod or Lua-defined component registers).

export type FieldKind =
    | 'i8'  | 'i16' | 'i32' | 'i64'
    | 'u8'  | 'u16' | 'u32' | 'u64'
    | 'f32' | 'f64'
    | 'bool'
    | 'enum'
    | 'string'
    | 'color'    // RGBA8 packed in a u32; widget = color picker.
    | 'vec2' | 'vec3' | 'vec4'
    | 'quat';    // four floats — surfaced as Euler degrees for editing.

export interface NumericFieldHints {
    min?: number;
    max?: number;
    step?: number;
    unit?: string;     // e.g. "m", "deg", "kg" — displayed next to the input.
}

export interface EnumFieldHints {
    /** Display label → wire value. Wire values are u32 by convention. */
    options: Array<{ label: string; value: number }>;
}

export interface FieldSchema {
    name: string;
    kind: FieldKind;
    /** Per-kind extra metadata; absent for kinds that need none. */
    numeric?: NumericFieldHints;
    enum?: EnumFieldHints;
    /** When true, the inspector renders the widget read-only. */
    readonly?: boolean;
    /** Tooltip text for hover help; pulled from C++ `// @help: ...`. */
    help?: string;
}

export interface ComponentSchema {
    /** Canonical PSYNDER_COMPONENT(Name) identifier. */
    name: string;
    /** FNV-1a64 of the field layout. Used for client-side cache busting. */
    layout_hash: string;
    fields: FieldSchema[];
}

export interface SchemaCatalog {
    components: ComponentSchema[];
}

export interface SchemaDelta {
    added?: ComponentSchema[];
    removed?: string[];          // component names dropped from the registry
}

// ─── Selection channel ───────────────────────────────────────────────────
//
// Engine → panel:
//   `state` : full set of components on the currently-selected entity.
//             Sent on selection change and whenever a component is added /
//             removed. Field-level edits arrive as `patch`.
//   `patch` : { component, field, value } — incremental update.
//   `cleared` : nothing is selected.
//
// Panel → engine:
//   `set`   : user edited a field in the inspector form.

export type ComponentValueMap = Record<string, unknown>;

export interface SelectionState {
    entity_id: number;
    entity_label?: string;
    /** componentName → fieldName → value (msgpack-native). */
    components: Record<string, ComponentValueMap>;
}

export interface SelectionPatch {
    entity_id: number;
    component: string;
    field: string;
    value: unknown;
}

export interface SelectionSet {
    entity_id: number;
    component: string;
    field: string;
    value: unknown;
}

// ─── Console channel ─────────────────────────────────────────────────────
//
// `eval`        : panel → engine. Pushes either an engine-console command/cvar
//                 or an explicit Lua REPL snippet.
// `complete`    : panel → engine. Requests native console completions.
// `completions` : engine → panel. Ranked native console completion matches.
// `log`         : engine → panel. Stream of log lines tagged with a severity.
// `result`      : engine → panel. The terminal value (or error) of the prior eval.

export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error';

export interface ConsoleEval {
    /** Monotonic id so the panel can correlate the `result` reply. */
    id: number;
    source: string;
    /** Optional UI hint for engines that route multiple console languages. */
    mode?: 'console' | 'lua';
}

export interface ConsoleLog {
    level: LogLevel;
    /** Wall-clock unix millis as emitted by the engine; clients display tod. */
    ts: number;
    /** Optional subsystem tag — e.g. "phys", "render", "lua". */
    tag?: string;
    text: string;
}

export interface ConsoleResult {
    id: number;
    ok: boolean;
    /** Pretty-printed value (engine-side `tostring`) or an error message. */
    text: string;
    /** Optional engine-side timing for the evaluated request. */
    duration_ms?: number;
    /** Optional coarse value label for richer GUI consoles. */
    value_kind?: 'nil' | 'boolean' | 'number' | 'string' | 'table' | 'error' | 'text';
}

export type ConsoleCompletionRequest = ConsoleCompletionQuery;

export type ConsoleCompletionKind = 'cvar' | 'command' | 'value';

export interface ConsoleCompletionItem {
    name: string;
    kind: ConsoleCompletionKind;
    value?: string;
    description?: string;
}

export interface ConsoleCompletionReply {
    id: number;
    start: number;
    end: number;
    items: ConsoleCompletionItem[];
}

// ─── Profiler channel ────────────────────────────────────────────────────
//
// One `frame` per rendered frame. The panel keeps a ring of the most recent
// `PROFILER_HISTORY` samples for the strip-chart.

export interface ProfilerSection {
    name: string;
    ms: number;
}

export interface ProfilerFrame {
    /** Engine-side frame counter. Monotonic; gaps imply dropped frames. */
    frame: number;
    cpu_ms: number;
    render_ms: number;
    draw_calls?: number;
    entities?: number;
    /** Per-system / per-pass breakdown — `sum(sections.ms) ≈ cpu_ms`. */
    sections: ProfilerSection[];
}

export interface StatsTick {
    frame_index: number;
    cpu_ms: number;
    render_ms: number;
    draw_calls: number;
    entities: number;
}

// ─── Assets channel ──────────────────────────────────────────────────────
//
// The engine emits one `catalog` frame on connect (full set of `.lmpak`
// entries currently mounted) plus `delta` frames when packs mount/unmount
// at runtime. The asset browser surfaces these as a searchable tree, with
// entries grouped by their `category` (meshes, textures, audio, levels,
// scripts, …) — Wave C wires real-data filters on top of this surface.

export type AssetCategory =
    | 'mesh'
    | 'texture'
    | 'audio'
    | 'level'
    | 'script'
    | 'prefab'
    | 'material'
    | 'other';

export interface AssetEntry {
    /** Canonical VFS path inside the pack, e.g. "meshes/crate_red.lmm". */
    path: string;
    category: AssetCategory;
    /** Source `.lmpak` archive — Wave-A names it for filtering. */
    pack: string;
    /** Uncompressed size in bytes, for display in the browser. */
    size_bytes: number;
    /** FNV-1a64 of the cooked file — Wave C wires hot-reload off this. */
    content_hash: string;
}

export interface AssetCatalog {
    entries: AssetEntry[];
}

export interface AssetDelta {
    added?: AssetEntry[];
    /** Canonical paths of entries removed (e.g. when a pack unmounts). */
    removed?: string[];
}

// ─── Props channel ───────────────────────────────────────────────────────
//
// Sandbox-mode prop spawn menu (DESIGN.md §10.8). Engine pushes a `catalog`
// of available props on connect; the panel sends `spawn_prop` commands back
// through the `selection` channel (which already handles entity creation).

export interface PropEntry {
    /** Stable id — what `selection.spawn_prop` references. */
    id: string;
    /** Display name shown in the grid tile. */
    name: string;
    /** Coarse category tag used for filter chips. */
    category: string;
    /** Thumbnail URL — engine serves these from the IPC HTTP side. */
    thumbnail_url?: string;
    /** Optional tags used by the fuzzy search. */
    tags?: string[];
}

export interface PropCatalog {
    props: PropEntry[];
}

export interface SpawnPropCommand {
    /** Prop to spawn — must match an id from the catalog. */
    prop_id: string;
    /** Optional placement hint; engine picks a cursor location when absent. */
    position?: [number, number, number];
}

export interface EditorCommandAck {
    command: string;
    ok: boolean;
    text: string;
}

// ─── PsyGraph channel ───────────────────────────────────────────────────
//
// Visual behavior authoring. The editor manipulates this graph document; the
// cooker lowers it to packed behavior ops in `.psyscene`.

export type PsyGraphValue =
    | number
    | string
    | boolean
    | [number, number, number]
    | { type: 'constant'; value: number }
    | { type: 'linearIndex'; base: number; step: number };

export interface PsyGraphPin {
    id: string;
    label: string;
    kind: 'flow' | 'float' | 'vec3' | 'group' | 'bool';
}

export interface PsyGraphNode {
    id: string;
    op: string;
    title: string;
    x: number;
    y: number;
    inputs: PsyGraphPin[];
    outputs: PsyGraphPin[];
    values: Record<string, PsyGraphValue>;
}

export interface PsyGraphLink {
    id: string;
    from_node: string;
    from_pin: string;
    to_node: string;
    to_pin: string;
}

export interface PsyGraphDocument {
    id: string;
    name: string;
    source_path: string;
    target_group: string;
    nodes: PsyGraphNode[];
    links: PsyGraphLink[];
    diagnostics: Array<{ level: LogLevel; text: string; node_id?: string }>;
}
