#!/usr/bin/env python3
"""
tools/check_node_portal.py — prove the node's setup portal page is real.

WHY THIS EXISTS
---------------
node/src/ConfigPortal.cpp serves a page that is HTML, CSS and JavaScript
written as C++ string literals. Nothing checks any of it:

  * the compiler sees one long string, so a missing brace in the JavaScript
    compiles perfectly and produces a page whose Next button does nothing;
  * the ESP8266 toolchain is not installed in most environments (and not in
    the sandbox this repository's agent runs in), so even a compile is often
    not available;
  * the page cannot be opened without a node, and a node with a broken portal
    is exactly the node you cannot configure.

So this reconstructs the page the way the firmware assembles it — the literals
in handleRoot(), plus the fields row()/pinRow() emit, plus the lookup tables
the C++ loops write out of NodePins.h — and then checks it:

  1. the JavaScript parses (node --check), and
  2. every element the JavaScript reaches for by id actually exists in the
     markup, which is the failure the string-in-a-string form invites.

Run:  python3 tools/check_node_portal.py
Exits non-zero with an explanation on any failure.
"""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT   = Path(__file__).resolve().parent.parent
PORTAL = ROOT / "node" / "src" / "ConfigPortal.cpp"
PINS   = ROOT / "node" / "src" / "NodePins.h"


def fail(msg: str) -> None:
    print(f"ERROR: {msg}")
    sys.exit(1)


# ---------------------------------------------------------------------------
# C++ string literals → text
# ---------------------------------------------------------------------------
_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "\0"}


def unescape(lit: str) -> str:
    """Turn the body of a C string literal into the bytes it stands for."""
    out, i = [], 0
    while i < len(lit):
        c = lit[i]
        if c == "\\" and i + 1 < len(lit):
            nxt = lit[i + 1]
            if nxt in _ESCAPES:
                out.append(_ESCAPES[nxt])
                i += 2
                continue
            if nxt == "x":                      # \xE2 — the UTF-8 in NodePins.h
                out.append(chr(int(lit[i + 2:i + 4], 16)))
                i += 4
                continue
            if nxt == "u":                      # \uXXXX passes through to JS
                out.append(lit[i:i + 6])
                i += 6
                continue
        out.append(c)
        i += 1
    return "".join(out)


_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def literals_of(chunk: str) -> str:
    """Every string literal in `chunk`, concatenated, comments removed."""
    chunk = re.sub(r"//[^\n]*", "", chunk)
    chunk = re.sub(r"/\*.*?\*/", "", chunk, flags=re.S)
    return "".join(unescape(m.group(1)) for m in _LITERAL.finditer(chunk))


def function_body(src: str, signature: str) -> str:
    """The braced body of a function, by brace counting from its signature."""
    start = src.index(signature)
    open_brace = src.index("{", start)
    depth, i = 0, open_brace
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_brace + 1:i]
        i += 1
    fail(f"unbalanced braces after {signature!r}")
    return ""


