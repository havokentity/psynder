// SPDX-License-Identifier: MIT
// Psynder editor - primitive and material palette metadata.

import React from 'react';

export type PrimitiveKind = 'box' | 'sphere' | 'plane' | 'cone' | 'pyramid' | 'triangle';

export type MaterialPresetId = 'default' | 'clay' | 'metal' | 'glass' | 'emissive';

export interface PrimitiveOption {
    kind: PrimitiveKind;
    label: string;
}

export interface MaterialPresetMetadata {
    albedo: string;
    metallic: number;
    roughness: number;
    emission?: string;
    alpha?: number;
}

export interface MaterialPreset {
    id: MaterialPresetId;
    label: string;
    metadata: MaterialPresetMetadata;
}

export type MaterialComponentPresetValues = Record<string, unknown>;

export interface PrimitiveMaterialSelection {
    primitive_kind: PrimitiveKind;
    material_preset_id: MaterialPresetId;
}

export interface PrimitiveAddCommandOptions {
    include_material?: boolean;
    material_preset_id?: MaterialPresetId;
}

export const PRIMITIVE_OPTIONS: readonly PrimitiveOption[] = [
    { kind: 'box', label: 'Box' },
    { kind: 'sphere', label: 'Sphere' },
    { kind: 'plane', label: 'Plane' },
    { kind: 'cone', label: 'Cone' },
    { kind: 'pyramid', label: 'Pyramid' },
    { kind: 'triangle', label: 'Triangle' },
];

export const MATERIAL_PRESETS: readonly MaterialPreset[] = [
    { id: 'default', label: 'Default', metadata: { albedo: '#d8d8d8', metallic: 0, roughness: 0.6 } },
    { id: 'clay', label: 'Clay', metadata: { albedo: '#b9684a', metallic: 0, roughness: 0.9 } },
    { id: 'metal', label: 'Metal', metadata: { albedo: '#a8b0b6', metallic: 1, roughness: 0.35 } },
    { id: 'glass', label: 'Glass', metadata: { albedo: '#b7ddff', metallic: 0, roughness: 0.05, alpha: 0.35 } },
    { id: 'emissive', label: 'Emissive', metadata: { albedo: '#ffffff', metallic: 0, roughness: 0.4, emission: '#7fdcff' } },
];

export const DEFAULT_PRIMITIVE_MATERIAL_SELECTION: PrimitiveMaterialSelection = {
    primitive_kind: 'box',
    material_preset_id: 'default',
};

export function primitive_add_command(kind: PrimitiveKind, options: PrimitiveAddCommandOptions = {}): string {
    const command = `primitive_add ${kind}`;
    if (!options.include_material || !options.material_preset_id) {
        return command;
    }
    return `${command} --material=${options.material_preset_id}`;
}

export function primitive_material_add_command(
    selection: PrimitiveMaterialSelection,
    options: Pick<PrimitiveAddCommandOptions, 'include_material'> = {},
): string {
    return primitive_add_command(selection.primitive_kind, {
        include_material: options.include_material,
        material_preset_id: selection.material_preset_id,
    });
}

export function primitive_label(kind: PrimitiveKind): string {
    return PRIMITIVE_OPTIONS.find((option) => option.kind === kind)?.label ?? kind;
}

export function material_preset(id: MaterialPresetId): MaterialPreset {
    return MATERIAL_PRESETS.find((preset) => preset.id === id) ?? MATERIAL_PRESETS[0];
}

export function material_preset_apply_command(entity_id: number, preset_id: MaterialPresetId): string {
    return `material_apply ${entity_id} ${preset_id}`;
}

function pack_rgba8(r: number, g: number, b: number, a = 0xff): number {
    return (((a & 0xff) << 24) |
        ((b & 0xff) << 16) |
        ((g & 0xff) << 8) |
        (r & 0xff)) >>> 0;
}

function entity_index(entity_id: number): number {
    return entity_id & 0x00ffffff;
}

