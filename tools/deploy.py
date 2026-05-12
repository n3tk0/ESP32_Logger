#!/usr/bin/env python3
"""
tools/deploy.py — All-in-one build, flash & deploy tool for ESP32 Logger.

Replaces the separate workflow:
    build_web.py  →  flash_bootloader.py  →  flash_clean.py  →  upload_www.py

One interactive menu, last settings remembered in .flash_tool.json.

Steps
─────
  1  Build web assets      www/ → data/www/  (minify + gzip via build_web.py)
  2  Flash bootloader      rollback-enabled bootloader (via flash_bootloader.py)
  3  Erase chip flash      full wipe (pio run -t erase)
  4  Compile firmware      pio run -e <env>
  5  Flash firmware        pio run -t upload
  6  Upload LittleFS       pio run -t uploadfs  (reads data/www/)
  7  Upload web via HTTP   POST /upload to device IP over Wi-Fi
  8  Open serial monitor   pio device monitor

Presets
───────
  F  Full flash    = 1,3,4,5,6  (clean slate + firmware + LittleFS)
  Q  Quick flash   = 4,5         (recompile & flash, no erase)
  H  HTTP deploy   = 1,7         (rebuild web + push to running device)
  A  All steps     = 1-8

Usage
─────
    python3 tools/deploy.py          # interactive menu
    python3 tools/deploy.py --run    # run saved steps non-interactively

Settings are saved to .flash_tool.json in the project root after each run.
"""

from __future__ import annotations

import argparse
import glob as _glob
import json
import mimetypes
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

# ── Project layout ────────────────────────────────────────────────────────────
ROOT     = Path(__file__).resolve().parent.parent
WWW_SRC  = ROOT / "www"
DATA_WWW = ROOT / "data" / "www"
TOOLS    = ROOT / "tools"
CFG_FILE = ROOT / ".flash_tool.json"

# ── Colour helpers ────────────────────────────────────────────────────────────
_TTY = sys.stdout.isatty()
def _c(s: str, code: str) -> str:
    return f"\033[{code}m{s}\033[0m" if _TTY else s
def _bold(s):   return _c(str(s), "1")
def _cyan(s):   return _c(str(s), "1;36")
def _green(s):  return _c(str(s), "1;32")
def _yellow(s): return _c(str(s), "1;33")
def _red(s):    return _c(str(s), "1;31")
def _dim(s):    return _c(str(s), "0;90")

# ── Step catalogue ────────────────────────────────────────────────────────────
STEP_NAMES: dict[int, str] = {
    1: "Build web assets      www/ → data/www/",
    2: "Flash bootloader      rollback-enabled, via esptool",
    3: "Erase chip flash      full wipe",
    4: "Compile firmware      pio run",
    5: "Flash firmware        pio run -t upload",
    6: "Upload LittleFS       pio run -t uploadfs",
    7: "Upload web via HTTP   POST /upload to device IP",
    8: "Open serial monitor   pio device monitor",
}

PRESETS: dict[str, tuple[str, list[int]]] = {
    "F": ("Full flash",   [1, 3, 4, 5, 6]),
    "Q": ("Quick flash",  [4, 5]),
    "H": ("HTTP deploy",  [1, 7]),
    "A": ("All steps",    list(range(1, 9))),
    "N": ("None",         []),
}

# ── Config defaults + persistence ─────────────────────────────────────────────
DEFAULT_CFG: dict[str, Any] = {
    "env":       None,
    "port":      None,
    "chip":      "esp32c3",
    "baud":      921600,
    "device_ip": "192.168.4.1",
    "steps":     [1, 3, 4, 5, 6],
}


def _detect_env() -> str:
    try:
        txt = (ROOT / "platformio.ini").read_text()
        m = re.search(r"^\[env:([^\]]+)\]", txt, re.MULTILINE)
        if m:
            return m.group(1).strip()
    except OSError:
        pass
    return "esp32c3_supermini"


