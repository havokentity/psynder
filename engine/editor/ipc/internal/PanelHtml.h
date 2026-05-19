// SPDX-License-Identifier: MIT
// Psynder editor IPC — tiny embedded bootstrap HTML for editor panels.
//
// Wave-A scope: the real React panel UI lives under engine/editor/web/ (Lane
// 20). In editor mode, the engine spawns Chrome at
//   http://127.0.0.1:7654/panels/<name>?token=...
// and the browser fetches this minimal page. The page connects back to the
// engine via WebSocket (same port, ws://) for live data. Once Lane 20 builds
// the React bundle this is replaced by the bundled static assets.

#pragma once

#include <string_view>

namespace psynder::editor::ipc::internal {

inline constexpr std::string_view kPanelBootstrapHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<title>Psynder Editor Panel</title>
<style>
  body { font-family: system-ui, sans-serif; background: #1a1a1a; color: #eee; margin: 0; padding: 1.5rem; }
  h1 { font-size: 1.1rem; margin: 0 0 0.5rem; color: #9cf; }
  pre { background: #0e0e0e; padding: 0.75rem; border-radius: 4px; overflow-x: auto; max-height: 60vh; }
  .meta { font-size: 0.85rem; color: #888; margin-bottom: 0.75rem; }
</style>
</head>
<body>
<h1>Psynder Editor — IPC bootstrap</h1>
<div class="meta">Wave-A placeholder. React panels (Lane 20) replace this page.</div>
<pre id="log">connecting...
</pre>
<script>
  (function () {
    const log = document.getElementById('log');
    function append(s) { log.textContent += s + '\n'; }
    // The session token arrives in the URL fragment per DESIGN.md §10.6.
    const fragMatch = location.hash.match(/token=([^&]+)/);
    const queryMatch = location.search.match(/[?&]token=([^&]+)/);
    const token = fragMatch ? fragMatch[1] : (queryMatch ? queryMatch[1] : '');
    if (!token) { append('error: no token in URL fragment'); return; }
    const ws = new WebSocket('ws://' + location.host + '/ws#token=' + encodeURIComponent(token));
    ws.binaryType = 'arraybuffer';
    ws.onopen = function () { append('ws: open'); };
    ws.onmessage = function (ev) {
      const sz = (typeof ev.data === 'string') ? ev.data.length : ev.data.byteLength;
      append('ws: msg ' + sz + ' bytes');
    };
    ws.onerror = function () { append('ws: error'); };
    ws.onclose = function (ev) { append('ws: close (' + ev.code + ')'); };
  })();
</script>
</body>
</html>
)HTML";

}  // namespace psynder::editor::ipc::internal
