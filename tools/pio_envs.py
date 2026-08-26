#!/usr/bin/env python3
"""
pio_envs.py — one place that knows what boards this project builds for.

Before this file, the answer was hardcoded in four: deploy.py's chip prompt,
deploy_gui.py's dropdowns, deploy_core.py's `supported_envs` set, and
flash_clean.py's fallback. Each listed a different subset, and all four had
gone stale — xiao_esp32s3 and esp32s3_n16r8 had been buildable for weeks
while the deploy tools would not offer them, and the USB CDC toggle silently
did nothing for anyone who typed the name in by hand.

So the answer is derived from platformio.ini itself. Add an `[env:…]` there
and every tool picks it up; there is nothing else to remember.

    from pio_envs import environments, env_info, default_env

    for e in environments():          # deployable envs, ini order
        print(e.name, e.chip, e.flash_size, e.supports_usb_cdc)

Run it directly to see what the tools will show:

    python3 tools/pio_envs.py
"""
from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INI = ROOT / "platformio.ini"

# Frozen by PyInstaller, __file__ lives in an extraction directory that has no
# platformio.ini above it. The deploy tools are run from the project root in
# that case, so fall back to the working directory rather than reporting no
# boards at all.
if not INI.is_file() and (Path.cwd() / "platformio.ini").is_file():
    ROOT = Path.cwd()
    INI = ROOT / "platformio.ini"

# Envs that build but must never be offered as a flash target.
# The value is shown to the user when something asks for one by name.
_NOT_DEPLOYABLE = {
    "chaos_simulator":
        "resilience-test build: it deliberately drops WiFi, starves mutexes "
        "and holds heap. Never flash it to a real device.",
}

# Fallback when a board's JSON cannot be found (no PlatformIO install, or a
# board from a platform that is not downloaded yet). Board ids are consistent
# enough about naming the part that this is a reasonable guess, and every
# caller treats the chip as a hint rather than as authority.
_CHIP_HINTS = (("c3", "esp32c3"), ("s2", "esp32s2"), ("s3", "esp32s3"),
               ("c6", "esp32c6"), ("h2", "esp32h2"), ("8266", "esp8266"))


@dataclass(frozen=True)
class EnvInfo:
    name: str
    board: str
    chip: str               # esptool chip family, e.g. "esp32c3"
    flash_size: str         # e.g. "4MB"; "" when unknown
    partitions: str         # partition CSV named by the env, or ""
    supports_usb_cdc: bool  # env carries a -DARDUINO_USB_CDC_ON_BOOT flag
    deployable: bool
    note: str = ""          # why not deployable, when it is not

    # Settings the project has already stated for this board, so the deploy
    # tools can adopt them instead of keeping a second copy that drifts. Each
    # resolves env -> extends chain -> [env] -> the board JSON -> a last
    # resort; `source` records which, because "921600, and here is where that
    # came from" is the difference between a default and a mystery.
    upload_speed: int = 0
    upload_speed_src: str = ""
    monitor_speed: int = 0
    monitor_speed_src: str = ""
    upload_protocol: str = ""
    upload_port: str = ""        # a port pinned by the ini, usually empty
    monitor_port: str = ""
    filesystem: str = ""         # littlefs / spiffs
    # (vid, pid) pairs the board is expected to enumerate as. Used to pick the
    # right serial port when several are plugged in.
    hwids: tuple = ()
    # The -DARDUINO_USB_CDC_ON_BOOT currently written in the env's own
    # build_flags: True, False, or None when the env does not carry one. This
    # is the state the NEXT build will have, which is the only honest thing to
    # show in a checkbox.
    usb_cdc_on_boot: object = None

    @property
    def label(self) -> str:
        bits = [self.name]
        if self.chip:
            bits.append(self.chip)
        if self.flash_size:
            bits.append(self.flash_size)
        return f"{bits[0]}  ({', '.join(bits[1:])})" if len(bits) > 1 else bits[0]


# --------------------------------------------------------------------------
# platformio.ini parsing
#
# configparser is not used: platformio.ini allows duplicate keys inside a
# section (several `build_flags` lines is idiomatic) and inline `;` comments,
# and it is read here only for a handful of keys. A 40-line reader that
# cannot raise on someone's valid ini is the better trade.
# --------------------------------------------------------------------------
def _sections() -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    if not INI.is_file():
        return out
    cur: dict[str, str] | None = None
    key: str | None = None
    for raw in INI.read_text(encoding="utf-8").splitlines():
        line = raw.split(";", 1)[0].rstrip() if not raw.lstrip().startswith(";") else ""
        if not line.strip():
            continue
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            cur = out.setdefault(m.group(1).strip(), {})
            key = None
            continue
        if cur is None:
            continue
        if line[0] in " \t" and key:          # continuation of a multi-line value
            cur[key] += "\n" + line.strip()
            continue
        if "=" in line:
            key, val = line.split("=", 1)
            key = key.strip()
            cur[key] = val.strip()
    return out


