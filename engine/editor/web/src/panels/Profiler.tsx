// SPDX-License-Identifier: MIT
// Psynder editor — Profiler panel. Streams per-frame samples from the engine
// over the `profiler` channel and renders them as:
//   1. a scrolling cpu/render line strip chart (top), and
//   2. a per-subsystem stacked-bar strip showing where each frame's CPU
//      budget went (render/physics/audio/ui/…) — Wave B addition, mirroring
//      the in-engine immediate-mode allocator heatmap idea from DESIGN.md
//      §10.6 / §14.
// Both charts share the same horizontal ring history. They draw to canvas2d
// so 60 Hz updates don't thrash React reconciliation.

import React from 'react';

import { get_client } from '../ipc/client';
import type {
    Envelope,
    ProfilerFrame,
    ProfilerSection,
} from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

const HISTORY = 256;
const TARGET_MS = 16.6;   // 60 Hz budget — drawn as a horizontal guide line.
const BAD_MS    = 33.3;   // 30 Hz threshold — bar color flips warning red.
const STACK_PALETTE = ['#5fb0ff', '#f49a4b', '#6dd49e', '#c98ee0', '#f6c244', '#e16a6a', '#7dc7d8'];

interface Sample {
    frame: number;
    cpu_ms: number;
    render_ms: number;
    /** Per-subsystem breakdown for this frame — drives the stacked-bar strip. */
    sections: ProfilerSection[];
}

// Stable color per subsystem name across the strip — names hash into the
// palette so 'render' is always the same hue frame-to-frame even if the
// engine reorders the section list.
function color_for_section(name: string): string {
    let h = 0;
    for (let i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) | 0;
    return STACK_PALETTE[Math.abs(h) % STACK_PALETTE.length];
}

export function Profiler() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    // Ring buffer of recent samples lives in a ref because the chart draws
    // imperatively. React state is reserved for the textual "latest" panel.
    const ring_ref = React.useRef<Sample[]>([]);
    const latest_ref = React.useRef<ProfilerFrame | null>(null);
    const canvas_ref = React.useRef<HTMLCanvasElement | null>(null);
    const stack_ref  = React.useRef<HTMLCanvasElement | null>(null);
    const rAF_handle = React.useRef<number | null>(null);

    const [latest, set_latest] = React.useState<ProfilerFrame | null>(null);
    const [paused, set_paused] = React.useState(false);

    React.useEffect(() => {
        const unsub = client.subscribe('profiler', (env: Envelope) => {
            if (env.type !== 'frame' || paused) return;
            const frame = merge_profiler_frame(latest_ref.current, normalize_profiler_frame(env.payload));
            latest_ref.current = frame;
            const ring = ring_ref.current;
            const sample = {
                frame: frame.frame,
                cpu_ms: frame.cpu_ms,
                render_ms: frame.render_ms,
                // Defensive copy — protocol envelopes are nominally immutable
                // but we don't trust upstream code to keep them that way.
                sections: frame.sections.map((s) => ({ name: s.name, ms: s.ms })),
            };
            if (ring.length > 0 && ring[ring.length - 1].frame === sample.frame) {
                ring[ring.length - 1] = sample;
            } else {
                ring.push(sample);
            }
            if (ring.length > HISTORY) ring.splice(0, ring.length - HISTORY);
        });
        return unsub;
    }, [client, paused]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') {
                client.send('profiler', 'subscribe', {});
                client.send('stats', 'subscribe', {});
                client.send('perf', 'subscribe', {});
            }
        });
        return unsub;
    }, [client]);

    // Throttle the textual panel to ~10 Hz so the small list-of-sections
    // doesn't repaint at 60 Hz.
    React.useEffect(() => {
        const handle = setInterval(() => {
            set_latest(latest_ref.current);
        }, 100);
        return () => clearInterval(handle);
    }, []);

    // ── Canvas draw loop ────────────────────────────────────────────────
    React.useEffect(() => {
        const draw = () => {
            const c = canvas_ref.current;
            const s = stack_ref.current;
            if (c) draw_strip(c, ring_ref.current);
            if (s) draw_subsystem_stack(s, ring_ref.current);
            rAF_handle.current = requestAnimationFrame(draw);
        };
        rAF_handle.current = requestAnimationFrame(draw);
        return () => {
            if (rAF_handle.current != null) cancelAnimationFrame(rAF_handle.current);
        };
    }, []);

    return (
        <div className="psy-panel psy-profiler">
            <header className="psy-panel-header">
                <h2>Profiler</h2>
                <ConnectionBadge />
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={() => set_paused((p) => !p)}
                >
                    {paused ? 'resume' : 'pause'}
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-ghost"
                    onClick={() => { ring_ref.current = []; }}
                >
                    clear
                </button>
            </header>

            <div className="psy-profiler-chart-wrap">
                <canvas
                    ref={canvas_ref}
                    className="psy-profiler-chart"
                    width={HISTORY}
                    height={120}
                    title={`Last ${HISTORY} frames. Dashed lines = 16.6 / 33.3 ms.`}
                />
                <div className="psy-profiler-legend">
                    <span className="psy-legend-swatch is-cpu" /> cpu
                    <span className="psy-legend-swatch is-render" /> render
                    <span className="psy-legend-swatch is-target" /> 16.6 ms
                </div>
            </div>

            <div className="psy-profiler-stack-wrap">
                <div className="psy-profiler-substack-label">CPU subsystems</div>
                <canvas
                    ref={stack_ref}
                    className="psy-profiler-substack"
                    width={HISTORY}
                    height={80}
                    title={`Per-frame CPU subsystem breakdown over the last ${HISTORY} frames.`}
                />
                <SubsystemLegend frame={latest} />
            </div>

            <div className="psy-profiler-stats">
                <FrameStats frame={latest} />
                <SectionBars sections={latest?.sections ?? []} />
            </div>
        </div>
    );
}

