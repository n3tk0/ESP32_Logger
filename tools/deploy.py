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
import urllib.parse
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
    4: "Clean build artifacts pio run -t clean",
    5: "Compile firmware      pio run",
    6: "Flash firmware        pio run -t upload",
    7: "Upload LittleFS       pio run -t uploadfs",
    8: "Upload web via HTTP   POST /upload to device IP",
    9: "Open serial monitor   pio device monitor",
}

PRESETS: dict[str, tuple[str, list[int]]] = {
    "F": ("Full flash",    [1, 3, 5, 6, 7]),
    "C": ("Clean build",   [4, 5, 6]),
    "Q": ("Quick flash",   [5, 6]),
    "H": ("HTTP deploy",   [1, 8]),
    "A": ("All steps",     list(range(1, 10))),  # 1-9
    "N": ("None",          []),
}

# ── Config defaults + persistence ─────────────────────────────────────────────
DEFAULT_CFG: dict[str, Any] = {
    "env":           None,
    "port":          None,
    "chip":          "esp32c3",
    "baud":          921600,
    "device_ip":     "192.168.4.1",
    "steps":         [1, 3, 5, 6, 7],
    "upload_filter":      "all",   # all | gz | plain
    "wipe_before_upload": False,   # delete /www on device before uploading
}

