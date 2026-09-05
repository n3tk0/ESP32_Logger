#!/usr/bin/env python3
"""
tools/check_sensor_plugins.py — a sensor that loads must be a sensor that runs.

THE BUG THIS EXISTS FOR
-----------------------
`ISensor::_enabled` used to default to **false**, and every plugin was expected
to turn it on in its own `init()`:

    _enabled = cfg["enabled"] | true;

Twenty plugins did. `RemoteNodeSensor` did not, and nothing said so. The sensor
loaded, `init()` returned true, `[SensorManager] Sensor 'balcony' (remote)
ready` appeared in the log — and then `tickFiltered()` skipped it on every tick
because `isEnabled()` was false. It never drained a reading, never grew a
metric, and reported itself to the UI as `{"enabled":false,"status":
"disabled"}`, which the dashboard filters out of its card grid entirely.

The satellite node was meanwhile plainly alive on the Remote-nodes page, which
reads the ingest mailbox directly and never asks a sensor anything. So the one
screen that could have explained the silence was the one screen that looked
perfectly healthy.

THE FIX, AND WHY THE CHECK IS SHAPED THIS WAY
---------------------------------------------
The default is now **true**, because `SensorManager::loadAndInit()` skips every
config entry whose "enabled" is false before constructing anything: a sensor
that exists is one the user asked for. That removes the per-plugin obligation
rather than restating it — a plugin that says nothing is now correct.

Two things have to stay true for that to hold, and this checks both:

1. `ISensor::_enabled` defaults to true. Flipping it back re-arms the trap for
   every plugin at once.
2. Nothing writes `_enabled` back over a plugin's decision after `init()`
   returns. A driver whose hardware is optional must be able to come back from
   init() saying "loaded, but not usable" — the pattern `MqttExporter` and
   `HeaterModule` already use — and an unconditional `setEnabled()` in the
   manager would overrule it, tick it against absent hardware, and report it
   as faulted rather than as off.

Run:  python3 tools/check_sensor_plugins.py
Exits non-zero, naming the file, on any failure.
"""

import re
import sys
from pathlib import Path

ROOT    = Path(__file__).resolve().parent.parent
ISENSOR = ROOT / "src" / "sensors" / "ISensor.h"
MANAGER = ROOT / "src" / "sensors" / "SensorManager.cpp"
PLUGINS = ROOT / "src" / "sensors" / "plugins"


def strip_comments(src: str) -> str:
    src = re.sub(r"//[^\n]*", "", src)
    return re.sub(r"/\*.*?\*/", "", src, flags=re.S)


def function_body(src: str, start: int) -> str:
    """The braced body that follows `start`, by brace counting."""
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
    return ""


def main() -> int:
    problems = []

    # ── 1. the default ──────────────────────────────────────────────────────
    isensor = strip_comments(ISENSOR.read_text())
    m = re.search(r"bool\s+_enabled\s*=\s*(true|false)\s*;", isensor)
    if not m:
        problems.append(
            f"{ISENSOR.relative_to(ROOT)}: could not find the _enabled member. "
            f"If it moved, this check has to move with it.")
    elif m.group(1) != "true":
        problems.append(
            f"{ISENSOR.relative_to(ROOT)}: ISensor::_enabled defaults to "
            f"{m.group(1)}. Every plugin then has to remember to set it in "
            f"init(), and the one that forgets loads, logs \"ready\", and is "
            f"skipped by every tick while reporting itself \"disabled\" — "
            f"which is exactly how a working remote node produced no readings "
            f"for a day.")

    # ── 2. nobody overrules init() ──────────────────────────────────────────
    mgr = strip_comments(MANAGER.read_text())
    m = re.search(r"\bbool\s+SensorManager::loadAndInit\s*\(", mgr)
    if not m:
        problems.append(f"{MANAGER.relative_to(ROOT)}: loadAndInit() not found.")
    else:
        body = function_body(mgr, m.end())
        if re.search(r"setEnabled\s*\(", body) or re.search(r"_enabled\s*=", body):
            problems.append(
                f"{MANAGER.relative_to(ROOT)}: loadAndInit() writes _enabled "
                f"after calling init(). The config value is already decided "
                f"before construction (the loop skips disabled entries), so "
                f"this can only overrule a plugin that deliberately came back "
                f"from init() unusable — it would then be ticked against "
                f"hardware it just said was absent.")

    # ── 3. and the plugins are consistent about what they do say ────────────
    #
    # Not required any more — the default carries it — but a plugin that
    # assigns _enabled must read it from the config rather than hardcoding
    # true, or the "enabled" key stops meaning anything for that sensor.
    checked = 0
    for path in sorted(PLUGINS.glob("*.cpp")):
        src = strip_comments(path.read_text())
        m = re.search(r"\bbool\s+(\w+)::init\s*\(", src)
        if not m:
            continue
        checked += 1
        body = function_body(src, m.end())
        for assign in re.finditer(r"_enabled\s*=\s*([^;]+);", body):
            rhs = assign.group(1).strip()
            if rhs in ("true", "false"):
                continue          # a deliberate, unconditional decision
            if "enabled" not in rhs:
                problems.append(
                    f"{path.relative_to(ROOT)}: {m.group(1)}::init() sets "
                    f"_enabled to `{rhs}`, which does not read the config's "
                    f'"enabled" key — the switch in the UI would do nothing '
                    f"for this sensor.")

    if problems:
        print("ERROR: the 'a sensor that loads is a sensor that runs' "
              "invariant is broken:\n")
        for p in problems:
            print("  " + p + "\n")
        return 1

    print(f"OK: ISensor::_enabled defaults to true, loadAndInit() does not "
          f"overrule init(), and {checked} plugin init(s) read the config.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
