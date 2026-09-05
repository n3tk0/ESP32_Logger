#!/usr/bin/env python3
"""
tools/check_sensor_plugins.py — a sensor that loads must be a sensor that runs.

THE BUG THIS EXISTS FOR
-----------------------
`ISensor::_enabled` defaults to **false**, and every plugin is expected to turn
it on in its own `init()`:

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

WHAT IS CHECKED
---------------
1. Every plugin's `init()` assigns `_enabled`. SensorManager now also sets it
   centrally, so a plugin that forgets is no longer broken in the firmware —
   but it is still broken anywhere else it is constructed (a test, a future
   caller), and the class of bug is cheap to keep out.
2. SensorManager still sets it centrally. That line is the guarantee; a
   refactor that drops it puts every plugin back on its own honour.

Run:  python3 tools/check_sensor_plugins.py
Exits non-zero, naming the plugin, on any failure.
"""

import re
import sys
from pathlib import Path

ROOT    = Path(__file__).resolve().parent.parent
PLUGINS = ROOT / "src" / "sensors" / "plugins"
MANAGER = ROOT / "src" / "sensors" / "SensorManager.cpp"


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
    checked = 0

    for path in sorted(PLUGINS.glob("*.cpp")):
        src = strip_comments(path.read_text())
        m = re.search(r"\bbool\s+(\w+)::init\s*\(", src)
        if not m:
            continue          # a plugin with no init of its own
        checked += 1
        body = function_body(src, m.end())
        if not re.search(r"\b_enabled\s*=", body):
            problems.append(
                f"{path.relative_to(ROOT)}: {m.group(1)}::init() never assigns "
                f"_enabled. ISensor::_enabled defaults to false, so this sensor "
                f"loads and is then skipped by every tick — it reports "
                f'"disabled" to the UI and never produces a reading.')

    if not checked:
        print("ERROR: no sensor plugins found — has the layout changed?")
        return 1

    # The central guarantee. Losing it puts every plugin back on its own.
    mgr = strip_comments(MANAGER.read_text())
    if not re.search(r"setEnabled\s*\(\s*sensor\s*\[\s*\"enabled\"\s*\]", mgr):
        problems.append(
            f"{MANAGER.relative_to(ROOT)}: loadAndInit() no longer sets "
            f"setEnabled() from the config after init(). That call is what "
            f"makes 'a sensor that loaded is a sensor that runs' the manager's "
            f"invariant instead of each plugin's private habit.")

    if problems:
        print("ERROR: sensor plugins that would load and never run:\n")
        for p in problems:
            print("  " + p + "\n")
        return 1

    print(f"OK: {checked} sensor plugin(s) enable themselves, and the manager "
          f"sets the flag centrally.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