_UPLOAD_FILTERS = ["all", "gz", "plain"]
_UPLOAD_FILTER_LABELS = {
    "all":   "Both (compressed + uncompressed)",
    "gz":    "Compressed only (.gz + binaries)",
    "plain": "Uncompressed only (plain + binaries)",
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
    uf   = cfg.get("upload_filter", "all")
    wipe = cfg.get("wipe_before_upload", False)
    for key, label, val in [
        ("e", "PlatformIO env ", cfg.get("env", "")),
        ("p", "Serial port    ", port_disp),
        ("i", "Device IP      ", cfg.get("device_ip", "")),
        ("c", "Chip type      ", cfg.get("chip", "")),
        ("b", "Baud rate      ", str(cfg.get("baud", 921600))),
        ("u", "HTTP upload    ", _UPLOAD_FILTER_LABELS.get(uf, uf)),
        ("w", "Wipe /www first", _green("YES — delete all before upload") if wipe else _dim("no")),
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
    print(f"  {_cyan('[W]')}  WiFi provision  via serial COM port  {_dim('(uppercase W)')}")
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

        elif ch == "u":
            cur = cfg.get("upload_filter", "all")
            nxt = _UPLOAD_FILTERS[((_UPLOAD_FILTERS.index(cur) + 1) % len(_UPLOAD_FILTERS))
                                   if cur in _UPLOAD_FILTERS else 0]
            cfg["upload_filter"] = nxt
            print(_dim(f"  → {_UPLOAD_FILTER_LABELS[nxt]}"))
            time.sleep(0.5)

        elif choice == "W":          # uppercase W — WiFi provisioner
            s_wifi_provision(cfg)

        elif ch == "w":              # lowercase w — wipe toggle
            cfg["wipe_before_upload"] = not cfg.get("wipe_before_upload", False)
            state = _green("ON") if cfg["wipe_before_upload"] else _dim("off")
            print(_dim(f"  → Wipe /www before upload: ") + state)
            time.sleep(0.5)

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


def s4_clean(cfg: dict[str, Any], pio: str) -> int:
    _hdr("4 · clean", f"Cleaning build artifacts  env={cfg['env']}")
    rc = _run([pio, "run", "-t", "clean", "-e", cfg["env"]])
    if rc == 0:
        print(_green("  ✓ Build artifacts cleaned."))
    return rc


def s5_compile(cfg: dict[str, Any], pio: str) -> int:
    _hdr("5 · compile", f"Compiling firmware  env={cfg['env']}")
    rc = _run([pio, "run", "-e", cfg["env"]])
    if rc == 0:
        print(_green("  ✓ Firmware compiled."))
    return rc


def s6_flash_fw(cfg: dict[str, Any], pio: str) -> int:
    _hdr("6 · flash", f"Flashing firmware  env={cfg['env']}")
    cmd = [pio, "run", "-t", "upload", "-e", cfg["env"]]
    if cfg.get("port"):
        cmd += ["--upload-port", cfg["port"]]
    rc = _run(cmd)
    if rc == 0:
        print(_green("  ✓ Firmware flashed."))
    return rc


def s7_upload_fs(cfg: dict[str, Any], pio: str) -> int:
    _hdr("7 · uploadfs", f"Uploading LittleFS  env={cfg['env']}  src={DATA_WWW}")
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


def s8_upload_http(cfg: dict[str, Any]) -> int:
    ip = cfg.get("device_ip", "192.168.4.1")
    base = f"http://{ip}"
    _hdr("8 · http-upload", f"Uploading web via HTTP  {base}")

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

    # Optional wipe of existing /www before upload
    if cfg.get("wipe_before_upload"):
        print()
        print(_yellow("  ── Wiping /www on device ─────────────────────────────"))
        deleted, wfail = _http_wipe_www(base)
        print(_yellow(f"  Deleted {deleted} file(s)" + (f", {wfail} failed" if wfail else "") + "."))
        print()

    # Ensure /www and the full directory tree exist on device before uploading.
    # Mirrors rglob("*") used below — handles arbitrarily nested layouts such as
    # www/fonts/, www/assets/icons/, etc. without manual maintenance.
    _http_mkdir(base, "/", "www")
    for dpath in sorted(p for p in DATA_WWW.rglob("*") if p.is_dir()):
        rel    = dpath.relative_to(DATA_WWW)   # e.g. PosixPath("assets/fonts")
        parts  = rel.parts                      # ("assets", "fonts")
        parent = "/www/" + "/".join(parts[:-1]) if len(parts) > 1 else "/www"
        _http_mkdir(base, parent, parts[-1])
    time.sleep(0.3)

    # Upload files — filtered by upload_filter setting
    uf = cfg.get("upload_filter", "all")
    _BIN_EXT = {".ico", ".jpg", ".jpeg", ".png", ".gif", ".svg", ".woff", ".woff2"}
    ok = fail = 0
    skipped = 0
    for fpath in sorted(DATA_WWW.rglob("*")):
        if not fpath.is_file():
            continue
        if "platform_config" in fpath.name:
            continue
        is_gz  = fpath.suffix == ".gz"
        is_bin = fpath.suffix.lower() in _BIN_EXT
        # Apply filter (binaries are always included — they are neither gz nor plain text)
        if not is_bin:
            if uf == "gz" and not is_gz:
                skipped += 1
                continue
            if uf == "plain" and is_gz:
                skipped += 1
                continue

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
    skip_note = f", {skipped} skipped" if skipped else ""
    if fail == 0:
        print(_green(f"  ✓  Uploaded {ok} files successfully{skip_note}."))
        return 0
    print(_yellow(f"  ⚠  {ok} uploaded, {fail} failed{skip_note}."))
    return 1


def _http_wipe_www(base: str) -> tuple[int, int]:
    """Delete every file under /www on the device via GET /api/filelist + POST /delete.
    Returns (deleted, failed) counts."""
    print(_yellow("  Fetching file list from device…"))
    try:
        url = f"{base}/api/filelist?dir=/www&recursive=1&storage=internal"
        with urllib.request.urlopen(url, timeout=8) as resp:
            import json as _json
            data = _json.loads(resp.read().decode())
    except Exception as exc:
        print(_red(f"  ✗ Could not fetch file list: {exc}"))
        return 0, 0

    files = data.get("files") or []
    if not files:
        print(_dim("  /www appears empty — nothing to delete."))
        return 0, 0

    deleted = failed = 0
    for f in files:
        path = f.get("path") or f.get("name") or ""
        if not path:
            continue
        disp = path if path.startswith("/") else "/" + path
        print(f"  🗑  {disp:<52} … ", end="", flush=True)
        try:
            del_url = f"{base}/delete?path={urllib.parse.quote(disp)}&storage=internal"
            req = urllib.request.Request(del_url, data=b"", method="POST")
            with urllib.request.urlopen(req, timeout=6):
                pass
            print(_green("✓"))
            deleted += 1
        except Exception as exc:
            print(_red(f"✗  {exc}"))
            failed += 1
        time.sleep(0.05)

    return deleted, failed


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


def s9_monitor(cfg: dict[str, Any], pio: str) -> int:
    _hdr("9 · monitor", f"Serial monitor  env={cfg['env']}")
    cmd = [pio, "device", "monitor", "-e", cfg["env"]]
    if cfg.get("port"):
        cmd += ["--port", cfg["port"]]
    return _run(cmd)


# ── WiFi provisioner via serial ────────────────────────────────────────────────

def _monitor_baud() -> int:
    """Read monitor_speed from platformio.ini; fall back to 115200."""
    try:
        txt = (ROOT / "platformio.ini").read_text()
        m = re.search(r"^\s*monitor_speed\s*=\s*(\d+)", txt, re.MULTILINE)
        if m:
            return int(m.group(1))
    except OSError:
        pass
    return 115200


def s_wifi_provision(cfg: dict[str, Any]) -> None:
    """Interactive WiFi provisioner via serial COM port (deploy.py [W])."""
    _hdr("W · wifi-provision", "Switch device from AP → Client via serial")

    # ── Check pyserial ────────────────────────────────────────────────────────
    try:
        import serial          # type: ignore  # noqa: F401
        import serial.tools.list_ports  # type: ignore
        import serial as _serial
    except ImportError:
        print(_red("  pyserial is not installed."))
        print(_dim("  Install it with:  pip install pyserial"))
        input(_dim("  Press Enter to return to menu… "))
        return

    import getpass as _getpass

    port = cfg.get("port") or _detect_port()
    if not port:
        print(_red("  No serial port found. Connect the device and set port via [p]."))
        input(_dim("  Press Enter to return to menu… "))
        return

    baud = _monitor_baud()
    print(f"  Port: {_bold(port)}   Baud: {_bold(baud)}")
    print()

    # ── Open port ─────────────────────────────────────────────────────────────
    try:
        ser = _serial.Serial(port, baud, timeout=1)
        ser.reset_input_buffer()
    except Exception as exc:
        print(_red(f"  Cannot open {port}: {exc}"))
        input(_dim("  Press Enter to return to menu… "))
        return

    PREFIX = ">>SP<<"  # must match SERIAL_RESP_PREFIX in SerialProvisioner.h

    def _send_recv(obj: dict, timeout_s: float = 10.0) -> "dict | None":
        """Send JSON command, wait for a >>SP<<-prefixed response line."""
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        ser.write(line.encode())
        ser.flush()
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                raw = ser.readline().decode(errors="replace").strip()
            except Exception:
                return None
            if not raw:
                continue
            if raw.startswith(PREFIX):
                payload = raw[len(PREFIX):]
                try:
                    return json.loads(payload)
                except Exception:
                    return None
            # Non-SP line = firmware log — print dimmed for visibility
            print(_dim(f"    {raw}"))
        return None  # timeout

    # ── Ping ──────────────────────────────────────────────────────────────────
    print(_dim("  Pinging device…"), end=" ", flush=True)
    resp = _send_recv({"cmd": "ping"}, timeout_s=5)
    if not resp or not resp.get("ok"):
        print(_red("✗"))
        print()
        print(_red("  No response from device."))
        print(_dim("  • Make sure the firmware is flashed (step 5+6)."))
        print(_dim("  • The device must not be in deep sleep."))
        print(_dim("  • Try pressing the reset button and retrying."))
        ser.close()
        input(_dim("  Press Enter to return to menu… "))
        return

    print(_green("✓"))
    mode = resp.get("mode", "?")
    if resp.get("ip"):
        print(f"  Mode: {_bold(mode)}   IP: {_bold(resp['ip'])}")
    elif resp.get("ap_ip"):
        print(f"  Mode: {_bold(mode)}   AP IP: {_bold(resp['ap_ip'])}")
    else:
        print(f"  Mode: {_bold(mode)}")
    print()

    # ── Scan ──────────────────────────────────────────────────────────────────
    print(_bold("  Scanning for WiFi networks…"), end=" ", flush=True)
    # wifi_scan sends an intermediate {"ok":"scanning"} ACK within ~100 ms,
    # then the real result after 2-4 s.  We read both in sequence.
    ser.write((json.dumps({"cmd": "wifi_scan"}, separators=(",", ":")) + "\n").encode())
    ser.flush()

    nets: list[dict] = []
    scan_ok = False
    deadline = time.time() + 15.0
    while time.time() < deadline:
        try:
            raw = ser.readline().decode(errors="replace").strip()
        except Exception:
            break
        if not raw:
            continue
        if raw.startswith(PREFIX):
            r = {}
            try:
                r = json.loads(raw[len(PREFIX):])
            except Exception:
                pass
            if r.get("ok") == "scanning":
                print(_dim("(scanning…)"), end=" ", flush=True)
                continue
            if r.get("ok") is True:
                nets = r.get("nets", [])
                scan_ok = True
                break
            # ok==false
            print(_red("✗"))
            print(_red(f"  Scan failed: {r.get('err', '?')}"))
            ser.close()
            input(_dim("  Press Enter to return to menu… "))
            return
        else:
            print(_dim(f"\n    {raw}"), end=" ", flush=True)

    if not scan_ok:
        print(_red("✗ (timeout)"))
        ser.close()
        input(_dim("  Press Enter to return to menu… "))
        return

    print(_green(f"✓  {len(nets)} network(s) found"))
    print()

    if not nets:
        print(_yellow("  No WiFi networks found — is the antenna connected?"))
        ser.close()
        input(_dim("  Press Enter to return to menu… "))
        return

    # ── Display network list ───────────────────────────────────────────────────
    print(f"  {'#':>3}  {'SSID':<34}  {'Signal':>9}  Security")
    print("  " + "─" * 60)
    for i, net in enumerate(nets):
        ssid = (net.get("ssid") or "")[:34]
        rssi = int(net.get("rssi", -99))
        enc  = net.get("enc", 1)
        lock = "🔒 WPA/2" if enc else "  open  "
        # Colour signal strength
        if rssi >= -60:
            sig = _green(f"{rssi:>4} dBm")
        elif rssi >= -75:
            sig = _yellow(f"{rssi:>4} dBm")
        else:
            sig = _red(f"{rssi:>4} dBm")
        print(f"  {i + 1:>3}  {ssid:<34}  {sig}  {lock}")
    print()

    # ── Select network ────────────────────────────────────────────────────────
    try:
        sel = input(_bold("  Select network (number or type SSID): ")).strip()
    except (KeyboardInterrupt, EOFError):
        print()
        ser.close()
        return

    # Resolve to SSID string
    chosen_ssid = ""
    chosen_enc  = 1
    try:
        idx = int(sel) - 1
        if 0 <= idx < len(nets):
            chosen_ssid = nets[idx].get("ssid", "")
            chosen_enc  = nets[idx].get("enc", 1)
    except ValueError:
        chosen_ssid = sel  # user typed SSID directly
        # Try to look up enc from scan results
        for n in nets:
            if n.get("ssid") == chosen_ssid:
                chosen_enc = n.get("enc", 1)
                break

    if not chosen_ssid:
        print(_red("  Invalid selection."))
        ser.close()
        input(_dim("  Press Enter to return to menu… "))
        return

    # ── Password ──────────────────────────────────────────────────────────────
    if chosen_enc == 0:
        chosen_pass = ""
        print(f"  Connecting to {_bold(chosen_ssid)} (open network)…")
    else:
        try:
            chosen_pass = _getpass.getpass(f"  Password for \"{chosen_ssid}\": ")
        except (KeyboardInterrupt, EOFError):
            print()
            ser.close()
            return
        if not chosen_pass:
            print(_yellow("  Empty password — treating as open network."))
        print(f"  Connecting to {_bold(chosen_ssid)}…")

    # ── Connect ───────────────────────────────────────────────────────────────
    print(_dim("  (waiting up to 20 s for association…)"))
    resp = _send_recv(
        {"cmd": "wifi_connect", "ssid": chosen_ssid, "pass": chosen_pass},
        timeout_s=25.0,
    )

    print()
    if resp is None:
        print(_red("  ✗ No response — the device may have crashed or reset."))
    elif resp.get("ok"):
        ip = resp.get("ip", "?")
        gw = resp.get("gw", "?")
        print(_green(f"  ✓ Connected to {chosen_ssid}"))
        print(_green(f"    IP:      {ip}"))
        print(_green(f"    Gateway: {gw}"))
        print()
        print(_dim(f"  Web UI:  http://{ip}"))
        print(_dim("  The device is now reachable on your local network."))
        print(_dim("  Use the web UI to save WiFi credentials permanently."))
        # Update saved device IP so step 8 (HTTP deploy) targets the new address
        cfg["device_ip"] = ip
        save_cfg(cfg)
    else:
        err = resp.get("err", "unknown")
        print(_red(f"  ✗ Connection failed: {err}"))
        print(_dim("  Device has restored AP mode — you can still connect via AP."))

    ser.close()
    print()
    input(_dim("  Press Enter to return to menu… "))


# ── Orchestrator ───────────────────────────────────────────────────────────────

def run_steps(cfg: dict[str, Any]) -> None:
    steps = sorted(cfg.get("steps", []))
    if not steps:
        print(_yellow("  No steps selected. Use the menu to toggle steps."))
        input(_dim("  Press Enter to return to menu… "))
        return

    # Find PlatformIO once — required for steps 3-7, 9
    pio = shutil.which("pio") or shutil.which("platformio")
    pio_steps = {3, 4, 5, 6, 7, 9}
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
        4: lambda: s4_clean(cfg, pio),
        5: lambda: s5_compile(cfg, pio),
        6: lambda: s6_flash_fw(cfg, pio),
        7: lambda: s7_upload_fs(cfg, pio),
        8: lambda: s8_upload_http(cfg),
        9: lambda: s9_monitor(cfg, pio),
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