export function material_preset_component_values(
    preset_id: MaterialPresetId,
    entity_id: number,
): MaterialComponentPresetValues {
    const palette = [
        pack_rgba8(0xff, 0x7a, 0x59),
        pack_rgba8(0x4f, 0xc3, 0xf7),
        pack_rgba8(0xb8, 0xf2, 0x7d),
        pack_rgba8(0xff, 0xd1, 0x66),
        pack_rgba8(0xd3, 0x8b, 0xff),
        pack_rgba8(0x6e, 0xe7, 0xb7),
        pack_rgba8(0xff, 0x8f, 0xb3),
        pack_rgba8(0xa3, 0xbf, 0xfa),
    ];
    const values: MaterialComponentPresetValues = {
        albedo_rgba8: palette[entity_index(entity_id) % palette.length],
        reflectivity: 0.05,
        roughness: 0.78,
        emissive: 0,
        alpha_cutoff: 0.5,
        blend: 0,
        shadow_opacity: 0.5,
        shadow_softness: 0.5,
    };

    if (preset_id === 'clay') {
        values.albedo_rgba8 = pack_rgba8(0xb9, 0x68, 0x4a);
        values.roughness = 0.92;
    } else if (preset_id === 'metal') {
        values.albedo_rgba8 = pack_rgba8(0xa8, 0xb0, 0xb6);
        values.reflectivity = 0.7;
        values.roughness = 0.35;
    } else if (preset_id === 'glass') {
        values.albedo_rgba8 = pack_rgba8(0xb7, 0xdd, 0xff, 0x59);
        values.blend = 2;
        values.reflectivity = 0.25;
        values.roughness = 0.05;
    } else if (preset_id === 'emissive') {
        values.albedo_rgba8 = pack_rgba8(0x7f, 0xdc, 0xff);
        values.emissive = 1.0;
        values.roughness = 0.45;
    }

    return values;
}

// Future backend contract: replace console-only primitive_add with a structured
// scene/primitive_add request carrying both the primitive kind and preset metadata.
export function future_material_preset_contract(selection: PrimitiveMaterialSelection): string {
    const preset = material_preset(selection.material_preset_id);
    return [
        'scene/primitive_add',
        JSON.stringify({
            kind: selection.primitive_kind,
            material_preset: {
                id: preset.id,
                ...preset.metadata,
            },
        }),
    ].join(' ');
}

interface PrimitiveMaterialPaletteProps {
    value: PrimitiveMaterialSelection;
    on_change(next: PrimitiveMaterialSelection): void;
    on_add(selection: PrimitiveMaterialSelection): void;
}

export function PrimitiveMaterialPalette({ value, on_change, on_add }: PrimitiveMaterialPaletteProps) {
    const update_primitive = React.useCallback((primitive_kind: PrimitiveKind) => {
        on_change({ ...value, primitive_kind });
    }, [on_change, value]);

    const update_material = React.useCallback((material_preset_id: MaterialPresetId) => {
        on_change({ ...value, material_preset_id });
    }, [on_change, value]);

    return (
        <>
            <select
                className="psy-input"
                value={value.primitive_kind}
                onChange={(e) => update_primitive(e.target.value as PrimitiveKind)}
                aria-label="Primitive kind"
            >
                {PRIMITIVE_OPTIONS.map((option) => (
                    <option key={option.kind} value={option.kind}>
                        {option.label}
                    </option>
                ))}
            </select>
            <select
                className="psy-input"
                value={value.material_preset_id}
                onChange={(e) => update_material(e.target.value as MaterialPresetId)}
                aria-label="Material preset"
                title={future_material_preset_contract(value)}
            >
                {MATERIAL_PRESETS.map((preset) => (
                    <option key={preset.id} value={preset.id}>
                        {preset.label}
                    </option>
                ))}
            </select>
            <button type="button" className="psy-btn" onClick={() => on_add(value)}>
                add
            </button>
        </>
    );
}
