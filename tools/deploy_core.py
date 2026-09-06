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
from features import build_flags_for, is_known, optional_features
from pio_envs import (  # noqa: E402
    ROOT as _PIO_ROOT, chip_for, default_env, defaults_for, env_info,
    env_names, environments, ports_for, usb_pins,
)

# ── Project layout ────────────────────────────────────────────────────────────
# Taken from pio_envs rather than computed again: under PyInstaller the two
# would otherwise disagree, and the failure is silent — the board list would
# come from the real platformio.ini while the USB CDC rewrite edited a copy
# inside the extraction directory and the config saved beside it.
ROOT     = _PIO_ROOT
WWW_SRC  = ROOT / "www"
DATA_WWW = ROOT / "data" / "www"
TOOLS    = ROOT / "tools"
CFG_FILE = ROOT / ".flash_tool.json"


def _python() -> str:
    """Return the path to the Python interpreter.

    In a normal ``python deploy_gui.py`` invocation ``sys.executable`` is the
    interpreter.  Inside a PyInstaller frozen bundle it is the ``.exe`` itself
    (e.g. ``ESP32_Deploy.exe``), and using it to spawn a helper script opens a
    new GUI window instead of running the script.  Fall back to whichever
    ``python`` / ``python3`` is on PATH so the helper scripts still work.
    """
    if not getattr(sys, "frozen", False):
        return sys.executable
    for name in ("python3", "python"):
        found = shutil.which(name)
        if found:
            return found
    # Last resort: the frozen executable cannot run .py scripts, but returning
    # it keeps the error message intelligible ("cannot run build_web.py")
    # rather than crashing with a NoneType later.
    return sys.executable


# ── Windows: children without console windows ─────────────────────────────────
#
# Every step runs a console-subsystem program (python.exe, pio.exe, esptool),
# and Windows gives such a child a console window of its own whenever the
# parent has none. The frozen GUI is built windowed (console=False in
# deploy_gui.spec), so each step popped an empty black window that showed
# nothing — its output is on a pipe, being written into the GUI's log.
#
# The helpers live in win_console.py because flash_bootloader.py needs exactly
# the same decision, and a copy in each file is a copy to keep in step.
from win_console import has_console as _has_console, no_window as _no_window  # noqa: E402


# ── Step catalogue ────────────────────────────────────────────────────────────
#: Each step as (what it does, how it does it). Two halves rather than one
#: padded string because they have two different readers: the CLI prints them
#: in an aligned column, and the GUI needs them apart — it has no column to
#: align to, and truncating the single string cut "pio run -d node… -t upload"
#: down to something that no longer said which project it built.
STEP_DETAIL: dict[int, tuple[str, str]] = {
    1:  ("Build web assets",      "www/ → data/www/"),
    2:  ("Flash bootloader",      "rollback-enabled, via esptool"),
    3:  ("Erase chip flash",      "full wipe"),
    4:  ("Clean build artifacts", "pio run -t clean"),
    5:  ("Compile firmware",      "pio run"),
    6:  ("Flash firmware",        "pio run -t upload"),
    7:  ("Upload LittleFS",       "pio run -t uploadfs"),
    8:  ("Upload web via HTTP",   "POST /upload to device IP"),
    9:  ("Open serial monitor",   "pio device monitor"),
    10: ("Erase node flash",      "pio run -d node… -t erase"),
    11: ("Compile node firmware", "pio run -d node…"),
    12: ("Flash node firmware",   "pio run -d node… -t upload"),
}

#: The catalogue as the CLI prints it: title padded to one column, then detail.
_STEP_COL = max(len(title) for title, _ in STEP_DETAIL.values())
STEP_NAMES: dict[int, str] = {
    n: f"{title:<{_STEP_COL}} {detail}"
    for n, (title, detail) in STEP_DETAIL.items()
}


def step_parts(step: int) -> tuple[str, str]:
    """A step's name split into what it does and how, e.g.
    ``("Build web assets", "www/ → data/www/")``.
    """
    return STEP_DETAIL.get(step, ("", ""))