def _detect_port() -> str:
    if sys.platform == "linux":
        pats = ["/dev/ttyACM*", "/dev/ttyUSB*"]
    elif sys.platform == "darwin":
        pats = ["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB*"]
    elif sys.platform == "win32":
        try:
            import serial.tools.list_ports  # type: ignore
            ports = [p.device for p in serial.tools.list_ports.comports()]
            return ports[0] if ports else ""
        except ImportError:
            return ""
    else:
        return ""
    for pat in pats:
        found = sorted(_glob.glob(pat))
        if found:
            return found[0]
    return ""


def load_cfg() -> dict[str, Any]:
    cfg: dict[str, Any] = dict(DEFAULT_CFG)
    if CFG_FILE.is_file():
        try:
            cfg.update(json.loads(CFG_FILE.read_text()))
        except Exception:
            pass
    if not cfg.get("env"):
        cfg["env"] = _detect_env()
    if not cfg.get("port"):
        cfg["port"] = _detect_port()
    return cfg


def save_cfg(cfg: dict[str, Any]) -> None:
    try:
        CFG_FILE.write_text(json.dumps(cfg, indent=2) + "\n")
    except Exception as exc:
        print(_yellow(f"  Warning: could not save config: {exc}"))


# ── Menu rendering ─────────────────────────────────────────────────────────────
_W = 64


def _clear() -> None:
    os.system("cls" if sys.platform == "win32" else "clear")


def _prompt(label: str, default: str = "") -> str:
    hint = f"  {label} [{_dim(default)}]: " if default else f"  {label}: "
    try:
        v = input(hint).strip()
    except (KeyboardInterrupt, EOFError):
        print()
        return default
    return v or default


def _print_menu(cfg: dict[str, Any]) -> None:
    _clear()
    title = "ESP32 Logger  —  Flash & Deploy"
    pad = (_W - len(title)) // 2
    print(_cyan("╔" + "═" * _W + "╗"))
    print(_cyan("║") + " " * pad + _bold(title) + " " * (_W - pad - len(title)) + _cyan("║"))
    print(_cyan("╚" + "═" * _W + "╝"))
    print()

    # Settings block
    print(_bold("  ── Settings " + "─" * 50))
    port_disp = cfg.get("port") or _dim("auto-detect")
    for key, label, val in [
        ("e", "PlatformIO env ", cfg.get("env", "")),
        ("p", "Serial port    ", port_disp),
        ("i", "Device IP      ", cfg.get("device_ip", "")),
        ("c", "Chip type      ", cfg.get("chip", "")),
        ("b", "Baud rate      ", str(cfg.get("baud", 921600))),
    ]:
        print(f"  {_cyan(f'[{key}]')}  {label}: {_bold(val)}")
    print()

    # Steps block
    enabled = set(cfg.get("steps", []))
    print(_bold("  ── Steps " + "─" * 53))
    for n, name in STEP_NAMES.items():
        tick = _green("✓") if n in enabled else " "
        print(f"  {_cyan(f'[{n}]')}  [{tick}] {name}")
    print()

    # Presets block
    print(_bold("  ── Presets " + "─" * 51))
    for key, (pname, psteps) in PRESETS.items():
        steps_s = ", ".join(str(s) for s in psteps) if psteps else "—"
        print(f"  {_cyan(f'[{key}]')}  {pname:<18} {_dim(f'steps: {steps_s}')}")
    print()

    # Actions block
    enabled_list = ", ".join(str(s) for s in sorted(enabled)) if enabled else _dim("none selected")
    print(_bold("  ── Actions " + "─" * 51))
    print(f"  {_cyan('[r]')}  Run  {_dim(f'({enabled_list})')}")
    print(f"  {_cyan('[s]')}  Save as default")
    print(f"  {_cyan('[q]')}  Quit")
    print()


