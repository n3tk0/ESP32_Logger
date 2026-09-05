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
  - each one resolves an upload speed and a monitor speed, since the deploy
    tools now take those from here rather than keeping their own copies
  - the default env is real
  - the flash tools can name a bootloader offset for every chip in use
  - the board dropdown in .github/workflows/build-ota-firmware.yml offers
    exactly the deployable envs — a workflow_dispatch `choice` is the one
    board list that has to be written out by hand, so it is the one that can
    go stale

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
        # The deploy tools adopt these instead of keeping their own copies, so
        # an env that resolves neither would quietly fall back to a built-in
        # number — the drift this whole arrangement exists to end.
        if not e.upload_speed:
            problems.append(f"[env:{e.name}] resolves to no upload speed "
                            f"(no `upload_speed`, and the board JSON has no "
                            f"upload.speed)")
        if not e.monitor_speed:
            problems.append(f"[env:{e.name}] resolves to no monitor speed "
                            f"(nothing in the env, its parents, or [env])")

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

    # The one board list that CANNOT be computed: a workflow_dispatch `choice`
    # has to be spelled out in the YAML, because GitHub builds the dropdown
    # before any of our code runs. So it is the one list that can silently go
    # stale — a board added to platformio.ini and missing from the form is a
    # board nobody can build a firmware for without a toolchain, and nothing
    # else would say so.
    wf = ROOT / ".github" / "workflows" / "build-ota-firmware.yml"
    if wf.is_file():
        text = wf.read_text(encoding="utf-8")
        block = re.search(r"options:\n((?:\s*-\s*\S+\n)+)", text)
        listed = set(re.findall(r"-\s*(\S+)", block.group(1))) if block else set()
        deployable = set(env_names())
        if not block:
            problems.append(f"{wf.name}: no `options:` list found for the board input")
        else:
            for name in sorted(deployable - listed):
                problems.append(f"{wf.name}: [env:{name}] is deployable but is not "
                                f"offered in the board dropdown")
            for name in sorted(listed - deployable):
                problems.append(f"{wf.name}: the board dropdown offers {name!r}, "
                                f"which is not a deployable env "
                                f"({'not deployable' if name in set(env_names(include_all=True)) else 'no such env'})")

    if problems:
        print("FAIL: the deploy tools and platformio.ini disagree.\n")
        for p in problems:
            print(f"  - {p}")
        return 1

    print(f"OK: {len(raw)} environment(s), all visible to the deploy tools:")
    for e in environments(include_all=True):
        flag = "" if e.deployable else "   (not offered as a flash target)"
        print(f"  {e.name:<20} {e.board:<24} {e.chip:<9} {e.flash_size:<6} "
              f"up={e.upload_speed} mon={e.monitor_speed}{flag}")
    return 0


if __name__ == "__main__":
    # `check_pio_envs.py | head` closes the pipe partway through the listing,
    # and without this Python turns that into a BrokenPipeError traceback on
    # stderr — a tool that prints a stack trace when it worked is a tool people
    # stop trusting. tools/features.py has carried the same guard for the same
    # reason since the CI check started grepping its output.
    try:
        sys.exit(main())
    except BrokenPipeError:
        try:
            sys.stdout.close()
        finally:
            sys.exit(0)
