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

import { decode, encode } from '@msgpack/msgpack';

import type {
    Channel,
    Envelope,
} from './protocol';
import { PROTOCOL_VERSION } from './protocol';

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
        const env: Envelope<T> = { v: PROTOCOL_VERSION, ch, type, payload };
        try {
            this.ws.send(encode(env));
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

        const set = this.channel_listeners.get(env.ch);
        if (!set) return;
        for (const fn of set) {
            try { fn(env); }
            catch (err) {
                // eslint-disable-next-line no-console
                console.error('[psynder ipc] listener threw', err);
            }
        }
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
