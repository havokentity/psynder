// SPDX-License-Identifier: MIT
// Psynder editor — scene control panel for Arcade sessions.

import React from 'react';

import { get_client } from '../ipc/client';
import type { ConnectionState } from '../ipc/client';
import type { ConsoleEval, ConsoleResult, Envelope } from '../ipc/protocol';
import { ConnectionBadge } from './shared/ConnectionBadge';
import { use_mock_when_offline } from './shared/use_mock_when_offline';

type SceneStatus = 'idle' | 'busy' | 'ok' | 'error';

interface SceneCommand {
    id: number;
    source: string;
    label: string;
}

const EXAMPLE_SCENES = [
    'assets/main.psyscene',
    'assets/crate_room.psyscene',
];

export function SceneView() {
    use_mock_when_offline();
    const client = React.useMemo(() => get_client(), []);
    const seq = React.useRef(24000);
    const pending = React.useRef(new Map<number, SceneCommand>());
    const [connection, set_connection] = React.useState<ConnectionState>(client.current_state());
    const [status, set_status] = React.useState<SceneStatus>('idle');
    const [message, set_message] = React.useState('No scene active');
    const [scene_path, set_scene_path] = React.useState(EXAMPLE_SCENES[0]);
    const [recent, set_recent] = React.useState<SceneCommand[]>([]);

    React.useEffect(() => {
        const unsub_state = client.on_state((state) => {
            set_connection(state);
            if (state === 'open') client.send('console', 'subscribe', {});
        });
        const unsub_console = client.subscribe('console', (env: Envelope) => {
            if (env.type !== 'result') return;
            const result = env.payload as ConsoleResult;
            const command = pending.current.get(result.id);
            if (!command) return;
            pending.current.delete(result.id);
            set_status(result.ok ? 'ok' : 'error');
            set_message(result.text || command.label);
        });
        return () => {
            unsub_state();
            unsub_console();
        };
    }, [client]);

    const send_command = React.useCallback((source: string, label: string) => {
        const id = ++seq.current;
        const command = { id, source, label };
        pending.current.set(id, command);
        set_recent((prev) => [command, ...prev.filter((item) => item.source !== source)].slice(0, 5));
        set_status('busy');
        set_message(label);
        if (connection === 'open') {
            client.send<ConsoleEval>('console', 'eval', { id, source, mode: 'console' });
        } else {
            pending.current.delete(id);
            set_status('error');
            set_message(`connection is ${connection}`);
        }
    }, [client, connection]);

    const load_scene = React.useCallback(() => {
        const path = scene_path.trim();
        if (!path) {
            set_status('error');
            set_message('scene path required');
            return;
        }
        send_command(`arcade_load_scene ${path}`, `loading ${path}`);
    }, [scene_path, send_command]);

    return (
        <div className="psy-panel psy-scene">
            <header className="psy-panel-header">
                <h2>Scene</h2>
                <ConnectionBadge />
                <button
                    type="button"
                    className="psy-btn psy-btn-primary"
                    onClick={() => send_command('arcade_new_scene', 'creating blank scene')}
                >
                    new scene
                </button>
                <button
                    type="button"
                    className="psy-btn psy-btn-danger"
                    onClick={() => {
                        if (window.confirm('Quit Psynder Arcade?')) {
                            send_command('quit', 'quitting Psynder Arcade');
                        }
                    }}
                >
                    quit
                </button>
            </header>

            <div className="psy-scene-body">
                <section className="psy-scene-viewport" aria-label="Arcade viewport bridge">
                    <div className="psy-scene-grid" aria-hidden="true" />
                    <div className="psy-scene-reticle" aria-hidden="true" />
                    <div className="psy-scene-viewport-meta">
                        <span>Psynder Arcade</span>
                        <b>{status}</b>
                    </div>
                </section>

                <section className="psy-scene-actions" aria-label="Scene actions">
                    <div className={`psy-scene-status is-${status}`} role="status">
                        <span className="psy-scene-status-dot" />
                        <span>{message}</span>
                    </div>

                    <div className="psy-scene-load-row">
                        <input
                            className="psy-input"
                            value={scene_path}
                            onChange={(e) => set_scene_path(e.target.value)}
                            spellCheck={false}
                            aria-label="Cooked scene path"
                        />
                        <button type="button" className="psy-btn" onClick={load_scene}>
                            load
                        </button>
                    </div>

                    <div className="psy-scene-quick" aria-label="Scene quick actions">
                        <button
                            type="button"
                            className="psy-btn psy-btn-ghost"
                            onClick={() => send_command('arcade_open_editor', 'opening workbench')}
                        >
                            workbench
                        </button>
                        <button
                            type="button"
                            className="psy-btn psy-btn-ghost"
                            onClick={() => send_command('editor_panel props', 'opening props')}
                        >
                            props
                        </button>
                        <button
                            type="button"
                            className="psy-btn psy-btn-ghost"
                            onClick={() => send_command('editor_panel psygraph', 'opening PsyGraph')}
                        >
                            psygraph
                        </button>
                    </div>

                    <div className="psy-scene-examples" aria-label="Example scene paths">
                        {EXAMPLE_SCENES.map((path) => (
                            <button
                                key={path}
                                type="button"
                                className="psy-chip-btn"
                                onClick={() => set_scene_path(path)}
                            >
                                {path}
                            </button>
                        ))}
                    </div>

                    <div className="psy-scene-recent" aria-label="Recent scene commands">
                        {recent.length === 0 ? (
                            <div className="psy-empty">idle</div>
                        ) : recent.map((item) => (
                            <button
                                key={item.id}
                                type="button"
                                className="psy-scene-recent-row"
                                onClick={() => send_command(item.source, item.label)}
                            >
                                <span>{item.label}</span>
                                <code>{item.source}</code>
                            </button>
                        ))}
                    </div>
                </section>
            </div>
        </div>
    );
}
