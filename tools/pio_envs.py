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
             _seen: frozenset[str] = frozenset()) -> str:
    """Value of `key` in [env:name], following `extends` up the chain."""
    if name in _seen:                          # a cycle in extends
        return ""
    sec = sections.get(f"env:{name}", {})
    if key in sec:
        return sec[key]
    parent = sec.get("extends", "")
    if parent.startswith("env:"):
        return _resolve(sections, parent[4:], key, _seen | {name})
    return ""


def _build_flags(sections: dict[str, dict[str, str]], name: str) -> str:
    """All build_flags reaching this env, including through extends and
    ${env:x.build_flags} interpolation. Good enough to answer yes/no
    questions about a flag; not a substitute for what SCons finally emits."""
    text = _resolve(sections, name, "build_flags")
    for ref in re.findall(r"\$\{env:([^.}]+)\.build_flags\}", text):
        if ref != name:
            text += "\n" + _build_flags(sections, ref)
    return text


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
    board = _resolve(sections, name, "board")
    js = _board_json(board)
    chip = (js.get("build", {}) or {}).get("mcu", "") or _chip_hint(board)
    flash = _resolve(sections, name, "board_upload.flash_size") \
        or (js.get("upload", {}) or {}).get("flash_size", "")
    return EnvInfo(
        name=name,
        board=board,
        chip=chip,
        flash_size=flash,
        partitions=_resolve(sections, name, "board_build.partitions"),
        # The toggle rewrites an existing flag in the env's own block, so an
        # env that inherits one from a parent is not togglable in place.
        supports_usb_cdc="-DARDUINO_USB_CDC_ON_BOOT="
                         in sections.get(f"env:{name}", {}).get("build_flags", ""),
        deployable=name not in _NOT_DEPLOYABLE,
        note=_NOT_DEPLOYABLE.get(name, ""),
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


if __name__ == "__main__":
    print(f"platformio.ini: {INI}")
    print(f"default env:    {default_env()}\n")
    for e in environments(include_all=True):
        mark = "  " if e.deployable else "! "
        print(f"{mark}{e.name:<20} board={e.board:<24} chip={e.chip:<9} "
              f"flash={e.flash_size or '?':<6} parts={e.partitions or '-':<24} "
              f"usb_cdc={'yes' if e.supports_usb_cdc else 'no '}")
        if e.note:
            print(f"    not deployable: {e.note}")
