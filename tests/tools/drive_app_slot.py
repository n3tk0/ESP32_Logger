#!/usr/bin/env python3
"""
drive_app_slot.py — the OTA size check has to actually check something.

WHY THIS EXISTS
---------------
tools/app_slot.py answers one question — how many bytes may an OTA image be on
this board — and the first version of it got the answer from the wrong place.
It read `.pio/build/<env>/partitions.csv`, which a PlatformIO build does not
leave behind, so the very first run of the workflow printed

    ##[warning]No partition table found, so the image was not size-checked

and handed over a 1,306 KB image without mentioning that the slot it has to fit
in is 1,472 KB. Nothing failed. That is the shape of the bug worth testing for:
not a wrong answer, but a check that quietly stops being one and still reads
like it passed.

    python3 tests/tools/drive_app_slot.py
"""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from app_slot import app_slot_bytes, table_for_env  # noqa: E402
from pio_envs import environments  # noqa: E402

FAILURES: list[str] = []
CHECKS = 0


def check(cond: bool, what: str) -> None:
    global CHECKS
    CHECKS += 1
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILURES.append(what)


print("Reading a partition table:")

# The project's own, and the number the workflows have had typed into them by
# hand since before this file existed.
check(app_slot_bytes(ROOT / "partitions_balanced.csv") == 1507328,
      "partitions_balanced.csv -> 1507328 bytes (0x170000, app0)")

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)

    # Two app slots, one size: an OTA image is written whole into ONE of them.
    # Summing them would report 2,944 KB of room that no single image can use.
    (tmp / "two.csv").write_text(
        "# Name, Type, SubType, Offset, Size, Flags\n"
        "nvs,      data, nvs,     0x9000,   0x5000,\n"
        "app0,     app,  ota_0,   0x10000,  0x170000,\n"
        "app1,     app,  ota_1,   0x180000, 0x170000,\n"
        "spiffs,   data, spiffs,  0x2f0000, 0x110000,\n")
    check(app_slot_bytes(tmp / "two.csv") == 1507328,
          "two app slots is still one image's worth of room, not both")

    # Sizes are written every way the format allows.
    (tmp / "units.csv").write_text(
        "app0, app, ota_0, 0x10000, 3M,\n"
        "app1, app, ota_1, 0x310000, 1472K,\n")
    check(app_slot_bytes(tmp / "units.csv") == 3 * 1024 * 1024,
          "3M and 1472K are both read, and the larger app slot wins")

    (tmp / "decimal.csv").write_text("factory, app, factory, 65536, 1507328,\n")
    check(app_slot_bytes(tmp / "decimal.csv") == 1507328,
          "a decimal size is read as decimal, not as hex")

    # A data partition is not somewhere firmware goes.
    (tmp / "nodata.csv").write_text(
        "nvs,    data, nvs,    0x9000,  0x5000,\n"
        "spiffs, data, spiffs, 0x10000, 0x300000,\n")
    check(app_slot_bytes(tmp / "nodata.csv") == 0,
          "a table with no app partition answers 0, not the biggest data one")

    # Comments and blank lines are the normal shape of these files.
    (tmp / "comments.csv").write_text(
        "# Custom partition table\n\n"
        "app0, app, ota_0, 0x10000, 0x170000, # the OTA slot\n")
    check(app_slot_bytes(tmp / "comments.csv") == 1507328,
          "comments and blank lines do not confuse it")

print("\nFinding the table an env actually builds with:")

# The three C3 envs use the table in this repository...
for env in ("xiao_esp32c3", "esp32c3_supermini", "lolin_c3_pico"):
    found = table_for_env(env)
    check(found is not None and found.name == "partitions_balanced.csv",
          f"{env} -> {found.name if found else None}")
    if found:
        check(app_slot_bytes(found) == 1507328, f"  and that is 1472 KB of app slot")

# ...and the S3 ones use tables that ship with the Arduino core, which live
# under the PlatformIO core directory rather than in the project. That path is
# the one the first version of this could not follow, so it is the one worth
# standing a fake package up for.
with tempfile.TemporaryDirectory() as fake:
    parts = Path(fake) / "packages" / "framework-arduinoespressif32" / "tools" / "partitions"
    parts.mkdir(parents=True)
    (parts / "default_8MB.csv").write_text(
        "# Name, Type, SubType, Offset, Size, Flags\n"
        "app0, app, ota_0, 0x10000,  0x330000,\n"
        "app1, app, ota_1, 0x340000, 0x330000,\n")
    old = os.environ.get("PLATFORMIO_CORE_DIR")
    os.environ["PLATFORMIO_CORE_DIR"] = fake
    try:
        found = table_for_env("esp32s3")
        check(found is not None and found.name == "default_8MB.csv",
              f"esp32s3 -> the Arduino core's own default_8MB.csv ({found})")
        check(found is not None and app_slot_bytes(found) == 3342336,
              "  and that is 3264 KB, the number build-firmware.yml has by hand")
    finally:
        if old is None:
            os.environ.pop("PLATFORMIO_CORE_DIR", None)
        else:
            os.environ["PLATFORMIO_CORE_DIR"] = old

# Not "did it find one for the board I remembered" — for every board the tools
# offer. A new env with a table nobody can locate is a firmware nobody checks.
named = [e.name for e in environments() if e.partitions]
check(len(named) == len(list(environments())),
      f"every deployable env names a partition table ({len(named)})")

print()
if FAILURES:
    print(f"FAIL: {len(FAILURES)} of {CHECKS} check(s) failed")
    for f in FAILURES:
        print("  - " + f)
    sys.exit(1)
print(f"OK: {CHECKS} checks — the size check knows where the table is")
