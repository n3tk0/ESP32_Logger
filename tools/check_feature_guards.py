#!/usr/bin/env python3
"""
check_feature_guards.py — a file that tests a feature macro must be able to see it.

THE BUG THIS EXISTS FOR
-----------------------
`src/alerts/AlertEngine.cpp` guarded its MQTT publishing with

    #ifdef EXPORT_MQTT_ENABLED
        g_mqttExporter->send(...)
    #endif

and never included `setup.h`. Its include chain — AlertEngine.h,
DataPipeline.h, MqttExporter.h — bottoms out at `SensorTypes.h` and
`<Arduino.h>`, neither of which reaches the toggles. So the macro was simply
undefined in that translation unit, the block was compiled out of every
default build, and alerts silently never reached MQTT. Everything about the
device said the exporter was on.

Nothing catches that. It is not a warning, not a link error, not a test
failure: it is 240 bytes of code that quietly does not exist. It surfaced only
because the deploy tools began passing the feature set on the command line,
where a `-D` reaches every file, and the image grew.

WHAT THIS CHECKS
----------------
For every source file under src/ that tests one of the toggle macros, resolve
its local includes transitively and require that `setup.h` is reachable. A
macro tested where it cannot be defined is either a block that never compiles
or one that always does, and both are wrong.

Usage:
    python3 tools/check_feature_guards.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pio_envs import _project_root                    # noqa: E402

#: `#ifdef X`, `#ifndef X`, `#if defined(X)`, `#elif defined(X)` — the shapes a
#: feature guard is written in. A bare mention in a comment or a string is not
#: a guard and is deliberately not matched.
_GUARD = re.compile(
    r"^\s*#\s*(?:ifdef|ifndef|el?if\b.*?\bdefined\s*\(|if\b.*?\bdefined\s*\()"
    r"\s*([A-Z][A-Z0-9_]*)")

_PREFIXES = ("SENSOR_", "MODULE_", "EXPORT_", "FEATURE_")

_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def guards_in(path: Path) -> set[str]:
    """Toggle macros this file tests in a preprocessor conditional."""
    out: set[str] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").split("\n"):
        m = _GUARD.match(line)
        if not m:
            # `#if defined(A) || defined(B)` names more than one; catch the rest.
            if line.lstrip().startswith("#"):
                for name in re.findall(r"defined\s*\(\s*([A-Z][A-Z0-9_]*)\s*\)", line):
                    if name.startswith(_PREFIXES):
                        out.add(name)
            continue
        if m.group(1).startswith(_PREFIXES):
            out.add(m.group(1))
    return out


def reaches_setup_h(path: Path, root: Path, seen: set[Path] | None = None) -> bool:
    """Whether `#include "setup.h"` is reachable from `path` through local includes.

    Quoted includes only. An angle-bracket include is a library header and
    cannot lead back into src/, and following them would mean modelling the
    whole framework's search path to answer a question they cannot affect.
    """
    if seen is None:
        seen = set()
    path = path.resolve()
    if path in seen or not path.is_file():
        return False
    seen.add(path)
    if path.name == "setup.h":
        return True

    for line in path.read_text(encoding="utf-8", errors="replace").split("\n"):
        m = _INCLUDE.match(line)
        if not m:
            continue
        # Three ways a quoted include resolves in this project, tried in the
        # order the compiler would. The third is not padding: the sketch is
        # copied to the project root before it is built, so ESP_Logger.ino
        # writes `#include "src/setup.h"` — correct from where it compiles and
        # nonsense relative to where it is tracked. Without this the checker
        # reports the one file that gets it most right.
        candidates = (path.parent / m.group(1),
                      root / "src" / m.group(1),
                      root / m.group(1))
        for target in candidates:
            if target.is_file() and reaches_setup_h(target, root, seen):
                return True
    return False


def main() -> int:
    root = _project_root()
    if root is None:
        print("FAIL: cannot locate the project root")
        return 1

    src = root / "src"
    problems: list[str] = []
    checked = 0
    guarded = 0

    for path in sorted(src.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".ino") or not path.is_file():
            continue
        checked += 1
        names = guards_in(path)
        if not names:
            continue
        guarded += 1
        if reaches_setup_h(path, root):
            continue
        rel = path.relative_to(root)
        problems.append(
            f"{rel} tests {', '.join(sorted(names))} but cannot see setup.h.\n"
            f"    The macro is undefined here, so the guarded code is compiled "
            f"out of every build that does not pass it on the command line — "
            f"silently. Add #include \"../core/Config.h\" (it pulls in setup.h).")

    if problems:
        print(f"FAIL: {len(problems)} file(s) guard on a macro they cannot see.\n")
        for p in problems:
            print(f"  {p}")
        return 1

    print(f"OK: {guarded} of {checked} source file(s) guard on a feature macro, "
          f"and every one of them can see src/setup.h.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
