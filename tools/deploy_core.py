"""
tools/deploy_core.py — Shared core logic for CLI and GUI deploy tools.

Encapsulates all step implementations, configuration management, and orchestration
so both deploy.py (CLI) and deploy_gui.py (GUI) use identical logic.
"""

from __future__ import annotations

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
from typing import Any, Callable, Optional

# The board list is derived from platformio.ini, never hardcoded here — see
# tools/pio_envs.py for why. sys.path juggling because these tools are run
# both as `python3 tools/deploy.py` and from a PyInstaller bundle.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pio_envs import (  # noqa: E402
    chip_for, default_env, env_info, env_names, environments, usb_pins,
)

# ── Project layout ────────────────────────────────────────────────────────────
ROOT     = Path(__file__).resolve().parent.parent
WWW_SRC  = ROOT / "www"
DATA_WWW = ROOT / "data" / "www"
TOOLS    = ROOT / "tools"
CFG_FILE = ROOT / ".flash_tool.json"

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
    "A": ("All steps",     list(range(1, 10))),
    "N": ("None",          []),
}

# ── Config defaults + persistence ─────────────────────────────────────────────
DEFAULT_CFG: dict[str, Any] = {
    "env":                  None,
    "port":                 None,
    "chip":                 "esp32c3",
    "baud":                 921600,
    "device_ip":            "192.168.4.1",
    "steps":                [1, 3, 5, 6, 7],
    "upload_filter":        "all",
    "wipe_before_upload":   False,
    "usb_cdc_on_boot":      True,  # ESP32-C3: USB serial CDC enabled by default
}

_UPLOAD_FILTERS = ["all", "gz", "plain"]
_UPLOAD_FILTER_LABELS = {
    "all":   "Both (compressed + uncompressed)",
    "gz":    "Compressed only (.gz + binaries)",
    "plain": "Uncompressed only (plain + binaries)",
}


def detect_env() -> str:
    """The project's default PlatformIO environment.

    Previously this took the FIRST [env:…] in the file, which is not the same
    question: reordering platformio.ini would have silently changed what the
    deploy tool flashed. It now honours [platformio] default_envs, and skips
    the test-only builds.
    """
    return default_env()


def detect_port() -> str:
    """Auto-detect serial port."""
    if sys.platform == "linux":
        pats = ["/dev/ttyACM*", "/dev/ttyUSB*"]
    elif sys.platform == "darwin":
        pats = ["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB*"]
    elif sys.platform == "win32":
        try:
            import serial.tools.list_ports
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
    """Load configuration from file or use defaults."""
    cfg: dict[str, Any] = dict(DEFAULT_CFG)
    if CFG_FILE.is_file():
        try:
            cfg.update(json.loads(CFG_FILE.read_text()))
        except Exception:
            pass
    if not cfg.get("env"):
        cfg["env"] = detect_env()
    # The chip follows the env, always. It used to be a free-text field kept
    # next to it, so a saved config could name esp32s3 as the environment and
    # esp32c3 as the chip — and step 2 would then write a C3 bootloader to an
    # S3. There is no board for which the two legitimately disagree, so it is
    # derived rather than remembered.
    cfg["chip"] = chip_for(cfg["env"])
    if not cfg.get("port"):
        cfg["port"] = detect_port()
    return cfg


def save_cfg(cfg: dict[str, Any]) -> None:
    """Save configuration to file."""
    try:
        CFG_FILE.write_text(json.dumps(cfg, indent=2) + "\n")
    except Exception as exc:
        print(f"Warning: could not save config: {exc}")


