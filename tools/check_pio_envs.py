#!/usr/bin/env python3
"""
check_pio_envs.py — the deploy tools must see every board platformio.ini has.

tools/pio_envs.py parses platformio.ini with a small hand-written reader
(configparser cannot: duplicate keys per section are idiomatic there). A
parser is only useful while it keeps up with the file, and the failure mode is
quiet — an env it cannot see is simply a board the flash tool will not offer,
which is exactly the drift pio_envs.py was written to end.

So this asserts, against a plain regex scan of the same file:
  - every [env:NAME] is reachable through pio_envs
  - each one resolves to a board, and that board resolves to a chip
  - the default env is real
  - the flash tools can name a bootloader offset for every chip in use

Usage:
    python3 tools/check_pio_envs.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from pio_envs import INI, default_env, env_names, environments  # noqa: E402


def main() -> int:
    if not INI.is_file():
        print(f"FAIL: {INI} not found")
        return 1

    raw = set(re.findall(r"^\[env:([^\]]+)\]", INI.read_text(encoding="utf-8"),
                         re.MULTILINE))
    raw = {n.strip() for n in raw}
    seen = set(env_names(include_all=True))

    problems = []
    missing = raw - seen
    if missing:
        problems.append(f"pio_envs does not see: {sorted(missing)}")
    invented = seen - raw
    if invented:
        problems.append(f"pio_envs invented envs not in the ini: {sorted(invented)}")

    for e in environments(include_all=True):
        if not e.board:
            problems.append(f"[env:{e.name}] resolves to no board "
                            f"(a missing `board =`, or an `extends` chain this "
                            f"reader cannot follow)")
        if not e.chip:
            problems.append(f"[env:{e.name}] resolves to no chip family")

    if default_env() not in raw:
        problems.append(f"default env {default_env()!r} is not an env in the ini")

    # The flash tools key their bootloader offsets by chip. A board whose chip
    # has no entry there cannot be bootloader-flashed, and the place to find
    # that out is here, not with a device in hand.
    sys.path.insert(0, str(ROOT / "tools"))
    from flash_bootloader import CHIP_CONFIG
    for e in environments():
        if e.chip not in CHIP_CONFIG:
            problems.append(f"[env:{e.name}] is {e.chip}, which "
                            f"tools/flash_bootloader.py has no CHIP_CONFIG for")

    if problems:
        print("FAIL: the deploy tools and platformio.ini disagree.\n")
        for p in problems:
            print(f"  - {p}")
        return 1

    print(f"OK: {len(raw)} environment(s), all visible to the deploy tools:")
    for e in environments(include_all=True):
        flag = "" if e.deployable else "   (not offered as a flash target)"
        print(f"  {e.name:<20} {e.board:<24} {e.chip:<9} {e.flash_size}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
