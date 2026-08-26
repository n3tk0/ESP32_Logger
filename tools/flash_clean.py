#!/usr/bin/env python3
"""
tools/flash_clean.py — full-erase + flash helper for PlatformIO.

Why this exists
---------------
Switching from Arduino IDE to PlatformIO leaves a stale partition table on
the chip from the previous flash.  The new firmware references partitions
defined in partitions_balanced.csv, but the on-flash table still points
elsewhere — so LittleFS can't find or format its data partition and you get:

    E (501) esp_littlefs: Failed to format filesystem
    [CFG] LittleFS mount failed - using hardcoded defaults
    ERR: No storage available!

Running esptool's `erase_flash` once before re-flashing fixes it
permanently: the new bootloader + partition table land on a clean chip,
LittleFS formats cleanly on first boot, and subsequent uploads behave
exactly like Arduino IDE used to.

Usage
-----
    python3 tools/flash_clean.py                         # auto-detect env from platformio.ini
    python3 tools/flash_clean.py -e esp32s3              # explicit env
    python3 tools/flash_clean.py --port /dev/ttyACM0     # explicit port
    python3 tools/flash_clean.py --with-fs               # also flash LittleFS
    python3 tools/flash_clean.py -y                      # skip confirmation

Run from the project root (where platformio.ini lives).
"""

import argparse
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


def _color(s: str, code: str) -> str:
    return f"\033[{code}m{s}\033[0m" if sys.stdout.isatty() else s


def _step(label: str, msg: str) -> None:
    print(_color(f"[{label}]", "1;36"), msg, flush=True)


def _ok(msg: str) -> None:
    print(_color("[ ok ]", "1;32"), msg, flush=True)


def _err(msg: str) -> None:
    print(_color("[fail]", "1;31"), msg, file=sys.stderr, flush=True)


def _run(cmd: list[str]) -> int:
    """Run a subprocess, streaming its output, return its exit code."""
    print(_color("  $ " + shlex.join(cmd), "0;90"), flush=True)
    return subprocess.call(cmd)


def _check_project_root() -> Path:
    here = Path.cwd()
    if not (here / "platformio.ini").is_file():
        _err(f"platformio.ini not found in {here}. Run from project root.")
        sys.exit(2)
    return here


def _default_env(root: Path) -> str:
    """The project's default PlatformIO environment.

    This used to take the FIRST [env:…] in platformio.ini, which answers a
    different question: reordering the file, or adding a board above the
    others, would have silently changed what this script erased and reflashed.
    tools/pio_envs.py honours [platformio] default_envs and skips the
    test-only builds.
    """
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from pio_envs import default_env
        return default_env()
    except Exception:
        # Never let a helper import take the flash tool down with it.
        return "xiao_esp32c3"


def _ensure_data_dir(root: Path) -> None:
    """Populate data/www/ from www/ via build_web.py.

    PlatformIO's `uploadfs` writes the contents of `data/` verbatim to the
    LittleFS root, and the firmware looks up assets at `/www/...`.  So the
    on-disk staging path must be `data/www/`, not `data/` — otherwise files
    land at the LittleFS root and `/www/index.html` is still missing on the
    device even after a successful upload.
    """
    data_www = root / "data" / "www"
    build_web = root / "tools" / "build_web.py"

    populated = data_www.is_dir() and any(data_www.iterdir())
    if populated:
        return

    if not build_web.is_file():
        _err(
            f"data/www/ is missing and tools/build_web.py not found.\n"
            f"  Create data/www/ manually by copying your web assets there, then retry."
        )
        sys.exit(2)

    _step("web", "Building web assets into data/www/ via tools/build_web.py")
    rc = _run([sys.executable, str(build_web), "--dst", str(data_www)])
    if rc != 0:
        _err("build_web.py failed — cannot build LittleFS image without data/www/.")
        sys.exit(rc)
    _ok("Web assets ready in data/www/.")