PRESETS: dict[str, tuple[str, list[int]]] = {
    "F": ("Full flash",    [1, 3, 5, 6, 7]),
    "C": ("Clean build",   [4, 5, 6]),
    "Q": ("Quick flash",   [5, 6]),
    "H": ("HTTP deploy",   [1, 8]),
    # The node is its own board on its own USB device, so it gets its own
    # preset rather than joining "All steps" — running both in one pass would
    # flash whichever happens to be plugged in twice.
    "D": ("Node flash",    [11, 12]),
    "A": ("All steps",     list(range(1, 10))),
    "N": ("None",          []),
}

#: What each preset is FOR, in the words of somebody who has not memorised the
#: step numbers. "Full flash — steps 1, 3, 5, 6, 7" tells you nothing you did
#: not already know from the name; the question a person actually has in front
#: of this menu is which of these seven they want, and the step list answers it
#: only if you already know the catalogue by heart.
PRESET_BLURBS: dict[str, str] = {
    "F": "New or misbehaving board: wipe it, then build and flash everything.",
    "C": "Compile from scratch after a toolchain or library change, then flash.",
    "Q": "The everyday one — compile the firmware and flash it.",
    "H": "Web UI only, over WiFi. No USB cable, no reflash.",
    "D": "Build and flash the satellite node board on its own port.",
    "A": "Every collector step in order, monitor included.",
    "N": "Untick everything and start from a clean sheet.",
}

# ── Config defaults + persistence ─────────────────────────────────────────────
#
# `None` means "take it from the environment" — see load_cfg(). Anything the
# project has already stated in platformio.ini or a board definition is not
# copied here: a second copy of the upload speed is a second copy that drifts,
# and this file used to hold three of them (chip, baud, USB CDC state).
DEFAULT_CFG: dict[str, Any] = {
    "env":                  None,
    "port":                 None,   # None = auto-detect, board USB IDs first
    "chip":                 None,   # always derived from env; never pinnable
    "baud":                 None,   # None = the env's upload_speed
    "monitor_speed":        None,   # None = the env's monitor_speed
    "usb_cdc_on_boot":      None,   # None = whatever the env's flag says now
    "device_ip":            "192.168.4.1",
    "steps":                [1, 3, 5, 6, 7],
    "upload_filter":        "all",
    "wipe_before_upload":   False,

    # Interface scale for the GUI, 1.0 = the default type size. Kept with the
    # rest of the settings rather than in a file of its own: it is a per-person
    # accessibility setting, and a second config file is a second thing to find
    # when it is wrong. The CLI ignores it.
    "ui_scale":             1.0,
    "ui_theme":             "Dark",   # Dark | Light | System, GUI only
    "steps_panel_open":     False,    # GUI only: is the step list unfolded

    # Optional compile-time features, by macro name. Read from src/setup.h by
    # tools/features.py rather than listed here, so adding one to the firmware
    # is enough to make every tool offer it.
    #
    # These are applied through PLATFORMIO_BUILD_FLAGS and touch no file. The
    # USB CDC toggle above has to rewrite platformio.ini because the flag it
    # changes lives there, and the comments on _configure_usb_cdc() are a
    # catalogue of what goes wrong when a tool edits the project's own source.
    # Nothing here does, so an abandoned deploy leaves the checkout untouched.
    "features":             [],

    # The 16-byte key shared with an ESP-NOW battery node. Only meaningful with
    # FEATURE_ESPNOW_INGEST, and the same value has to be flashed into the node
    # or nothing pairs and nothing decrypts. Generate one with
    # generate_espnow_key(); it is carried into whichever target is built, so
    # collector and node agree without anybody retyping it.
    "espnow_lmk":           "",

    # Which satellite project the node steps build. Its own env and port,
    # because a node is a different board on a different USB device and
    # reusing the collector's would flash the wrong one.
    "node_project":         "node_espnow",
    "node_env":             None,   # None = the project's own default
    "node_port":            None,   # None = auto-detect
}


# ── The satellite projects ────────────────────────────────────────────────────
#
# Each is a self-contained PlatformIO project beside the collector's, built
# with `pio run -d <dir>`. Listed here rather than discovered, because there
# are two of them and they differ in ways a scan could not infer: only one
# shares the ESP-NOW key, and their default envs come from different chips.
class NodeProject:
    def __init__(self, key: str, directory: str, label: str,
                 default_env: str, wants_key: bool, blurb: str):
        self.key = key
        self.directory = directory
        self.label = label
        self.default_env = default_env
        self.wants_key = wants_key
        self.blurb = blurb


