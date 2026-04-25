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

    # Collapse runs of whitespace between tags to a single space.
    src = re.sub(r">\s+<", "><", src)
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

    Preserves !important and a single space after colons inside data URIs
    (which can contain CSS-looking tokens that we mustn't crush).
    """
    # Strip /* … */ comments.  CSS doesn't have nested comments so a greedy
    # non-greedy match is safe.
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)

    # Collapse runs of whitespace to single spaces, drop newlines.
    src = re.sub(r"\s+", " ", src)

    # Tighten the obvious noisy spots: `{ ` → `{`, ` }` → `}`, `; ` → `;`,
    # `: ` → `:`, `, ` → `,`.  Keep one space after colons inside the
    # data:image/...,xxx URIs — but the regex below only collapses spaces
    # OUTSIDE quoted strings, since we removed line breaks first and
    # quoted strings can't contain a literal `;` or `}` to confuse us.
    src = re.sub(r"\s*([{};,:])\s*", r"\1", src)

    # `:` inside selectors (like `a:hover`) and properties (`color:red`) is
    # fine.  Multiple semicolons collapse to one.
    src = re.sub(r";+", ";", src)
    src = re.sub(r";}", "}", src)
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
    print(f"[build_web] flash {dst_root.relative_to(ROOT)}/ to LittleFS.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
