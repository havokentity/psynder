// SPDX-License-Identifier: MIT
// Browser-local editor state shared by docked web-editor panels.

const DIRTY_KEY = 'psynder.editor.scene.dirty';
const RECENT_PATHS_KEY = 'psynder.editor.scene.recent_paths';
const LAST_PATH_KEY = 'psynder.editor.scene.last_path';
const MAX_RECENT_PATHS = 6;

type DirtyListener = (dirty: boolean) => void;

function safe_local_storage(): Storage | null {
    try {
        return window.localStorage;
    } catch {
        return null;
    }
}

function normalize_path(path: string): string {
    return path.trim();
}

function emit_dirty_change(dirty: boolean): void {
    window.dispatchEvent(new CustomEvent('psynder:editor-dirty', { detail: { dirty } }));
}

export function editor_scene_dirty(): boolean {
    return safe_local_storage()?.getItem(DIRTY_KEY) === '1';
}

export function set_editor_scene_dirty(dirty: boolean): void {
    safe_local_storage()?.setItem(DIRTY_KEY, dirty ? '1' : '0');
    emit_dirty_change(dirty);
}

export function mark_editor_scene_dirty(): void {
    set_editor_scene_dirty(true);
}

export function subscribe_editor_scene_dirty(listener: DirtyListener): () => void {
    const on_custom = (event: Event) => {
        const detail = (event as CustomEvent<{ dirty?: boolean }>).detail;
        listener(!!detail?.dirty);
    };
    const on_storage = (event: StorageEvent) => {
        if (event.key === DIRTY_KEY) listener(event.newValue === '1');
    };
    window.addEventListener('psynder:editor-dirty', on_custom);
    window.addEventListener('storage', on_storage);
    return () => {
        window.removeEventListener('psynder:editor-dirty', on_custom);
        window.removeEventListener('storage', on_storage);
    };
}

export function recent_scene_paths(): string[] {
    const raw = safe_local_storage()?.getItem(RECENT_PATHS_KEY);
    if (!raw) return [];
    try {
        const parsed = JSON.parse(raw);
        if (!Array.isArray(parsed)) return [];
        return parsed.filter((path): path is string => typeof path === 'string' && path.trim().length > 0);
    } catch {
        return [];
    }
}

export function last_scene_path(): string | null {
    const path = safe_local_storage()?.getItem(LAST_PATH_KEY);
    return path && path.trim().length > 0 ? path : null;
}

export function remember_scene_path(path: string): string[] {
    const clean = normalize_path(path);
    if (!clean) return recent_scene_paths();
    const next = [
        clean,
        ...recent_scene_paths().filter((recent) => recent !== clean),
    ].slice(0, MAX_RECENT_PATHS);
    const storage = safe_local_storage();
    storage?.setItem(LAST_PATH_KEY, clean);
    storage?.setItem(RECENT_PATHS_KEY, JSON.stringify(next));
    window.dispatchEvent(new CustomEvent('psynder:scene-paths', { detail: { paths: next, last: clean } }));
    return next;
}

export function subscribe_recent_scene_paths(listener: (paths: string[]) => void): () => void {
    const on_custom = (event: Event) => {
        const detail = (event as CustomEvent<{ paths?: string[] }>).detail;
        if (Array.isArray(detail?.paths)) listener(detail.paths);
    };
    const on_storage = (event: StorageEvent) => {
        if (event.key === RECENT_PATHS_KEY) listener(recent_scene_paths());
    };
    window.addEventListener('psynder:scene-paths', on_custom);
    window.addEventListener('storage', on_storage);
    return () => {
        window.removeEventListener('psynder:scene-paths', on_custom);
        window.removeEventListener('storage', on_storage);
    };
}
