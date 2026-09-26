#!/usr/bin/env python3
"""
tools/check_node_portal_css.py — keep the node's setup page honest.

The node page (node_portal/, docs/NODE_CONFIG.md §6) is meant to look like
the collector's UI, and it does that by carrying a hand-kept copy of
www/style.css's design tokens. A hand-kept copy drifts: somebody retunes
--accent for contrast in www/ and the node pages quietly keep the old one.
And the page ships as a generated, committed header, which drifts the other
way: somebody edits app.js and forgets to rebuild, and the node serves the
old page. Nothing else notices either — the node page is not compiled, and a
stale header compiles perfectly.

So this fails when:

  1. a custom property (--bg, --accent, …) in node_portal/style.css has a
     different value from the same property in www/style.css — the light
     block (:root) against www's :root, and the dark block
     (@media (prefers-color-scheme: dark) { :root }) against www's
     [data-theme="dark"], falling back to www's :root for tokens the
     collector does not redefine in dark;
  2. the node page defines a token www/style.css does not have at all, or
     defines one in light that www redefines for dark but the node page does
     not (its dark theme would silently keep the light value);
  3. src/nodecfg/NodePortalPage.h does not inflate to what
     tools/build_node_portal.py builds from the sources right now (compared by
     content, not by compressed bytes — see that tool's docstring), or the
     page is over its 21 KB budget;
  4. the page's JavaScript does not parse (only when `node` is installed).

Run:  python3 tools/check_node_portal_css.py
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WWW_CSS = ROOT / "www" / "style.css"
NODE_CSS = ROOT / "node_portal" / "style.css"

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_node_portal  # noqa: E402


def blocks(css: str):
    """Yield (selector-path, body) for every rule, e.g. (("@media …", ":root"), "…").

    A brace matcher, not a CSS parser: enough for the two stylesheets this
    reads, which put no braces inside strings in the blocks that matter.
    """
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)

    def walk(text: str, path: tuple):
        i = 0
        while True:
            open_ = text.find("{", i)
            if open_ < 0:
                return
            sel = " ".join(text[i:open_].split())
            sel = sel.split(";")[-1].strip()  # drop a preceding @import-style statement
            depth, j = 1, open_ + 1
            while j < len(text) and depth:
                depth += {"{": 1, "}": -1}.get(text[j], 0)
                j += 1
            body = text[open_ + 1:j - 1]
            if sel.startswith("@media") or sel.startswith("@supports"):
                yield from walk(body, path + (sel,))
            else:
                yield path + (sel,), body
            i = j

    yield from walk(css, ())


def props(body: str) -> dict[str, str]:
    out = {}
    for m in re.finditer(r"(--[\w-]+)\s*:\s*([^;]+);?", body):
        out[m.group(1)] = " ".join(m.group(2).split())
    return out


def find(css: str, want) -> dict[str, str]:
    got: dict[str, str] = {}
    for path, body in blocks(css):
        if want(path):
            got.update(props(body))
    return got


def is_dark_media(sel: str) -> bool:
    return re.fullmatch(r"@media\s*\(\s*prefers-color-scheme\s*:\s*dark\s*\)", sel) is not None


def check_tokens(errors: list[str]) -> int:
    www = WWW_CSS.read_text(encoding="utf-8")
    node = NODE_CSS.read_text(encoding="utf-8")
    www_light = find(www, lambda p: p == (":root",))
    www_dark = find(www, lambda p: p == ('[data-theme="dark"]',))
    node_light = find(node, lambda p: p == (":root",))
    node_dark = find(node, lambda p: len(p) == 2 and is_dark_media(p[0]) and p[1] == ":root")

    if not www_light or not www_dark:
        errors.append("could not find :root and [data-theme=\"dark\"] in www/style.css — has its structure changed?")
        return 0
    if not node_light or not node_dark:
        errors.append("could not find :root and @media (prefers-color-scheme: dark) { :root } in node_portal/style.css")
        return 0

    n = 0
    for name, val in node_light.items():
        n += 1
        if name not in www_light:
            errors.append(f"light {name}: not a www/style.css token — the node page may only use the collector's tokens")
        elif www_light[name] != val:
            errors.append(f"light {name}: node_portal has {val!r}, www/style.css has {www_light[name]!r}")
    for name, val in node_dark.items():
        n += 1
        want = www_dark.get(name, www_light.get(name))
        if want is None:
            errors.append(f"dark {name}: not a www/style.css token")
        elif want != val:
            errors.append(f"dark {name}: node_portal has {val!r}, www/style.css dark resolves to {want!r}")
    for name in node_light:
        if name in www_dark and name not in node_dark and www_dark[name] != www_light.get(name):
            errors.append(f"dark {name}: www/style.css redefines it for dark ({www_dark[name]!r}) "
                          f"but node_portal's dark block does not, so the node page keeps the light value")
    return n


def check_header(errors: list[str]) -> str:
    raw = build_node_portal.build_html()
    ok, detail = build_node_portal.committed_is_current(raw)
    if not ok:
        errors.append(f"src/nodecfg/NodePortalPage.h is stale — {detail}. Run: python3 tools/build_node_portal.py")
    gz = build_node_portal.compress(raw)
    if len(gz) > build_node_portal.BUDGET:
        errors.append(f"the page is {len(gz):,} bytes gzipped, over the {build_node_portal.BUDGET:,}-byte budget")
    return f"{len(gz):,} bytes gzipped"


def check_js(errors: list[str]) -> str:
    node = shutil.which("node")
    if not node:
        return "skipped (no node)"
    html = build_node_portal.build_html().decode("utf-8")
    m = re.search(r"<script>(.*)</script>", html, re.S)
    if not m:
        errors.append("the built page has no inline <script>")
        return "missing"
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as f:
        f.write(m.group(1))
        path = f.name
    try:
        r = subprocess.run([node, "--check", path], capture_output=True, text=True)
    finally:
        Path(path).unlink(missing_ok=True)
    if r.returncode != 0:
        errors.append("the built page's JavaScript does not parse:\n" + (r.stderr or r.stdout).strip())
        return "does not parse"
    return "parses"


def main() -> int:
    errors: list[str] = []
    n = check_tokens(errors)
    size = check_header(errors)
    js = check_js(errors)
    if errors:
        for e in errors:
            print(f"ERROR: {e}")
        return 1
    print(f"OK: {n} node_portal tokens match www/style.css (light + dark); "
          f"NodePortalPage.h is current ({size}); JavaScript {js}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
