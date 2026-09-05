#!/usr/bin/env python3
"""
app_slot.py — how many bytes an OTA image is allowed to be, for this build.

WHY IT IS COMPUTED AND NOT WRITTEN DOWN
---------------------------------------
The limit is the size of the app partition, and it differs per board: 1,472 KB
on the 4 MB C3s, 3,264 KB on the 8 MB S3s, 6,400 KB on the 16 MB one. Those
numbers were typed into .github/workflows/build-firmware.yml once, with the hex
in a comment beside them, and they are correct — until somebody changes a
partition table, at which point they are a check that passes while the image no
longer fits.

PlatformIO writes the partition table it actually built with to
`.pio/build/<env>/partitions.csv`. Asking that file is the difference between
checking the build in front of you and checking the build somebody had in mind
when they wrote the workflow.

WHY THE LARGEST APP PARTITION AND NOT THE SUM
---------------------------------------------
An OTA image is written whole into ONE slot. A table with app0 and app1 has
twice the app flash and the same limit per image — the second slot is what
makes a rollback possible, not what makes a bigger firmware possible.

    python3 tools/app_slot.py partitions_balanced.csv
    1507328
    python3 tools/app_slot.py --env esp32s3
    3342336

WHERE THE TABLE IS, WHICH IS NOT WHERE IT LOOKS LIKE IT SHOULD BE
-----------------------------------------------------------------
The first version of this read `.pio/build/<env>/partitions.csv`, on the
reasonable assumption that a build leaves the table it used beside the binary.
It does not — the first CI run said "No partition table found, so the image was
not size-checked" and handed over a 1,306 KB image with no idea that the slot
is 1,472 KB. A check that quietly does nothing is worse than no check, because
it reads like one that passed.

So `--env` resolves it the way PlatformIO does: `board_build.partitions` names
either a file in the project (partitions_balanced.csv) or one of the Arduino
core's own (default_8MB.csv, default_16MB.csv), which live in the framework
package under the PlatformIO core directory. Both are searched, in that order.

Exits 1, saying so, when the file names no app partition at all: a size check
that silently returns 0 would fail every build, and one that returned a default
would pass builds that do not fit.
"""
from __future__ import annotations

import sys
from pathlib import Path


def app_slot_bytes(csv_path: str | Path) -> int:
    """The largest `app` partition in an ESP-IDF partition table, in bytes.

    Returns 0 when the table has no app partition — the caller decides whether
    that is fatal, because it is fatal in a size check and merely uninteresting
    in a listing.
    """
    largest = 0
    text = Path(csv_path).read_text(encoding="utf-8", errors="replace")
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        # name, type, subtype, offset, size, flags — offset and size may be
        # hex, decimal, or a K/M suffix, and any of them may be blank.
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5 or cols[1].lower() != "app":
            continue
        value = _as_bytes(cols[4])
        if value > largest:
            largest = value
    return largest


def _as_bytes(size: str) -> int:
    """`0x170000`, `1507328`, `1472K` and `3M` all mean a number of bytes."""
    text = size.strip()
    if not text:
        return 0
    mult = 1
    if text[-1] in "kK":
        mult, text = 1024, text[:-1]
    elif text[-1] in "mM":
        mult, text = 1024 * 1024, text[:-1]
    try:
        base = int(text, 16) if text.lower().startswith("0x") else int(text)
    except ValueError:
        return 0
    return base * mult


def core_dir() -> Path:
    """PlatformIO's own directory, wherever this machine keeps it."""
    import os
    return Path(os.environ.get("PLATFORMIO_CORE_DIR")
                or os.environ.get("PLATFORMIO_HOME_DIR")
                or (Path.home() / ".platformio"))


def table_for_env(env: str) -> Path | None:
    """The partition table `pio run -e env` builds with, or None if not found."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pio_envs import environments, _project_root  # type: ignore

    match = [e for e in environments(include_all=True) if e.name == env]
    if not match or not match[0].partitions:
        return None
    name = match[0].partitions

    root = _project_root()
    if root is not None:
        local = Path(root) / name
        if local.is_file():
            return local

    # The Arduino core ships the default tables. The package directory carries
    # a version in its name on some installs, so it is globbed rather than
    # spelled out.
    packages = core_dir() / "packages"
    for pattern in ("framework-arduinoespressif32*/tools/partitions/" + name,
                    "framework-arduinoespressif32*/*/tools/partitions/" + name):
        for hit in sorted(packages.glob(pattern)):
            if hit.is_file():
                return hit
    return None


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[0] == "--env":
        found = table_for_env(argv[1])
        if found is None:
            print(f"app_slot: no partition table found for env {argv[1]!r}",
                  file=sys.stderr)
            return 1
        argv = [str(found)]

    if len(argv) != 1:
        print("usage: app_slot.py <partitions.csv> | --env <name>",
              file=sys.stderr)
        return 2
    path = Path(argv[0])
    if not path.is_file():
        print(f"app_slot: {path} does not exist", file=sys.stderr)
        return 1
    size = app_slot_bytes(path)
    if size <= 0:
        print(f"app_slot: {path} declares no app partition", file=sys.stderr)
        return 1
    print(size)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
