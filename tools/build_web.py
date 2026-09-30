#!/usr/bin/env python3
"""
tools/build_web.py — produce a flash-ready /www/ tree for LittleFS upload.

Pipeline (Pass 4 C2 from Audit_report_17042026.md):
    www/   (source of truth, hand-edited)
       │
       ▼  build_web.py
    dist/www/   (minified + gzipped, what you flash)

For each source file under www/:
  • .html / .css → conservative whitespace + comment minification, then gzip.
  • .js          → gzip only.  We deliberately don't try to minify JavaScript
                   without a real parser; the regex tricks that work for
                   CSS produce broken output on regex literals, JSDoc, etc.
  • .json / .txt → gzip only.
  • binary (.png .jpg .ico .woff …) → copied as-is (already compressed).

WHICH COPIES GET WRITTEN — `--filter`
    all    both the (possibly minified) plain file AND its `.gz` sibling.
           The firmware's serveStatic() probes for `.gz` first and emits
           Content-Encoding: gzip when the client supports it, falling back
           to the plain file otherwise (Pass 4 C1).
    gz     the `.gz` only, for any file where it came out smaller. HALVES
           WHAT THE TREE COSTS ON FLASH, which is the whole reason this is a
           setting: a 4 MB C3 has a LittleFS partition measured in hundreds
           of kilobytes, and two copies of every page did not fit. The
           firmware serves a gz-only tree — see the "flash-saving mode"
           probe in WebServer.cpp — so nothing is lost but the fallback for
           a client that cannot do gzip, and there has not been one of those
           in fifteen years.
    plain  no `.gz` at all. Debugging, and the same thing `--no-gzip` did.

Binaries (.png .jpg .ico .woff …) are already compressed and are copied
as-is under every filter — gzipping them makes them bigger.

ONE SCRIPT FOR THE MAIN PAGE — `--no-bundle` turns it off
    index.html names some 27 deferred scripts. With an empty cache the
    browser fetches them six at a time, and on a C3 each open connection
    holds about 9 KB (the server's send buffer plus lwIP's unacknowledged
    segments). Six of those after boot took the heap to 5 KB free with a
    4.6 KB largest block, and the device stopped answering. So the build
    concatenates every `<script src="/…" defer>` tag of index.html, in
    document order, into js/app.js and leaves one tag in their place.
    Deferred scripts run in document order anyway, so the order of
    execution is unchanged. Every source file opens with "use strict", and
    the bundle does too, so no file changes mode. A file another page still
    names (firstrun.html) is kept; the rest are not written.

Stdlib only — no pip install needed.

Usage:
    python3 tools/build_web.py                 # build dist/www/
    python3 tools/build_web.py --clean         # remove dist/www/ first
    python3 tools/build_web.py --filter gz     # one copy of each, .gz where it wins
    python3 tools/build_web.py --no-gzip       # alias for --filter plain
"""

from __future__ import annotations

import argparse
import gzip
import re
import shutil
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "www"
DST = ROOT / "dist" / "www"

# Extensions that benefit from text minification.
MIN_HTML_EXTS = {".html", ".htm"}
MIN_CSS_EXTS = {".css"}

# Extensions that compress well via gzip (text-y).  Binaries aren't gzipped
# because the result is usually larger than the original.
GZIP_TEXT_EXTS = {".html", ".htm", ".css", ".js", ".json", ".txt", ".svg", ".xml", ".csv"}