def run_menu(cfg: dict[str, Any]) -> dict[str, Any]:
    """Interactive menu. Returns cfg when user presses 'r'; exits on 'q'."""
    while True:
        _print_menu(cfg)
        try:
            choice = input(_bold("  Choice: ")).strip()
        except (KeyboardInterrupt, EOFError):
            print()
            sys.exit(0)

        ch = choice.lower()

        if ch == "q":
            sys.exit(0)

        elif ch == "r":
            return cfg

        elif ch == "s":
            save_cfg(cfg)
            print(_green("  Saved."))
            time.sleep(0.7)

        elif ch == "e":
            v = _prompt("PlatformIO env", cfg.get("env", ""))
            if v:
                cfg["env"] = v

        elif ch == "p":
            detected = _detect_port()
            v = _prompt("Serial port (blank = auto-detect)", cfg.get("port") or detected)
            cfg["port"] = v

        elif ch == "i":
            v = _prompt("Device IP", cfg.get("device_ip", "192.168.4.1"))
            if v:
                cfg["device_ip"] = v

        elif ch == "c":
            print(_dim("  Choices: esp32c3  esp32c3_supermini  esp32"))
            v = _prompt("Chip type", cfg.get("chip", "esp32c3"))
            if v:
                cfg["chip"] = v

        elif ch == "b":
            v = _prompt("Baud rate", str(cfg.get("baud", 921600)))
            try:
                cfg["baud"] = int(v)
            except ValueError:
                pass

        elif ch in {str(n) for n in STEP_NAMES}:
            n = int(ch)
            steps = set(cfg.get("steps", []))
            steps ^= {n}  # toggle
            cfg["steps"] = sorted(steps)

        elif choice.upper() in PRESETS:
            cfg["steps"] = list(PRESETS[choice.upper()][1])


# ── Step implementations ───────────────────────────────────────────────────────

def _hdr(step: str, msg: str) -> None:
    print()
    print(_cyan(f"  ┌─[{step}]"))
    print(_cyan("  │ ") + msg)
    print(_cyan("  └" + "─" * 54))


def _run(cmd: list[str]) -> int:
    print(_dim("  $ " + shlex.join(cmd)))
    return subprocess.call(cmd)


def s1_build_web() -> int:
    _hdr("1 · build-web", f"Building web assets  {WWW_SRC} → {DATA_WWW}")
    script = TOOLS / "build_web.py"
    if not script.is_file():
        print(_red("  build_web.py not found in tools/"))
        return 2
    rc = _run([sys.executable, str(script), "--dst", str(DATA_WWW)])
    if rc == 0:
        print(_green("  ✓ Web assets built."))
    return rc


def s2_flash_bootloader(cfg: dict[str, Any]) -> int:
    _hdr("2 · bootloader", "Flashing rollback-enabled bootloader")
    script = TOOLS / "flash_bootloader.py"
    if not script.is_file():
        print(_red("  flash_bootloader.py not found in tools/"))
        return 2
    cmd = [sys.executable, str(script), "--chip", cfg["chip"], "--baud", str(cfg["baud"])]
    if cfg.get("port"):
        cmd += ["--port", cfg["port"]]
    return _run(cmd)


def s3_erase(cfg: dict[str, Any], pio: str) -> int:
    _hdr("3 · erase", f"Full chip erase  env={cfg['env']}")
    print(_yellow("  *** Wipes all flash (config, logs, LittleFS). ***"))
    try:
        ans = input("  Continue? [y/N] ").strip().lower()
    except (KeyboardInterrupt, EOFError):
        ans = ""
    if ans not in ("y", "yes"):
        print(_dim("  Skipped."))
        return 0  # user skipped voluntarily — not a failure
    cmd = [pio, "run", "-t", "erase", "-e", cfg["env"]]
    if cfg.get("port"):
        cmd += ["--upload-port", cfg["port"]]
    rc = _run(cmd)
    if rc == 0:
        print(_green("  ✓ Flash erased."))
    return rc


def s4_compile(cfg: dict[str, Any], pio: str) -> int:
    _hdr("4 · compile", f"Compiling firmware  env={cfg['env']}")
    rc = _run([pio, "run", "-e", cfg["env"]])
    if rc == 0:
        print(_green("  ✓ Firmware compiled."))
    return rc


def s5_flash_fw(cfg: dict[str, Any], pio: str) -> int:
    _hdr("5 · flash", f"Flashing firmware  env={cfg['env']}")
    cmd = [pio, "run", "-t", "upload", "-e", cfg["env"]]
    if cfg.get("port"):
        cmd += ["--upload-port", cfg["port"]]
    rc = _run(cmd)
    if rc == 0:
        print(_green("  ✓ Firmware flashed."))
    return rc


