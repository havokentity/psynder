// SPDX-License-Identifier: MIT
// Psynder editor — widget atoms for the auto-generated inspector form.
// Each widget owns its own local input state so React can keep typing fast,
// then commits on blur / change as appropriate. The mapping rule lives in
// `Form.tsx` and obeys DESIGN.md §10.6: number→input[number], enum→select,
// bool→checkbox, color→color picker.

import React from 'react';

import type {
    EnumFieldHints,
    FieldKind,
    FieldSchema,
    NumericFieldHints,
} from '../ipc/protocol';

export interface WidgetProps<T = unknown> {
    field: FieldSchema;
    value: T;
    on_change: (next: T) => void;
}

// ─── Numeric ─────────────────────────────────────────────────────────────

const INTEGER_KINDS: ReadonlySet<FieldKind> = new Set<FieldKind>([
    'i8', 'i16', 'i32', 'i64',
    'u8', 'u16', 'u32', 'u64',
]);

export function is_integer_kind(k: FieldKind): boolean {
    return INTEGER_KINDS.has(k);
}

function numeric_step(field: FieldSchema): number {
    if (field.numeric?.step != null) return field.numeric.step;
    return is_integer_kind(field.kind) ? 1 : 0.01;
}

function clamp(v: number, hints?: NumericFieldHints): number {
    if (!hints) return v;
    if (hints.min != null && v < hints.min) return hints.min;
    if (hints.max != null && v > hints.max) return hints.max;
    return v;
}

export function NumberField({ field, value, on_change }: WidgetProps<number>) {
    const [draft, set_draft] = React.useState(String(value ?? 0));
    React.useEffect(() => { set_draft(String(value ?? 0)); }, [value]);

    const commit = (raw: string) => {
        const n = Number(raw);
        if (!Number.isFinite(n)) {
            set_draft(String(value ?? 0));
            return;
        }
        const clamped = clamp(n, field.numeric);
        on_change(clamped);
        if (clamped !== n) set_draft(String(clamped));
    };

    return (
        <input
            type="number"
            className="psy-input psy-input-number"
            disabled={field.readonly}
            value={draft}
            step={numeric_step(field)}
            min={field.numeric?.min}
            max={field.numeric?.max}
            onChange={(e) => set_draft(e.target.value)}
            onBlur={(e) => commit(e.target.value)}
            onKeyDown={(e) => {
                if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
            }}
        />
    );
}

// ─── Bool ────────────────────────────────────────────────────────────────

export function BoolField({ field, value, on_change }: WidgetProps<boolean>) {
    return (
        <label className="psy-bool">
            <input
                type="checkbox"
                disabled={field.readonly}
                checked={!!value}
                onChange={(e) => on_change(e.target.checked)}
            />
            <span className="psy-bool-label">{value ? 'true' : 'false'}</span>
        </label>
    );
}

// ─── Enum ────────────────────────────────────────────────────────────────

export function EnumField(
    { field, value, on_change }: WidgetProps<number>,
) {
    const hints: EnumFieldHints | undefined = field.enum;
    const options = hints?.options ?? [];
    return (
        <select
            className="psy-input psy-input-enum"
            disabled={field.readonly}
            value={String(value ?? options[0]?.value ?? 0)}
            onChange={(e) => on_change(Number(e.target.value))}
        >
            {options.map((opt) => (
                <option key={opt.value} value={String(opt.value)}>
                    {opt.label}
                </option>
            ))}
        </select>
    );
}

// ─── String ──────────────────────────────────────────────────────────────

export function StringField({ field, value, on_change }: WidgetProps<string>) {
    const [draft, set_draft] = React.useState(value ?? '');
    React.useEffect(() => { set_draft(value ?? ''); }, [value]);
    return (
        <input
            type="text"
            className="psy-input psy-input-string"
            disabled={field.readonly}
            value={draft}
            onChange={(e) => set_draft(e.target.value)}
            onBlur={() => { if (draft !== value) on_change(draft); }}
            onKeyDown={(e) => {
                if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
            }}
        />
    );
}

// ─── Color ───────────────────────────────────────────────────────────────
//
// Wire format: RGBA8 packed into a u32 in 0xAARRGGBB order. The HTML input
// type=color only takes #RRGGBB so we surface the alpha as a separate slider.

function u32_to_hex(packed: number): string {
    const r = (packed >>> 16) & 0xff;
    const g = (packed >>>  8) & 0xff;
    const b = (packed >>>  0) & 0xff;
    const pad = (n: number) => n.toString(16).padStart(2, '0');
    return `#${pad(r)}${pad(g)}${pad(b)}`;
}

function hex_to_rgb(hex: string): [number, number, number] {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
    if (!m) return [0, 0, 0];
    const n = parseInt(m[1], 16);
    return [(n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff];
}

function pack_rgba(r: number, g: number, b: number, a: number): number {
    return (
        ((a & 0xff) << 24) |
        ((r & 0xff) << 16) |
        ((g & 0xff) <<  8) |
        ((b & 0xff) <<  0)
    ) >>> 0;
}

export function ColorField({ field, value, on_change }: WidgetProps<number>) {
    const packed = (value ?? 0xff000000) >>> 0;
    const hex = u32_to_hex(packed);
    const alpha = (packed >>> 24) & 0xff;

    const update = (rgb_hex: string, a: number) => {
        const [r, g, b] = hex_to_rgb(rgb_hex);
        on_change(pack_rgba(r, g, b, a));
    };

    return (
        <span className="psy-color">
            <input
                type="color"
                className="psy-color-swatch"
                disabled={field.readonly}
                value={hex}
                onChange={(e) => update(e.target.value, alpha)}
            />
            <input
                type="range"
                className="psy-color-alpha"
                min={0}
                max={255}
                step={1}
                disabled={field.readonly}
                value={alpha}
                onChange={(e) => update(hex, Number(e.target.value))}
                title={`alpha = ${alpha}`}
            />
            <code className="psy-color-hex">{hex}{alpha < 255 ? `+${alpha}` : ''}</code>
        </span>
    );
}

// ─── Vec ─────────────────────────────────────────────────────────────────

export function VecField(
    { field, value, on_change }: WidgetProps<number[]>,
) {
    const arity =
        field.kind === 'vec2' ? 2 :
        field.kind === 'vec3' ? 3 :
        field.kind === 'vec4' ? 4 :
        field.kind === 'quat' ? 4 : 3;
    const labels =
        arity === 2 ? ['x', 'y'] :
        arity === 3 ? ['x', 'y', 'z'] :
        arity === 4 && field.kind === 'quat' ? ['x', 'y', 'z', 'w'] :
        ['x', 'y', 'z', 'w'];
    const arr = Array.isArray(value)
        ? value.slice(0, arity)
        : new Array(arity).fill(0);
    while (arr.length < arity) arr.push(0);

    const update = (i: number, n: number) => {
        const next = arr.slice();
        next[i] = clamp(n, field.numeric);
        on_change(next);
    };

    return (
        <span className={`psy-vec psy-vec-${arity}`}>
            {labels.slice(0, arity).map((lab, i) => (
                <span key={lab} className="psy-vec-cell">
                    <span className="psy-vec-axis">{lab}</span>
                    <NumberField
                        field={{
                            name: `${field.name}.${lab}`,
                            kind: 'f32',
                            numeric: field.numeric,
                            readonly: field.readonly,
                        }}
                        value={arr[i]}
                        on_change={(n) => update(i, n)}
                    />
                </span>
            ))}
        </span>
    );
}
