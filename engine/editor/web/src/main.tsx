// SPDX-License-Identifier: MIT
// Psynder editor — React entrypoint. Lane 20 owns. Routed via the engine's
// HTTP server at 127.0.0.1:7654/panels/<name> with the session token in the
// URL fragment. The App component picks which panel to render based on the
// URL; this file just mounts it and kicks the IPC client into life.

import React from 'react';
import { createRoot } from 'react-dom/client';

import { App } from './App';
import { get_client } from './ipc/client';
import './styles/panels.css';

// Eagerly create the IPC singleton so reconnect retries are running by the
// time the first panel `useEffect` fires.
get_client();

const container = document.getElementById('root');
if (!container) throw new Error('Psynder editor: missing #root element');

createRoot(container).render(
    <React.StrictMode>
        <App />
    </React.StrictMode>,
);
