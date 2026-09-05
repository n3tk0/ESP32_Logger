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

    python3 tools/app_slot.py .pio/build/lolin_c3_pico/partitions.csv
    1507328

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


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: app_slot.py <partitions.csv>", file=sys.stderr)
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