# ---------------------------------------------------------------------------
# Rebuild the page
# ---------------------------------------------------------------------------
def build_page(src: str) -> str:
    body = function_body(src, "static void handleRoot()")
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)

    # A linear scan, because the page is assembled in source order and the
    # order is what makes it a page: the <script> has to come after the
    # elements it addresses. Two kinds of thing contribute — a row()/pinRow()
    # call, which emits a field, and a string literal, which is markup. Nothing
    # else in handleRoot() reaches the browser.
    call_re = re.compile(r"\b(row|pinRow)\(p,")
    page, i = [], 0
    while i < len(body):
        m_call = call_re.search(body, i)
        m_str  = _LITERAL.search(body, i)
        if m_call and (not m_str or m_call.start() < m_str.start()):
            # Consume the whole call so its argument strings are not mistaken
            # for markup, and synthesise what the helper writes.
            # Count from the call's OWN opening paren. Starting inside the
            # argument list closes on the first ")" of a nested call — esc(...)
            # is right there in every row() — and the scan then walks back
            # into the arguments treating them as markup.
            depth, j = 0, body.index("(", m_call.start())
            while j < len(body):
                if body[j] == "(":
                    depth += 1
                elif body[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            args  = body[m_call.end():j]
            field = unescape(_LITERAL.search(args).group(1))
            page.append(f'<label for="{field}"></label>'
                        f'<input id="{field}" name="{field}">')
            if m_call.group(1) == "pinRow":
                # The resolver line under a pin field, addressed as o_<id>.
                page.append(f'<p class="pinout" id="o_{field}"></p>')
            i = j + 1
        elif m_str:
            page.append(unescape(m_str.group(1)))
            i = m_str.end()
        else:
            break

    html = "".join(page)

    # The three lookup tables are written by C++ loops, so their literals come
    # out of the scan above once each and in pieces (",", ":[", "\"]") rather
    # than as the JSON they become at runtime. Replace that whole stretch with
    # the same tables built from NodePins.h: it keeps the JavaScript parseable
    # for the check below — which is aimed at the hand-written JS after it —
    # and it is built from the same header the firmware reads, so a table that
    # loses its shape is caught here rather than on a phone.
    pins = PINS.read_text()
    risk = ",".join(f'{g}:[0,""]' for g in range(0, 17))
    labels = re.findall(r'\{\s*"([A-Z0-9]+)",\s*(\d+)\s*\}', pins)
    if len(labels) < 9:
        fail("NodePins.h: could not find the silkscreen label table")
    dl = ",".join(f'"{name}":{gpio}' for name, gpio in labels)
    boards = re.findall(r'\{\s*"([^"]+)",\s*\n?\s*"([^"]+)",\s*\n?\s*"([^"]+)"\s*\}', pins)
    if not boards:
        fail("NodePins.h: could not find the board layout table")
    bd = ",".join('{n:"%s",l:"%s",r:"%s"}' % b for b in boards)

    try:
        start = html.index("var RISK={")
        end   = html.index("var st=1,MAX=")
    except ValueError:
        fail("the page no longer emits the RISK table before its wizard JS — "
             "check_node_portal.py needs updating alongside ConfigPortal.cpp")
    html = (html[:start]
            + "var RISK={" + risk + "};"
            + "var DL={" + dl + "};"
            + "var BOARDS=[" + bd + "];"
            + html[end:])
    return html


# ---------------------------------------------------------------------------
def main() -> int:
    if not PORTAL.is_file():
        fail(f"{PORTAL} not found")
    src = PORTAL.read_text()
    html = build_page(src)

    # ── 1. the JavaScript parses ────────────────────────────────────────────
    scripts = re.findall(r"<script>(.*?)</script>", html, flags=re.S)
    if not scripts:
        fail("no <script> block in the rebuilt page — the portal lost its JS")
    js = "\n".join(scripts)

    node = shutil.which("node") or shutil.which("nodejs")
    if node:
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as fh:
            # The page's JS is written for a browser; --check only parses, so
            # `document` never has to exist.
            fh.write(js)
            tmp = fh.name
        res = subprocess.run([node, "--check", tmp],
                             capture_output=True, text=True)
        Path(tmp).unlink(missing_ok=True)
        if res.returncode != 0:
            print(res.stderr.strip())
            fail("the portal's JavaScript does not parse (see above); the page "
                 "would load and do nothing")
        print(f"OK: portal JavaScript parses ({len(js)} chars)")
    else:
        print("WARN: node not found — skipped the JavaScript parse check")

    # ── 2. every id the JS addresses exists in the markup ───────────────────
    #
    # This is the failure the C++-string form invites: rename an input, and
    # the getElementById that drove it silently returns null. The old page had
    # no way to catch it short of loading it on a phone.
    ids_in_html = set(re.findall(r'id="([^"]+)"', html))
    wanted = set(re.findall(r"getElementById\(['\"]([^'\"]+)['\"]\)", js))
    missing = sorted(wanted - ids_in_html)
    if missing:
        fail("the page's JavaScript addresses element(s) that the markup does "
             f"not contain: {', '.join(missing)}")
    print(f"OK: all {len(wanted)} element id(s) the JavaScript uses exist "
          f"in the page")

    # ── 3. the wizard's steps are the ones it counts ────────────────────────
    steps = re.findall(r'data-step="(\d+)"', html)
    max_js = re.search(r"var st=1,MAX=(\d+)", js)
    if not max_js:
        fail("could not find the step counter (var st=1,MAX=N) in the page's JS")
    if sorted(steps) != sorted(str(i) for i in range(1, int(max_js.group(1)) + 1)):
        fail(f"the page has steps {sorted(steps)} but its JS walks "
             f"1..{max_js.group(1)} — Next would run off the end")
    print(f"OK: {max_js.group(1)} wizard steps, and the JS walks exactly those")

    # ── 4. the pin fields all have their resolver line ──────────────────────
    pin_fields = re.findall(r'pinRow\(p,\s*"([^"]+)"', src)
    for f in pin_fields:
        if f'id="o_{f}"' not in html:
            fail(f'pin field "{f}" has no resolver line (id="o_{f}") — the '
                 f"page would not say which GPIO it resolved to")
    print(f"OK: {len(pin_fields)} pin field(s), each with its resolver line")

    return 0


if __name__ == "__main__":
    sys.exit(main())
