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
    on_change: (field: string, value: unknown) => void;
}

type WidgetKey =
    | 'number' | 'bool' | 'enum' | 'color'
    | 'string' | 'vec' | 'unknown';

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
    const which = widget_for(field);
    switch (which) {
        case 'number':
            return (
                <NumberField
                    field={field}
                    value={typeof value === 'number' ? value : 0}
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
                    value={typeof value === 'number' ? value : 0}
                    on_change={on_change as (n: number) => void}
                />
            );
        case 'color':
            return (
                <ColorField
                    field={field}
                    value={typeof value === 'number' ? value : 0xff000000}
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

export function SchemaForm({ schema, values, on_change }: SchemaFormProps) {
    return (
        <div className="psy-form">
            {schema.fields.map((f) => (
                <div className="psy-field-row" key={f.name}>
                    <label
                        className="psy-field-label"
                        title={f.help ?? f.name}
                    >
                        {f.name}
                        {f.numeric?.unit
                            ? <span className="psy-field-unit"> ({f.numeric.unit})</span>
                            : null}
                    </label>
                    <div className="psy-field-widget">
                        {render_widget(f, values?.[f.name], (next) => on_change(f.name, next))}
                    </div>
                </div>
            ))}
        </div>
    );
}
