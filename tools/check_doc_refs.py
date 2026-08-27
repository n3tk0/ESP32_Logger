#!/usr/bin/env python3
"""
check_doc_refs.py — keep the documentation's pointers into the code honest.

TWO RULES, AND THE SECOND IS THE POINT
--------------------------------------
1. Every source path a document names must exist.

2. No document may cite a LINE NUMBER.

Rule 2 looks pedantic and is not. Line citations rot silently and there is no
way for a reader to tell a stale one from their own confusion: they follow
`WebServer.cpp:328`, land in the middle of an unrelated function, and conclude
they misread the document. When this check was written, every single
`file:line` reference in docs/ was wrong -- 53 of them:

    first-run gate      claimed WebServer.cpp:328-356   actually at 1524
    /api/format_fs      claimed WebServer.cpp:1529      actually at 2000
    /do_update          claimed WebServer.cpp:1992      actually at 2366
    handleApiDiag       claimed ApiHandlers.cpp:325     actually at 364
    sendFailsafePage()  claimed WebServer.cpp:229       actually at 232

That last one had been written two days earlier and had already drifted three
lines. Line numbers are not a citation format a repository can keep true; a
symbol name is, because renaming one is a code change that grep will find.

So: name the symbol, not the line.

    BAD   the OTA guard (`src/web/WebServer.cpp:2069`)
    GOOD  the OTA guard (`sendOtaDisabled()` in `src/web/WebServer.cpp`)
    GOOD  `_claimI2cAddress(addr, who)` (`src/sensors/SensorManager.cpp`)

docs/AUDIT_LOG.md is exempt from both rules. It is an append-only record of
what was found at a point in time, and its line numbers are part of that
record -- rewriting them would falsify the log rather than fix it.

Usage:
    python3 tools/check_doc_refs.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: Append-only historical records: their line numbers are evidence, not links.
EXEMPT = {"docs/AUDIT_LOG.md"}

#: Top-level directories of this repository. A cited path must start at one of
#: these (or resolve next to the citing document) before we claim it is ours.
TOP_LEVEL = {"src", "tools", "scripts", "tests", "node", "www", "docs",
             "boards", "schematics", ".github"}

#: Extensions we treat as source a document can point into.
SRC_EXT = ("cpp", "h", "hpp", "c", "py", "ino", "js", "css", "html", "csv")

_PATHISH = "[A-Za-z0-9_./-]+"
LINE_REF = re.compile(rf"`?({_PATHISH}\.(?:{'|'.join(SRC_EXT)})):(\d+)(?:-\d+)?`?")
PATH_REF = re.compile(rf"`({_PATHISH}\.(?:{'|'.join(SRC_EXT)}))`")


def tracked_markdown() -> list[str]:
    out = subprocess.run(["git", "ls-files", "*.md"], cwd=ROOT,
                         capture_output=True, text=True).stdout
    return [p for p in out.split("\n") if p and p not in EXEMPT]


def main() -> int:
    problems: list[str] = []
    paths = 0

    for rel in tracked_markdown():
        text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")

        for m in LINE_REF.finditer(text):
            line = text[:m.start()].count("\n") + 1
            problems.append(
                f"{rel}:{line}  cites a line number: {m.group(0)}\n"
                f"    Name the symbol instead — see the header of this script.")

        for m in PATH_REF.finditer(text):
            p = m.group(1)
            if "://" in p or "*" in p or "/" not in p:
                continue        # URLs, globs, bare basenames used as prose
            if p.startswith("/"):
                continue        # a runtime path on the device's filesystem, not ours
            # A path is only ours if it starts at a top-level directory of this
            # repository, or resolves next to the document citing it. Anything
            # else is prose about a foreign tree -- `mbedtls/sha256.h` is an
            # ESP-IDF include, `builder/frameworks/espidf.py` lives inside the
            # PlatformIO platform package -- and this script has no business
            # asserting those exist here.
            candidates = [ROOT / p, (ROOT / rel).parent / p]
            if not any(c.exists() for c in candidates):
                if p.split("/")[0] not in TOP_LEVEL:
                    continue    # foreign tree, not a claim about this repo
                line = text[:m.start()].count("\n") + 1
                problems.append(f"{rel}:{line}  references a path that does not exist: {p}")
            paths += 1

    if problems:
        print(f"FAIL: {len(problems)} documentation reference problem(s).\n")
        for p in problems:
            print(f"  {p}")
        return 1

    print(f"OK: {len(tracked_markdown())} document(s), {paths} source path(s) "
          f"all exist, no line-number citations.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
