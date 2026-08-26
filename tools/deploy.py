#!/usr/bin/env python3
"""
tools/deploy.py — All-in-one build, flash & deploy tool for ESP32 Logger.

Interactive CLI for the deployment workflow. Shares business logic with
deploy_gui.py via deploy_core.py.

Usage:
    python3 tools/deploy.py          # interactive menu
    python3 tools/deploy.py --run    # run saved steps non-interactively
    python3 tools/deploy_gui.py      # launch modern GUI (requires customtkinter)

Settings are saved to .flash_tool.json in the project root after each run.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path
from typing import Any

from deploy_core import (
    DeployManager,
    load_cfg,
    save_cfg,
    detect_port,
    detect_env,
    STEP_NAMES,
    PRESETS,
    _UPLOAD_FILTERS,
    _UPLOAD_FILTER_LABELS,
)
from pio_envs import chip_for, environments, env_names, env_info, usb_pins

# ── Project layout ────────────────────────────────────────────────────────────
ROOT = Path(__file__).resolve().parent.parent

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


def _detect_port() -> str:
    """Wrapper for backward compatibility."""
    return detect_port()


def _detect_env() -> str:
    """Wrapper for backward compatibility."""
    return detect_env()


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
    usb_cdc = cfg.get("usb_cdc_on_boot", True)
    # The board is what the env says it is, so it is shown rather than asked
    # for. A chip that could disagree with the environment was a way to write
    # a C3 bootloader onto an S3.
    info = env_info(cfg.get("env", ""))
    env_disp = f"{info.name} {_dim(f'({info.board}, {info.chip}, {info.flash_size})')}" \
        if info.board else cfg.get("env", "")
    if not usb_cdc and info.supports_usb_cdc:
        usb_state = _dim(f"OFF — {usb_pins(info.chip) or 'USB pins'} free as GPIO")
    elif info.supports_usb_cdc:
        usb_state = _green(f"ON — {usb_pins(info.chip) or 'USB pins'} locked for serial")
    else:
        usb_state = _dim("not togglable for this env — see the parent env")
    for key, label, val in [
        ("e", "PlatformIO env ", env_disp),
        ("p", "Serial port    ", port_disp),
        ("i", "Device IP      ", cfg.get("device_ip", "")),
        ("b", "Baud rate      ", str(cfg.get("baud", 921600))),
        ("u", "HTTP upload    ", _UPLOAD_FILTER_LABELS.get(uf, uf)),
        ("w", "Wipe /www first", _green("YES — delete all before upload") if wipe else _dim("no")),
        ("U", "USB CDC on boot", usb_state),
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
            # A numbered pick out of platformio.ini, not free text: typing an
            # env name that does not exist used to be accepted here and only
            # failed several minutes later, inside pio.
            envs = environments()
            print()
            for i, e in enumerate(envs, 1):
                cur = _green(" ←") if e.name == cfg.get("env") else ""
                print(f"  {_cyan(f'[{i}]')}  {e.name:<20} "
                      f"{_dim(f'{e.board}, {e.chip}, {e.flash_size}')}{cur}")
            v = _prompt("Environment (number, or name)", "")
            if v.isdigit() and 1 <= int(v) <= len(envs):
                cfg["env"] = envs[int(v) - 1].name
            elif v in env_names():
                cfg["env"] = v
            elif v:
                print(_red(f"  No [env:{v}] in platformio.ini — unchanged."))
                time.sleep(1.2)
            cfg["chip"] = chip_for(cfg["env"])

        elif ch == "p":
            detected = _detect_port()
            v = _prompt("Serial port (blank = auto-detect)", cfg.get("port") or detected)
            cfg["port"] = v

        elif ch == "i":
            v = _prompt("Device IP", cfg.get("device_ip", "192.168.4.1"))
            if v:
                cfg["device_ip"] = v

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

        elif choice == "U":          # uppercase U — USB CDC toggle
            # The pins come from the chip family, which is what the firmware
            # keys off too (src/modules/UsbCdcModule.cpp). Guessing them from
            # the env NAME, as this used to, was wrong for any env whose name
            # did not spell out the part.
            info = env_info(cfg.get("env", ""))
            if not info.supports_usb_cdc:
                print(_yellow(f"  → [env:{info.name}] has no -DARDUINO_USB_CDC_ON_BOOT "
                              f"of its own; toggle it in the env it extends."))
                time.sleep(1.5)
                continue
            cfg["usb_cdc_on_boot"] = not cfg.get("usb_cdc_on_boot", True)
            state = _green("ON") if cfg["usb_cdc_on_boot"] else _dim("OFF")
            pins = usb_pins(info.chip) or "USB pins"
            hint = _dim(f"({pins} locked for serial)") if cfg["usb_cdc_on_boot"] \
                else _dim(f"({pins} available as GPIO)")
            print(_dim("  → USB CDC on boot: ") + state + " " + hint)
            time.sleep(0.5)

        elif ch in {str(n) for n in STEP_NAMES}:
            n = int(ch)
            steps = set(cfg.get("steps", []))
            steps ^= {n}  # toggle
            cfg["steps"] = sorted(steps)

        elif choice.upper() in PRESETS:
            cfg["steps"] = list(PRESETS[choice.upper()][1])


# ── Step implementations (delegated to DeployManager) ──────────────────────────

def s_wifi_provision(cfg: dict[str, Any]) -> None:
    """WiFi provisioning via serial."""
    manager = DeployManager(cfg)

    def _input(prompt: str) -> str:
        try:
            return input(_bold(f"  {prompt}"))
        except (KeyboardInterrupt, EOFError):
            return ""

    def _getpass(prompt: str) -> str:
        try:
            import getpass
            return getpass.getpass(_bold(f"  {prompt}"))
        except (KeyboardInterrupt, EOFError):
            return ""

    success = manager.provision_wifi(
        input_fn=_input,
        getpass_fn=_getpass,
    )

    if not success:
        print()
    try:
        input(_dim("  Press Enter to return to menu… "))
    except (KeyboardInterrupt, EOFError):
        pass


# ── Orchestrator ───────────────────────────────────────────────────────────────

def run_steps(cfg: dict[str, Any]) -> None:
    steps = sorted(cfg.get("steps", []))
    if not steps:
        print(_yellow("  No steps selected. Use the menu to toggle steps."))
        input(_dim("  Press Enter to return to menu… "))
        return

    # Persist config before running
    save_cfg(cfg)

    # Create manager with CLI output callbacks
    manager = DeployManager(cfg)

    def _confirm_erase() -> bool:
        """Confirmation for chip erase."""
        try:
            ans = input(_yellow("  Continue? [y/N] ")).strip().lower()
        except (KeyboardInterrupt, EOFError):
            ans = ""
        return ans in ("y", "yes")

    # Run with confirmation callback
    success = manager.run_steps(steps, confirm_erase_callback=_confirm_erase)

    print()
    bar = "═" * 46
    if success:
        print(_green(f"  ╔{bar}╗"))
        print(_green(f"  ║  ✓  All steps completed successfully.         ║"))
        print(_green(f"  ╚{bar}╝"))
    else:
        print(_red(  f"  ╔{bar}╗"))
        print(_red(  f"  ║  ✗  Some steps failed. See logs above.       ║"))
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