NODE_PROJECTS: dict[str, NodeProject] = {
    "node_espnow": NodeProject(
        "node_espnow", "node_espnow", "ESP-NOW battery node",
        "xiao_esp32c3", True,
        "Deep-sleeping ESP32-C3. Shares the 16-byte key with the collector."),
    "node": NodeProject(
        "node", "node", "ESP8266 WiFi node",
        "nodemcuv2", False,
        "Mains-powered ESP8266. Pushes readings to POST /api/ingest."),
}


def node_project(cfg: dict[str, Any]) -> NodeProject:
    """The satellite project the node steps act on."""
    return NODE_PROJECTS.get(cfg.get("node_project") or "",
                             NODE_PROJECTS["node_espnow"])


#: The alphabet a generated ESP-NOW key is drawn from.
#:
#: Letters and digits only, and that is a decision rather than laziness. The
#: key reaches the compiler as -DESPNOW_LMK=\"...\" through a shell and a
#: PlatformIO ini parser, so a quote, a backslash, a dollar or a semicolon in
#: it is a broken build at best. Ambiguous glyphs are dropped too — somebody
#: reads this off one screen and types it into another, and 0/O and 1/l/I is
#: where that goes wrong.
_KEY_ALPHABET = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def generate_espnow_key(length: int = 16) -> str:
    """A fresh shared key, from the system's cryptographic RNG.

    `secrets`, not `random`: this is the only thing standing between the
    collector's pipeline and any ESP-NOW frame in radio range, and
    `random.choice` is seeded predictably enough to enumerate.

    Exactly 16 characters because both firmwares static_assert on it — see
    ESPNOW_LMK in node_config.h and in the collector's setup.h.
    """
    import secrets
    return "".join(secrets.choice(_KEY_ALPHABET) for _ in range(length))

# Keys load_cfg() re-derives from the environment when they are None. `chip` is
# absent on purpose: it is re-derived unconditionally, because an env and a
# chip that disagree is how a C3 bootloader gets written to an S3.
_ENV_DERIVED = ("baud", "monitor_speed")

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


def detect_port(env: str | None = None) -> str:
    """Auto-detect the serial port, preferring one that matches the board.

    With `env` given, ports whose USB VID:PID matches that board's `hwids` are
    tried first. It matters as soon as more than one device is plugged in: a
    collector and an ESP8266 node both show up as /dev/ttyUSB*, and "the first
    one" is a coin toss that hands the firmware to whichever enumerated first.

    Falls back to the old glob when pyserial is missing or nothing matches.
    """
    if env:
        ranked = ports_for(env)
        for device, _desc, matched in ranked:
            if matched:
                return device
        if ranked:
            return ranked[0][0]

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
    """Load configuration, taking everything derivable from the environment.

    The rule: `null` in .flash_tool.json means "follow platformio.ini". A value
    means the user pinned it deliberately. So the ordinary config is nearly
    empty and a board switch carries its own settings with it — which is the
    point, because the alternative was three numbers remembered here that could
    each disagree with the project.
    """
    cfg: dict[str, Any] = dict(DEFAULT_CFG)
    if CFG_FILE.is_file():
        try:
            cfg.update(json.loads(CFG_FILE.read_text()))
        except Exception:
            pass
    if not cfg.get("env"):
        cfg["env"] = detect_env()

    env_defaults = defaults_for(cfg["env"])

    # The chip follows the env, always. It used to be a free-text field kept
    # next to it, so a saved config could name esp32s3 as the environment and
    # esp32c3 as the chip — and step 2 would then write a C3 bootloader to an
    # S3. There is no board for which the two legitimately disagree, so it is
    # derived rather than remembered.
    cfg["chip"] = env_defaults["chip"]

    for key in _ENV_DERIVED:
        if cfg.get(key) is None:
            cfg[key] = env_defaults[key]

    # USB CDC is a line in platformio.ini, not a preference of this tool. Read
    # the current one rather than a remembered wish: a checkbox that says ON
    # while the env's flag says 0 is describing a build that will not happen.
    # A toggle sets this, the next compile writes it back to the ini, and the
    # two agree again.
    cfg["usb_cdc_on_boot"] = env_defaults["usb_cdc_on_boot"]
    if cfg["usb_cdc_on_boot"] is None:
        cfg["usb_cdc_on_boot"] = True   # env inherits its flag; nothing to show

    if not cfg.get("port"):
        cfg["port"] = detect_port(cfg["env"])
    return cfg