function normalize_profiler_frame(payload: unknown): ProfilerFrame {
    const rec = typeof payload === 'object' && payload !== null && !Array.isArray(payload)
        ? payload as Record<string, unknown>
        : {};
    const cpu_ms = number_value(rec.cpu_ms);
    const raw_sections = Array.isArray(rec.sections) ? rec.sections : [];
    const sections = raw_sections
        .map((section) => {
            if (typeof section !== 'object' || section === null || Array.isArray(section)) {
                return null;
            }
            const s = section as Record<string, unknown>;
            if (typeof s.name !== 'string') return null;
            return { name: s.name, ms: number_value(s.ms) };
        })
        .filter((s): s is ProfilerSection => s !== null);

    return {
        frame: number_value(rec.frame ?? rec.frame_index),
        cpu_ms,
        render_ms: number_value(rec.render_ms),
        draw_calls: number_value(rec.draw_calls),
        entities: number_value(rec.entities),
        sections: sections.length > 0 ? sections : [{ name: 'frame', ms: cpu_ms }],
    };
}

function merge_profiler_frame(previous: ProfilerFrame | null, incoming: ProfilerFrame): ProfilerFrame {
    if (!previous || previous.frame !== incoming.frame) return incoming;
    if (has_rich_sections(incoming.sections)) return incoming;
    if (!has_rich_sections(previous.sections)) return incoming;
    return {
        ...incoming,
        sections: previous.sections.map((s) => ({ name: s.name, ms: s.ms })),
    };
}

function has_rich_sections(sections: ProfilerSection[]): boolean {
    return sections.length > 1 || sections.some((s) => s.name !== 'frame');
}

function number_value(value: unknown): number {
    if (typeof value === 'bigint') return Number(value);
    if (typeof value === 'number' && Number.isFinite(value)) return value;
    return 0;
}

function FrameStats({ frame }: { frame: ProfilerFrame | null }) {
    if (!frame) {
        return <div className="psy-empty">Waiting for first frame…</div>;
    }
    return (
        <ul className="psy-stats-grid">
            <li><span>frame</span><code>{frame.frame}</code></li>
            <li><span>cpu</span><code>{frame.cpu_ms.toFixed(2)} ms</code></li>
            <li><span>render</span><code>{frame.render_ms.toFixed(2)} ms</code></li>
            <li><span>fps</span><code>{(1000 / Math.max(frame.cpu_ms, 0.0001)).toFixed(1)}</code></li>
            <li><span>draws</span><code>{frame.draw_calls ?? 0}</code></li>
            <li><span>entities</span><code>{frame.entities ?? 0}</code></li>
        </ul>
    );
}

