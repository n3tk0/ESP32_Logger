#!/usr/bin/env python3
"""
gen_module_schemas.py — compress src/modules/schemas/*.json into one C header.

WHAT THIS IS FOR
----------------
Each module on Settings → Modules draws its form from a schema: a JSON list of
fields with their labels, limits and help text. They used to be PROGMEM string
literals in the module .cpp files, 10 KB of text linked into every image and
sent inside GET /api/modules/:id. They are now JSON files next to the modules,
stored gzipped (about 4.4 KB) and served as they are stored from
GET /api/modules/:id/schema with Content-Encoding: gzip — the browser inflates
them, the firmware never does. The same arrangement as the failsafe page
(scripts/gen_failsafe.py), for the same reason: compressing at build time is
the only way the flash saving exists at all.

The schemas are also CHECKED here, not at boot. ModuleRegistry::add() used to
parse every schema with ArduinoJson to catch a malformed literal; a file that
does not parse, or a field without an id or with a type the page cannot draw,
now fails this script — in the build and in CI — before it can reach a device.

The generated header IS COMMITTED, for the Arduino IDE path, which runs no
scripts. CI runs `--check`, which INFLATES each stored array and compares it
with its source, never the compressed bytes: zlib and zlib-ng produce
different, equally valid streams. A build regenerates the header only when it
no longer inflates to the sources, so a machine with a different zlib does not
dirty the working tree. See gen_failsafe.py for the longer version of both.

Usage:
    python3 scripts/gen_module_schemas.py           # regenerate if stale
    python3 scripts/gen_module_schemas.py --force   # regenerate unconditionally
    python3 scripts/gen_module_schemas.py --check   # exit 1 if stale or invalid
"""
from __future__ import annotations

import gzip
import json
import re
import sys
from pathlib import Path

# Under PlatformIO this runs through SCons, where __file__ is not defined.
try:
    Import("env")  # noqa: F821
    _PIO = True
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821
except NameError:
    _PIO = False
    ROOT = Path(__file__).resolve().parent.parent

SRC_DIR = ROOT / "src" / "modules" / "schemas"
OUT = ROOT / "src" / "modules" / "ModuleSchemas.h"

# What renderField() in www/js/settings.js knows how to draw.
TYPES = {"string", "password", "int", "float", "ipv4", "bool", "color", "enum"}


def sources() -> list[Path]:
    return sorted(SRC_DIR.glob("*.json"))


def c_name(path: Path) -> str:
    return "MODULE_SCHEMA_" + re.sub(r"[^A-Z0-9]", "_", path.stem.upper()) + "_GZ"


def validate(path: Path, doc) -> list[str]:
    """What the settings page needs from a schema, as a list of problems."""
    errs = []
    fields = doc.get("fields") if isinstance(doc, dict) else None
    if not isinstance(fields, list) or not fields:
        return [f"{path.name}: needs a non-empty \"fields\" list"]
    seen = set()
    for i, f in enumerate(fields):
        where = f"{path.name}: field {i}"
        if not isinstance(f, dict):
            errs.append(f"{where} is not an object")
            continue
        fid = f.get("id")
        if not isinstance(fid, str) or not fid:
            errs.append(f"{where} has no id")
        elif fid in seen:
            errs.append(f"{where}: id {fid!r} appears twice")
        else:
            seen.add(fid)
        if f.get("type") not in TYPES:
            errs.append(f"{where} ({fid}): type {f.get('type')!r} is not one of "
                        + ", ".join(sorted(TYPES)))
        if f.get("type") == "enum":
            opts = f.get("options")
            if (not isinstance(opts, list) or not opts
                    or not all(isinstance(o, dict) and "v" in o and "l" in o for o in opts)):
                errs.append(f"{where} ({fid}): an enum needs options [{{\"v\":…,\"l\":…}}]")
    return errs


def minified(path: Path) -> bytes:
    """The schema as it is sent: no whitespace, UTF-8, keys in source order."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    return json.dumps(doc, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def problems() -> list[str]:
    errs = []
    srcs = sources()
    if not srcs:
        return [f"no schemas in {SRC_DIR.relative_to(ROOT)}"]
    for p in srcs:
        try:
            doc = json.loads(p.read_text(encoding="utf-8"))
        except ValueError as exc:
            errs.append(f"{p.name}: not JSON ({exc})")
            continue
        errs += validate(p, doc)
    return errs


def render() -> str:
    arrays, rows = [], []
    for p in sources():
        raw = minified(p)
        # mtime=0: the output depends only on the input.
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        body = "\n".join("    " + " ".join(f"0x{b:02x}," for b in gz[i:i + 16])
                         for i in range(0, len(gz), 16))
        arrays.append(f"static const uint8_t {c_name(p)}[] PROGMEM = {{\n{body}\n}};\n")
        rows.append(f"//   {p.name:<14} {len(raw):>6,} B -> {len(gz):>5,} B")
    return f"""// ============================================================================
// src/modules/ModuleSchemas.h — GENERATED by scripts/gen_module_schemas.py.
// Do not edit: change src/modules/schemas/<module>.json and rebuild.
//
// The forms on Settings → Modules, gzipped. A module's schema() returns its
// array; GET /api/modules/:id/schema sends it with Content-Encoding: gzip and
// the browser inflates it.
//
{chr(10).join(rows)}
//
// Each array is static and is referenced only by its own module, so an image
// carries the schemas of the modules it is built with and no others.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pgmspace.h>

{chr(10).join(arrays)}"""


_ARRAY_RE = re.compile(r"(MODULE_SCHEMA_\w+_GZ)\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", re.S)


def committed() -> dict[str, bytes]:
    if not OUT.is_file():
        return {}
    return {m.group(1): bytes(int(t, 16) for t in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
            for m in _ARRAY_RE.finditer(OUT.read_text(encoding="utf-8"))}


def committed_is_current() -> tuple[bool, str]:
    have = committed()
    want = {c_name(p): p for p in sources()}
    if set(have) != set(want):
        missing = sorted(set(want) - set(have))
        extra = sorted(set(have) - set(want))
        return False, f"arrays differ from the sources (missing {missing}, extra {extra})"
    total = 0
    for name, p in want.items():
        try:
            inflated = gzip.decompress(have[name])
        except (OSError, EOFError) as exc:
            return False, f"{name} is not a valid gzip stream ({exc})"
        if inflated != minified(p):
            return False, f"{name} does not inflate to {p.name}"
        total += len(have[name])
    return True, f"{len(want)} schemas, {total:,} stored bytes, each inflates to its source"


def main() -> int:
    errs = problems()
    if errs:
        print("FAIL: module schemas are invalid:")
        for e in errs:
            print("  " + e)
        return 1

    ok, detail = committed_is_current()
    rel = OUT.relative_to(ROOT)

    if "--check" in sys.argv:
        if ok:
            print(f"OK: {rel} — {detail}")
            return 0
        print(f"FAIL: {rel} is stale — {detail}.\n"
              f"  Run: python3 scripts/gen_module_schemas.py")
        return 1

    if ok and "--force" not in sys.argv:
        print(f"up to date: {rel} — {detail}")
        return 0

    OUT.write_text(render(), encoding="utf-8")
    print(f"wrote {rel}")
    return 0


if _PIO:
    if main() != 0:
        # A schema the page cannot draw must not reach a device.
        env.Exit(1)  # noqa: F821
elif __name__ == "__main__":
    sys.exit(main())
