// SPDX-License-Identifier: MIT
// Psynder editor — Console panel. Bidirectional engine console + Lua REPL over
// lane-19 IPC. Console mode runs commands/cvars only; Lua mode is explicit.

import React from 'react';

import { get_client } from '../ipc/client';
import type { ConnectionState } from '../ipc/client';
import { mock_console_eval } from '../ipc/mock';
import type {
    ConsoleEval,
    ConsoleLog,
    ConsoleResult,
    Envelope,
    LogLevel,
} from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

interface ConsoleEntry {
    id: string;
    kind: 'eval' | 'log' | 'result' | 'system';
    mode?: ConsoleMode;
    request_id?: number;
    level?: LogLevel;
    tag?: string;
    ts: number;
    text: string;
    ok?: boolean;
    duration_ms?: number;
    value_kind?: ConsoleResult['value_kind'];
}

interface ConsoleRequest {
    id: number;
    mode: ConsoleMode;
    source: string;
    started_at: number;
    status: 'pending' | 'ok' | 'error';
    duration_ms?: number;
}

const MAX_HISTORY = 5000;
const MAX_INPUT_RING = 200;
const MAX_REQUESTS = 64;

const LEVEL_ORDER: LogLevel[] = ['trace', 'debug', 'info', 'warn', 'error'];
type ConsoleFilter = 'all' | 'warn' | 'error';
type ConsoleMode = 'console' | 'lua';

function new_id(): string {
    return `${Date.now().toString(36)}.${Math.random().toString(36).slice(2, 8)}`;
}