def minify_html(src: str) -> str:
    """Conservative HTML minify: drop comments + collapse interstitial whitespace.

    Skips content inside <pre>, <textarea>, <script>, and <style> blocks so
    we don't break formatted code or whitespace-sensitive markup.
    """
    # Strip HTML comments — but keep IE-style conditionals (start with <!--[).
    src = re.sub(r"<!--(?!\[).*?-->", "", src, flags=re.DOTALL)

    # Protect <pre> / <textarea> / <script> / <style> bodies.
    placeholders: list[str] = []

    def stash(match: re.Match) -> str:
        placeholders.append(match.group(0))
        return f"\x00P{len(placeholders) - 1}\x00"

    src = re.sub(
        r"<(pre|textarea|script|style)\b[^>]*>.*?</\1>",
        stash,
        src,
        flags=re.DOTALL | re.IGNORECASE,
    )

    # Collapse runs of whitespace between tags to a SINGLE space (not zero
    # — gemini review PR #49: dropping the gap entirely breaks the visual
    # space between adjacent inline / inline-block elements like
    # `<span>A</span> <span>B</span>`).
    src = re.sub(r">\s+<", "> <", src)
    # Collapse runs of whitespace inside text nodes.
    src = re.sub(r"[ \t]+", " ", src)
    src = re.sub(r"\n\s*\n", "\n", src)

    # Restore the protected blocks.
    def unstash(match: re.Match) -> str:
        idx = int(match.group(1))
        return placeholders[idx]

    src = re.sub(r"\x00P(\d+)\x00", unstash, src)
    return src.strip() + "\n"


def minify_css(src: str) -> str:
    """Conservative CSS minify: drop /* … */ comments + collapse whitespace.

    Quoted strings are stashed before whitespace collapse so values like
    `content: " : "` or `font-family: "Open Sans"` survive intact (gemini
    review PR #49).  data: URIs in url() are also quote-protected via the
    same mechanism when wrapped in single/double quotes.
    """
    # Strip /* … */ comments first — CSS doesn't have nested comments so a
    # greedy non-greedy match is safe.
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)

    # Stash quoted strings (single + double) before whitespace collapse so
    # any whitespace they legitimately contain is preserved.  Escapes (\")
    # are honoured so `"a\"b"` doesn't terminate early.
    strings: list[str] = []

    def stash_str(match: re.Match) -> str:
        strings.append(match.group(0))
        return f"\x00S{len(strings) - 1}\x00"

    src = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', stash_str, src)

    # Collapse runs of whitespace to single spaces, drop newlines.
    src = re.sub(r"\s+", " ", src)

    # Tighten the obvious noisy spots: `{ ` → `{`, ` }` → `}`, `; ` → `;`,
    # `: ` → `:`, `, ` → `,`.  Strings are stashed at this point so the
    # punctuation regex can't reach into them.
    src = re.sub(r"\s*([{};,:])\s*", r"\1", src)

    # Multiple semicolons collapse to one; trailing `;}` simplifies to `}`.
    src = re.sub(r";+", ";", src)
    src = re.sub(r";}", "}", src)

    # Restore the stashed quoted strings.
    def unstash_str(match: re.Match) -> str:
        return strings[int(match.group(1))]

    src = re.sub(r"\x00S(\d+)\x00", unstash_str, src)
    return src.strip() + "\n"


BUNDLE_REL = "js/app.js"

# A local deferred script tag and nothing else: `<script src="/x.js" defer></script>`.
_DEFER_TAG = re.compile(r'<script src="/([^"]+\.js)" defer></script>')
# "use strict" as the first statement, or as the first of a wrapping IIFE's.
_STRICT_HEAD = re.compile(
    r"""(?s);?(?:\(function\s*\([^)]*\)\s*\{(?:\s*//[^\n]*|\s*/\*.*?\*/)*\s*)?["']use strict["']""")
_SRC_ATTR = re.compile(r'<script[^>]*\bsrc="/([^"]+)"')


