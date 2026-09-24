#!/usr/bin/env python3
"""
check_boards_json.py — www/boards.json must describe the boards the firmware
knows, with the pins the firmware knows.

www/boards.json is how the Collector's pages draw a board's header and turn a
silkscreen label ("D6") into a GPIO. It is display data kept in LittleFS so it
costs the app image nothing, which also means nothing compiles it against
src/core/BoardProfiles.cpp. This script is that compile step:

  * every entry is keyed by a real profile shortId, and every profile named
    after a specific board has an entry (generic_* and custom are drawn as a
    GPIO grid on purpose);
  * every label maps to a GPIO the profile's chip has (<= maxGpio);
  * every label is drawn on the header, every header pad is a label or a
    known power / control pad, and no GPIO pad is drawn twice;
  * the XIAO ESP32-C3 and C3 SuperMini labels equal the node's own table in
    src/nodecfg/HwPins.h, so the two pages never disagree about one board;
  * every LOGGER_BOARD_PROFILE in platformio.ini names a real profile.

Stdlib only. Exit status 1 with one line per problem.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILES_CPP = ROOT / "src" / "core" / "BoardProfiles.cpp"
HWPINS_H = ROOT / "src" / "nodecfg" / "HwPins.h"
BOARDS_JSON = ROOT / "www" / "boards.json"
PIO_INI = ROOT / "platformio.ini"

# Pads that are not GPIOs. Anything else on a header must be a label.
POWER_PADS = {"GND", "G", "3V3", "5V", "VIN", "EN", "RST"}
# Drawn as a grid: there is no one carrier to promise a silkscreen for.
GRID_ONLY = {"generic_c3", "generic_s3", "custom"}
# Boards both the node and the Collector draw: profile id -> HwPins.h table.
SHARED_WITH_NODE = {
    "xiao_c3": "ESP32C3_XIAO_LABELS",
    "supermini_c3": "ESP32C3_SUPERMINI_LABELS",
}


def parse_profiles(text: str) -> dict[str, dict]:
    """shortId -> {maxGpio, flash:[...]} from the constexpr initialisers."""
    out = {}
    text = re.sub(r"//[^\n]*", "", text)
    pat = re.compile(
        r"constexpr\s+BoardProfile\s+\w+\s*=\s*\{\s*(\w+)\s*,\s*"
        r"\"[^\"]*\"\s*,\s*\"(\w+)\"\s*,\s*(\d+)\s*,((?:\s*\{[^}]*\}\s*,?)+)\s*\};"
    )
    for m in pat.finditer(text):
        lists = re.findall(r"\{([^}]*)\}", m.group(4))
        nums = [[int(x) for x in re.findall(r"\b\d+\b", l)] for l in lists]
        out[m.group(2)] = {"maxGpio": int(m.group(3)), "flash": nums[2] if len(nums) > 2 else []}
    return out


def parse_node_labels(text: str, name: str) -> dict[str, int]:
    m = re.search(r"static const PinLabel " + name + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        return {}
    return {lbl: int(g) for lbl, g in re.findall(r"\{\s*\"([^\"]+)\"\s*,\s*(\d+)\s*\}", m.group(1))}


def main() -> int:
    errs: list[str] = []
    profiles = parse_profiles(PROFILES_CPP.read_text(encoding="utf-8"))
    if len(profiles) < 3:
        print(f"check_boards_json: parsed only {len(profiles)} profiles from "
              f"{PROFILES_CPP.relative_to(ROOT)} — the initialiser shape changed, fix this script")
        return 1
    boards = json.loads(BOARDS_JSON.read_text(encoding="utf-8"))
    node_text = HWPINS_H.read_text(encoding="utf-8")

    for pid in profiles:
        if pid not in GRID_ONLY and pid not in boards:
            errs.append(f"profile '{pid}' has no drawing in www/boards.json")

    for pid, b in boards.items():
        if pid.startswith("_"):
            continue
        if pid not in profiles:
            errs.append(f"'{pid}': not a profile shortId in BoardProfiles.cpp")
            continue
        maxg = profiles[pid]["maxGpio"]
        pins = b.get("pins", {})
        for lbl, g in pins.items():
            if not isinstance(g, int) or g < 0 or g > maxg:
                errs.append(f"'{pid}': label {lbl} = {g!r}, chip has GPIO 0..{maxg}")
        pads = list(b.get("left", [])) + list(b.get("right", []))
        seen = set()
        for p in pads:
            if p in POWER_PADS:
                continue
            if p not in pins:
                errs.append(f"'{pid}': header pad '{p}' is neither a label nor a power pad")
            elif p in seen:
                errs.append(f"'{pid}': header pad '{p}' is drawn twice")
            seen.add(p)
        for lbl in pins:
            if lbl not in seen:
                errs.append(f"'{pid}': label '{lbl}' is not drawn on the header")
        if len(set(pins.values())) != len(pins):
            errs.append(f"'{pid}': two labels name the same GPIO")

    for pid, table in SHARED_WITH_NODE.items():
        node = parse_node_labels(node_text, table)
        if not node:
            errs.append(f"HwPins.h: table {table} not found")
        elif pid in boards and boards[pid].get("pins") != node:
            errs.append(f"'{pid}': labels differ from src/nodecfg/HwPins.h {table}")

    for m in re.finditer(r"-DLOGGER_BOARD_PROFILE=\\\"(\w+)\\\"", PIO_INI.read_text(encoding="utf-8")):
        if m.group(1) not in profiles:
            errs.append(f"platformio.ini: LOGGER_BOARD_PROFILE '{m.group(1)}' is not a profile")

    for e in errs:
        print("check_boards_json:", e)
    if not errs:
        print(f"check_boards_json: {len([k for k in boards if not k.startswith('_')])} drawings "
              f"agree with {len(profiles)} firmware profiles")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
