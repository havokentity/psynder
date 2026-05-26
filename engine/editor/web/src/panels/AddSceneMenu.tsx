// SPDX-License-Identifier: MIT
// Psynder editor - compact scene add menu for hierarchy actions.

import React from 'react';

import {
    MATERIAL_PRESETS,
    PRIMITIVE_OPTIONS,
    primitive_label,
    primitive_material_add_command,
    type MaterialPresetId,
    type PrimitiveKind,
    type PrimitiveMaterialSelection,
} from './PrimitiveMaterialPalette';

interface AddSceneMenuProps {
    value: PrimitiveMaterialSelection;
    on_change(next: PrimitiveMaterialSelection): void;
    on_add_primitive(selection: PrimitiveMaterialSelection): void;
    on_add_empty_entity(): void;
    on_add_camera(): void;
    on_add_light(): void;
    on_add_gameplay(kind: string): void;
    on_create_fps_starter(): void;
}

export function AddSceneMenu({
    value,
    on_change,
    on_add_primitive,
    on_add_empty_entity,
    on_add_camera,
    on_add_light,
    on_add_gameplay,
    on_create_fps_starter,
}: AddSceneMenuProps) {
    const details_ref = React.useRef<HTMLDetailsElement | null>(null);

    const update_material = React.useCallback((material_preset_id: MaterialPresetId) => {
        on_change({ ...value, material_preset_id });
    }, [on_change, value]);

    const add_primitive_kind = React.useCallback((primitive_kind: PrimitiveKind) => {
        on_add_primitive({ ...value, primitive_kind });
        if (details_ref.current) details_ref.current.open = false;
    }, [on_add_primitive, value]);

    const run_template = React.useCallback((handler: () => void) => {
        handler();
        if (details_ref.current) details_ref.current.open = false;
    }, []);

    return (
        <details ref={details_ref} className="psy-add-menu">
            <summary
                className="psy-btn psy-btn-primary"
                aria-label="Add scene item"
                title="Add primitive, entity, camera, or light"
            >
                add
            </summary>
            <div className="psy-add-menu-popover" role="menu" aria-label="Add scene item">
                <section className="psy-add-section" aria-label="Material preset">
                    <span className="psy-add-section-title">material</span>
                    <div className="psy-material-swatches" role="radiogroup" aria-label="Primitive material preset">
                        {MATERIAL_PRESETS.map((preset) => (
                            <button
                                key={preset.id}
                                type="button"
                                className={`psy-material-swatch ${value.material_preset_id === preset.id ? 'is-selected' : ''}`}
                                role="radio"
                                aria-checked={value.material_preset_id === preset.id}
                                onClick={() => update_material(preset.id)}
                                title={preset.label}
                            >
                                <span style={{ '--psy-swatch': preset.metadata.albedo } as React.CSSProperties} />
                            </button>
                        ))}
                    </div>
                </section>

                <section className="psy-add-section" aria-label="Primitive objects">
                    <span className="psy-add-section-title">primitive</span>
                    <div className="psy-add-grid">
                        {PRIMITIVE_OPTIONS.map((option) => (
                            <button
                                key={option.kind}
                                type="button"
                                className="psy-btn"
                                role="menuitem"
                                title={primitive_material_add_command({ ...value, primitive_kind: option.kind }, { include_material: true })}
                                onClick={() => add_primitive_kind(option.kind)}
                            >
                                {primitive_label(option.kind)}
                            </button>
                        ))}
                    </div>
                </section>

                <section className="psy-add-section" aria-label="Scene templates">
                    <span className="psy-add-section-title">scene</span>
                    <div className="psy-add-grid">
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(on_add_empty_entity)}>
                            Empty
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(on_add_camera)}>
                            Camera
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(on_add_light)}>
                            Light
                        </button>
                    </div>
                </section>

                <section className="psy-add-section" aria-label="Gameplay templates">
                    <span className="psy-add-section-title">gameplay</span>
                    <button
                        type="button"
                        className="psy-btn psy-btn-primary psy-add-wide-action"
                        role="menuitem"
                        onClick={() => run_template(on_create_fps_starter)}
                        title="template_create fps_starter"
                    >
                        FPS Starter
                    </button>
                    <div className="psy-add-grid">
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(() => on_add_gameplay('player_start'))}>
                            Player Start
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(() => on_add_gameplay('fps_player'))}>
                            FPS Player
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(() => on_add_gameplay('enemy'))}>
                            Enemy
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(() => on_add_gameplay('pickup'))}>
                            Pickup
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(() => on_add_gameplay('trigger'))}>
                            Trigger
                        </button>
                        <button type="button" className="psy-btn" role="menuitem" onClick={() => run_template(() => on_add_gameplay('door'))}>
                            Door
                        </button>
                    </div>
                </section>
            </div>
        </details>
    );
}