class DeployManager:
    """Orchestrates the deployment workflow with callback support for progress/output."""

    def __init__(self, cfg: dict[str, Any]):
        self.cfg = cfg
        self.pio = shutil.which("pio") or shutil.which("platformio")

        # Callbacks for GUI/CLI integration
        self.on_step_start: Optional[Callable[[int, str], None]] = None
        self.on_step_output: Optional[Callable[[str], None]] = None
        self.on_step_complete: Optional[Callable[[int, int], None]] = None
        self.on_error: Optional[Callable[[str], None]] = None

    def _log(self, msg: str, end: str = "\n") -> None:
        """Internal logging. Supports end parameter for in-line progress."""
        if self.on_step_output:
            try:
                self.on_step_output(msg + end)
            except TypeError:
                self.on_step_output(msg)
        else:
            print(msg, end=end)

    def _emit_start(self, step: int, name: str) -> None:
        if self.on_step_start:
            self.on_step_start(step, name)
        else:
            self._log(f"\n[{step}] {name}")

    def _emit_complete(self, step: int, rc: int) -> None:
        if self.on_step_complete:
            self.on_step_complete(step, rc)

    def _configure_usb_cdc(self) -> None:
        """Rewrite -DARDUINO_USB_CDC_ON_BOOT in platformio.ini before compiling.

        On the C3 and S3 the USB Serial/JTAG pins are either the serial console
        or general GPIO — never both — and which one is a compile-time decision.
        Toggling here is what lets the wizard offer those pins.

        Which envs can be toggled is read from platformio.ini rather than
        listed here: the env must carry its own -DARDUINO_USB_CDC_ON_BOOT line,
        because this rewrites the flag in place. An env that inherits the flag
        through `extends` (esp32s3_n16r8 does) has nothing here to rewrite, and
        editing its parent would quietly change a second board too — so it is
        reported instead of guessed at.
        """
        env = self.cfg.get("env") or detect_env()
        usb_cdc = self.cfg.get("usb_cdc_on_boot", True)

        info = env_info(env)
        if not info.supports_usb_cdc:
            pins = usb_pins(info.chip)
            if pins:
                self._log(f"  ! USB CDC toggle skipped: [env:{env}] has no "
                          f"-DARDUINO_USB_CDC_ON_BOOT of its own (it inherits one). "
                          f"{pins} stay as the flag in its parent env leaves them.")
            return

        ini_file = ROOT / "platformio.ini"
        if not ini_file.is_file():
            return

        want = f"-DARDUINO_USB_CDC_ON_BOOT={'1' if usb_cdc else '0'}"
        lines = ini_file.read_text().split("\n")

        # The section runs from its header to the NEXT section header of any
        # kind. The previous version ended it only at another "[env:", so with
        # the target env last in the file it ran to EOF and rewrote whatever it
        # found on the way — including the "; Set -DARDUINO_USB_CDC_ON_BOOT=0
        # to free GPIO…" comments that document the other boards.
        start = end = None
        for i, line in enumerate(lines):
            stripped = line.strip()
            if start is None:
                if stripped == f"[env:{env}]":
                    start = i
                continue
            if stripped.startswith("["):
                end = i
                break
        if start is None:
            return
        if end is None:
            end = len(lines)

        # Rewrite the flag where it already is. Comment lines are skipped: a
        # line beginning with ; or # is documentation, not a build flag, and
        # editing it silently rewrote the file's own explanation of itself.
        modified = False
        found = False
        for i in range(start, end):
            stripped = lines[i].lstrip()
            if stripped.startswith((";", "#")):
                continue
            if "-DARDUINO_USB_CDC_ON_BOOT=" not in lines[i]:
                continue
            found = True
            new = re.sub(r"-DARDUINO_USB_CDC_ON_BOOT=[01]", want, lines[i])
            if new != lines[i]:
                lines[i] = new
                modified = True

        # Nothing to rewrite: add the flag as the first entry of build_flags.
        #
        # This branch used to fire even when the flag WAS present, because it
        # only checked whether the flag had been seen BEFORE the build_flags
        # line — and in every env here the flag comes after it. The result was
        # one more duplicate -DARDUINO_USB_CDC_ON_BOOT appended to the block on
        # every single run of the deploy tool.
        if not found:
            for i in range(start, end):
                stripped = lines[i].lstrip()
                if stripped.startswith((";", "#")):
                    continue
                if stripped.startswith("build_flags") and lines[i].rstrip().endswith("="):
                    lines.insert(i + 1, f"    {want}")
                    modified = True
                    break
            else:
                self._log(f"  ! USB CDC toggle skipped: [env:{env}] has no "
                          f"multi-line build_flags block to add the flag to.")
                return

        if modified:
            ini_file.write_text("\n".join(lines))
            self._log(f"  platformio.ini: [env:{env}] {want}")

    def _run_cmd(self, cmd: list[str]) -> int:
        """Run a subprocess command and stream output to callback."""
        self._log(f"$ {shlex.join(cmd)}")
        try:
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
        except Exception as exc:
            self._log(f"Error: {exc}")
            return 1

        # Don't use Popen as a context manager: its __exit__ calls wait()
        # without terminating first, so a KeyboardInterrupt (or an exception
        # from the logging callback) would block forever on a still-running
        # child. Instead, terminate the child on any exception, then re-raise.
        try:
            if process.stdout:
                for line in process.stdout:
                    self._log(line, end="")
            return process.wait()
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
            raise
        finally:
            if process.stdout:
                process.stdout.close()

    # ── Step implementations ───────────────────────────────────────────────────

    def s1_build_web(self) -> int:
        self._emit_start(1, STEP_NAMES[1])
        script = TOOLS / "build_web.py"
        if not script.is_file():
            self._log("ERROR: build_web.py not found in tools/")
            return 2
        rc = self._run_cmd([sys.executable, str(script), "--dst", str(DATA_WWW)])
        if rc == 0:
            self._log("✓ Web assets built.")
        self._emit_complete(1, rc)
        return rc

    def s2_flash_bootloader(self) -> int:
        self._emit_start(2, STEP_NAMES[2])
        script = TOOLS / "flash_bootloader.py"
        if not script.is_file():
            self._log("ERROR: flash_bootloader.py not found in tools/")
            return 2
        cmd = [sys.executable, str(script), "--chip", self.cfg["chip"], "--baud", str(self.cfg["baud"])]
        if self.cfg.get("port"):
            cmd += ["--port", self.cfg["port"]]
        rc = self._run_cmd(cmd)
        if rc == 0:
            self._log("✓ Bootloader flashed.")
        self._emit_complete(2, rc)
        return rc

    def s3_erase(self, confirm_callback: Optional[Callable[[], bool]] = None) -> int:
        self._emit_start(3, STEP_NAMES[3])
        self._log("*** WARNING: Wipes all flash (config, logs, LittleFS). ***")

        # Allow confirmation callback (for GUI to show dialog)
        if confirm_callback:
            if not confirm_callback():
                self._log("Skipped.")
                self._emit_complete(3, 0)
                return 0

        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return 1

        cmd = [self.pio, "run", "-t", "erase", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--upload-port", self.cfg["port"]]
        rc = self._run_cmd(cmd)
        if rc == 0:
            self._log("✓ Flash erased.")
        self._emit_complete(3, rc)
        return rc

    def s4_clean(self) -> int:
        self._emit_start(4, STEP_NAMES[4])
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return 1
        rc = self._run_cmd([self.pio, "run", "-t", "clean", "-e", self.cfg["env"]])
        if rc == 0:
            self._log("✓ Build artifacts cleaned.")
        self._emit_complete(4, rc)
        return rc

    def s5_compile(self) -> int:
        self._emit_start(5, STEP_NAMES[5])
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return 1
        # Configure USB CDC flag before compilation
        self._configure_usb_cdc()
        rc = self._run_cmd([self.pio, "run", "-e", self.cfg["env"]])
        if rc == 0:
            self._log("✓ Firmware compiled.")
        self._emit_complete(5, rc)
        return rc

    def s6_flash_fw(self) -> int:
        self._emit_start(6, STEP_NAMES[6])
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return 1
        cmd = [self.pio, "run", "-t", "upload", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--upload-port", self.cfg["port"]]
        rc = self._run_cmd(cmd)
        if rc == 0:
            self._log("✓ Firmware flashed.")
        self._emit_complete(6, rc)
        return rc

    def s7_upload_fs(self) -> int:
        self._emit_start(7, STEP_NAMES[7])
        if not DATA_WWW.is_dir() or not any(DATA_WWW.iterdir()):
            self._log("data/www/ is empty — running Build web first…")
            rc = self.s1_build_web()
            if rc != 0:
                return rc

        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return 1

        cmd = [self.pio, "run", "-t", "uploadfs", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--upload-port", self.cfg["port"]]
        rc = self._run_cmd(cmd)
        if rc == 0:
            self._log("✓ LittleFS image uploaded.")
        self._emit_complete(7, rc)
        return rc

    def s8_upload_http(self) -> int:
        self._emit_start(8, STEP_NAMES[8])
        ip = self.cfg.get("device_ip", "192.168.4.1")
        base = f"http://{ip}"
        self._log(f"Uploading web via HTTP to {base}")

        if not DATA_WWW.is_dir() or not any(DATA_WWW.iterdir()):
            self._log("ERROR: data/www/ is empty. Run step 1 (Build web assets) first.")
            self._emit_complete(8, 2)
            return 2

        # Connectivity check
        try:
            with urllib.request.urlopen(f"{base}/api/status", timeout=4) as resp:
                self._log(f"Device online (HTTP {resp.status})")
        except Exception as exc:
            self._log(f"ERROR: Device not reachable at {base}: {exc}")
            self._emit_complete(8, 1)
            return 1

        # Fetch the per-boot CSRF token. The mutating endpoints (/upload,
        # /delete, /mkdir) reject any request without a matching token with
        # HTTP 403 {"error":"csrf"}; the SPA injects it on every call, so the
        # deploy tooling must do the same.
        self._csrf_token = self._fetch_csrf(base)

        # Optional wipe
        if self.cfg.get("wipe_before_upload"):
            self._log("Wiping /www on device…")
            deleted, failed = self._http_wipe_www(base)
            self._log(f"Deleted {deleted} file(s)" + (f", {failed} failed" if failed else "") + ".")

        # Create directories
        self._http_mkdir(base, "/", "www")
        for dpath in sorted(p for p in DATA_WWW.rglob("*") if p.is_dir()):
            rel    = dpath.relative_to(DATA_WWW)
            parts  = rel.parts
            parent = "/www/" + "/".join(parts[:-1]) if len(parts) > 1 else "/www"
            self._http_mkdir(base, parent, parts[-1])
        time.sleep(0.3)

        # Upload files
        uf = self.cfg.get("upload_filter", "all")
        _BIN_EXT = {".ico", ".jpg", ".jpeg", ".png", ".gif", ".svg", ".woff", ".woff2"}
        ok = fail = skipped = 0

        for fpath in sorted(DATA_WWW.rglob("*")):
            if not fpath.is_file():
                continue
            if "platform_config" in fpath.name:
                continue

            is_gz  = fpath.suffix == ".gz"
            is_bin = fpath.suffix.lower() in _BIN_EXT

            # Apply filter
            if not is_bin:
                if uf == "gz" and not is_gz:
                    skipped += 1
                    continue
                if uf == "plain" and is_gz:
                    skipped += 1
                    continue

            rel   = fpath.relative_to(DATA_WWW)
            parts = rel.parts
            upload_dir = "/www/" if len(parts) == 1 else "/www/" + "/".join(parts[:-1]) + "/"

            size = fpath.stat().st_size
            disp = f"/www/{rel.as_posix()}"
            self._log(f"  ↑  {disp:<46} {size:>6} B … ", end="")

            try:
                self._http_post_file(base, upload_dir, fpath)
                self._log(" ✓")
                ok += 1
            except Exception as exc:
                self._log(f" ✗ {exc}")
                fail += 1
            time.sleep(0.12)

        self._log("")
        skip_note = f", {skipped} skipped" if skipped else ""
        if fail == 0:
            self._log(f"✓ Uploaded {ok} files successfully{skip_note}.")
            self._emit_complete(8, 0)
            return 0

        self._log(f"⚠ {ok} uploaded, {fail} failed{skip_note}.")
        self._emit_complete(8, 1)
        return 1

    def _fetch_csrf(self, base: str) -> str:
        """Fetch the per-boot CSRF token required by mutating endpoints.

        Returns '' if the endpoint is unavailable (e.g. older firmware), so
        callers degrade gracefully rather than crashing — the request will
        still 403 in that case, but the failure mode is unchanged.
        """
        try:
            with urllib.request.urlopen(f"{base}/api/csrf-token", timeout=4) as resp:
                data = json.loads(resp.read().decode())
            tok = data.get("token", "") if isinstance(data, dict) else ""
            if isinstance(tok, str) and tok:
                self._log("Fetched CSRF token.")
                return tok
            self._log("⚠ CSRF token endpoint returned no token.")
        except Exception as exc:
            self._log(f"⚠ Could not fetch CSRF token ({exc}); uploads may be rejected (403).")
        return ""

    def _csrf_qs(self) -> str:
        """'&csrf=<token>' query-string fragment, or '' when no token is held."""
        tok = getattr(self, "_csrf_token", "")
        return f"&csrf={urllib.parse.quote(tok)}" if tok else ""

    def _http_wipe_www(self, base: str) -> tuple[int, int]:
        """Delete every file under /www on the device."""
        self._log("Fetching file list from device…")
        try:
            url = f"{base}/api/filelist?dir=/www&recursive=1&storage=internal"
            with urllib.request.urlopen(url, timeout=8) as resp:
                data = json.loads(resp.read().decode())
        except Exception as exc:
            self._log(f"✗ Could not fetch file list: {exc}")
            return 0, 0

        files = data.get("files") or []
        if not files:
            self._log("/www appears empty — nothing to delete.")
            return 0, 0

        deleted = failed = 0
        for f in files:
            path = f.get("path") or f.get("name") or ""
            if not path:
                continue
            disp = path if path.startswith("/") else "/" + path
            self._log(f"  🗑  {disp:<52} … ", end="")
            try:
                del_url = f"{base}/delete?path={urllib.parse.quote(disp)}&storage=internal{self._csrf_qs()}"
                req = urllib.request.Request(del_url, data=b"", method="POST")
                with urllib.request.urlopen(req, timeout=6):
                    pass
                self._log("✓")
                deleted += 1
            except Exception as exc:
                self._log(f"✗ {exc}")
                failed += 1
            time.sleep(0.05)

        return deleted, failed

    def _http_mkdir(self, base: str, parent: str, name: str) -> None:
        q = urllib.parse.quote
        url = f"{base}/mkdir?name={q(name)}&dir={q(parent)}&storage=internal{self._csrf_qs()}"
        try:
            req = urllib.request.Request(url, data=b"", method="POST")
            with urllib.request.urlopen(req, timeout=5):
                pass
        except Exception:
            pass

    def _http_post_file(self, base: str, upload_dir: str, fpath: Path) -> None:
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
        url = f"{base}/upload?path={urllib.parse.quote(upload_dir)}&storage=internal{self._csrf_qs()}"
        req = urllib.request.Request(
            url, data=body, method="POST",
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
        )
        with urllib.request.urlopen(req, timeout=30):
            pass

    def s9_monitor(self) -> int:
        self._emit_start(9, STEP_NAMES[9])
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return 1
        cmd = [self.pio, "device", "monitor", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--port", self.cfg["port"]]
        return self._run_cmd(cmd)

    def provision_wifi(
        self,
        input_fn: Optional[Callable[[str], str]] = None,
        getpass_fn: Optional[Callable[[str], str]] = None,
        confirm_callback: Optional[Callable[[str], bool]] = None,
    ) -> bool:
        """WiFi provisioning via serial. Returns True if successful."""
        try:
            import serial
            import serial.tools.list_ports
        except ImportError:
            self._log("Error: pyserial not installed. Install with: pip install pyserial")
            return False

        # Helpers
        if input_fn is None:
            input_fn = input
        if getpass_fn is None:
            import getpass
            getpass_fn = getpass.getpass

        # Detect port
        port = self.cfg.get("port") or detect_port()
        if not port:
            self._log("Error: No serial port found. Connect device and set port via settings.")
            return False

        # Detect baud from platformio.ini
        try:
            txt = (ROOT / "platformio.ini").read_text()
            m = re.search(r"^\s*monitor_speed\s*=\s*(\d+)", txt, re.MULTILINE)
            baud = int(m.group(1)) if m else 115200
        except OSError:
            baud = 115200

        self._log(f"WiFi Provisioning via serial")
        self._log(f"Port: {port}   Baud: {baud}\n")

        # Open serial port
        try:
            ser = serial.Serial(port, baud, timeout=1)
            ser.reset_input_buffer()
        except Exception as exc:
            self._log(f"Error: Cannot open {port}: {exc}")
            return False

        PREFIX = ">>SP<<"

        def _send_recv(obj: dict, timeout_s: float = 10.0) -> dict | None:
            """Send JSON, wait for >>SP<<-prefixed response."""
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
                # Non-SP line = device log
                self._log(f"    {raw}")
            return None

        # Ping device
        self._log("Pinging device… ", end="")
        resp = _send_recv({"cmd": "ping"}, timeout_s=5)
        if not resp or not resp.get("ok"):
            self._log("✗\n")
            self._log("Error: No response from device.")
            self._log("  • Make sure firmware is flashed")
            self._log("  • Device must not be in deep sleep")
            self._log("  • Try pressing reset button")
            ser.close()
            return False

        self._log("✓")
        mode = resp.get("mode", "?")
        if resp.get("ip"):
            self._log(f"Mode: {mode}   IP: {resp['ip']}\n")
        elif resp.get("ap_ip"):
            self._log(f"Mode: {mode}   AP IP: {resp['ap_ip']}\n")
        else:
            self._log(f"Mode: {mode}\n")

        # Scan networks
        self._log("Scanning for WiFi networks… ", end="")
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
                    self._log("(scanning…) ", end="")
                    continue
                if r.get("ok") is True:
                    nets = r.get("nets", [])
                    scan_ok = True
                    break
                # ok==false
                self._log("✗")
                self._log(f"Error: Scan failed: {r.get('err', '?')}\n")
                ser.close()
                return False
            else:
                self._log(f"\n    {raw} ", end="")

        if not scan_ok:
            self._log("✗ (timeout)\n")
            ser.close()
            return False

        self._log(f"✓  {len(nets)} network(s) found\n")

        if not nets:
            self._log("Error: No WiFi networks found — antenna connected?\n")
            ser.close()
            return False

        # Display networks
        self._log(f"  {'#':>3}  {'SSID':<34}  {'Signal':>9}  Security")
        self._log("  " + "─" * 60)
        for i, net in enumerate(nets):
            ssid = (net.get("ssid") or "")[:34]
            rssi = int(net.get("rssi", -99))
            enc = net.get("enc", 1)
            lock = "🔒 WPA/2" if enc else "  open  "
            sig = f"{rssi:>4} dBm"
            self._log(f"  {i + 1:>3}  {ssid:<34}  {sig}  {lock}")
        self._log("")

        # Select network
        try:
            sel = input_fn("Select network (number or type SSID): ").strip()
        except (KeyboardInterrupt, EOFError):
            self._log("")
            ser.close()
            return False

        # Resolve to SSID
        chosen_ssid = ""
        chosen_enc = 1
        try:
            idx = int(sel) - 1
            if 0 <= idx < len(nets):
                chosen_ssid = nets[idx].get("ssid", "")
                chosen_enc = nets[idx].get("enc", 1)
        except ValueError:
            chosen_ssid = sel
            for n in nets:
                if n.get("ssid") == chosen_ssid:
                    chosen_enc = n.get("enc", 1)
                    break

        if not chosen_ssid:
            self._log("Error: Invalid selection.\n")
            ser.close()
            return False

        # Get password
        if chosen_enc == 0:
            chosen_pass = ""
            self._log(f"Connecting to {chosen_ssid} (open network)…")
        else:
            try:
                chosen_pass = getpass_fn(f"Password for \"{chosen_ssid}\": ")
            except (KeyboardInterrupt, EOFError):
                self._log("")
                ser.close()
                return False
            if not chosen_pass:
                self._log("Warning: Empty password — treating as open network.")
            self._log(f"Connecting to {chosen_ssid}…")

        self._log("(waiting up to 20 s for association…)")

        # Connect
        resp = _send_recv(
            {"cmd": "wifi_connect", "ssid": chosen_ssid, "pass": chosen_pass},
            timeout_s=25.0,
        )

        self._log("")
        if resp is None:
            self._log("Error: No response — device may have crashed or reset.\n")
        elif resp.get("ok"):
            ip = resp.get("ip", "?")
            gw = resp.get("gw", "?")
            self._log(f"✓ Connected to {chosen_ssid}")
            self._log(f"  IP:      {ip}")
            self._log(f"  Gateway: {gw}\n")
            self._log(f"Web UI:  http://{ip}")
            self._log("Device is now on your local network.")
            self._log("Use web UI to save WiFi credentials permanently.\n")
            # Update config
            self.cfg["device_ip"] = ip
            save_cfg(self.cfg)
            ser.close()
            return True
        else:
            err = resp.get("err", "unknown")
            self._log(f"Error: Connection failed: {err}")
            self._log("Device has restored AP mode — you can still connect via AP.\n")

        ser.close()
        return False

    # ── Orchestration ──────────────────────────────────────────────────────────

    def run_steps(self, steps: list[int], confirm_erase_callback: Optional[Callable[[], bool]] = None) -> bool:
        """Run selected steps. Returns True if all succeeded."""
        steps = sorted(steps)
        if not steps:
            self._log("No steps selected.")
            return False

        dispatch = {
            1: self.s1_build_web,
            2: self.s2_flash_bootloader,
            3: lambda: self.s3_erase(confirm_erase_callback),
            4: self.s4_clean,
            5: self.s5_compile,
            6: self.s6_flash_fw,
            7: self.s7_upload_fs,
            8: self.s8_upload_http,
            9: self.s9_monitor,
        }

        failed: list[int] = []
        self._log(f"\n{'='*60}")
        self._log(f"Running steps: {', '.join(str(s) for s in steps)}")
        self._log(f"Environment: {self.cfg['env']}")
        self._log(f"Port: {self.cfg.get('port') or 'auto'}")
        self._log(f"Chip: {self.cfg.get('chip')}")
        self._log(f"{'='*60}\n")

        for s in steps:
            fn = dispatch.get(s)
            if fn is None:
                continue
            try:
                rc = fn()
                if rc != 0:
                    failed.append(s)
                    self._log(f"✗ Step {s} failed (exit {rc}).")
            except Exception as exc:
                self._log(f"✗ Step {s} error: {exc}")
                failed.append(s)

        self._log("")
        if not failed:
            self._log("✓ All steps completed successfully.")
            return True

        fs = ", ".join(str(s) for s in failed)
        self._log(f"✗ Completed with errors on step(s): {fs}")
        return False
