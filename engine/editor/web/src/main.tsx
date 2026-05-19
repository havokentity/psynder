// SPDX-License-Identifier: MIT
// Psynder editor — React entrypoint. Lane 20 owns. Routed via the engine's
// HTTP server at 127.0.0.1:7654/panels/<name> with the session token in
// the URL fragment.

import React from 'react';
import { createRoot } from 'react-dom/client';

const root = createRoot(document.getElementById('root')!);

const params = new URLSearchParams(window.location.search);
const panel = params.get('panel') ?? 'inspector';

root.render(
    <React.StrictMode>
        <div style={{ fontFamily: 'system-ui', padding: 16 }}>
            <h1>Psynder editor — {panel}</h1>
            <p>Lane 20 implements the panel suite.</p>
        </div>
    </React.StrictMode>,
);
