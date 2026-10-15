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
inline int urlHostLen(const char* url) {
    if (!url) return 0;
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    return (int)(p - url);
}

/// "/…" when anything follows the host, so the line still says there was a path.
inline const char* urlRestMark(const char* url) {
    return url && url[urlHostLen(url)] ? "/…" : "";
}