def _resolve(sections: dict[str, dict[str, str]], name: str, key: str,
             _seen: frozenset[str] = frozenset()) -> tuple[str, str]:
    """Value of `key` for [env:name], and where it came from.

    PlatformIO's own order: the env's own value, then whatever it `extends`,
    then the shared [env] section. That last fallback matters — `monitor_speed`
    lives in [env] here and in no individual env, so a resolver that stopped at
    the extends chain would report every board as having no monitor speed.

    Returns ("", "") when nothing defines it.
    """
    if name in _seen:                          # a cycle in extends
        return "", ""
    sec = sections.get(f"env:{name}", {})
    if key in sec:
        return sec[key], f"[env:{name}]"
    parent = sec.get("extends", "")
    if parent.startswith("env:"):
        val, src = _resolve(sections, parent[4:], key, _seen | {name})
        if val:
            return val, src
    if key in sections.get("env", {}):
        return sections["env"][key], "[env]"
    return "", ""


def _value(sections: dict[str, dict[str, str]], name: str, key: str) -> str:
    return _resolve(sections, name, key)[0]


def _build_flags(sections: dict[str, dict[str, str]], name: str) -> str:
    """All build_flags reaching this env, including through extends and
    ${env:x.build_flags} interpolation. Good enough to answer yes/no
    questions about a flag; not a substitute for what SCons finally emits."""
    text = _value(sections, name, "build_flags")
    for ref in re.findall(r"\$\{env:([^.}]+)\.build_flags\}", text):
        if ref != name:
            text += "\n" + _build_flags(sections, ref)
    return text


def _int(text: str) -> int:
    m = re.match(r"\s*(\d+)", text or "")
    return int(m.group(1)) if m else 0


def _own_cdc_flag(sections: dict[str, dict[str, str]], name: str):
    """The -DARDUINO_USB_CDC_ON_BOOT written in this env's OWN build_flags.

    Deliberately not the inherited one: this is what the in-place toggle can
    change, so it is what a checkbox may claim to control. Comment lines are
    skipped — platformio.ini documents the flag in prose next to several
    boards, and reading those as settings would report the opposite value.
    """
    block = sections.get(f"env:{name}", {}).get("build_flags", "")
    for line in block.split("\n"):
        if line.lstrip().startswith((";", "#")):
            continue
        m = re.search(r"-DARDUINO_USB_CDC_ON_BOOT=([01])", line)
        if m:
            return m.group(1) == "1"
    return None


def _board_json(board: str) -> dict:
    """Board definition, project boards/ first — that is where PlatformIO
    looks first too, so a project-local override wins here as well."""
    candidates = [ROOT / "boards" / f"{board}.json"]
    pio_home = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    candidates += sorted(pio_home.glob(f"platforms/*/boards/{board}.json"))
    for path in candidates:
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
    return {}


def _chip_hint(board: str) -> str:
    low = board.lower()
    for needle, chip in _CHIP_HINTS:
        if needle in low:
            return chip
    return "esp32"


# --------------------------------------------------------------------------
# Public API
# --------------------------------------------------------------------------
def env_info(name: str) -> EnvInfo:
    sections = _sections()
    return _info(sections, name)


def _info(sections: dict[str, dict[str, str]], name: str) -> EnvInfo:
    board = _value(sections, name, "board")
    js = _board_json(board)
    upload_js = js.get("upload", {}) or {}
    chip = (js.get("build", {}) or {}).get("mcu", "") or _chip_hint(board)
    flash = _value(sections, name, "board_upload.flash_size") \
        or upload_js.get("flash_size", "")

    # Upload speed: the ini wins, then the board's own default, then the
    # esptool default. Recording which one answered is the point — the deploy
    # tool can then say "921600 from [env:xiao_esp32c3]" instead of presenting
    # a number with no provenance.
    up, up_src = _resolve(sections, name, "upload_speed")
    if not up and upload_js.get("speed"):
        up, up_src = str(upload_js["speed"]), f"board {board}"
    mon, mon_src = _resolve(sections, name, "monitor_speed")

    hwids = tuple(
        (str(pair[0]).lower(), str(pair[1]).lower())
        for pair in (js.get("build", {}) or {}).get("hwids", []) or []
        if isinstance(pair, (list, tuple)) and len(pair) >= 2
    )

    return EnvInfo(
        name=name,
        board=board,
        chip=chip,
        flash_size=flash,
        partitions=_value(sections, name, "board_build.partitions"),
        # The toggle rewrites an existing flag in the env's own block, so an
        # env that inherits one from a parent is not togglable in place.
        supports_usb_cdc=_own_cdc_flag(sections, name) is not None,
        deployable=name not in _NOT_DEPLOYABLE,
        note=_NOT_DEPLOYABLE.get(name, ""),
        upload_speed=_int(up),
        upload_speed_src=up_src,
        monitor_speed=_int(mon),
        monitor_speed_src=mon_src,
        upload_protocol=_value(sections, name, "upload_protocol"),
        upload_port=_value(sections, name, "upload_port"),
        monitor_port=_value(sections, name, "monitor_port"),
        filesystem=_value(sections, name, "board_build.filesystem"),
        hwids=hwids,
        usb_cdc_on_boot=_own_cdc_flag(sections, name),
    )


