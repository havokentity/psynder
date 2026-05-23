// SPDX-License-Identifier: MIT
// Psynder editor IPC — WebSocket transport. Manages the connection to the
// engine's local server at 127.0.0.1:7654 (lane 19), msgpack-encoded frames,
// auto-reconnect with exponential backoff, and channel-level subscriptions.
//
// The lifecycle is intentionally simple: one connection per panel window
// (the Inspector / Console / Profiler each open their own tab and so each
// holds its own socket — the engine doesn't fan-out across multiple Chrome
// windows from a single ws). Reconnection is idempotent; subscribers are
// re-applied on the new socket without dropping events seen prior to the
// disconnect.

import { decode, decodeMulti, encode } from '@msgpack/msgpack';

import type {
    Channel,
    Envelope,
    ProfilerFrame,
} from './protocol';
import { PROTOCOL_VERSION } from './protocol';
import { opcodes as OPCODES } from './protocol.gen';

export interface ClientOptions {
    /** Override the engine endpoint; default derives from window.location. */
    url?: string;
    /** Session token passed by the engine; usually injected via the URL hash. */
    token?: string;
    /** When true, fall back to mock data if the socket cannot be opened. */
    allow_mock?: boolean;
}

export type ConnectionState =
    | 'connecting'
    | 'open'
    | 'closed'
    | 'reconnecting'
    | 'mock';

type Listener = (env: Envelope) => void;
type StateListener = (state: ConnectionState) => void;
type SceneSlice = Channel | 'perf' | 'scene';

const RECONNECT_BASE_MS = 250;
const RECONNECT_MAX_MS  = 8000;

export class IpcClient {
    private url: string;
    private token: string;
    private allow_mock: boolean;
    private ws: WebSocket | null = null;
    private channel_listeners = new Map<Channel, Set<Listener>>();
    private state_listeners   = new Set<StateListener>();
    private state: ConnectionState = 'closed';
    private reconnect_delay_ms = RECONNECT_BASE_MS;
    private reconnect_timer: ReturnType<typeof setTimeout> | null = null;
    private destroyed = false;
    private highest_seen_version = PROTOCOL_VERSION;
    private version_warned = false;
    private pending_console_ids: number[] = [];

    constructor(opts: ClientOptions = {}) {
        this.url = opts.url ?? this.default_url();
        this.token = opts.token ?? this.extract_token();
        this.allow_mock = opts.allow_mock ?? true;
    }

    // ─── Public API ──────────────────────────────────────────────────────

    connect(): void {
        if (this.destroyed) return;
        this.open_socket();
    }

    destroy(): void {
        this.destroyed = true;
        if (this.reconnect_timer) {
            clearTimeout(this.reconnect_timer);
            this.reconnect_timer = null;
        }
        if (this.ws) {
            try { this.ws.close(); } catch { /* ignore */ }
            this.ws = null;
        }
        this.set_state('closed');
    }

    /** Subscribe to a channel. Returns the unsubscribe handle. */
    subscribe(ch: Channel, fn: Listener): () => void {
        let set = this.channel_listeners.get(ch);
        if (!set) {
            set = new Set();
            this.channel_listeners.set(ch, set);
        }
        set.add(fn);
        return () => {
            set!.delete(fn);
            if (set!.size === 0) this.channel_listeners.delete(ch);
        };
    }

    /** Subscribe to high-level connection state transitions. */
    on_state(fn: StateListener): () => void {
        this.state_listeners.add(fn);
        // Synchronously deliver the current state so UIs paint immediately.
        try { fn(this.state); } catch { /* ignore */ }
        return () => { this.state_listeners.delete(fn); };
    }

    /** Send an envelope. Silently dropped if not connected. */
    send<T>(ch: Channel, type: string, payload: T): void {
        if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
        try {
            const frame = this.encode_frame(ch, type, payload);
            if (frame) this.ws.send(frame);
        } catch (err) {
            // Surface as a console message but never throw to the React tree.
            // eslint-disable-next-line no-console
            console.warn('[psynder ipc] send failed', err);
        }
    }

    current_state(): ConnectionState { return this.state; }

    /**
     * Inject a synthetic envelope as if it came in over the wire. Used by
     * the offline mock driver in `mock.ts`; the real engine path goes
     * through `handle_message`. Listeners cannot tell the difference.
     */
    deliver(env: Envelope): void {
        this.handle_envelope(env);
    }

