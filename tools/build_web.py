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

Both the (possibly minified) plain file AND its `.gz` sibling are written.
The firmware's serveStatic() probes for `.gz` first and emits
Content-Encoding: gzip when the client supports it, falling back to the
plain file otherwise (Pass 4 C1).

Stdlib only — no pip install needed.

Usage:
    python3 tools/build_web.py            # build dist/www/
    python3 tools/build_web.py --clean    # remove dist/www/ first
    python3 tools/build_web.py --no-gzip  # skip the .gz siblings (debug)
"""

from __future__ import annotations

import argparse
import gzip
import re
import shutil
import sys
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


def gzip_bytes(data: bytes) -> bytes:
    """Maximum compression — flash space is at a premium, decode is fast."""
    return gzip.compress(data, compresslevel=9)


def build(src_root: Path, dst_root: Path, *, do_gzip: bool) -> dict:
    """Walk src_root and emit a flash-ready tree under dst_root.

    Returns a dict with byte totals for the size summary printed by main():
        in_bytes    — sum of source files
        plain_bytes — minified plain output (what serves to non-gzip clients)
        gz_bytes    — minified+gzipped output (what serves to gzip clients)
        flash_bytes — total bytes consumed on LittleFS (plain + gz siblings)
    """
    in_bytes = 0
    plain_bytes = 0
    gz_bytes = 0
    flash_bytes = 0

    for src in src_root.rglob("*"):
        if not src.is_file():
            continue
        rel = src.relative_to(src_root)
        # Skip pre-compressed siblings — we'll regenerate them.
        if src.suffix == ".gz":
            continue
        dst = dst_root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)

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

        dst.write_bytes(out_bytes)
        plain_bytes += len(out_bytes)
        flash_bytes += len(out_bytes)

        # Emit a `.gz` sibling for any text-y file so AsyncStaticWebHandler
        # can serve it with Content-Encoding: gzip.  Skip when the gzipped
        # size is larger than plain (rare but happens for tiny files).
        wire_bytes = len(out_bytes)
        if do_gzip and src.suffix.lower() in GZIP_TEXT_EXTS:
            gz = gzip_bytes(out_bytes)
            if len(gz) < len(out_bytes):
                gz_path = dst.with_suffix(dst.suffix + ".gz")
                gz_path.write_bytes(gz)
                flash_bytes += len(gz)
                wire_bytes = len(gz)
        gz_bytes += wire_bytes

        rel_pct = 100.0 * wire_bytes / max(1, len(raw))
        print(f"  {rel}: {len(raw):>7} → {wire_bytes:>7} B over the wire  ({rel_pct:5.1f}%)")

    return {
        "in_bytes": in_bytes,
        "plain_bytes": plain_bytes,
        "gz_bytes": gz_bytes,
        "flash_bytes": flash_bytes,
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--clean", action="store_true",
                   help="remove dist/www/ before building")
    p.add_argument("--no-gzip", action="store_true",
                   help="skip producing .gz siblings (debugging)")
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

    if args.clean and dst_root.exists():
        print(f"[clean] removing {dst_root}")
        shutil.rmtree(dst_root)

    dst_root.mkdir(parents=True, exist_ok=True)

    print(f"[build_web] {src_root} → {dst_root}")
    totals = build(src_root, dst_root, do_gzip=not args.no_gzip)

    in_b   = totals["in_bytes"]
    wire_b = totals["gz_bytes"]
    flash_b = totals["flash_bytes"]
    wire_pct = 100.0 * wire_b / max(1, in_b)
    print()
    print(f"[build_web] source : {in_b:>9} B")
    print(f"[build_web] wire   : {wire_b:>9} B  ({wire_pct:.1f}% of source — what gzip-aware browsers download)")
    print(f"[build_web] flash  : {flash_b:>9} B  (plain + .gz siblings on LittleFS)")
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
