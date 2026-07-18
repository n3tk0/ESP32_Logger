#!/usr/bin/env python3
"""
check_api_docs.py — keep docs/ARCHITECTURE.md §6 in sync with the real HTTP API.

The web API is registered with `server.on("<path>", HTTP_<METHOD>, ...)` across
src/web/. This script extracts every registered route and fails if any
API route is missing a row in the ARCHITECTURE.md "Local API Reference" table.
It exists because the hand-written table had drifted to ~10% coverage.

Usage:
    python3 tools/check_api_docs.py            # verify (exit 1 on drift)
    python3 tools/check_api_docs.py --list     # print the extracted route set

Captive-portal OS probes and SPA HTML redirects are intentionally NOT part of
the documented JSON/action API and are allow-listed below.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_FILES = [
    "src/web/WebServer.cpp",
    "src/web/ApiHandlers.cpp",
    "src/web/FirstRunHandler.cpp",
]
DOC = "docs/ARCHITECTURE.md"

# Routes that are real but do NOT belong in the API reference:
#  - captive-portal / OS connectivity probes (return redirects/204s)
#  - SPA HTML shell + hash-route redirects (serve or 302 to index.html)
ALLOWLIST = {
    "/", "/setup", "/dashboard", "/files", "/live", "/settings",
    "/settings_device", "/settings_hardware", "/settings_theme",
    "/settings_time", "/settings_network", "/settings_datalog",
    "/cdn-boot.js",
    "/hotspot-detect.html", "/library/test/success.html", "/generate_204",
    "/gen_204", "/connecttest.txt", "/redirect", "/ncsi.txt",
    "/canonical.html", "/success.txt",
}

# Dynamic per-module routes are built at runtime as `base + "/enable"` etc.
# (ApiHandlers.cpp loops over the module registry). Represent them canonically.
DYNAMIC = {
    "enablePath.c_str()":  "/api/modules/:id/enable",
    "restartPath.c_str()": "/api/modules/:id/restart",
    "base.c_str()":        "/api/modules/:id",
}

# `\s*` between tokens spans newlines, so this matches registrations wrapped
# across lines (e.g. by clang-format) as well as single-line ones.
REG_RE = re.compile(
    r'server\.on\s*\(\s*(?:"([^"]+)"|([A-Za-z_][A-Za-z0-9_.()]*))\s*,\s*HTTP_(GET|POST|PUT|DELETE)'
)


def _strip_comments(text):
    """Remove C/C++ comments so commented-out registrations aren't matched.
    Block comments first (non-greedy), then line comments. The server.on(...)
    call sites contain no string literals with `//` or `/*`, so this is safe
    for route extraction (the mangled PROGMEM HTML/CSS blobs hold no routes)."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def extract_routes():
    routes = set()
    for rel in WEB_FILES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as fh:
            content = _strip_comments(fh.read())
        for m in REG_RE.finditer(content):
            literal, ident, method = m.group(1), m.group(2), m.group(3)
            if literal is not None:
                route = literal
            elif ident in DYNAMIC:
                route = DYNAMIC[ident]
            else:
                # Unknown non-literal registration target — surface it so a
                # new dynamic pattern can't slip through unnoticed.
                route = f"<dynamic:{ident}>"
            routes.add((method, route))
    return routes


def documented_routes():
    """Routes named in the ARCHITECTURE.md API reference, matching both forms:
      - table rows:  | GET | `/api/status` | ... |
      - inline prose: `POST /sync_time`
    """
    doc = os.path.join(ROOT, DOC)
    with open(doc, encoding="utf-8") as fh:
        text = fh.read()
    documented = set()
    # Table-row form: a bare METHOD cell followed by a `/path` cell.
    for m in re.finditer(r'\|\s*(GET|POST|PUT|DELETE)\s*\|\s*`(/[^\s`?]+)`', text):
        documented.add((m.group(1), m.group(2)))
    # Combined inline form: `METHOD /path`.
    for m in re.finditer(r'`(GET|POST|PUT|DELETE)\s+(/[^\s`?]+)', text):
        documented.add((m.group(1), m.group(2)))
    return documented


def main():
    routes = extract_routes()
    if "--list" in sys.argv:
        for method, route in sorted(routes, key=lambda x: (x[1], x[0])):
            tag = "  (allow)" if route in ALLOWLIST else ""
            print(f"{method:5} {route}{tag}")
        return 0

    documented = documented_routes()
    api_routes = {(m, r) for (m, r) in routes if r not in ALLOWLIST}

    missing = sorted(r for r in api_routes if r not in documented)
    dynamic_unknown = sorted(r for (_, r) in routes if r.startswith("<dynamic:"))

    if dynamic_unknown:
        print("ERROR: unrecognized dynamic route registration(s):")
        for r in dynamic_unknown:
            print(f"  {r}  — add a mapping to DYNAMIC in {os.path.relpath(__file__, ROOT)}")
    if missing:
        print(f"ERROR: {len(missing)} API route(s) registered in src/web/ but "
              f"missing from {DOC} §6 (Local API Reference):")
        for method, route in missing:
            print(f"  {method} {route}")
        print("\nAdd a row for each (with its auth/CSRF requirement), or "
              "allow-list it in tools/check_api_docs.py if it is not a public API route.")

    if missing or dynamic_unknown:
        return 1
    print(f"OK: all {len(api_routes)} API routes are documented in {DOC} §6.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