function SectionBars({ sections }: { sections: ProfilerSection[] }) {
    if (sections.length === 0) return null;
    const total = sections.reduce((s, x) => s + x.ms, 0) || 1;
    return (
        <div className="psy-section-bars">
            <div className="psy-section-stack" role="img" aria-label="section breakdown">
                {sections.map((s, i) => {
                    const pct = (s.ms / total) * 100;
                    return (
                        <span
                            key={s.name + i}
                            className="psy-section-slice"
                            style={{ width: `${pct}%`, background: color_for_section(s.name) }}
                            title={`${s.name} — ${s.ms.toFixed(2)} ms (${pct.toFixed(1)}%)`}
                        />
                    );
                })}
            </div>
            <ul className="psy-section-list">
                {sections.map((s, i) => (
                    <li key={s.name + i}>
                        <span
                            className="psy-legend-swatch"
                            style={{ background: color_for_section(s.name) }}
                        />
                        <span className="psy-section-name">{s.name}</span>
                        <code>{s.ms.toFixed(2)} ms</code>
                    </li>
                ))}
            </ul>
        </div>
    );
}

function SubsystemLegend({ frame }: { frame: ProfilerFrame | null }) {
    if (!frame || frame.sections.length === 0) return null;
    return (
        <ul className="psy-profiler-substack-legend">
            {frame.sections.map((s) => (
                <li key={s.name}>
                    <span
                        className="psy-legend-swatch"
                        style={{ background: color_for_section(s.name) }}
                    />
                    <span className="psy-section-name">{s.name}</span>
                </li>
            ))}
        </ul>
    );
}

function draw_strip(canvas: HTMLCanvasElement, ring: Sample[]) {
    const w = canvas.width;
    const h = canvas.height;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.clearRect(0, 0, w, h);

    // Background gridlines at 16.6 ms / 33.3 ms.
    const max_ms = Math.max(BAD_MS * 1.2, BAD_MS);
    const y_for = (ms: number) => h - (ms / max_ms) * h;

    ctx.strokeStyle = '#3a3a3a';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 4]);
    ctx.beginPath();
    ctx.moveTo(0, Math.round(y_for(TARGET_MS)) + 0.5);
    ctx.lineTo(w, Math.round(y_for(TARGET_MS)) + 0.5);
    ctx.moveTo(0, Math.round(y_for(BAD_MS)) + 0.5);
    ctx.lineTo(w, Math.round(y_for(BAD_MS)) + 0.5);
    ctx.stroke();
    ctx.setLineDash([]);

    if (ring.length === 0) return;

    const draw_series = (key: 'cpu_ms' | 'render_ms', color: string) => {
        ctx.strokeStyle = color;
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        const x_step = w / Math.max(HISTORY - 1, 1);
        const start_x = w - ring.length * x_step;
        for (let i = 0; i < ring.length; i++) {
            const x = start_x + i * x_step;
            const y = y_for(ring[i][key]);
            if (i === 0) ctx.moveTo(x, y);
            else         ctx.lineTo(x, y);
        }
        ctx.stroke();
    };

    draw_series('cpu_ms', '#5fb0ff');
    draw_series('render_ms', '#f49a4b');
}

// Per-column stacked-bar of subsystem timings — one column per recent frame.
// The vertical axis is fraction-of-budget (16.6 ms) rather than fraction-of-
// total-cpu so a frame that overshoots the budget visibly spills past the
// top of the column.
function draw_subsystem_stack(canvas: HTMLCanvasElement, ring: Sample[]) {
    const w = canvas.width;
    const h = canvas.height;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.clearRect(0, 0, w, h);

    // Background guide line at the 16.6 ms budget.
    const max_ms = BAD_MS;
    const y_for = (ms: number) => h - Math.min(ms, max_ms) / max_ms * h;

    ctx.strokeStyle = '#3a3a3a';
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 4]);
    ctx.beginPath();
    ctx.moveTo(0, Math.round(y_for(TARGET_MS)) + 0.5);
    ctx.lineTo(w, Math.round(y_for(TARGET_MS)) + 0.5);
    ctx.stroke();
    ctx.setLineDash([]);

    if (ring.length === 0) return;

    const x_step = w / Math.max(HISTORY - 1, 1);
    const col_w = Math.max(1, Math.ceil(x_step));
    const start_x = w - ring.length * x_step;

    for (let i = 0; i < ring.length; i++) {
        const x = Math.floor(start_x + i * x_step);
        let y_top = h;
        const sample = ring[i];
        for (const sec of sample.sections) {
            const slice_h = Math.max(0, Math.min(sec.ms, max_ms) / max_ms * h);
            ctx.fillStyle = color_for_section(sec.name);
            ctx.fillRect(x, y_top - slice_h, col_w, slice_h);
            y_top -= slice_h;
            if (y_top <= 0) break;
        }
    }
}