    // ─── Internals ───────────────────────────────────────────────────────

    private default_url(): string {
        // Local engine WS endpoint. The Vite proxy in vite.config.ts forwards
        // `/ws` during development; in the embedded build we hit the engine
        // server directly from the file:// page Chrome loads via --app=http..
        const loc = typeof window !== 'undefined' ? window.location : null;
        if (loc && loc.protocol.startsWith('http')) {
            const scheme = loc.protocol === 'https:' ? 'wss' : 'ws';
            return `${scheme}://${loc.host}/ws`;
        }
        return 'ws://127.0.0.1:7654/ws';
    }

    private extract_token(): string {
        if (typeof window === 'undefined') return '';
        // The engine launches Chrome with token in the URL fragment so it
        // never lands in server logs. Fall back to the query string for dev.
        const hash = new URLSearchParams(window.location.hash.replace(/^#/, ''));
        const qs   = new URLSearchParams(window.location.search);
        return hash.get('token') ?? qs.get('token') ?? '';
    }

    private open_socket(): void {
        if (this.destroyed) return;
        this.clear_reconnect();
        this.set_state(this.ws ? 'reconnecting' : 'connecting');

        const url = this.token
            ? `${this.url}?token=${encodeURIComponent(this.token)}`
            : this.url;
        let ws: WebSocket;
        try {
            ws = new WebSocket(url);
        } catch (err) {
            // eslint-disable-next-line no-console
            console.warn('[psynder ipc] socket construction failed', err);
            this.fall_back_or_retry();
            return;
        }
        ws.binaryType = 'arraybuffer';
        this.ws = ws;

        ws.onopen = () => {
            this.reconnect_delay_ms = RECONNECT_BASE_MS;
            this.set_state('open');
        };

        ws.onmessage = (ev: MessageEvent<unknown>) => {
            this.handle_message(ev.data);
        };

        ws.onerror = () => {
            // The companion `close` event handles reconnection scheduling.
        };

        ws.onclose = () => {
            if (this.destroyed) return;
            this.ws = null;
            this.fall_back_or_retry();
        };
    }

    private handle_message(data: unknown): void {
        let buf: ArrayBuffer | Uint8Array | null = null;
        if (data instanceof ArrayBuffer) buf = data;
        else if (data instanceof Uint8Array) buf = data;
        // Strings are not part of the protocol; ignore.
        if (!buf) return;

        try {
            const parts = Array.from(decodeMulti(buf));
            if (this.handle_opcode_frame(parts)) return;
        } catch {
            // Fall through to the legacy envelope decoder below; useful while
            // browser mocks and generated engine frames coexist.
        }

        let env: Envelope;
        try {
            env = decode(buf) as Envelope;
        } catch (err) {
            // eslint-disable-next-line no-console
            console.warn('[psynder ipc] decode failed', err);
            return;
        }
        this.handle_envelope(env);
    }

    private handle_envelope(env: Envelope): void {
        if (!env || typeof env !== 'object' || !('ch' in env)) return;

        if (typeof env.v === 'number' && env.v > this.highest_seen_version) {
            this.highest_seen_version = env.v;
            if (env.v > PROTOCOL_VERSION && !this.version_warned) {
                this.version_warned = true;
                // eslint-disable-next-line no-console
                console.warn(
                    `[psynder ipc] engine protocol v${env.v} newer than bundle v${PROTOCOL_VERSION};`
                    + ' some fields may render best-effort.',
                );
            }
        }

        for (const delivery of fanout_envelopes(env)) {
            const set = this.channel_listeners.get(delivery.ch);
            if (!set) continue;
            for (const fn of set) {
                try { fn(delivery); }
                catch (err) {
                    // eslint-disable-next-line no-console
                    console.error('[psynder ipc] listener threw', err);
                }
            }
        }
    }

    private encode_frame<T>(ch: Channel, type: string, payload: T): Uint8Array | null {
        if (type === 'subscribe') {
            return concat_msgpack(OPCODES.SubscribeFrame, [ch]);
        }
        if (type === 'unsubscribe') {
            return concat_msgpack(OPCODES.UnsubscribeFrame, [ch]);
        }
        if (ch === 'console' && type === 'eval') {
            const p = payload as { id?: number; source?: string; text?: string; mode?: string };
            const text = p.source ?? p.text ?? '';
            const mode = p.mode === 'lua' ? 'lua' : 'console';
            if (typeof p.id === 'number') {
                this.pending_console_ids.push(p.id);
            }
            return concat_msgpack(OPCODES.ConsoleFrame, [text, mode]);
        }
        if (ch === 'console' && type === 'complete') {
            const p = payload as { id?: number; input?: string; cursor?: number };
            return concat_msgpack(OPCODES.ConsoleCompletionQueryFrame, [
                Number(p.id ?? 0),
                p.input ?? '',
                Number(p.cursor ?? 0),
            ]);
        }

        // Future panels can still use the previous envelope shape until their
        // generated opcode frames land. The current C++ server ignores these.
        const env: Envelope<T> = { v: PROTOCOL_VERSION, ch, type, payload };
        return encode(env);
    }

    private handle_opcode_frame(parts: unknown[]): boolean {
        if (parts.length < 2 || typeof parts[0] !== 'number') return false;
        const op = parts[0];
        const body = parts[1];
        if (op === OPCODES.WelcomeFrame) {
            const welcome = Array.isArray(body) ? body : [];
            const version = Number(welcome[1] ?? PROTOCOL_VERSION);
            if (version > this.highest_seen_version) {
                this.highest_seen_version = version;
            }
            return true;
        }
        if (op === OPCODES.ConsoleReplyFrame) {
            const reply = Array.isArray(body) ? body : [];
            const id = this.pending_console_ids.shift() ?? 0;
            const ok = Boolean(reply[0]);
            const text = typeof reply[1] === 'string' ? reply[1] : '';
            this.handle_envelope({
                v: PROTOCOL_VERSION,
                ch: 'console',
                type: 'result',
                payload: {
                    id,
                    ok,
                    text,
                    value_kind: ok ? 'text' : 'error',
                },
            });
            return true;
        }
        if (op === OPCODES.ConsoleCompletionReplyFrame) {
            const reply = Array.isArray(body) ? body : [];
            const names = Array.isArray(reply[3]) ? reply[3] : [];
            const kinds = Array.isArray(reply[4]) ? reply[4] : [];
            const values = Array.isArray(reply[5]) ? reply[5] : [];
            const descriptions = Array.isArray(reply[6]) ? reply[6] : [];
            this.handle_envelope({
                v: PROTOCOL_VERSION,
                ch: 'console',
                type: 'completions',
                payload: {
                    id: Number(reply[0] ?? 0),
                    start: Number(reply[1] ?? 0),
                    end: Number(reply[2] ?? 0),
                    items: names.map((name, index) => ({
                        name: String(name ?? ''),
                        kind: completion_kind(Number(kinds[index] ?? 0)),
                        value: typeof values[index] === 'string' ? values[index] : '',
                        description: typeof descriptions[index] === 'string'
                            ? descriptions[index]
                            : '',
                    })),
                },
            });
            return true;
        }
        if (op === OPCODES.LogFrame) {
            const log = Array.isArray(body) ? body : [];
            this.handle_envelope({
                v: PROTOCOL_VERSION,
                ch: 'console',
                type: 'log',
                payload: {
                    level: level_name(Number(log[0] ?? 2)),
                    ts: Date.now(),
                    text: typeof log[1] === 'string' ? log[1] : '',
                },
            });
            return true;
        }
        if (op === OPCODES.StatsFrame) {
            const frame = profiler_frame_from_stats(body);
            this.handle_envelope(profiler_envelope(frame));
            return true;
        }
        if (op === OPCODES.SceneDeltaFrame) {
            const env = this.decode_scene_delta_slice(body);
            if (env) this.handle_envelope(env);
            return true;
        }
        return true;
    }

    private decode_scene_delta_slice(body: unknown): Envelope | null {
        const slice = scene_slice_name(body);
        const payload_bytes = scene_slice_payload(body);
        if (!slice || !payload_bytes) return null;

        let decoded: unknown;
        try {
            decoded = decode(payload_bytes);
        } catch (err) {
            // eslint-disable-next-line no-console
            console.warn(`[psynder ipc] scene slice '${slice}' payload decode failed`, err);
            return null;
        }

        return envelope_from_scene_slice(slice, decoded);
    }

    private fall_back_or_retry(): void {
        if (this.destroyed) return;
        if (this.allow_mock && this.state !== 'open' && this.state !== 'mock') {
            // First failed connect → drop into mock mode so the dev experience
            // remains useful with no engine running. We continue retrying the
            // real socket in the background so a live engine can take over.
            this.set_state('mock');
        }
        this.schedule_reconnect();
    }

    private schedule_reconnect(): void {
        if (this.destroyed) return;
        const delay = this.reconnect_delay_ms;
        this.reconnect_delay_ms = Math.min(
            this.reconnect_delay_ms * 2,
            RECONNECT_MAX_MS,
        );
        this.reconnect_timer = setTimeout(() => this.open_socket(), delay);
    }

    private clear_reconnect(): void {
        if (this.reconnect_timer) {
            clearTimeout(this.reconnect_timer);
            this.reconnect_timer = null;
        }
    }

    private set_state(s: ConnectionState): void {
        if (this.state === s) return;
        this.state = s;
        for (const fn of this.state_listeners) {
            try { fn(s); } catch { /* ignore */ }
        }
    }
}

function concat_msgpack(op: number, body: unknown): Uint8Array {
    const a = encode(op);
    const b = encode(body);
    const out = new Uint8Array(a.length + b.length);
    out.set(a, 0);
    out.set(b, a.length);
    return out;
}

function scene_slice_name(body: unknown): SceneSlice | null {
    const raw = Array.isArray(body)
        ? body[0]
        : as_record(body)?.slice;
    return is_scene_slice(raw) ? raw : null;
}

function scene_slice_payload(body: unknown): Uint8Array | null {
    const raw = Array.isArray(body)
        ? body[1]
        : as_record(body)?.payload;
    return bytes_from_wire(raw);
}

function envelope_from_scene_slice(slice: SceneSlice, decoded: unknown): Envelope | null {
    const direct = direct_envelope(decoded);
    if (direct) return direct;

    const ch = legacy_channel_for_scene_slice(slice);
    if (!ch) return null;

    const typed = typed_payload(decoded);
    if (typed) {
        const payload = ch === 'profiler' && typed.type === 'frame'
            ? profiler_frame_from_stats(typed.payload)
            : typed.payload;
        return { v: PROTOCOL_VERSION, ch, type: typed.type, payload };
    }

    const inferred_type = infer_scene_delta_type(ch, decoded);
    if (!inferred_type) return null;
    const payload = ch === 'profiler' && inferred_type === 'frame'
        ? profiler_frame_from_stats(decoded)
        : decoded;
    return { v: PROTOCOL_VERSION, ch, type: inferred_type, payload };
}

function fanout_envelopes(env: Envelope): Envelope[] {
    if (env.ch === 'stats' || env.ch === 'perf') {
        return [env, profiler_envelope(profiler_frame_from_stats(env.payload))];
    }
    return [env];
}

function direct_envelope(value: unknown): Envelope | null {
    const rec = as_record(value);
    if (!rec) return null;
    if (!is_channel(rec.ch) || typeof rec.type !== 'string') return null;
    return {
        v: typeof rec.v === 'number' ? rec.v : PROTOCOL_VERSION,
        ch: rec.ch,
        type: rec.type,
        payload: rec.payload,
    };
}

function typed_payload(value: unknown): { type: string; payload: unknown } | null {
    const rec = as_record(value);
    if (!rec || typeof rec.type !== 'string') return null;
    if ('payload' in rec) return { type: rec.type, payload: rec.payload };

    const payload: Record<string, unknown> = {};
    for (const [key, item] of Object.entries(rec)) {
        if (key !== 'type') payload[key] = item;
    }
    return { type: rec.type, payload };
}

function legacy_channel_for_scene_slice(slice: SceneSlice): Channel | null {
    if (slice === 'perf') return 'profiler';
    if (slice === 'scene') return null;
    if (slice === 'stats') return 'profiler';
    if (slice === 'schemas'
        || slice === 'selection'
        || slice === 'assets'
        || slice === 'props'
        || slice === 'psygraph'
        || slice === 'profiler'
        || slice === 'console') {
        return slice;
    }
    return null;
}

function infer_scene_delta_type(ch: Channel, payload: unknown): string | null {
    if (ch === 'selection' && payload == null) return 'cleared';

    const rec = as_record(payload);
    if (!rec) return null;

    if (ch === 'schemas') {
        if (Array.isArray(rec.components)) return 'catalog';
        if ('added' in rec || 'removed' in rec) return 'delta';
    }
    if (ch === 'selection') {
        if (typeof rec.entity_id === 'number' && is_record(rec.components)) return 'state';
        if (typeof rec.component === 'string'
            && typeof rec.field === 'string'
            && 'value' in rec) return 'patch';
    }
    if (ch === 'assets') {
        if (Array.isArray(rec.entries)) return 'catalog';
        if ('added' in rec || 'removed' in rec) return 'delta';
    }
    if (ch === 'props') {
        if (Array.isArray(rec.props)) return 'catalog';
    }
    if (ch === 'psygraph') {
        if (Array.isArray(rec.nodes) && Array.isArray(rec.links)) return 'document';
    }
    if (ch === 'profiler') {
        if (Array.isArray(payload)
            || 'frame' in rec
            || 'frame_index' in rec
            || 'cpu_ms' in rec
            || 'gpu_ms' in rec) return 'frame';
    }

    return null;
}

function profiler_envelope(frame: ProfilerFrame): Envelope<ProfilerFrame> {
    return {
        v: PROTOCOL_VERSION,
        ch: 'profiler',
        type: 'frame',
        payload: frame,
    };
}

function profiler_frame_from_stats(value: unknown): ProfilerFrame {
    if (Array.isArray(value)) {
        const cpu_ms = number_from_wire(value[1]);
        return {
            frame: number_from_wire(value[0]),
            cpu_ms,
            gpu_ms: number_from_wire(value[2]),
            draw_calls: number_from_wire(value[3]),
            entities: number_from_wire(value[4]),
            sections: [{ name: 'frame', ms: cpu_ms }],
        };
    }

    const rec = as_record(value);
    if (rec) {
        const cpu_ms = number_from_wire(rec.cpu_ms);
        const raw_sections = Array.isArray(rec.sections) ? rec.sections : [];
        const sections = raw_sections
            .map((s) => {
                const sec = as_record(s);
                if (!sec || typeof sec.name !== 'string') return null;
                return { name: sec.name, ms: number_from_wire(sec.ms) };
            })
            .filter((s): s is { name: string; ms: number } => s !== null);
        return {
            frame: number_from_wire(rec.frame ?? rec.frame_index),
            cpu_ms,
            gpu_ms: number_from_wire(rec.gpu_ms),
            draw_calls: number_from_wire(rec.draw_calls),
            entities: number_from_wire(rec.entities),
            sections: sections.length > 0 ? sections : [{ name: 'frame', ms: cpu_ms }],
        };
    }

    return {
        frame: 0,
        cpu_ms: 0,
        gpu_ms: 0,
        draw_calls: 0,
        entities: 0,
        sections: [{ name: 'frame', ms: 0 }],
    };
}

function bytes_from_wire(value: unknown): Uint8Array | null {
    if (value instanceof Uint8Array) return value;
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (ArrayBuffer.isView(value)) {
        return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    }
    return null;
}

function number_from_wire(value: unknown): number {
    if (typeof value === 'bigint') return Number(value);
    if (typeof value === 'number' && Number.isFinite(value)) return value;
    return 0;
}

function is_scene_slice(value: unknown): value is SceneSlice {
    return value === 'perf' || value === 'scene' || is_channel(value);
}

function is_channel(value: unknown): value is Channel {
    return value === 'stats'
        || value === 'perf'
        || value === 'schemas'
        || value === 'selection'
        || value === 'console'
        || value === 'profiler'
        || value === 'assets'
        || value === 'props'
        || value === 'psygraph';
}

function as_record(value: unknown): Record<string, unknown> | null {
    return is_record(value) ? value : null;
}

function is_record(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function completion_kind(kind: number): 'cvar' | 'command' | 'value' {
    if (kind === 1) return 'command';
    if (kind === 2) return 'value';
    return 'cvar';
}

function level_name(level: number): 'trace' | 'debug' | 'info' | 'warn' | 'error' {
    switch (level) {
        case 0: return 'trace';
        case 1: return 'debug';
        case 3: return 'warn';
        case 4: return 'error';
        default: return 'info';
    }
}

let g_client: IpcClient | null = null;

/** Lazily-constructed singleton — convenient for panel components. */
export function get_client(): IpcClient {
    if (!g_client) {
        g_client = new IpcClient();
        g_client.connect();
    }
    return g_client;
}

/** Test/storybook override — replaces the singleton. */
export function set_client(c: IpcClient): void {
    if (g_client) g_client.destroy();
    g_client = c;
}