def s6_upload_fs(cfg: dict[str, Any], pio: str) -> int:
    _hdr("6 · uploadfs", f"Uploading LittleFS  env={cfg['env']}  src={DATA_WWW}")
    if not DATA_WWW.is_dir() or not any(DATA_WWW.iterdir()):
        print(_yellow("  data/www/ is empty — running Build web first…"))
        rc = s1_build_web()
        if rc != 0:
            return rc
    cmd = [pio, "run", "-t", "uploadfs", "-e", cfg["env"]]
    if cfg.get("port"):
        cmd += ["--upload-port", cfg["port"]]
    rc = _run(cmd)
    if rc == 0:
        print(_green("  ✓ LittleFS image uploaded."))
    return rc


def s7_upload_http(cfg: dict[str, Any]) -> int:
    ip = cfg.get("device_ip", "192.168.4.1")
    base = f"http://{ip}"
    _hdr("7 · http-upload", f"Uploading web via HTTP  {base}")

    if not DATA_WWW.is_dir() or not any(DATA_WWW.iterdir()):
        print(_red("  data/www/ is empty. Run step 1 (Build web assets) first."))
        return 2

    # Connectivity check
    try:
        with urllib.request.urlopen(f"{base}/api/status", timeout=4) as resp:
            print(_green(f"  Device online  (HTTP {resp.status})"))
    except Exception as exc:
        print(_red(f"  Device not reachable at {base}: {exc}"))
        return 1

    # Ensure /www and subdirs exist on device
    _http_mkdir(base, "/", "www")
    for sub in ["js", "pages"]:
        if (DATA_WWW / sub).is_dir():
            _http_mkdir(base, "/www", sub)
    time.sleep(0.3)

    # Upload files — .gz sidecars + non-compressible formats
    ok = fail = 0
    for fpath in sorted(DATA_WWW.rglob("*")):
        if not fpath.is_file():
            continue
        # Skip: platform_config.json stays on device; skip plain text if .gz exists
        if "platform_config" in fpath.name:
            continue
        is_gz  = fpath.suffix == ".gz"
        is_bin = fpath.suffix.lower() in {".ico", ".jpg", ".jpeg", ".png", ".gif", ".svg", ".woff", ".woff2"}
        if not is_gz and not is_bin:
            # Skip plain text file when its .gz sibling exists; the firmware
            # probes .gz first and falls back to plain — but we still need the
            # plain copy for clients that don't accept gzip encoding.
            # Upload both so the fallback path works.
            pass  # upload all files

        rel   = fpath.relative_to(DATA_WWW)
        parts = rel.parts
        # Build upload_dir: /www/ for root files, /www/<subdir>/ for subdirs
        if len(parts) == 1:
            upload_dir = "/www/"
        else:
            upload_dir = "/www/" + "/".join(parts[:-1]) + "/"

        size = fpath.stat().st_size
        disp = f"/www/{rel.as_posix()}"
        print(f"  ↑  {disp:<46} {size:>6} B … ", end="", flush=True)
        try:
            _http_post_file(base, upload_dir, fpath)
            print(_green("✓"))
            ok += 1
        except Exception as exc:
            print(_red(f"✗  {exc}"))
            fail += 1
        time.sleep(0.12)

    print()
    if fail == 0:
        print(_green(f"  ✓  Uploaded {ok} files successfully."))
        return 0
    print(_yellow(f"  ⚠  {ok} uploaded, {fail} failed."))
    return 1


def _http_mkdir(base: str, parent: str, name: str) -> None:
    url = f"{base}/mkdir?name={name}&dir={parent}&storage=internal"
    try:
        req = urllib.request.Request(url, data=b"", method="POST")
        with urllib.request.urlopen(req, timeout=5):
            pass
    except Exception:
        pass


