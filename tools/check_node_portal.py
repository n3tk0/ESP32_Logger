#!/usr/bin/env python3
"""
tools/check_node_portal.py — prove the WiFi node serves the page it ships.

WHY THIS EXISTS
---------------
The node's setup page is the shared one in node_portal/, built into
src/nodecfg/NodePortalPage.h (docs/NODE_CONFIG.md §6). Whether THAT page is
sound — tokens, freshness, JavaScript that parses — is
tools/check_node_portal_css.py's job. This one checks the other half: the
HTTP server in node/src/ConfigPortal.cpp that the page talks to.

That half cannot be compiled here (the ESP8266 toolchain is not installed in
most environments, and a compile would not catch any of this anyway), cannot
be opened without a node, and fails silently in every interesting way:

  * a route the page calls that the node does not answer — the page loads,
    and its Save button reports "HTTP 404" on a node on a wall;
  * a handler that forgot the basic-auth gate — the page is on the home LAN
    for the node's whole uptime, and that handler rewrites the collector
    address;
  * a GET that renders secrets, a POST that saves before validating, a page
    copied into RAM instead of streamed from flash;
  * a status reply missing a key the contract promises;
  * a portal loop that stopped pausing while a phone is connected, or stopped
    putting the radio back after a scan the browser walked away from.

So this reads the committed page and ConfigPortal.cpp and checks that:

  1. every request the page makes — req("METHOD", "/path") in its script — is
     a route ConfigPortal.cpp binds with that method, and every §6 route is;
  2. every route handler starts with the auth gate (authOk());
  3. "/" sends NODE_PORTAL_GZ with send_P and Content-Encoding: gzip;
  4. nothing in ConfigPortal.cpp encodes with NCJ_SECRETS;
  5. POST /api/config decodes without NCJ_DEC_REV, validates before it
     saves, and marks the config local;
  6. GET /api/status writes every key §6's table lists, and GET /api/scan
     writes state, nets and each net's ssid/rssi/ch/enc;
  7. the AP loop serves captive-portal DNS, stops its clock while a station
     is associated, and puts the radio back after a scan on its own.

Run:  python3 tools/check_node_portal.py
Exits non-zero with an explanation on any failure.
"""
from __future__ import annotations

import gzip
import re
import sys
from pathlib import Path

ROOT   = Path(__file__).resolve().parent.parent
PORTAL = ROOT / "node" / "src" / "ConfigPortal.cpp"
DOC    = ROOT / "docs" / "NODE_CONFIG.md"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_node_portal  # noqa: E402

# §6's route table, which the page may not exercise all of (it never GETs /
# by script, for one).
CONTRACT_ROUTES = {("GET", "/"), ("GET", "/api/config"), ("POST", "/api/config"),
                   ("GET", "/api/scan"), ("GET", "/api/status")}

errors: list[str] = []
_mark = [0]


def fail(msg: str) -> None:
    errors.append(msg)