def plan_bundle(src_root: Path) -> tuple[str, bytes, set[str]] | None:
    """index.html with its deferred scripts replaced by one, the bundle, and
    the source paths that need not be written on their own.

    None when index.html has fewer than two deferred scripts to merge.
    """
    index = src_root / "index.html"
    if not index.is_file():
        return None
    # Comments first: a tag commented out is not a script to bundle. The
    # page loses them anyway in minify_html.
    html = re.sub(r"<!--(?!\[).*?-->", "", index.read_text("utf-8"), flags=re.DOTALL)
    names = _DEFER_TAG.findall(html)
    if len(names) < 2:
        return None
    # A deferred tag written any other way would stay where it is and run
    # after js/app.js instead of in its place in the order.
    odd = [t for t in re.findall(r"<script\b[^>]*\bdefer\b[^>]*>", html)
           if not _DEFER_TAG.match(t + "</script>")]
    if odd:
        print(f"error: index.html has a deferred script the bundle cannot take: {odd[0]}",
              file=sys.stderr)
        sys.exit(1)

    parts = ['"use strict";\n']
    for name in names:
        path = src_root / name
        if not path.is_file():
            print(f"error: index.html names /{name}, which is not in {src_root}",
                  file=sys.stderr)
            sys.exit(1)
        body = path.read_text("utf-8")
        # Each file already opens with "use strict" (see the module
        # docstring); a sloppy one would silently turn strict here.
        head = re.sub(r"(?s)^(?:\s*//[^\n]*|\s*/\*.*?\*/)*\s*", "", body)
        if not _STRICT_HEAD.match(head):
            print(f"error: /{name} does not open with \"use strict\" "
                  f"(or an IIFE whose body does), "
                  f"and the bundle is strict", file=sys.stderr)
            sys.exit(1)
        parts.append(f"\n// ---- /{name} ----\n{body}\n;\n")
    bundle = "".join(parts).encode("utf-8")

    first = True

    def swap(m: re.Match) -> str:
        nonlocal first
        if first:
            first = False
            return f'<script src="/{BUNDLE_REL}" defer></script>'
        return ""

    new_html = _DEFER_TAG.sub(swap, html)

    # Anything another page loads by name keeps its own file.
    still_named: set[str] = set()
    for page in src_root.rglob("*.htm*"):
        if page != index:
            still_named.update(_SRC_ATTR.findall(page.read_text("utf-8")))
    dropped = {n for n in names if n not in still_named}
    return new_html, bundle, dropped


def gzip_bytes(data: bytes) -> bytes:
    """Maximum compression — flash space is at a premium, decode is fast."""
    return gzip.compress(data, compresslevel=9)