def environments(include_all: bool = False) -> list[EnvInfo]:
    """Every [env:NAME] in platformio.ini order. Test-only builds are left
    out unless include_all is set."""
    sections = _sections()
    names = [s[4:] for s in sections if s.startswith("env:")]
    infos = [_info(sections, n) for n in names]
    return infos if include_all else [e for e in infos if e.deployable]


def env_names(include_all: bool = False) -> list[str]:
    return [e.name for e in environments(include_all)]


def default_env() -> str:
    """[platformio] default_envs, else the first deployable env."""
    sections = _sections()
    default = sections.get("platformio", {}).get("default_envs", "")
    first = default.split(",")[0].strip() if default else ""
    names = env_names()
    if first in names:
        return first
    return names[0] if names else "xiao_esp32c3"


def chip_for(env: str) -> str:
    return env_info(env).chip


def usb_pins(chip: str) -> str:
    """The USB Serial/JTAG pair for a chip family, as the firmware reports it
    (src/modules/UsbCdcModule.cpp keys off the same silicon fact). Empty when
    the part has no native USB."""
    return {"esp32c3": "GPIO 18/19", "esp32s2": "GPIO 19/20",
            "esp32s3": "GPIO 19/20", "esp32c6": "GPIO 12/13"}.get(chip, "")


# Fallback upload speed when neither the ini nor the board states one. Every
# board here states one, so this is a floor, not a policy.
DEFAULT_UPLOAD_SPEED = 460800
DEFAULT_MONITOR_SPEED = 115200


def defaults_for(env: str) -> dict:
    """Everything the project has already said about this board, in the shape
    the deploy tools want it.

    The point of this function: those tools used to keep their own copy of the
    baud rate, the chip and the USB CDC state beside platformio.ini, and the
    copies drifted. Anything derivable is derived here, once.
    """
    e = env_info(env)
    return {
        "env":             e.name,
        "board":           e.board,
        "chip":            e.chip,
        "flash_size":      e.flash_size,
        "partitions":      e.partitions,
        "filesystem":      e.filesystem,
        "baud":            e.upload_speed or DEFAULT_UPLOAD_SPEED,
        "baud_src":        e.upload_speed_src or "built-in default",
        "monitor_speed":   e.monitor_speed or DEFAULT_MONITOR_SPEED,
        "monitor_src":     e.monitor_speed_src or "built-in default",
        "upload_protocol": e.upload_protocol,
        "upload_port":     e.upload_port,
        "usb_cdc_on_boot": e.usb_cdc_on_boot,
        "usb_pins":        usb_pins(e.chip),
        "hwids":           e.hwids,
    }


def ports_for(env: str) -> list:
    """Serial ports that look like this board, best match first.

    Ports whose USB VID:PID matches the board definition's `hwids` come first;
    everything else follows in the order the OS lists it. With a collector and
    a node both plugged in, "the first /dev/ttyACM*" is a coin toss, and the
    wrong one gets the firmware.

    Returns [(device, description, matched_hwid)]. Empty if pyserial is absent
    — callers fall back to their own globbing.
    """
    try:
        import serial.tools.list_ports  # type: ignore
    except ImportError:
        return []
    want = set(env_info(env).hwids)
    ranked = []
    for p in serial.tools.list_ports.comports():
        # No USB VID means it is not a USB device: a motherboard's /dev/ttyS0
        # or COM1, which pyserial lists and which no board is ever behind.
        # Returning one of those as "the port" is worse than returning nothing,
        # because the caller stops looking.
        if p.vid is None:
            continue
        vid = f"0x{p.vid:04x}"
        pid = f"0x{p.pid:04x}" if p.pid is not None else ""
        match = bool(want) and (vid, pid) in want
        ranked.append((0 if match else 1, p.device, p.description or "", match))
    ranked.sort(key=lambda r: (r[0], r[1]))
    return [(dev, desc, match) for _, dev, desc, match in ranked]


if __name__ == "__main__":
    print(f"platformio.ini: {INI}")
    print(f"default env:    {default_env()}\n")
    for e in environments(include_all=True):
        mark = "  " if e.deployable else "! "
        cdc = {True: "on", False: "off", None: "-"}[e.usb_cdc_on_boot]
        print(f"{mark}{e.name:<20} board={e.board:<24} chip={e.chip:<9} "
              f"flash={e.flash_size or '?':<6} parts={e.partitions or '-':<24} "
              f"cdc={cdc:<4} upload={e.upload_speed or '-':<7} "
              f"monitor={e.monitor_speed or '-'}")
        if e.note:
            print(f"    not deployable: {e.note}")
    print("\nDefaults the deploy tools adopt for the default env:")
    for k, v in defaults_for(default_env()).items():
        print(f"  {k:<16} {v}")
