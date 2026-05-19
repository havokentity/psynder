// SPDX-License-Identifier: MIT
// Psynder editor — hook that streams mock IPC frames into the active client
// whenever the connection is in the `mock` state, so panels stay usable when
// the engine is offline. The mock driver pushes frames through the client's
// `deliver()` synthetic-envelope entry point so the same listener fan-out is
// exercised in dev and in production.

import React from 'react';

import { get_client } from '../../ipc/client';
import { start_mock } from '../../ipc/mock';

export function use_mock_when_offline() {
    const client = React.useMemo(() => get_client(), []);

    React.useEffect(() => {
        let driver: ReturnType<typeof start_mock> | null = null;
        const unsub_state = client.on_state((s) => {
            if (s === 'mock' && !driver) {
                driver = start_mock((env) => client.deliver(env));
            } else if (s !== 'mock' && driver) {
                driver.stop();
                driver = null;
            }
        });
        return () => {
            unsub_state();
            if (driver) {
                driver.stop();
                driver = null;
            }
        };
    }, [client]);
}
