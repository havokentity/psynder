// SPDX-License-Identifier: MIT
// Shared browser-side scene command helpers for editor chrome and panels.

export const DEFAULT_SCENE_PATH = 'assets/main.psyscene';
export const EXAMPLE_SCENES = [
    DEFAULT_SCENE_PATH,
    'assets/crate_room.psyscene',
];
export const ENGINE_SCENE_PATH_HELP = 'engine-relative scene path, for example assets/main.psyscene';

export interface PsySaveFilePickerOptions {
    suggestedName?: string;
    types?: Array<{
        description?: string;
        accept: Record<string, string[]>;
    }>;
    excludeAcceptAllOption?: boolean;
}

export type PsySavePickerWindow = Window & {
    showSaveFilePicker?: (options?: PsySaveFilePickerOptions) => Promise<{ name?: string }>;
};

export function console_arg(value: string): string {
    if (value && /^[A-Za-z0-9_./:@-]+$/.test(value)) return value;
    const escaped = value
        .replace(/\\/g, '\\\\')
        .replace(/"/g, '\\"')
        .replace(/\n/g, '\\n')
        .replace(/\t/g, '\\t')
        .replace(/\r/g, '\\n')
        .replace(/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/g, ' ');
    return `"${escaped}"`;
}

export function console_command(name: string, ...args: Array<string | number>): string {
    return [name, ...args.map((arg) => console_arg(String(arg)))].join(' ');
}

export function load_scene_command(path: string): string {
    return console_command('arcade_load_scene', path);
}

export function save_scene_command(path: string): string {
    return console_command('scene_save', path);
}

export function fps_starter_template_command(): string {
    return console_command('template_create', 'fps_starter');
}

export function scene_basename(path: string): string {
    const normalized = path.replace(/\\/g, '/');
    const slash = normalized.lastIndexOf('/');
    return slash >= 0 ? normalized.slice(slash + 1) : normalized;
}

export function scene_dirname(path: string): string {
    const normalized = path.replace(/\\/g, '/');
    const slash = normalized.lastIndexOf('/');
    return slash >= 0 ? normalized.slice(0, slash + 1) : '';
}

export function ensure_scene_extension(name: string): string {
    return name.toLowerCase().endsWith('.psyscene') ? name : `${name}.psyscene`;
}

export function scene_path_with_basename(current_path: string, basename: string): string {
    return `${scene_dirname(current_path)}${ensure_scene_extension(basename)}`;
}

export async function prompt_save_scene_path(current_path: string): Promise<string | null> {
    const clean_path = current_path.trim() || DEFAULT_SCENE_PATH;
    const current_name = ensure_scene_extension(scene_basename(clean_path) || 'untitled.psyscene');
    let suggested_path = clean_path;
    const picker = (window as PsySavePickerWindow).showSaveFilePicker;

    if (picker) {
        try {
            const handle = await picker({
                suggestedName: current_name,
                types: [
                    {
                        description: 'Psynder scene',
                        accept: { 'application/octet-stream': ['.psyscene'] },
                    },
                ],
                excludeAcceptAllOption: false,
            });
            if (handle.name) suggested_path = scene_path_with_basename(clean_path, handle.name);
        } catch (err) {
            if (err instanceof DOMException && err.name === 'AbortError') return null;
        }
    }

    const next_path = window.prompt(`Save scene as ${ENGINE_SCENE_PATH_HELP}`, suggested_path);
    return next_path === null ? null : next_path;
}

export function confirm_dirty_scene_navigation(dirty: boolean, verb: string): boolean {
    if (!dirty) return true;
    return window.confirm(`Discard unsaved scene changes and ${verb}?`);
}

export function is_noop_history_result(action: string | undefined, text: string): boolean {
    if (action !== 'undo' && action !== 'redo') return false;
    return text.toLowerCase().includes(`nothing to ${action}`);
}

export interface SceneLoadFailure {
    path?: string;
    text: string;
    request_id?: number;
    code?: string;
}

// Expected backend shape: scene.load_failed with
// { path, error/message/text, request_id? }. The aliases keep older or
// generic scene error events useful while lane 19 catches up.
export function scene_load_failure_from_event(type: string, payload: unknown): SceneLoadFailure | null {
    const rec = is_record(payload) ? payload : {};
    const type_load_failed =
        type === 'load_failed' ||
        type === 'load_error' ||
        type === 'scene_load_failed';
    const raw_action = String(rec.action ?? rec.command ?? rec.source ?? '').toLowerCase();
    const generic_load_failed = (
        type === 'error' ||
        type === 'io_failed' ||
        type === 'command_failed'
    ) && (raw_action.includes('load') || raw_action.includes('arcade_load_scene'));

    if (!type_load_failed && !generic_load_failed) return null;

    const path = string_field(rec.path) ?? string_field(rec.scene_path) ?? string_field(rec.requested_path);
    const detail =
        string_field(rec.error) ??
        string_field(rec.message) ??
        string_field(rec.text) ??
        'backend did not provide a reason';
    const code = string_field(rec.code);
    const request_id = number_field(rec.request_id) ?? number_field(rec.id);
    const suffix = code ? ` (${code})` : '';
    return {
        path,
        text: path ? `failed to open ${path}: ${detail}${suffix}` : `scene load failed: ${detail}${suffix}`,
        request_id,
        code,
    };
}

function string_field(value: unknown): string | undefined {
    return typeof value === 'string' && value.trim() ? value : undefined;
}

function number_field(value: unknown): number | undefined {
    return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function is_record(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}