def build(src_root: Path, dst_root: Path, *, do_gzip: bool = True,
          filter_mode: str = "all", bundle: bool = True) -> dict:
    """Walk src_root and emit a flash-ready tree under dst_root.

    `filter_mode` is which copies to keep — "all", "gz" or "plain"; see the
    module docstring. `do_gzip=False` is the old spelling of "plain" and is
    honoured for callers that still pass it.

    Returns a dict with byte totals for the size summary printed by main():
        in_bytes    — sum of source files
        plain_bytes — minified plain output (what serves to non-gzip clients)
        gz_bytes    — minified+gzipped output (what serves to gzip clients)
        flash_bytes — total bytes consumed on LittleFS
    """
    if not do_gzip:
        filter_mode = "plain"
    if filter_mode not in ("all", "gz", "plain"):
        raise ValueError(f"unknown filter {filter_mode!r}")
    do_gzip = filter_mode != "plain"
    in_bytes = 0
    plain_bytes = 0
    gz_bytes = 0
    flash_bytes = 0

    plan = plan_bundle(src_root) if bundle else None
    if not plan:
        # A bundle from an earlier build, next to an index.html that no
        # longer names it, reads to the deploy tool as a bundled tree.
        for stale in (dst_root / BUNDLE_REL, dst_root / (BUNDLE_REL + ".gz")):
            if stale.exists():
                stale.unlink()
    overrides: dict[str, bytes] = {}
    dropped: set[str] = set()
    if plan:
        html, bundled, dropped = plan
        overrides["index.html"] = html.encode("utf-8")
        overrides[BUNDLE_REL] = bundled
        if (src_root / BUNDLE_REL).exists():
            print(f"error: www/{BUNDLE_REL} exists, and the build writes it",
                  file=sys.stderr)
            sys.exit(1)

    sources = [p for p in src_root.rglob("*") if p.is_file()]
    if plan:
        sources.append(src_root / BUNDLE_REL)

    for src in sources:
        rel = src.relative_to(src_root)
        # Skip pre-compressed siblings — we'll regenerate them.
        if src.suffix == ".gz":
            continue
        dst = dst_root / rel
        if rel.as_posix() in dropped:
            # In js/app.js now. A copy left by an earlier build would be
            # flash spent on a file nothing asks for.
            for stale in (dst, dst.with_suffix(dst.suffix + ".gz")):
                if stale.exists():
                    stale.unlink()
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)

        raw = overrides.get(rel.as_posix())
        if raw is None:
            raw = src.read_bytes()
        in_bytes += len(raw)

        # Minify text formats; pass everything else through unchanged.
        # Wrap decode in try/except so a non-UTF-8 file (e.g. a stray
        # Windows-1252 page) names itself in the error rather than crashing
        # mid-walk (gemini review PR #49).
        try:
            if src.suffix.lower() in MIN_HTML_EXTS:
                text = raw.decode("utf-8")
                text = minify_html(text)
                out_bytes = text.encode("utf-8")
            elif src.suffix.lower() in MIN_CSS_EXTS:
                text = raw.decode("utf-8")
                text = minify_css(text)
                out_bytes = text.encode("utf-8")
            else:
                out_bytes = raw
        except UnicodeDecodeError as ex:
            print(f"error: {src} is not valid UTF-8 ({ex})", file=sys.stderr)
            sys.exit(1)

        plain_bytes += len(out_bytes)

        # Emit a `.gz` sibling for any text-y file so AsyncStaticWebHandler
        # can serve it with Content-Encoding: gzip.  Skip when the gzipped
        # size is larger than plain (rare but happens for tiny files).
        wire_bytes = len(out_bytes)
        gz_path = dst.with_suffix(dst.suffix + ".gz")
        gz: bytes | None = None
        if do_gzip and src.suffix.lower() in GZIP_TEXT_EXTS:
            candidate = gzip_bytes(out_bytes)
            if len(candidate) < len(out_bytes):
                gz = candidate
                wire_bytes = len(candidate)

        # UNDER "gz", THE PLAIN FILE IS DROPPED — but only where there is a
        # .gz to drop it in favour of. A binary has none, and a text file
        # whose gzip came out bigger has none either, and writing neither
        # copy of those would be a tree with holes in it rather than a
        # smaller tree.
        keep_plain = not (filter_mode == "gz" and gz is not None)
        if keep_plain:
            dst.write_bytes(out_bytes)
            flash_bytes += len(out_bytes)
        elif dst.exists():
            # A previous build with a different filter left it there. The
            # tree is what gets imaged onto LittleFS, so a stale plain
            # sibling is the flash this filter was chosen to save.
            dst.unlink()
        if gz is not None:
            gz_path.write_bytes(gz)
            flash_bytes += len(gz)
        elif gz_path.exists():
            gz_path.unlink()          # same, the other way round
        gz_bytes += wire_bytes

        rel_pct = 100.0 * wire_bytes / max(1, len(raw))
        print(f"  {rel}: {len(raw):>7} -> {wire_bytes:>7} B over the wire  ({rel_pct:5.1f}%)")

    return {
        "in_bytes": in_bytes,
        "plain_bytes": plain_bytes,
        "gz_bytes": gz_bytes,
        "flash_bytes": flash_bytes,
    }


# ── Font bootstrap ────────────────────────────────────────────────────────────
# Maps the local filename (under www/fonts/) to the Google Fonts CSS2 API URL
# that serves the variable-weight woff2 for the latin subset.
_FONTS: dict[str, str] = {
    "inter-var.woff2": (
        "https://fonts.googleapis.com/css2"
        "?family=Inter:wght@100..900&display=swap"
    ),
    "jetbrainsmono-var.woff2": (
        "https://fonts.googleapis.com/css2"
        "?family=JetBrains+Mono:wght@100..800&display=swap"
    ),
}

_GFONTS_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36"
)