def _check_pio() -> str:
    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        _err("PlatformIO CLI not found in PATH. Install with `pip install platformio`.")
        sys.exit(2)
    return pio


def _confirm(env: str, port: str | None) -> None:
    target = f"env={env}" + (f" port={port}" if port else " port=auto-detect")
    print()
    print(_color("  *** About to ERASE THE ENTIRE FLASH ***", "1;33"))
    print(f"  Target: {target}")
    print(f"  This wipes config, log files, and the LittleFS partition.")
    print()
    try:
        ans = input("  Continue? [y/N] ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        ans = ""
    if ans not in ("y", "yes"):
        print("Aborted.")
        sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-e", "--env", default=None,
                    help="PlatformIO environment "
                         "(default: [platformio] default_envs in platformio.ini)")
    ap.add_argument("--port", default=None,
                    help="Serial port (e.g. /dev/ttyACM0, COM5). "
                         "Omit to let PlatformIO auto-detect.")
    ap.add_argument("--with-fs", action="store_true",
                    help="Also build and upload the LittleFS image after firmware.")
    ap.add_argument("--skip-build", action="store_true",
                    help="Don't rebuild before upload — use existing artifacts.")
    ap.add_argument("-y", "--yes", action="store_true",
                    help="Skip the destructive-action confirmation prompt.")
    args = ap.parse_args()

    root = _check_project_root()
    pio = _check_pio()

    env = args.env or _default_env(root)

    if not args.yes:
        _confirm(env, args.port)

    port_args: list[str] = ["--upload-port", args.port] if args.port else []

    # Step 1 — full chip erase.  Without this, a partition table left over
    # from a previous Arduino IDE flash makes LittleFS unable to find its
    # data partition.  pio's `erase` target invokes esptool erase_flash
    # under the hood and respects --upload-port for the serial selection.
    _step("erase", f"Full chip erase via PlatformIO (env={env})")
    rc = _run([pio, "run", "-t", "erase", "-e", env, *port_args])
    if rc != 0:
        _err("Erase failed. Is the device in download mode and reachable on the chosen port?")
        return rc
    _ok("Flash erased.")

    # Step 2 — build (unless --skip-build).  Keeping the build separate
    # from upload gives a clearer failure surface — a build error here is
    # surfaced before we touch the device a second time.
    if not args.skip_build:
        _step("build", f"Compiling firmware (env={env})")
        rc = _run([pio, "run", "-e", env])
        if rc != 0:
            _err("Build failed.")
            return rc
        _ok("Build OK.")

    # Step 3 — upload firmware.  This writes bootloader + partition table
    # + app to a clean flash, so LittleFS can format its newly-aligned
    # partition cleanly on first boot.
    _step("flash", "Uploading firmware")
    rc = _run([pio, "run", "-t", "upload", "-e", env, *port_args])
    if rc != 0:
        _err("Firmware upload failed.")
        return rc
    _ok("Firmware uploaded.")

    # Step 4 (optional) — upload LittleFS image with web assets.  Skipped
    # by default because most users are happy letting the device auto-
    # format on first boot and uploading UI files via /setup afterwards.
    # Requires a populated data/ directory; build_web.py is invoked
    # automatically when data/ is absent or empty.
    if args.with_fs:
        _ensure_data_dir(root)
        _step("fs", "Uploading LittleFS image")
        rc = _run([pio, "run", "-t", "uploadfs", "-e", env, *port_args])
        if rc != 0:
            _err("LittleFS upload failed.")
            return rc
        _ok("LittleFS image uploaded.")

    print()
    _ok("All done. Open the serial monitor to confirm:")
    monitor_cmd = [pio, "device", "monitor", "-e", env]
    if args.port:
        monitor_cmd.extend(["--port", args.port])
    print(f"  {shlex.join(monitor_cmd)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nAborted.")
        sys.exit(1)
