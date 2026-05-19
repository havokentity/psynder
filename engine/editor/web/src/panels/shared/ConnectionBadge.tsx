// SPDX-License-Identifier: MIT
// Psynder editor — visible reminder of the WS state for every panel. Keeps
// the panel header from looking like a regular static page when the engine
// is offline.

import React from 'react';

import { get_client } from '../../ipc/client';
import type { ConnectionState } from '../../ipc/client';

const LABELS: Record<ConnectionState, string> = {
    connecting:   'connecting',
    open:         'live',
    closed:       'closed',
    reconnecting: 'reconnecting',
    mock:         'mock (offline)',
};

export function ConnectionBadge() {
    const client = React.useMemo(() => get_client(), []);
    const [state, set_state] = React.useState<ConnectionState>(client.current_state());
    React.useEffect(() => {
        const unsub = client.on_state(set_state);
        return unsub;
    }, [client]);
    return (
        <span
            className={`psy-conn-badge is-${state}`}
            title={`engine connection: ${LABELS[state]}`}
        >
            <span className="psy-conn-dot" />
            {LABELS[state]}
        </span>
    );
}