def _ensure_fonts(src_root: Path) -> None:
    """Download variable woff2 fonts into www/fonts/ if they are missing.

    Fonts are committed to the repository so this is a no-op for the common
    case.  It only runs when someone clones the repo without LFS or without
    the font files (e.g. a fresh checkout before the first commit of fonts).

    The download uses the Google Fonts CSS2 API with a Chrome User-Agent to
    receive the woff2 format, then extracts the first fonts.gstatic.com URL
    from the response (the latin subset, which is all the UI needs).
    """
    fonts_dir = src_root / "fonts"
    fonts_dir.mkdir(parents=True, exist_ok=True)

    missing = [name for name in _FONTS if not (fonts_dir / name).is_file()]
    if not missing:
        return

    print(f"[build_web] Downloading {len(missing)} missing font file(s)…")
    for name in missing:
        css_url = _FONTS[name]
        dst = fonts_dir / name
        try:
            req = urllib.request.Request(css_url, headers={"User-Agent": _GFONTS_UA})
            with urllib.request.urlopen(req, timeout=15) as resp:
                css = resp.read().decode()

            m = re.search(
                r"url\((https://fonts\.gstatic\.com/[^)]+\.woff2)\)", css
            )
            if not m:
                print(
                    f"[build_web] WARNING: could not find woff2 URL for {name} "
                    f"— UI will use fallback system font",
                    file=sys.stderr,
                )
                continue

            woff2_url = m.group(1)
            req2 = urllib.request.Request(
                woff2_url, headers={"User-Agent": _GFONTS_UA}
            )
            with urllib.request.urlopen(req2, timeout=30) as resp2:
                data = resp2.read()

            dst.write_bytes(data)
            print(f"[build_web]   {name}  ({len(data):,} B)")

        except Exception as exc:  # noqa: BLE001
            print(
                f"[build_web] WARNING: could not download {name}: {exc} "
                f"— UI will use fallback system font",
                file=sys.stderr,
            )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--clean", action="store_true",
                   help="remove dist/www/ before building")
    p.add_argument("--filter", choices=("all", "gz", "plain"), default="all",
                   help="which copies to keep: all (plain + .gz), gz (one "
                        "copy each, .gz where it is smaller — halves what the "
                        "tree costs on flash), plain (no .gz at all)")
    p.add_argument("--no-gzip", action="store_true",
                   help="alias for --filter plain (kept for old call sites)")
    p.add_argument("--no-bundle", action="store_true",
                   help="keep index.html's scripts as separate files instead "
                        "of js/app.js (debugging; the deploy tool rebuilds "
                        "such a tree with the bundle before uploading it)")
    p.add_argument("--src", default=str(SRC),
                   help="source directory (default: www/)")
    p.add_argument("--dst", default=str(DST),
                   help="output directory (default: dist/www/)")
    args = p.parse_args()

    src_root = Path(args.src).resolve()
    dst_root = Path(args.dst).resolve()

    if not src_root.is_dir():
        print(f"error: source {src_root} is not a directory", file=sys.stderr)
        return 1

    # Download variable-weight woff2 fonts if not already present in www/fonts/.
    _ensure_fonts(src_root)

    if args.clean and dst_root.exists():
        print(f"[clean] removing {dst_root}")
        shutil.rmtree(dst_root)

    dst_root.mkdir(parents=True, exist_ok=True)

    mode = "plain" if args.no_gzip else args.filter
    print(f"[build_web] {src_root} -> {dst_root}  (filter: {mode})")
    totals = build(src_root, dst_root, filter_mode=mode,
                   bundle=not args.no_bundle)

    in_b   = totals["in_bytes"]
    wire_b = totals["gz_bytes"]
    flash_b = totals["flash_bytes"]
    wire_pct = 100.0 * wire_b / max(1, in_b)
    print()
    print(f"[build_web] source : {in_b:>9} B")
    print(f"[build_web] wire   : {wire_b:>9} B  ({wire_pct:.1f}% of source - what gzip-aware browsers download)")
    what = {"all":   "plain + .gz siblings",
            "gz":    ".gz only where it is smaller",
            "plain": "plain only"}[mode]
    print(f"[build_web] flash  : {flash_b:>9} B  ({what} on LittleFS)")
    # `relative_to(ROOT)` raises ValueError when --dst points outside the
    # project root (gemini review PR #49); fall back to the absolute path
    # so the line still reads sensibly.
    try:
        rel_dst = dst_root.relative_to(ROOT)
    except ValueError:
        rel_dst = dst_root
    print(f"[build_web] flash {rel_dst}/ to LittleFS.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
