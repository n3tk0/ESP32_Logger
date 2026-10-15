// ============================================================================
// src/export/UrlMask.h — how much of a URL a log line may show
//
// Log lines go to GET /api/log, which anyone on the LAN can read. A webhook
// URL (Discord, Slack, IFTTT) carries its secret in the path or the query,
// so the log shows scheme://host[:port] and "/…" for the rest.
// ============================================================================
#pragma once

#include <string.h>

/// Length of `url`'s scheme://host[:port] part: print it with "%.*s".
/// A user:password@ in it stops the part at the scheme, so the
/// credentials never reach the log.
inline int urlHostLen(const char* url) {
    if (!url) return 0;
    const char* s = strstr(url, "://");
    const char* p = s ? s + 3 : url;
    const char* h = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') {
        if (*p == '@') return (int)(h - url);
        p++;
    }
    return (int)(p - url);
}

/// "…" for whatever the log leaves out ("/…" after a host), so the line
/// still says there was more.
inline const char* urlRestMark(const char* url) {
    if (!url) return "";
    const int n = urlHostLen(url);
    if (!url[n]) return "";
    return (n && url[n - 1] == '/') ? "…" : "/…";
}