def _http_post_file(base: str, upload_dir: str, fpath: Path) -> None:
    boundary = "--------ESP32DeployBnd"
    fname = fpath.name
    ct, enc = mimetypes.guess_type(fname)
    if enc == "gzip" or fname.endswith(".gz"):
        ct = "application/octet-stream"
    ct = ct or "application/octet-stream"
    data = fpath.read_bytes()
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{fname}"\r\n'
        f"Content-Type: {ct}\r\n\r\n"
    ).encode() + data + f"\r\n--{boundary}--\r\n".encode()
    url = f"{base}/upload?path={upload_dir}&storage=internal"
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
    )
    with urllib.request.urlopen(req, timeout=30):
        pass


def s8_monitor(cfg: dict[str, Any], pio: str) -> int:
    _hdr("8 · monitor", f"Serial monitor  env={cfg['env']}")
    cmd = [pio, "device", "monitor", "-e", cfg["env"]]
    if cfg.get("port"):
        cmd += ["--port", cfg["port"]]
    return _run(cmd)


# ── Orchestrator ───────────────────────────────────────────────────────────────

def run_steps(cfg: dict[str, Any]) -> None:
    steps = sorted(cfg.get("steps", []))
    if not steps:
        print(_yellow("  No steps selected. Use the menu to toggle steps."))
        input(_dim("  Press Enter to return to menu… "))
        return

    # Find PlatformIO once — required for steps 3-6, 8
    pio = shutil.which("pio") or shutil.which("platformio")
    pio_steps = {3, 4, 5, 6, 8}
    if pio is None and pio_steps & set(steps):
        print(_red("  PlatformIO CLI (pio) not found in PATH."))
        print(_red("  Install:  pip install platformio"))
        input(_dim("  Press Enter to return to menu… "))
        return

    print()
    print(_bold(f"  Running steps: {', '.join(str(s) for s in steps)}"))
    print(_bold(f"  env={cfg['env']}  port={cfg.get('port') or 'auto'}  chip={cfg.get('chip')}"))

    # Persist config before running so --run picks up latest choices
    save_cfg(cfg)

    dispatch = {
        1: lambda: s1_build_web(),
        2: lambda: s2_flash_bootloader(cfg),
        3: lambda: s3_erase(cfg, pio),
        4: lambda: s4_compile(cfg, pio),
        5: lambda: s5_flash_fw(cfg, pio),
        6: lambda: s6_upload_fs(cfg, pio),
        7: lambda: s7_upload_http(cfg),
        8: lambda: s8_monitor(cfg, pio),
    }

    failed: list[int] = []
    for s in steps:
        fn = dispatch.get(s)
        if fn is None:
            continue
        rc = fn()
        if rc != 0:
            failed.append(s)
            print(_red(f"\n  ✗ Step {s} failed (exit {rc})."))
            try:
                ans = input(_yellow("  Continue with remaining steps? [y/N] ")).strip().lower()
            except (KeyboardInterrupt, EOFError):
                ans = ""
            if ans not in ("y", "yes"):
                break

    print()
    bar = "═" * 46
    if not failed:
        print(_green(f"  ╔{bar}╗"))
        print(_green(f"  ║  ✓  All steps completed successfully.         ║"))
        print(_green(f"  ╚{bar}╝"))
    else:
        fs = ", ".join(str(s) for s in failed)
        print(_red(  f"  ╔{bar}╗"))
        print(_red(  f"  ║  ✗  Completed with errors on step(s): {fs:<8}║"))
        print(_red(  f"  ╚{bar}╝"))

    print()
    try:
        input(_dim("  Press Enter to return to menu… "))
    except (KeyboardInterrupt, EOFError):
        pass


# ── Entry point ────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description="All-in-one build, flash & deploy tool for ESP32 Logger.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--run", action="store_true",
        help="Run saved steps non-interactively (skips the menu)",
    )
    args = ap.parse_args()

    if not (ROOT / "platformio.ini").is_file():
        print(_red(f"platformio.ini not found in {ROOT}. Run from project root."))
        return 2

    cfg = load_cfg()

    if args.run:
        run_steps(cfg)
        return 0

    # Interactive loop: menu → run → menu → …
    while True:
        cfg = run_menu(cfg)  # blocks until user presses 'r' (or sys.exit on 'q')
        run_steps(cfg)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n  Interrupted.")
        sys.exit(1)
