// SPDX-License-Identifier: MIT
// Psynder editor IPC — TypeScript view of the msgpack frame envelope shared
// with `engine/editor/ipc/`. Until lane 19 ships `protocol.psy` codegen this
// file is the contract: every frame is an envelope { v, ch, type, payload },
// msgpack-encoded, framed as a single WebSocket binary message.
//
// Channels are one of:
//   - "schemas"   : engine pushes PSYNDER_COMPONENT schema catalog deltas.
//   - "selection" : engine pushes the currently-selected entity's component
//                   values; panel pushes back property edits.
//   - "console"   : bi-directional REPL — panel sends `eval`, engine streams
//                   `log` lines plus the eventual `result`.
//   - "profiler"  : engine pushes a `frame` sample per render frame (cpu_ms,
//                   gpu_ms, ms_per_section breakdown, fps).
//
// The version `v` is the protocol revision. Wave-A pegs it to 1. A drift
// detector in `client.ts` logs a warning if the engine reports a higher
// version than the bundle was built against; the React app degrades to
// best-effort rendering of channels it understands.

export const PROTOCOL_VERSION = 1;

export type Channel = 'schemas' | 'selection' | 'console' | 'profiler';

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
// `eval`   : panel → engine. Pushes a snippet to the Lua REPL host.
// `log`    : engine → panel. Stream of log lines tagged with a severity.
// `result` : engine → panel. The terminal value (or error) of the prior eval.

export type LogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error';

export interface ConsoleEval {
    /** Monotonic id so the panel can correlate the `result` reply. */
    id: number;
    source: string;
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
    gpu_ms: number;
    /** Per-system / per-pass breakdown — `sum(sections.ms) ≈ cpu_ms`. */
    sections: ProfilerSection[];
}
