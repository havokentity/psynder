// SPDX-License-Identifier: MIT
// Psynder editor IPC — minimal HTTP/1.1 request parser.
//
// Tolerates only the request flavor we actually receive: GET <path> HTTP/1.1,
// CRLF-terminated header lines, no body. Enough to spot a WebSocket upgrade
// request and serve a small set of static panel-bootstrap routes.

#pragma once

#include "core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace psynder::editor::ipc::http {

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    std::string method;
    std::string path;     // full path including ?query and #fragment
    std::string version;  // e.g. "HTTP/1.1"
    std::vector<Header> headers;

    const std::string* find(std::string_view name) const noexcept {
        for (const auto& h : headers) {
            if (h.name.size() != name.size()) continue;
            bool eq = true;
            for (::psynder::usize i = 0; i < name.size(); ++i) {
                char a = h.name[i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + ('a' - 'A'));
                if (a != b) { eq = false; break; }
            }
            if (eq) return &h.value;
        }
        return nullptr;
    }
};

// On insufficient data, returns 0 (need more). On parse error, returns SIZE_MAX.
// On success, returns the number of bytes consumed (offset to first body byte).
inline ::psynder::usize parse_request(const char* p, ::psynder::usize n, Request& req) {
    // Find end of headers (CRLF CRLF).
    static constexpr const char* kEoh = "\r\n\r\n";
    ::psynder::usize eoh = static_cast<::psynder::usize>(-1);
    for (::psynder::usize i = 0; i + 4 <= n; ++i) {
        if (p[i] == '\r' && p[i+1] == '\n' && p[i+2] == '\r' && p[i+3] == '\n') {
            eoh = i;
            break;
        }
    }
    if (eoh == static_cast<::psynder::usize>(-1)) {
        if (n > 32 * 1024) return SIZE_MAX;  // too big without headers terminating
        return 0;
    }

    // Request line: METHOD SP PATH SP VERSION CRLF
    ::psynder::usize i = 0;
    while (i < eoh && p[i] != ' ') ++i;
    if (i == eoh) return SIZE_MAX;
    req.method.assign(p, i);
    ++i;
    ::psynder::usize j = i;
    while (j < eoh && p[j] != ' ') ++j;
    if (j == eoh) return SIZE_MAX;
    req.path.assign(p + i, j - i);
    ++j;
    ::psynder::usize k = j;
    while (k < eoh && p[k] != '\r') ++k;
    if (k == eoh || p[k] != '\r' || k + 1 >= eoh || p[k+1] != '\n') return SIZE_MAX;
    req.version.assign(p + j, k - j);

    // Headers — the last header line ends at p[eoh..eoh+1] = "\r\n" (eoh is
    // the start of the trailing CRLF CRLF), so the scan bound is `<= eoh`.
    ::psynder::usize cur = k + 2;  // past request-line CRLF
    while (cur < eoh) {
        ::psynder::usize line_end = cur;
        while (line_end < eoh && p[line_end] != '\r') ++line_end;
        // The last header's CRLF starts exactly at eoh; that's valid.
        if (line_end > eoh) return SIZE_MAX;
        ::psynder::usize colon = cur;
        while (colon < line_end && p[colon] != ':') ++colon;
        if (colon >= line_end) return SIZE_MAX;
        Header h;
        h.name.assign(p + cur, colon - cur);
        ++colon;
        while (colon < line_end && (p[colon] == ' ' || p[colon] == '\t')) ++colon;
        h.value.assign(p + colon, line_end - colon);
        req.headers.emplace_back(std::move(h));
        if (line_end == eoh) break;  // we just consumed the last header
        cur = line_end + 2;          // skip the header's CRLF
    }
    (void)kEoh;
    return eoh + 4;  // include trailing CRLF CRLF
}

}  // namespace psynder::editor::ipc::http
