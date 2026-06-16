// SPDX-License-Identifier: MIT
// Psynder editor — schema-driven form. Reads a ComponentSchema + a value map
// and dispatches each field to the right widget. The mapping rule (single
// source of truth for the "auto-binding" feature) lives in `widget_for`.

import React from 'react';

import type {
    ComponentSchema,
    ComponentValueMap,
    FieldSchema,
} from '../ipc/protocol';
import {
    BoolField,
    ColorField,
    EnumField,
    NumberField,
    StringField,
    VecField,
    is_integer_kind,
} from './widgets';

export interface SchemaFormProps {
    schema: ComponentSchema;
    values: ComponentValueMap;
    on_change: (field: FieldSchema, value: unknown) => void;
}

type WidgetKey =
    | 'number' | 'bool' | 'enum' | 'color'
    | 'string' | 'vec' | 'unknown';

const MATERIAL_COLOR_FIELDS = new Set([
    'albedo',
    'albedo_rgba8',
    'base_color',
    'base_color_rgba8',
    'color',
    'emission_color',
    'emission_rgba8',
    'emissive_color',
    'emissive_rgba8',
    'tint',
    'tint_rgba8',
]);

const MATERIAL_UNIT_INTERVAL_FIELDS = new Set([
    'alpha',
    'alpha_cutoff',
    'reflectivity',
    'roughness',
]);

function is_numeric_kind(field: FieldSchema): boolean {
    const k = field.kind;
    return k === 'f32' || k === 'f64' || is_integer_kind(k);
}

function is_material_schema(schema: ComponentSchema): boolean {
    return /material/i.test(schema.name);
}

function material_field_for(schema: ComponentSchema, field: FieldSchema): FieldSchema {
    if (!is_material_schema(schema)) return field;

    const name = field.name.toLowerCase();
    if (MATERIAL_COLOR_FIELDS.has(name) && is_numeric_kind(field)) {
        return { ...field, kind: 'color' };
    }

    if (MATERIAL_UNIT_INTERVAL_FIELDS.has(name) && is_numeric_kind(field)) {
        return {
            ...field,
            kind: field.kind === 'f64' ? 'f64' : 'f32',
            numeric: {
                min: 0,
                max: 1,
                step: 0.01,
                ...field.numeric,
            },
        };
    }

    if (name === 'emissive' && is_numeric_kind(field)) {
        return {
            ...field,
            kind: field.kind === 'f64' ? 'f64' : 'f32',
            numeric: {
                min: 0,
                step: 0.05,
                ...field.numeric,
            },
        };
    }

    if (name === 'blend' && is_numeric_kind(field) && !field.enum) {
        return {
            ...field,
            kind: 'enum',
            enum: {
                options: [
                    { label: 'Opaque', value: 0 },
                    { label: 'Alpha test', value: 1 },
                    { label: 'Alpha blend', value: 2 },
                ],
            },
        };
    }

    return field;
}

function coerce_number(value: unknown, fallback = 0): number {
    const n = typeof value === 'number' ? value : Number(value);
    return Number.isFinite(n) ? n : fallback;
}

/** Pure mapping from field kind to widget id. Exported for tests. */
export function widget_for(field: FieldSchema): WidgetKey {
    const k = field.kind;
    if (k === 'bool')   return 'bool';
    if (k === 'enum')   return 'enum';
    if (k === 'color')  return 'color';
    if (k === 'string') return 'string';
    if (k === 'vec2' || k === 'vec3' || k === 'vec4' || k === 'quat') return 'vec';
    if (k === 'f32' || k === 'f64' || is_integer_kind(k)) return 'number';
    return 'unknown';
}

function render_widget(
    field: FieldSchema,
    value: unknown,
    on_change: (next: unknown) => void,
): React.ReactNode {
    if (field.readonly) return render_readonly_widget(field, value);

    const which = widget_for(field);
    switch (which) {
        case 'number':
            return (
                <NumberField
                    field={field}
                    value={coerce_number(value)}
                    on_change={on_change as (n: number) => void}
                />
            );
        case 'bool':
            return (
                <BoolField
                    field={field}
                    value={!!value}
                    on_change={on_change as (b: boolean) => void}
                />
            );
        case 'enum':
            return (
                <EnumField
                    field={field}
                    value={coerce_number(value)}
                    on_change={on_change as (n: number) => void}
                />
            );
        case 'color':
            return (
                <ColorField
                    field={field}
                    value={coerce_number(value, 0xff000000)}
                    on_change={on_change as (n: number) => void}
                />
            );
        case 'string':
            return (
                <StringField
                    field={field}
                    value={typeof value === 'string' ? value : ''}
                    on_change={on_change as (s: string) => void}
                />
            );
        case 'vec':
            return (
                <VecField
                    field={field}
                    value={Array.isArray(value) ? (value as number[]) : []}
                    on_change={on_change as (a: number[]) => void}
                />
            );
        case 'unknown':
        default:
            return (
                <code className="psy-field-unknown">
                    [{field.kind}] {JSON.stringify(value)}
                </code>
            );
    }
}

function hex_byte(n: number): string {
    return n.toString(16).padStart(2, '0');
}

function format_color(value: unknown): string {
    if (typeof value !== 'number') return String(value ?? '');
    const packed = value >>> 0;
    const a = (packed >>> 24) & 0xff;
    const r = (packed >>>  0) & 0xff;
    const g = (packed >>>  8) & 0xff;
    const b = (packed >>> 16) & 0xff;
    const rgb = `#${hex_byte(r)}${hex_byte(g)}${hex_byte(b)}`;
    return a < 255 ? `${rgb} alpha ${a}` : rgb;
}

function format_enum(field: FieldSchema, value: unknown): string {
    const raw = typeof value === 'number' ? value : Number(value ?? 0);
    const opt = field.enum?.options.find((o) => o.value === raw);
    return opt ? `${opt.label} (${raw})` : String(value ?? '');
}

function format_readonly_value(field: FieldSchema, value: unknown): string {
    if (value == null) return '';
    if (field.kind === 'bool') return value ? 'true' : 'false';
    if (field.kind === 'color') return format_color(value);
    if (field.kind === 'enum') return format_enum(field, value);
    if (Array.isArray(value)) return value.map((v) => String(v)).join(', ');
    if (typeof value === 'object') return JSON.stringify(value);
    return String(value);
}

function render_readonly_widget(
    field: FieldSchema,
    value: unknown,
): React.ReactNode {
    return (
        <input
            type="text"
            className="psy-input psy-input-string"
            readOnly
            aria-readonly="true"
            title="Read-only native ECS snapshot. Select or copy the value here."
            value={format_readonly_value(field, value)}
        />
    );
}

export function SchemaForm({ schema, values, on_change }: SchemaFormProps) {
    return (
        <div className="psy-form">
            {schema.fields.map((raw_field) => {
                const f = material_field_for(schema, raw_field);
                return (
                    <div
                        className="psy-field-row"
                        key={f.name}
                        data-readonly={f.readonly ? 'true' : undefined}
                    >
                        <label
                            className="psy-field-label"
                            title={f.help ?? f.name}
                        >
                            {f.name}
                            {f.numeric?.unit
                                ? <span className="psy-field-unit"> ({f.numeric.unit})</span>
                                : null}
                            {f.readonly
                                ? <span className="psy-field-unit"> read-only</span>
                                : null}
                        </label>
                        <div className="psy-field-widget">
                            {render_widget(f, values?.[f.name], (next) => on_change(raw_field, next))}
                        </div>
                    </div>
                );
            })}
        </div>
    );
}