def env_defaults_note(cfg: dict[str, Any]) -> list[str]:
    """One line per derived setting, saying where its value came from.

    Written for the tools to print: a number with no provenance is a number
    someone will eventually override for no reason.
    """
    d = defaults_for(cfg.get("env") or detect_env())
    pinned = _pinned_keys()
    lines = [
        f"board       {d['board']}  ({d['chip']}, {d['flash_size'] or 'flash ?'})",
        f"partitions  {d['partitions'] or 'platform default'}"
        f"   filesystem {d['filesystem'] or '-'}",
    ]
    for key, label, src in (("baud", "upload baud", d["baud_src"]),
                            ("monitor_speed", "monitor baud", d["monitor_src"])):
        if key in pinned:
            lines.append(f"{label:<11} {cfg[key]}  (pinned here; env says {d[key]})")
        else:
            lines.append(f"{label:<11} {cfg[key]}  (from {src})")
    if d["usb_cdc_on_boot"] is None:
        lines.append(f"USB CDC     inherited by [env:{d['env']}]; "
                     f"toggle it in the env it extends")
    else:
        state = "on" if d["usb_cdc_on_boot"] else "off"
        lines.append(f"USB CDC     {state}  (from [env:{d['env']}] build_flags; "
                     f"{d['usb_pins'] or 'no native USB'})")

    feats = [m for m in (cfg.get("features") or []) if is_known(m)]
    if feats:
        lines.append(f"features    {', '.join(feats)}")
        if "FEATURE_ESPNOW_INGEST" in feats:
            lmk = cfg.get("espnow_lmk") or ""
            if not lmk:
                lines.append("ESP-NOW key not set  (build uses the default in "
                             "setup.h — change it before it leaves the bench)")
            elif len(lmk) != 16:
                lines.append(f"ESP-NOW key {len(lmk)} chars  (must be exactly 16)")
            else:
                lines.append("ESP-NOW key set  (flash the SAME 16 bytes into the node)")
    else:
        lines.append("features    none  (setup.h defaults only)")
    return lines


def pinned_keys(cfg: dict[str, Any]) -> set:
    """Derived keys whose LIVE value differs from what the environment says.

    Takes the config in hand rather than re-reading .flash_tool.json. The
    on-disk version answers a different question — what was pinned when the
    file was last written — so a baud pinned earlier in the same session was
    invisible: the menu labelled it as coming from the env, and the next board
    switch overwrote it.

    Same rule save_cfg() uses, so what the menu marks as pinned and what
    survives a switch cannot disagree.
    """
    try:
        d = defaults_for(cfg.get("env") or detect_env())
    except Exception:
        return set()
    return {k for k in _ENV_DERIVED
            if cfg.get(k) is not None and cfg.get(k) != d.get(k)}


def _pinned_keys() -> set:                       # backwards-compatible shim
    """Deprecated: reads the file, so it cannot see the current session."""
    return pinned_keys(load_cfg())


def adopt_env_defaults(cfg: dict[str, Any], pinned: set | None = None) -> None:
    """Re-derive everything the environment states, in place.

    Called when the environment changes. Values in `pinned` are left alone;
    pass the set from BEFORE the switch, since pinned_keys() compares against
    whichever env is current.
    """
    pinned = set() if pinned is None else pinned
    d = defaults_for(cfg["env"])
    cfg["chip"] = d["chip"]
    for key in _ENV_DERIVED:
        if key not in pinned:
            cfg[key] = d[key]
    cfg["usb_cdc_on_boot"] = d["usb_cdc_on_boot"] \
        if d["usb_cdc_on_boot"] is not None else True


