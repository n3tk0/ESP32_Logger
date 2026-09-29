#!/usr/bin/env python3
"""
drive_ota_form.py — the Build OTA Firmware form must offer every feature, once,
with the defaults setup.h ships, and mean what its labels say.

WHY
---
The form is written in YAML and GitHub draws it before any of our code runs,
so nothing computes it: tools/ota_form.py says what each control turns on, and
the workflow has to list the same controls by hand. Every way those drift
apart is silent:

  - a feature added to setup.h and not to the form is a feature nobody can get
    from the button;
  - a control in the YAML that ota_form.py does not know is a checkbox that
    does nothing;
  - a default that differs from setup.h means pressing Run without touching
    anything builds something other than what a plain `pio run` builds;
  - a dropdown option spelled differently in the two places is a build that
    fails for the one person who picked it.

    python3 tests/tools/drive_ota_form.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from features import all_features, default_on_features  # noqa: E402
from ota_form import FORM, Check, Pick, macros_from_inputs  # noqa: E402

WORKFLOW = ROOT / ".github" / "workflows" / "build-ota-firmware.yml"
MAX_INPUTS = 25  # GitHub's workflow_dispatch limit since 2025-12

FAILURES: list[str] = []
CHECKS = 0


def check(cond: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILURES.append(what)


def _unquote(v: str) -> str:
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
        return v[1:-1]
    return v


def workflow_inputs() -> dict[str, dict]:
    """The workflow_dispatch inputs, parsed by indentation — no PyYAML needed.

    Only the shape this workflow uses: `name:` at six spaces, its keys at
    eight, option items at ten.
    """
    text = WORKFLOW.read_text(encoding="utf-8")
    block = text.split("    inputs:\n", 1)[1]
    inputs: dict[str, dict] = {}
    current = None
    in_options = False
    for line in block.split("\n"):
        if line and not line.startswith("      ") and not line.startswith("#"):
            break  # back out to `push:` or another top-level key
        if re.match(r"^      #", line) or not line.strip():
            continue
        m = re.match(r"^      ([a-z0-9_]+):\s*$", line)
        if m:
            current = inputs.setdefault(m.group(1), {"options": []})
            in_options = False
            continue
        m = re.match(r"^        ([a-z_]+):\s*(.*)$", line)
        if m and current is not None:
            key, val = m.group(1), m.group(2)
            in_options = key == "options"
            if not in_options:
                current[key] = _unquote(val)
            continue
        m = re.match(r"^          -\s*(.+)$", line)
        if m and current is not None and in_options:
            current["options"].append(_unquote(m.group(1)))
    return inputs


def main() -> int:
    features = {f.macro for f in all_features()}

    print("Every feature has exactly one control")
    reached: dict[str, list[str]] = {}
    for c in FORM:
        macros = [c.macro] if isinstance(c, Check) else \
            sorted({m for opt in c.options.values() for m in opt})
        for m in macros:
            reached.setdefault(m, []).append(c.name)
    for m in sorted(features):
        owners = reached.get(m, [])
        check(len(owners) == 1, f"{m} -> {owners or 'no control'}")
    for m in sorted(set(reached) - features):
        check(False, f"{m} is in the form but not in setup.h")

    print("Pressing Run untouched builds what setup.h ships")
    macros, errors = macros_from_inputs({})
    shipped = [f.macro for f in default_on_features()]
    check(not errors and macros == shipped,
          f"defaults -> {macros} (setup.h: {shipped})")

    print("The workflow offers the same controls")
    wf = workflow_inputs()
    check(len(wf) <= MAX_INPUTS, f"{len(wf)} inputs, GitHub allows {MAX_INPUTS}")
    check("board" in wf and list(wf)[0] == "board",
          "board is the first input (check_pio_envs.py reads the first options list)")
    extra = set(wf) - {c.name for c in FORM} - {"board", "label"}
    check(not extra, f"no input ota_form.py does not know: {sorted(extra) or 'none'}")
    for c in FORM:
        got = wf.get(c.name)
        if got is None:
            check(False, f"{c.name}: missing from the workflow")
            continue
        if isinstance(c, Check):
            check(got.get("type") == "boolean", f"{c.name}: a checkbox")
            check(got.get("default") == ("true" if c.default else "false"),
                  f"{c.name}: default {got.get('default')} == {c.default}")
        else:
            check(got.get("type") == "choice", f"{c.name}: a dropdown")
            check(got["options"] == list(c.options),
                  f"{c.name}: options {got['options']}")
            check(got.get("default") == c.default,
                  f"{c.name}: default {got.get('default')!r} == {c.default!r}")
            check("none" in c.options, f"{c.name}: can be switched off")
        check(got.get("description") == c.description, f"{c.name}: same label")

    print("What the controls mean")

    def picks(inputs: dict, want: set[str], what: str) -> None:
        got, errs = macros_from_inputs(inputs)
        check(not errs and set(got) == want, f"{what}: {got}{errs or ''}")

    base = set(shipped)
    picks({"particulate": "SPS30"},
          base - {"SENSOR_SDS011_ENABLED"} | {"SENSOR_SPS30_ENABLED"},
          "a dropdown swaps one sensor for another")
    picks({"kindle": True, "sd_card": False},
          base - {"FEATURE_SD_STORAGE"} | {"FEATURE_KINDLE_DASHBOARD"},
          "a checkbox adds, an untick removes")
    picks({"kindle": "true", "sd_card": "false"},
          base - {"FEATURE_SD_STORAGE"} | {"FEATURE_KINDLE_DASHBOARD"},
          "string booleans from the API mean the same")
    picks({"ac_mains": "voltage + current"},
          base | {"SENSOR_ZMPT101B_ENABLED", "SENSOR_ZMCT103C_ENABLED"},
          "one option can turn on two sensors")
    picks({"temp_humidity": "none", "particulate": "none", "nodes": "HTTP + ESP-NOW"},
          base - {"SENSOR_BME280_ENABLED", "SENSOR_SDS011_ENABLED"}
          | {"FEATURE_REMOTE_NODES", "FEATURE_ESPNOW_INGEST"},
          "a hub with no sensors of its own")
    picks({"light": "VEML7700", "uv": True},
          base | {"SENSOR_VEML7700_ENABLED", "SENSOR_VEML6075_ENABLED"},
          "VEML7700 (lux) and VEML6075 (UV) stay separate")

    _, errs = macros_from_inputs({"temp_humidity": "none", "particulate": "none"})
    check(bool(errs), f"nothing to read is refused: {errs}")
    _, errs = macros_from_inputs({"particulate": "SDS-011"})
    check(bool(errs), f"an option that is not offered is refused: {errs}")

    print(f"\n{CHECKS - len(FAILURES)}/{CHECKS} checks passed")
    if FAILURES:
        print("FAILED:")
        for f in FAILURES:
            print(f"  {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