export function Console() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    const [entries, set_entries] = React.useState<ConsoleEntry[]>([]);
    const [requests, set_requests] = React.useState<ConsoleRequest[]>([]);
    const [draft, set_draft] = React.useState('');
    const [input_ring, set_input_ring] = React.useState<string[]>([]);
    const [ring_idx, set_ring_idx] = React.useState(-1);
    const [filter, set_filter] = React.useState<ConsoleFilter>('all');
    const [mode, set_mode] = React.useState<ConsoleMode>('console');
    const [connection_state, set_connection_state] = React.useState<ConnectionState>(
        client.current_state(),
    );
    const eval_seq = React.useRef(0);
    const request_started = React.useRef(new Map<number, number>());
    const scroll_ref = React.useRef<HTMLDivElement | null>(null);

    // ── Stick scroll to the bottom unless the user has scrolled up ──────
    const [pinned, set_pinned] = React.useState(true);
    React.useEffect(() => {
        const el = scroll_ref.current;
        if (!el || !pinned) return;
        el.scrollTop = el.scrollHeight;
    }, [entries, pinned]);

    const push = React.useCallback((e: ConsoleEntry) => {
        set_entries((prev) => {
            const next = prev.length >= MAX_HISTORY
                ? prev.slice(prev.length - MAX_HISTORY + 1)
                : prev.slice();
            next.push(e);
            return next;
        });
    }, []);

    const update_request = React.useCallback((
        id: number,
        status: ConsoleRequest['status'],
        duration_ms?: number,
    ) => {
        set_requests((prev) => prev.map((req) => (
            req.id === id ? { ...req, status, duration_ms } : req
        )));
    }, []);

    React.useEffect(() => {
        const unsub = client.subscribe('console', (env: Envelope) => {
            if (env.type === 'log') {
                const log = env.payload as ConsoleLog;
                push({
                    id: new_id(),
                    kind: 'log',
                    level: log.level,
                    tag: log.tag,
                    ts: log.ts,
                    text: log.text,
                });
            } else if (env.type === 'result') {
                const r = env.payload as ConsoleResult;
                const now = Date.now();
                const started_at = request_started.current.get(r.id);
                const duration_ms = r.duration_ms ?? (
                    started_at ? now - started_at : undefined
                );
                request_started.current.delete(r.id);
                update_request(r.id, r.ok ? 'ok' : 'error', duration_ms);
                push({
                    id: new_id(),
                    kind: 'result',
                    request_id: r.id,
                    ts: now,
                    text: r.text,
                    ok: r.ok,
                    duration_ms,
                    value_kind: r.value_kind,
                });
            }
        });
        return unsub;
    }, [client, push, update_request]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            set_connection_state(s);
            if (s === 'open') {
                client.send('console', 'subscribe', {});
                push({
                    id: new_id(),
                    kind: 'system',
                    ts: Date.now(),
                    text: 'console: connected to engine',
                });
            } else if (s === 'mock') {
                push({
                    id: new_id(),
                    kind: 'system',
                    ts: Date.now(),
                    text: 'console: engine offline — running on mock stream',
                });
            }
        });
        return unsub;
    }, [client, push]);

    const settle_mock_request = React.useCallback((request_id: number, source: string) => {
        const reply = mock_console_eval(request_id, source);
        for (const log of reply.logs) {
            push({
                id: new_id(),
                kind: 'log',
                request_id,
                level: log.level,
                tag: log.tag,
                ts: log.ts,
                text: log.text,
            });
        }
        const started_at = request_started.current.get(request_id);
        const duration_ms = reply.result.duration_ms ?? (
            started_at ? Date.now() - started_at : undefined
        );
        request_started.current.delete(request_id);
        update_request(request_id, reply.result.ok ? 'ok' : 'error', duration_ms);
        push({
            id: new_id(),
            kind: 'result',
            request_id,
            ts: Date.now(),
            text: reply.result.text,
            ok: reply.result.ok,
            duration_ms,
            value_kind: reply.result.value_kind,
        });
    }, [push, update_request]);

    const submit = () => {
        const source = draft.trim();
        if (!source) return;
        eval_seq.current += 1;
        const id = eval_seq.current;
        const started_at = Date.now();
        request_started.current.set(id, started_at);
        push({
            id: new_id(),
            kind: 'eval',
            mode,
            request_id: id,
            ts: started_at,
            text: source,
        });
        set_requests((prev) => {
            const next = [
                ...prev,
                { id, mode, source, started_at, status: 'pending' as const },
            ];
            return next.length > MAX_REQUESTS
                ? next.slice(next.length - MAX_REQUESTS)
                : next;
        });
        if (source === 'clear' || source === 'cls') {
            set_entries([]);
            update_request(id, 'ok', 0);
            request_started.current.delete(id);
        } else if (connection_state === 'mock') {
            settle_mock_request(id, source);
        } else if (connection_state === 'open') {
            client.send<ConsoleEval>('console', 'eval', { id, source, mode });
        } else {
            request_started.current.delete(id);
            update_request(id, 'error', 0);
            push({
                id: new_id(),
                kind: 'result',
                request_id: id,
                ts: Date.now(),
                text: `console: connection is ${connection_state}; command was not sent`,
                ok: false,
                duration_ms: 0,
                value_kind: 'error',
            });
        }
        set_input_ring((prev) => {
            const ring = prev[prev.length - 1] === source ? prev : [...prev, source];
            return ring.length > MAX_INPUT_RING
                ? ring.slice(ring.length - MAX_INPUT_RING)
                : ring;
        });
        set_ring_idx(-1);
        set_draft('');
    };

    const on_key_down: React.KeyboardEventHandler<HTMLTextAreaElement> = (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            submit();
        } else if (e.key === 'ArrowUp' && (e.altKey || draft === '' || ring_idx >= 0)) {
            if (input_ring.length === 0) return;
            e.preventDefault();
            const next_idx = ring_idx < 0
                ? input_ring.length - 1
                : Math.max(0, ring_idx - 1);
            set_ring_idx(next_idx);
            set_draft(input_ring[next_idx] ?? '');
        } else if (e.key === 'ArrowDown' && (e.altKey || ring_idx >= 0)) {
            e.preventDefault();
            if (ring_idx < 0) return;
            const next_idx = ring_idx + 1;
            if (next_idx >= input_ring.length) {
                set_ring_idx(-1);
                set_draft('');
            } else {
                set_ring_idx(next_idx);
                set_draft(input_ring[next_idx] ?? '');
            }
        }
    };

    const on_scroll: React.UIEventHandler<HTMLDivElement> = (e) => {
        const el = e.currentTarget;
        const at_bottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
        set_pinned(at_bottom);
    };

    const pending_count = requests.filter((req) => req.status === 'pending').length;
    const visible_entries = React.useMemo(() => {
        if (filter === 'all') return entries;
        const min_level = filter === 'warn' ? 'warn' : 'error';
        const min_idx = LEVEL_ORDER.indexOf(min_level);
        return entries.filter((entry) => {
            if (entry.kind !== 'log') return true;
            if (!entry.level) return true;
            return LEVEL_ORDER.indexOf(entry.level) >= min_idx;
        });
    }, [entries, filter]);
    const recent_requests = requests.slice(-8).reverse();

    return (
        <div className="psy-panel psy-console">
            <header className="psy-panel-header">
                <h2>Console</h2>
                <div className="psy-console-counters" aria-label="Console counters">
                    <span>{entries.length}/{MAX_HISTORY}</span>
                    <span>{pending_count} pending</span>
                </div>
                <ConnectionBadge />
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={() => set_entries([])}
                    title="Clear scrollback"
                >
                    clear
                </button>
            </header>

            <div className="psy-console-toolbar">
                <div className="psy-console-mode" role="tablist" aria-label="Console mode">
                    {(['console', 'lua'] as ConsoleMode[]).map((value) => (
                        <button
                            key={value}
                            type="button"
                            role="tab"
                            aria-selected={mode === value}
                            className={`psy-console-mode-tab${mode === value ? ' is-active' : ''}`}
                            onClick={() => set_mode(value)}
                        >
                            {value}
                        </button>
                    ))}
                </div>
                <div className="psy-console-filter" role="group" aria-label="Console log filter">
                    {(['all', 'warn', 'error'] as ConsoleFilter[]).map((value) => (
                        <button
                            key={value}
                            type="button"
                            className={`psy-chip-btn${filter === value ? ' is-active' : ''}`}
                            onClick={() => set_filter(value)}
                        >
                            {value === 'all' ? 'all' : value === 'warn' ? 'warn+' : 'errors'}
                        </button>
                    ))}
                </div>
                {!pinned && (
                    <button
                        type="button"
                        className="psy-btn psy-btn-ghost"
                        onClick={() => {
                            set_pinned(true);
                            const el = scroll_ref.current;
                            if (el) el.scrollTop = el.scrollHeight;
                        }}
                    >
                        follow
                    </button>
                )}
            </div>

            <div className="psy-console-body">
                <div
                    ref={scroll_ref}
                    className="psy-console-scroll"
                    onScroll={on_scroll}
                >
                    {entries.length === 0 && (
                        <div className="psy-empty">
                            idle
                        </div>
                    )}
                    {entries.length > 0 && visible_entries.length === 0 && (
                        <div className="psy-empty">
                            No log lines match the current filter.
                        </div>
                    )}
                    {visible_entries.map((e) => (
                        <ConsoleRow key={e.id} entry={e} />
                    ))}
                </div>

                <aside className="psy-console-requests" aria-label="Console requests">
                    <div className="psy-console-requests-title">requests</div>
                    {recent_requests.length === 0 && (
                        <div className="psy-console-request-empty">idle</div>
                    )}
                    {recent_requests.map((req) => (
                        <button
                            key={req.id}
                            type="button"
                            className={`psy-console-request is-${req.status}`}
                            onClick={() => {
                                set_mode(req.mode);
                                set_draft(req.source);
                            }}
                            title="Load command"
                        >
                            <span className="psy-console-request-status" />
                            <span className="psy-console-request-id">#{req.id}</span>
                            <span className="psy-console-request-mode">{req.mode}</span>
                            <span className="psy-console-request-text">{req.source}</span>
                            <span className="psy-console-request-ms">
                                {req.status === 'pending'
                                    ? '...'
                                    : format_duration(req.duration_ms)}
                            </span>
                        </button>
                    ))}
                </aside>
            </div>

            <form
                className="psy-console-input-row"
                onSubmit={(e) => { e.preventDefault(); submit(); }}
            >
                <span className="psy-console-prompt">›</span>
                <textarea
                    className="psy-console-input"
                    placeholder={mode === 'lua' ? 'lua expression or statement' : 'engine command or cvar'}
                    spellCheck={false}
                    autoFocus
                    rows={2}
                    value={draft}
                    onChange={(e) => set_draft(e.target.value)}
                    onKeyDown={on_key_down}
                />
                <button type="submit" className="psy-btn psy-btn-primary">
                    run
                </button>
            </form>
        </div>
    );
}

