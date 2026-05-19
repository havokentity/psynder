// SPDX-License-Identifier: MIT
// Psynder editor — Profiler panel. Streams per-frame samples from the engine
// over the `profiler` channel and renders them as a scrolling strip chart
// (cpu/gpu) plus a stacked bar of the per-section breakdown. The chart is
// drawn via canvas2d so 60 Hz updates don't thrash React reconciliation.

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

interface Sample {
    cpu_ms: number;
    gpu_ms: number;
}

export function Profiler() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);

    // Ring buffer of recent samples lives in a ref because the chart draws
    // imperatively. React state is reserved for the textual "latest" panel.
    const ring_ref = React.useRef<Sample[]>([]);
    const latest_ref = React.useRef<ProfilerFrame | null>(null);
    const canvas_ref = React.useRef<HTMLCanvasElement | null>(null);
    const rAF_handle = React.useRef<number | null>(null);

    const [latest, set_latest] = React.useState<ProfilerFrame | null>(null);
    const [paused, set_paused] = React.useState(false);

    React.useEffect(() => {
        const unsub = client.subscribe('profiler', (env: Envelope) => {
            if (env.type !== 'frame' || paused) return;
            const frame = env.payload as ProfilerFrame;
            latest_ref.current = frame;
            const ring = ring_ref.current;
            ring.push({ cpu_ms: frame.cpu_ms, gpu_ms: frame.gpu_ms });
            if (ring.length > HISTORY) ring.splice(0, ring.length - HISTORY);
        });
        return unsub;
    }, [client, paused]);

    React.useEffect(() => {
        const unsub = client.on_state((s) => {
            if (s === 'open') client.send('profiler', 'subscribe', {});
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
            if (c) draw_strip(c, ring_ref.current);
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
                    title={`Last ${HISTORY} frames. Yellow line = 16.6 ms (60 Hz).`}
                />
                <div className="psy-profiler-legend">
                    <span className="psy-legend-swatch is-cpu" /> cpu
                    <span className="psy-legend-swatch is-gpu" /> gpu
                    <span className="psy-legend-swatch is-target" /> 16.6 ms
                </div>
            </div>

            <div className="psy-profiler-stats">
                <FrameStats frame={latest} />
                <SectionBars sections={latest?.sections ?? []} />
            </div>
        </div>
    );
}

function FrameStats({ frame }: { frame: ProfilerFrame | null }) {
    if (!frame) {
        return <div className="psy-empty">Waiting for first frame…</div>;
    }
    return (
        <ul className="psy-stats-grid">
            <li><span>frame</span><code>{frame.frame}</code></li>
            <li><span>cpu</span><code>{frame.cpu_ms.toFixed(2)} ms</code></li>
            <li><span>gpu</span><code>{frame.gpu_ms.toFixed(2)} ms</code></li>
            <li><span>fps</span><code>{(1000 / Math.max(frame.cpu_ms, 0.0001)).toFixed(1)}</code></li>
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
                            className={`psy-section-slice is-c${i % 5}`}
                            style={{ width: `${pct}%` }}
                            title={`${s.name} — ${s.ms.toFixed(2)} ms (${pct.toFixed(1)}%)`}
                        />
                    );
                })}
            </div>
            <ul className="psy-section-list">
                {sections.map((s, i) => (
                    <li key={s.name + i}>
                        <span className={`psy-legend-swatch is-c${i % 5}`} />
                        <span className="psy-section-name">{s.name}</span>
                        <code>{s.ms.toFixed(2)} ms</code>
                    </li>
                ))}
            </ul>
        </div>
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

    const draw_series = (key: 'cpu_ms' | 'gpu_ms', color: string) => {
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
    draw_series('gpu_ms', '#f49a4b');
}
