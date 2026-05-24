// SPDX-License-Identifier: MIT
// Local Inspector edit intent until lane 19 promotes this to generated IPC.
//
// Backend hook needed:
//   Handle msgpack envelope { ch: 'selection', type: 'component_edit', payload }
//   by validating entity_id + component + field against the current schema
//   layout_hash, applying payload.value, then echoing a selection `patch` or
//   refreshed `state`. Today IpcClient sends this through its legacy envelope
//   fallback because no typed opcode/channel exists yet.

import type {
    Channel,
    ComponentSchema,
    FieldSchema,
    SelectionState,
} from '../ipc/protocol';

export const COMPONENT_EDIT_CHANNEL: Channel = 'selection';
export const COMPONENT_EDIT_TYPE = 'component_edit';

export interface ComponentEditIntent {
    intent: 'component_field_edit.v1';
    source: 'inspector';
    entity_id: number;
    entity_label?: string;
    component: string;
    layout_hash?: string;
    field: string;
    field_kind: FieldSchema['kind'];
    value: unknown;
    previous_value?: unknown;
    committed_at_ms: number;
}

export function build_component_edit_intent(args: {
    selection: SelectionState;
    component: string;
    schema: ComponentSchema;
    field: FieldSchema;
    value: unknown;
    previous_value?: unknown;
}): ComponentEditIntent {
    return {
        intent: 'component_field_edit.v1',
        source: 'inspector',
        entity_id: args.selection.entity_id,
        entity_label: args.selection.entity_label,
        component: args.component,
        layout_hash: args.schema.layout_hash,
        field: args.field.name,
        field_kind: args.field.kind,
        value: args.value,
        previous_value: args.previous_value,
        committed_at_ms: Date.now(),
    };
}