function format_duration(ms: number | undefined): string {
    if (ms === undefined) return '--';
    if (ms < 10) return `${ms.toFixed(1)} ms`;
    return `${Math.round(ms)} ms`;
}

function ConsoleRow({ entry }: { entry: ConsoleEntry }) {
    const ts = new Date(entry.ts);
    const time = ts.toLocaleTimeString(undefined, { hour12: false });
    const classes = ['psy-console-row', `is-${entry.kind}`];
    if (entry.level)         classes.push(`is-level-${entry.level}`);
    if (entry.ok === false)  classes.push('is-error');

    let prefix = '';
    if (entry.kind === 'eval')   prefix = '›';
    else if (entry.kind === 'result') prefix = entry.ok === false ? '!' : '⇒';
    else if (entry.kind === 'system') prefix = '·';
    else                              prefix = '';

    return (
        <div className={classes.join(' ')}>
            <span className="psy-console-ts">{time}</span>
            {entry.request_id !== undefined && (
                <span className="psy-console-req">#{entry.request_id}</span>
            )}
            {entry.mode && <span className="psy-console-tag">[{entry.mode}]</span>}
            {entry.tag && <span className="psy-console-tag">[{entry.tag}]</span>}
            {entry.level && entry.kind === 'log' && (
                <span className={`psy-console-level is-level-${entry.level}`}>
                    {entry.level}
                </span>
            )}
            {prefix && <span className="psy-console-prefix">{prefix}</span>}
            {entry.kind === 'result' && (
                <span className="psy-console-meta">
                    {entry.value_kind ?? (entry.ok === false ? 'error' : 'result')}
                    {entry.duration_ms !== undefined ? ` ${format_duration(entry.duration_ms)}` : ''}
                </span>
            )}
            <pre className="psy-console-text">{entry.text}</pre>
        </div>
    );
}
