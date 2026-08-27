#!/usr/bin/env python3
"""
check_features.py — the deploy tools must offer every feature setup.h declares.

tools/features.py parses src/setup.h with a regex so the deploy CLI, the deploy
GUI and CI all read one list instead of keeping three that drift. A regex is
exactly the kind of thing that keeps working for years and then quietly matches
nothing after somebody realigns a column — and the failure is silent in the
worst way: the tools still run, the menu is just shorter, and the only people
who notice are the ones who already knew the missing feature existed.

So this compares the parser against a second, dumber scan that shares none of
its logic. If they disagree, one of them is wrong and the build says so.

The same reason tools/check_pio_envs.py exists for the board list: an env its
parser cannot see is a board the flash tool will not offer, and nothing else
would say so.

Usage:
    python3 tools/check_features.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from features import all_features, optional_features  # noqa: E402
from pio_envs import _project_root                    # noqa: E402

#: Features this project is known to have. Named rather than counted, because a
#: count only catches a change of size and these are the ones whose absence
#: from the menu would be a bug somebody reports months later.
MUST_OFFER = {
    "FEATURE_REMOTE_NODES",
    "FEATURE_ESPNOW_INGEST",
    "FEATURE_KINDLE_DASHBOARD",
    "MODULE_FORECAST_ENABLED",
    "MODULE_HEATER_ENABLED",
}


def independent_scan(text: str) -> tuple[set[str], set[str]]:
    """Every toggle in setup.h, found without features.py's regex.

    Deliberately naive and line-oriented: find `#define NAME` for a name with
    one of the known prefixes and nothing after it but a comment, then decide
    on/off by whether the line is commented out. If this and features.py ever
    disagree, that disagreement is the whole point of the file.
    """
    on: set[str] = set()
    off: set[str] = set()
    for raw in text.split("\n"):
        line = raw.strip()
        commented = line.startswith("//")
        body = line[2:].strip() if commented else line
        if not body.startswith("#"):
            continue
        body = body[1:].strip()
        if not body.startswith("define"):
            continue
        rest = body[len("define"):].strip()
        name = rest.split("//")[0].strip()
        if " " in name or "(" in name:
            continue                       # a valued constant, not a toggle
        if not name.startswith(("SENSOR_", "MODULE_", "EXPORT_", "FEATURE_")):
            continue
        (off if commented else on).add(name)
    # A macro that appears both ways is declared off and turned on by an
    # implication elsewhere (FEATURE_ESPNOW_INGEST does this to
    # FEATURE_REMOTE_NODES). Its declaration is what the tools must offer.
    return on - off, off


def main() -> int:
    root = _project_root()
    if root is None:
        print("FAIL: cannot locate the project root")
        return 1

    text = (root / "src" / "setup.h").read_text(encoding="utf-8", errors="replace")
    scan_on, scan_off = independent_scan(text)

    parsed = all_features()
    parsed_on = {f.macro for f in parsed if f.enabled}
    parsed_off = {f.macro for f in parsed if not f.enabled}

    problems: list[str] = []

    for label, a, b in (("optional (off by default)", parsed_off, scan_off),
                        ("always-on", parsed_on, scan_on)):
        missing = b - a
        extra = a - b
        if missing:
            problems.append(
                f"features.py misses {len(missing)} {label} toggle(s) the file "
                f"declares: {', '.join(sorted(missing))}\n"
                f"    The deploy tools will not offer these. Check the _DEFINE "
                f"regex in tools/features.py against how setup.h now writes them.")
        if extra:
            problems.append(
                f"features.py reports {len(extra)} {label} toggle(s) that are not "
                f"there: {', '.join(sorted(extra))}")

    absent = MUST_OFFER - parsed_off
    if absent:
        problems.append(
            f"these are not offered as optional features: {', '.join(sorted(absent))}\n"
            f"    Either setup.h stopped declaring them, or one is now on by "
            f"default — in which case MUST_OFFER here needs updating too.")

    if problems:
        print(f"FAIL: {len(problems)} problem(s) with the feature list.\n")
        for p in problems:
            print(f"  {p}")
        return 1

    print(f"OK: {len(parsed_off)} optional feature(s) offered by the deploy "
          f"tools, {len(parsed_on)} on by default, both agreeing with a "
          f"second independent scan of src/setup.h.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