def ok(msg: str) -> None:
    """Say a section passed — only if it added no error since the last one."""
    if len(errors) == _mark[0]:
        print(f"OK: {msg}")
    _mark[0] = len(errors)


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def function_body(src: str, name: str) -> str:
    """The braced body of `name(...)`'s definition, by brace counting."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", src)
    if not m:
        return ""
    depth, i = 0, m.end() - 1
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[m.end():i]
        i += 1
    return ""


def page_requests() -> set[tuple[str, str]]:
    gz = build_node_portal.committed_payload()
    if not gz:
        fail("src/nodecfg/NodePortalPage.h has no NODE_PORTAL_GZ array")
        return set()
    try:
        html = gzip.decompress(gz).decode("utf-8")
    except (OSError, EOFError, UnicodeDecodeError) as exc:
        fail(f"NODE_PORTAL_GZ does not inflate ({exc})")
        return set()
    reqs = set(re.findall(r'req\(\s*"([A-Z]+)"\s*,\s*"(/[^"?]*)', html))
    if not reqs:
        fail("found no req(\"METHOD\", \"/path\") calls in the page's script — "
             "this check needs updating alongside node_portal/app.js")
    return reqs


def status_keys_from_contract() -> list[str]:
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if line.startswith("| GET | `/api/status`"):
            cell = line.split("|")[3]
            return list(dict.fromkeys(re.findall(r'"([a-z_]+)"', cell)))
    fail("docs/NODE_CONFIG.md: the GET /api/status row of §6 is gone")
    return []


def main() -> int:
    if not PORTAL.is_file():
        print(f"ERROR: {PORTAL} not found")
        return 1
    raw = PORTAL.read_text(encoding="utf-8")
    src = strip_comments(raw)

    # ── 1. the routes ──────────────────────────────────────────────────────
    bind = function_body(src, "bindRoutes")
    if not bind:
        fail("ConfigPortal.cpp: bindRoutes() not found")
    routes: dict[tuple[str, str], str] = {}
    for path, method, handler in re.findall(
            r's_http\.on\(\s*"([^"]+)"\s*,\s*HTTP_([A-Z]+)\s*,\s*(\w+)\s*\)', bind):
        routes[(method, path)] = handler
    for path, handler in re.findall(r's_http\.on\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)', bind):
        routes[("ANY", path)] = handler
    nf = re.search(r"s_http\.onNotFound\(\s*(\w+)\s*\)", bind)

    def served(method: str, path: str) -> bool:
        return (method, path) in routes or ("ANY", path) in routes

    reqs = page_requests()
    for method, path in sorted(reqs | CONTRACT_ROUTES):
        if not served(method, path):
            who = "the page calls" if (method, path) in reqs else "§6 lists"
            fail(f"{who} {method} {path}, and ConfigPortal.cpp binds no such route — "
                 f"the node would answer it with the page, not the API")
    ok(f"{len(routes)} route(s) bound; the page's {len(reqs)} request(s) "
          f"and §6's {len(CONTRACT_ROUTES)} route(s) are all served")

    # ── 2. the auth gate on every handler ───────────────────────────────────
    handlers = set(routes.values()) | ({nf.group(1)} if nf else set())
    for h in sorted(handlers):
        body = function_body(src, h)
        if not body:
            fail(f"handler {h}() not found in ConfigPortal.cpp")
            continue
        first = body.strip().split(";", 1)[0]
        if "authOk()" not in first:
            fail(f"{h}() does not start with the auth gate (if (!authOk()) return;). "
                 f"On the LAN the page is served for the node's whole uptime, to "
                 f"anything on the network")
    gate = function_body(src, "authOk")
    if "s_background" not in gate or "authenticate(" not in gate:
        fail("authOk() no longer checks basic auth on the background server")
    start_bg = function_body(src, "portalStartBackground")
    if not re.search(r"basic_user\[0\]\s*==\s*'\\0'.*basic_pass\[0\]\s*==\s*'\\0'", start_bg, re.S):
        fail("portalStartBackground() no longer refuses to start without a basic-auth "
             "user and password — it must fail closed")
    ok(f"all {len(handlers)} handler(s) start with the auth gate, and the LAN "
          f"server refuses to start without credentials")

    # ── 3. the page, from flash, compressed ─────────────────────────────────
    page_h = routes.get(("GET", "/")) or routes.get(("ANY", "/"))
    body = function_body(src, page_h) if page_h else ""
    if not re.search(r"send_P\(.*?NODE_PORTAL_GZ.*?sizeof\(NODE_PORTAL_GZ\)\s*\)", body, re.S):
        fail("GET / must send NODE_PORTAL_GZ with send_P(…, sizeof(NODE_PORTAL_GZ)) — "
             "streamed from flash, never copied into the ESP8266's RAM")
    if not re.search(r'"Content-Encoding"\s*,\s*"gzip"', body):
        fail("GET / does not set Content-Encoding: gzip — the browser would show "
             "the compressed bytes")
    ok(f"/ streams the gzip page from flash with Content-Encoding: gzip")

    # ── 4. secrets are write-only ───────────────────────────────────────────
    if "NCJ_SECRETS" in src:
        fail("ConfigPortal.cpp uses NCJ_SECRETS: no GET may return the passphrase, "
             "the token or the basic-auth password (§0.5)")
    get_cfg = function_body(src, routes.get(("GET", "/api/config"), "")) if routes.get(("GET", "/api/config")) else ""
    if "encodeConfig" not in get_cfg or "Caps" not in get_cfg:
        fail("GET /api/config must answer {config: encodeConfig(…), caps: …}")
    ok(f"GET /api/config encodes config + caps, never with secrets")

    # ── 5. POST: decode, validate, then save, as local ──────────────────────
    post_h = routes.get(("POST", "/api/config"))
    post = function_body(src, post_h) if post_h else ""
    i_dec = post.find("decodeConfig(")
    i_val = min([i for i in (post.find("nodeValidate("), post.find("validate(")) if i >= 0] or [-1])
    i_save = post.find("storeSave(")
    if i_dec < 0 or i_val < 0 or i_save < 0:
        fail("POST /api/config must decodeConfig(), validate and storeSave()")
    elif not (i_dec < i_val < i_save):
        fail("POST /api/config must validate BEFORE it saves — a refused config "
             "must never reach /config.json")
    dec_call = post[i_dec:post.find(";", i_dec)] if i_dec >= 0 else ""
    if "NCJ_DEC_REV" in dec_call:
        fail("POST /api/config decodes rev/local from the page (NCJ_DEC_REV): a page "
             "echoing a stale rev would overwrite the real one")
    if not re.search(r"\.local\s*=\s*true", post):
        fail("POST /api/config does not mark the config local (§0.4) — the "
             "collector would never learn of the change")
    ok(f"POST /api/config decodes (no rev), validates, marks local, then saves")

    # ── 6. what the replies carry ───────────────────────────────────────────
    st = function_body(src, routes.get(("GET", "/api/status"), "")) if routes.get(("GET", "/api/status")) else ""
    keys = status_keys_from_contract()
    missing = [k for k in keys if f'doc["{k}"]' not in st]
    if missing:
        fail(f"GET /api/status does not write {', '.join(missing)} (§6)")
    scan = function_body(src, routes.get(("GET", "/api/scan"), "")) if routes.get(("GET", "/api/scan")) else ""
    for k in ("state", "nets"):
        if f'"{k}"' not in scan:
            fail(f"GET /api/scan does not write \"{k}\" (§6)")
    for k in ("ssid", "rssi", "ch", "enc"):
        if f'o["{k}"]' not in scan:
            fail(f"GET /api/scan does not write each net's \"{k}\" (§6)")
    ok(f"/api/status writes all {len(keys)} §6 keys; /api/scan writes "
          f"state, nets and ssid/rssi/ch/enc")

    # ── 7. the AP loop ──────────────────────────────────────────────────────
    loop = function_body(src, "portalRun")
    if not loop:
        fail("could not find portalRun()")
    if "s_dns.start(" not in loop or "processNextRequest" not in loop:
        fail("portalRun() no longer answers DNS for every name — phones stop showing "
             "the 'sign in to network' prompt")
    if "softAPgetStationNum" not in loop:
        fail("portalRun() no longer pauses its timeout while a phone is connected — "
             "the window closes under whoever is mid-configuration")
    # The scan widens the ESP8266 from AP to AP_STA, and the ESP8266 has ONE
    # radio: a station that associates drags the softAP onto its channel and
    # disconnects whoever is standing in the portal. narrowAfterScan() undoes
    # that — but reachable only from the /api/scan poll it is the browser's
    # job, and the browser is the half of this that can walk away. So
    # something in the portal's own loop must be able to put it back.
    if "WIFI_AP_STA" in src:
        narrowers = re.findall(r"\b(\w*[Ss]can\w*)\s*\(\s*\)\s*;", loop)
        if not narrowers:
            fail("portalRun()'s loop never calls anything that could put the radio "
                 "back after a scan. narrowAfterScan() is then reachable only from "
                 "the /api/scan poll, and a browser that walks away leaves the node "
                 "in AP_STA for the rest of the session — one radio, one channel, and "
                 "the phone in the portal gets dropped.")
        watchdog = function_body(src, "scanWatchdog")
        if "narrowAfterScan" not in watchdog:
            fail("scanWatchdog() does not narrow the radio, so calling it from the "
                 "loop achieves nothing")
        if narrowers and "narrowAfterScan" in watchdog:
            ok(f"the portal loop calls {', '.join(sorted(set(narrowers)))} — "
                  f"the radio is put back without the browser's help")
    ok(f"the AP loop answers DNS and holds its window open while a phone is connected")

    if errors:
        for e in errors:
            print(f"ERROR: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