def save_cfg(cfg: dict[str, Any]) -> None:
    """Save configuration, writing back only what the environment does not say.

    Without this, saving would defeat the whole arrangement: load_cfg() fills
    `baud` in from the env, and a naive round-trip would write that value back
    as though the user had chosen it — pinning it, so the next board switch
    would silently keep the old board's speed.

    A derived key is written only when it DIFFERS from what the env says. The
    chip and the USB CDC state are never written: both are read from the
    project every time, and a stale copy of either is how the wrong bootloader
    gets flashed or a checkbox describes a build that will not happen.
    """
    out = dict(cfg)
    try:
        env_defaults = defaults_for(cfg.get("env") or detect_env())
    except Exception:
        env_defaults = {}

    for key in _ENV_DERIVED:
        if key in env_defaults and out.get(key) == env_defaults[key]:
            out[key] = None
    out.pop("chip", None)
    out.pop("usb_cdc_on_boot", None)

    # The port gets the same treatment for the same reason. load_cfg() fills
    # in whatever auto-detection found, and writing that back pinned it — so
    # the very first save froze one device path into the config and the new
    # VID:PID ranking never ran again. Store it only when the user picked
    # something detection would not have.
    try:
        if not out.get("port") or out["port"] == detect_port(out.get("env")):
            out["port"] = None      # "" and "what detection finds" both mean auto
    except Exception:
        pass

    try:
        CFG_FILE.write_text(json.dumps(out, indent=2) + "\n")
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

        # Steps the user was asked about and declined. HERE AND NOT IN
        # run_steps(), because the step methods are public and independently
        # callable — s2_flash_bootloader(lambda: False) on a fresh manager
        # reached `self._skipped.append(2)` and raised AttributeError instead
        # of skipping the step, which is a worse answer to "no" than either
        # running it or not.
        self._skipped: list[int] = []

    @property
    def skipped(self) -> list[int]:
        """The steps of the last run that were offered and declined.

        A front end has to be able to ask. run_steps() returning True means
        "nothing failed", which is not the same as "everything ran", and the
        difference is a bootloader that was never written under a green box
        saying it was.
        """
        return sorted(self._skipped)

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

    def _pio_env(self) -> dict[str, str] | None:
        """os.environ plus PLATFORMIO_BUILD_FLAGS, or None when nothing is set.

        MUST be passed to EVERY `pio run`, not only the compile step. `pio run
        -t upload` relinks before it flashes, so an upload that did not carry
        the same flags would quietly rebuild the firmware WITHOUT the selected
        features and flash that instead — a board that boots fine and is
        missing exactly what the user asked for, with a successful compile step
        scrolled off the screen above it.
        """
        wanted = [m for m in (self.cfg.get("features") or []) if is_known(m)]
        extra: dict[str, str] = {}
        if "FEATURE_ESPNOW_INGEST" in wanted and self.cfg.get("espnow_lmk"):
            extra["ESPNOW_LMK"] = self.cfg["espnow_lmk"]

        flags = build_flags_for(wanted, extra)
        if not flags:
            return None

        env = dict(os.environ)
        # Appended, not replaced: a developer who exported the variable for a
        # reason keeps it, and PlatformIO concatenates the lot.
        existing = env.get("PLATFORMIO_BUILD_FLAGS", "").strip()
        env["PLATFORMIO_BUILD_FLAGS"] = f"{existing} {flags}".strip()
        return env

    def _log_build_flags(self) -> None:
        """Say what the build will carry, before it carries it."""
        wanted = [m for m in (self.cfg.get("features") or []) if is_known(m)]
        unknown = [m for m in (self.cfg.get("features") or []) if not is_known(m)]

        if unknown:
            # A feature that used to exist and no longer does. Dropping it
            # silently would leave the config claiming a build it is not
            # getting, which is the same class of lie the USB CDC checkbox
            # used to tell.
            self._log(f"  ! ignoring unknown feature(s): {', '.join(unknown)} "
                      f"— not declared in src/setup.h any more")
        if not wanted:
            self._log("  features: none (setup.h defaults only)")
            return

        self._log(f"  features: {', '.join(wanted)}")
        if "FEATURE_ESPNOW_INGEST" in wanted:
            lmk = self.cfg.get("espnow_lmk") or ""
            if not lmk:
                self._log("  ! ESP-NOW key not set here; the build falls back to "
                          "the default in setup.h. Set it, and flash the SAME "
                          "16 bytes into the node, or nothing will pair.")
            elif len(lmk) != 16:
                self._log(f"  ! ESP-NOW key is {len(lmk)} characters; it must be "
                          f"exactly 16. The build will refuse it.")

    def _run_cmd(self, cmd: list[str], env: dict[str, str] | None = None,
                 interactive: bool = False) -> int:
        """Run a subprocess command and stream output to callback.

        stdin is closed unless `interactive`. A step that asks a question
        nobody can answer is worse than one that never asks: run from the
        GUI, the bootloader step inherited a stdin that did not exist and
        died on an EOFError traceback. The steps that need an answer get it
        from the front end now, and pass it on the command line. Only the
        serial monitor genuinely wants a keyboard.
        """
        self._log(f"$ {shlex.join(cmd)}")
        try:
            process = subprocess.Popen(
                cmd,
                # `interactive` asks for a keyboard; a console is what
                # supplies one. Without a console — the frozen windowed GUI —
                # inheriting stdin hands the child an invalid handle, which is
                # the same "prompting on a stdin nobody has" this commit
                # removed from step 2. DEVNULL is the honest answer there.
                stdin=None if (interactive and _has_console())
                      else subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=env,
                cwd=str(ROOT),
                **_no_window(inherits_console=interactive),
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
            self._emit_complete(1, 2)
            return 2
        rc = self._run_cmd([_python(), str(script), "--dst", str(DATA_WWW)])
        if rc == 0:
            self._log("✓ Web assets built.")
        self._emit_complete(1, rc)
        return rc

    def s2_flash_bootloader(
            self, confirm_callback: Optional[Callable[[], bool]] = None) -> int:
        self._emit_start(2, STEP_NAMES[2])
        script = TOOLS / "flash_bootloader.py"
        if not script.is_file():
            self._log("ERROR: flash_bootloader.py not found in tools/")
            # Paired with the _emit_start above. Every exit from a step has to
            # emit a completion or the GUI's progress counter, which counts
            # them, stays a step behind for the rest of the run.
            self._emit_complete(2, 2)
            return 2

        # The question is asked HERE, by whoever has a user to ask — the CLI
        # on its terminal, the GUI in its status bar — and the answer is
        # passed to the script as --yes. flash_bootloader.py used to ask it
        # itself, on a stdin the windowed GUI does not have, and the step
        # ended on "EOFError: EOF when reading a line" every time.
        self._log("*** Overwrites the existing bootloader on the device. ***")
        if confirm_callback and not confirm_callback():
            self._log("Skipped.")
            self._skipped.append(2)
            self._emit_complete(2, 0)
            return 0

        # Pass the ENV name, not the chip family. flash_bootloader resolves the
        # chip from it and — crucially — the flash size, which the chip alone
        # cannot give: esp32s3 and esp32s3_n16r8 are the same silicon with 8
        # and 16 MB behind it, and the size lands in the bootloader image
        # header.
        cmd = [_python(), str(script), "--chip", self.cfg["env"],
               "--baud", str(self.cfg["baud"]), "--yes"]
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
                self._skipped.append(3)
                self._emit_complete(3, 0)
                return 0

        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            self._emit_complete(3, 1)
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
            self._emit_complete(4, 1)
            return 1
        rc = self._run_cmd([self.pio, "run", "-t", "clean", "-e", self.cfg["env"]],
                           env=self._pio_env())
        if rc == 0:
            self._log("✓ Build artifacts cleaned.")
        self._emit_complete(4, rc)
        return rc

    def s5_compile(self) -> int:
        self._emit_start(5, STEP_NAMES[5])
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            self._emit_complete(5, 1)
            return 1
        # Configure USB CDC flag before compilation
        self._configure_usb_cdc()
        self._log_build_flags()
        rc = self._run_cmd([self.pio, "run", "-e", self.cfg["env"]],
                           env=self._pio_env())
        if rc == 0:
            self._log("✓ Firmware compiled.")
        self._emit_complete(5, rc)
        return rc

    def s6_flash_fw(self) -> int:
        self._emit_start(6, STEP_NAMES[6])
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            self._emit_complete(6, 1)
            return 1
        cmd = [self.pio, "run", "-t", "upload", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--upload-port", self.cfg["port"]]
        # Same flags as the compile step. `pio run -t upload` relinks first, so
        # flashing without them would rebuild the firmware WITHOUT the selected
        # features and flash that — a board that boots fine and is missing
        # exactly what was asked for.
        rc = self._run_cmd(cmd, env=self._pio_env())
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
                self._emit_complete(7, rc)
                return rc

        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            self._emit_complete(7, 1)
            return 1

        cmd = [self.pio, "run", "-t", "uploadfs", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--upload-port", self.cfg["port"]]
        rc = self._run_cmd(cmd, env=self._pio_env())
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
            self._emit_complete(9, 1)
            return 1
        cmd = [self.pio, "device", "monitor", "-e", self.cfg["env"]]
        if self.cfg.get("port"):
            cmd += ["--port", self.cfg["port"]]
        # The one step with a keyboard: miniterm's own keys (Ctrl-C to quit,
        # Ctrl-T for its menu) need a stdin, and a console to read it from.
        return self._run_cmd(cmd, interactive=True)

    # ── The satellite boards ─────────────────────────────────────────────────
    #
    # A node is a separate PlatformIO project (`pio run -d node_espnow`), a
    # separate board, and a separate USB device. It therefore gets its own env
    # and its own port rather than borrowing the collector's — sharing them was
    # the shortest path to flashing an ESP8266 image at an ESP32-C3.
    def _node_env(self) -> dict[str, str] | None:
        """os.environ plus the node's build flags, or None when it needs none.

        The ESP-NOW key is the whole point. The collector and the node have to
        hold the same 16 bytes or nothing pairs and nothing decrypts, and the
        two are built minutes apart from the same config — so it is passed
        here rather than left for somebody to copy into node_config.h by hand
        and mistype once.
        """
        proj = node_project(self.cfg)
        lmk = (self.cfg.get("espnow_lmk") or "").strip()
        if not (proj.wants_key and lmk):
            return None

        env = dict(os.environ)
        existing = env.get("PLATFORMIO_BUILD_FLAGS", "").strip()
        flag = f'-DESPNOW_LMK=\\"{lmk}\\"'
        env["PLATFORMIO_BUILD_FLAGS"] = f"{existing} {flag}".strip()
        return env

    def _node_target(self) -> tuple[Path, str] | None:
        """Which directory and env the node steps act on, or None if unbuildable.

        Split from _node_cmd() because it needs no PlatformIO to answer, and
        the question "would this flash the right project" is worth being able
        to ask on a machine that has no toolchain installed.
        """
        proj = node_project(self.cfg)
        directory = ROOT / proj.directory
        if not (directory / "platformio.ini").is_file():
            self._log(f"ERROR: no PlatformIO project at {directory}.")
            return None
        return directory, (self.cfg.get("node_env") or proj.default_env)

    def _node_cmd(self, *extra: str) -> list[str] | None:
        # None, not a list beginning with None. Returning [None, "run", …]
        # produced a command that looked fine until something joined it, and
        # the traceback named the join rather than the missing toolchain.
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return None
        target = self._node_target()
        if target is None:
            return None
        directory, env_name = target
        return [self.pio, "run", "-d", str(directory), "-e", env_name, *extra]

    def _node_preamble(self) -> bool:
        if self.pio is None:
            self._log("ERROR: PlatformIO CLI (pio) not found in PATH.")
            return False
        proj = node_project(self.cfg)
        self._log(f"Node project: {proj.label}  ({proj.directory}/)")
        if proj.wants_key:
            lmk = (self.cfg.get("espnow_lmk") or "").strip()
            if not lmk:
                # Not an error: node_config.h has a placeholder and the build
                # succeeds. Said loudly because a node flashed with the
                # placeholder pairs with nothing and gives no clue why.
                self._log("WARNING: no ESP-NOW key set — the node will carry the "
                          "placeholder from node_config.h and will not pair with "
                          "a collector built with a real one.")
            elif len(lmk) != 16:
                self._log(f"ERROR: the ESP-NOW key is {len(lmk)} characters; "
                          f"both firmwares static_assert on exactly 16.")
                return False
            else:
                self._log("ESP-NOW key: the same 16 bytes this tool gives the "
                          "collector.")
        return True

    def s10_erase_node(self, confirm_callback: Optional[Callable[[], bool]] = None) -> int:
        self._emit_start(10, STEP_NAMES[10])
        self._log("*** WARNING: Wipes all node flash (config, logs, FS). ***")

        if confirm_callback:
            if not confirm_callback():
                self._log("Skipped.")
                self._skipped.append(10)
                self._emit_complete(10, 0)
                return 0

        if not self._node_preamble():
            self._emit_complete(10, 1)
            return 1

        cmd = self._node_cmd("-t", "erase")
        if cmd is not None and self.cfg.get("node_port"):
            cmd += ["--upload-port", self.cfg["node_port"]]

        rc = 1 if cmd is None else self._run_cmd(cmd, env=self._node_env())
        if rc == 0:
            self._log("✓ Node flash erased.")
        self._emit_complete(10, rc)
        return rc

    def s11_compile_node(self) -> int:
        self._emit_start(11, STEP_NAMES[11])
        if not self._node_preamble():
            self._emit_complete(11, 1)
            return 1
        cmd = self._node_cmd()
        rc = 1 if cmd is None else self._run_cmd(cmd, env=self._node_env())
        if rc == 0:
            self._log("✓ Node firmware compiled.")
        self._emit_complete(11, rc)
        return rc

    def s12_flash_node(self) -> int:
        self._emit_start(12, STEP_NAMES[12])
        if not self._node_preamble():
            self._emit_complete(12, 1)
            return 1
        cmd = self._node_cmd("-t", "upload")
        if cmd is not None and self.cfg.get("node_port"):
            cmd += ["--upload-port", self.cfg["node_port"]]
        # The same env as the compile step, and for the same reason it matters
        # on the collector: `pio run -t upload` relinks before it flashes, so
        # an upload without the key would rebuild the node WITHOUT it and
        # flash that — a board that boots, sweeps every channel, and pairs
        # with nothing.
        rc = 1 if cmd is None else self._run_cmd(cmd, env=self._node_env())
        if rc == 0:
            self._log("✓ Node firmware flashed.")
        self._emit_complete(12, rc)
        return rc

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
        port = self.cfg.get("port") or detect_port(self.cfg.get("env"))
        if not port:
            self._log("Error: No serial port found. Connect device and set port via settings.")
            return False

        # The monitor speed is resolved for THIS env (its own value, then what
        # it extends, then [env]). It used to be the first `monitor_speed`
        # anywhere in platformio.ini, which is the right number here only
        # because no env overrides the shared one — the first env to do so
        # would have had the provisioner talking at the wrong rate.
        baud = self.cfg.get("monitor_speed") \
            or defaults_for(self.cfg.get("env") or detect_env())["monitor_speed"]

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

    def run_steps(self, steps: list[int],
                  confirm_erase_callback: Optional[Callable[[], bool]] = None,
                  confirm_bootloader_callback: Optional[Callable[[], bool]] = None) -> bool:
        """Run selected steps. Returns True if all succeeded.

        The confirm callbacks are how a front end asks its user about the two
        destructive steps. A caller that passes none has no one to ask and
        gets the steps it selected.
        """
        steps = sorted(steps)
        if not steps:
            self._log("No steps selected.")
            return False

        dispatch = {
            1: self.s1_build_web,
            2: lambda: self.s2_flash_bootloader(confirm_bootloader_callback),
            3: lambda: self.s3_erase(confirm_erase_callback),
            4: self.s4_clean,
            5: self.s5_compile,
            6: self.s6_flash_fw,
            7: self.s7_upload_fs,
            8: self.s8_upload_http,
            9: self.s9_monitor,
           10: lambda: self.s10_erase_node(confirm_erase_callback),
           11: self.s11_compile_node,
           12: self.s12_flash_node,
        }

        failed: list[int] = []
        # Declining a destructive step is not a failure — but it is not the
        # step having run, either, and run_steps() returns one bool for both.
        # So the list is cleared here and read back through `skipped` by
        # whoever draws the banner: deploy.py and deploy_gui.py both used to
        # print "All steps completed successfully" for a deploy whose
        # bootloader was never written, because True was all they were told.
        self._skipped = []
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
            if self._skipped:
                sk = ", ".join(str(s) for s in sorted(self._skipped))
                self._log(f"✓ Completed — but step(s) {sk} were DECLINED and "
                          f"did not run.")
            else:
                self._log("✓ All steps completed successfully.")
            return True

        fs = ", ".join(str(s) for s in failed)
        self._log(f"✗ Completed with errors on step(s): {fs}")
        return False
