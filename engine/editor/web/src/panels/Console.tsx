// SPDX-License-Identifier: MIT
// Psynder editor — Console panel. Bidirectional REPL over the lane-19 IPC.
// Outbound: `eval` envelopes carrying a snippet of Lua / cvar text. Inbound:
// `log` lines (interleaved engine logging) and `result` (terminal value of
// the prior eval). The terminal keeps a bounded scroll history.

import React from 'react';

import { get_client } from '../ipc/client';
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
    level?: LogLevel;
    tag?: string;
    ts: number;
    text: string;
    ok?: boolean;
}

const MAX_HISTORY = 5000;
const MAX_INPUT_RING = 200;

function new_id(): string {
    return `${Date.now().toString(36)}.${Math.random().toString(36).slice(2, 8)}`;
}

export function Console() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    const [entries, set_entries] = React.useState<ConsoleEntry[]>([]);
    const [draft, set_draft] = React.useState('');
    const [input_ring, set_input_ring] = React.useState<string[]>([]);
    const [ring_idx, set_ring_idx] = React.useState(-1);
    const eval_seq = React.useRef(0);
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
                push({
                    id: new_id(),
                    kind: 'result',
                    ts: Date.now(),
                    text: r.text,
                    ok: r.ok,
                });
            }
        });
        return unsub;
    }, [client, push]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
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

    const submit = () => {
        const source = draft.trim();
        if (!source) return;
        eval_seq.current += 1;
        const id = eval_seq.current;
        push({
            id: new_id(),
            kind: 'eval',
            ts: Date.now(),
            text: source,
        });
        client.send<ConsoleEval>('console', 'eval', { id, source });
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

    return (
        <div className="psy-panel psy-console">
            <header className="psy-panel-header">
                <h2>Console</h2>
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

            <div
                ref={scroll_ref}
                className="psy-console-scroll"
                onScroll={on_scroll}
            >
                {entries.length === 0 && (
                    <div className="psy-empty">
                        Type a command and press Enter. Shift+Enter inserts a newline.
                    </div>
                )}
                {entries.map((e) => (
                    <ConsoleRow key={e.id} entry={e} />
                ))}
            </div>

            <form
                className="psy-console-input-row"
                onSubmit={(e) => { e.preventDefault(); submit(); }}
            >
                <span className="psy-console-prompt">›</span>
                <textarea
                    className="psy-console-input"
                    placeholder="lua snippet or `help`"
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
            {entry.tag && <span className="psy-console-tag">[{entry.tag}]</span>}
            {entry.level && entry.kind === 'log' && (
                <span className={`psy-console-level is-level-${entry.level}`}>
                    {entry.level}
                </span>
            )}
            {prefix && <span className="psy-console-prefix">{prefix}</span>}
            <pre className="psy-console-text">{entry.text}</pre>
        </div>
    );
}
