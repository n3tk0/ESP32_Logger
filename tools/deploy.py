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
import textwrap
import time
from pathlib import Path
from typing import Any

from deploy_core import (
    DeployManager,
    NODE_PROJECTS,
    generate_espnow_key,
    node_project,
    load_cfg,
    save_cfg,
    detect_port,
    detect_env,
    adopt_env_defaults,
    pinned_keys,
    STEP_DETAIL,
    STEP_NAMES,
    PRESETS,
    PRESET_BLURBS,
    _UPLOAD_FILTERS,
    _UPLOAD_FILTER_LABELS,
)
from features import (default_on_features, grouped, has_a_reading_source,
                      is_known, optional_features)
from pio_envs import (
    chip_for, defaults_for, environments, env_names, env_info, ports_for,
    usb_pins,
)

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


def _tag(n: int) -> str:
    """A menu key padded to one column: "[1] " and "[10]" are both 4 wide.

    Steps 10 to 12 used to hang a character out of the column, because the
    padding was two literal spaces after a bracket whose width depends on the
    number inside it.
    """
    return f"[{n}]".ljust(4)


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
    # Values the project already states get their provenance shown beside them,
    # so it is obvious which numbers are the env's and which someone pinned.
    d = defaults_for(info.name) if info.board else {}
    pinned = pinned_keys(cfg)

    def _derived(key: str, val) -> str:
        if not d:
            return str(val)
        if key in pinned:
            return f"{val} {_yellow(f'(pinned; env says {d[key]})')}"
        src = d["baud_src"] if key == "baud" else d["monitor_src"]
        return f"{val} {_dim(f'(from {src})')}"

    for key, label, val in [
        ("e", "PlatformIO env ", env_disp),
        ("p", "Serial port    ", port_disp),
        ("i", "Device IP      ", cfg.get("device_ip", "")),
        ("b", "Upload baud    ", _derived("baud", cfg.get("baud"))),
        ("m", "Monitor baud   ", _derived("monitor_speed", cfg.get("monitor_speed"))),
        ("u", "HTTP upload    ", _UPLOAD_FILTER_LABELS.get(uf, uf)),
        ("w", "Wipe /www first", _green("YES — delete all before upload") if wipe else _dim("no")),
        ("U", "USB CDC on boot", usb_state),
    ]:
        print(f"  {_cyan(f'[{key}]')}  {label}: {_bold(val)}")
    if d:
        print(_dim(f"        partitions {d['partitions'] or 'platform default'}"
                   f"   fs {d['filesystem'] or '-'}"
                   f"   flash {d['flash_size'] or '?'}"))
    print()

    # Steps block
    enabled = set(cfg.get("steps", []))
    print(_bold("  ── Steps " + "─" * 53))
    for n, name in STEP_NAMES.items():
        tick = _green("✓") if n in enabled else " "
        print(f"  {_cyan(_tag(n))} [{tick}] {name}")
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
    feats = [m for m in (cfg.get("features") or []) if is_known(m)]
    fsum = _green(f"{len(feats)} selected") if feats else _dim("none")
    print(f"  {_cyan('[F]')}  Build features  {fsum}  {_dim('(uppercase F)')}")
    np = node_project(cfg)
    key_state = _green("key set") if len(cfg.get("espnow_lmk") or "") == 16 \
        else (_yellow("no key") if np.wants_key else _dim("no key needed"))
    print(f"  {_cyan('[N]')}  Node target     {np.label}  {key_state}  {_dim('(uppercase N)')}")
    print(f"  {_cyan('[W]')}  WiFi provision  via serial COM port  {_dim('(uppercase W)')}")
    print(f"  {_cyan('[?]')}  What should I pick?  {_dim('presets and steps, explained')}")
    print(f"  {_cyan('[q]')}  Quit")
    print()


def _help_screen() -> None:
    """What to pick, in plain words.

    The menu is dense on purpose — settings, twelve steps, seven presets and
    the actions all on one screen — and density is what makes it fast on the
    twentieth run and opaque on the first. Rather than pad every preset with a
    sentence nobody rereads, the sentences live here, behind [?].
    """
    _clear()
    print(_cyan("╔" + "═" * _W + "╗"))
    print(_cyan("║") + _bold("  What do you want to do?").ljust(_W) + _cyan("║"))
    print(_cyan("╚" + "═" * _W + "╝"))
    print()
    for key, (pname, psteps) in PRESETS.items():
        steps_s = ", ".join(str(s) for s in psteps) if psteps else "—"
        # Padded before it is coloured: an escape sequence counts toward a
        # format width and pads to nothing on screen.
        print(f"  {_cyan(f'[{key}]')}  {_bold(pname.ljust(14))}"
              f"{_dim(f'steps {steps_s}')}")
        blurb = PRESET_BLURBS.get(key)
        if blurb:
            print(f"       {blurb}")
    print()
    print(_bold("  ── The steps " + "─" * 49))
    for n, (title, detail) in STEP_DETAIL.items():
        print(f"  {_cyan(_tag(n))} {title:<22} {_dim(detail)}")
    print()
    print(_bold("  ── Good to know " + "─" * 46))
    for fact in (
        "Settings you do not pin follow platformio.ini, so switching board "
        "carries that board's own upload speed, chip and USB CDC flag.",
        "[F] build features are passed as -D flags; no project file is edited.",
        "[N] node steps (10–12) act on a DIFFERENT board, on its own port.",
        "Everything is saved to .flash_tool.json in the project root.",
        "The same tool with a window: python3 tools/deploy_gui.py",
    ):
        wrapped = textwrap.wrap(fact, width=_W + 2)
        print(f"  {_dim('•')} {wrapped[0]}")
        for line in wrapped[1:]:
            print(f"    {line}")
    print()
    try:
        input(_dim("  Press Enter to return to the menu… "))
    except (KeyboardInterrupt, EOFError):
        pass


def _node_menu(cfg: dict[str, Any]) -> None:
    """Which satellite board steps 10 and 11 build and flash.

    Its own project, env and port. A node is a different board on a different
    USB device, and borrowing the collector's port is the shortest path to
    flashing an ESP8266 image at an ESP32-C3.
    """
    while True:
        proj = node_project(cfg)
        print()
        print(_bold("  ── Node target " + "─" * 47))
        for i, (key, p) in enumerate(NODE_PROJECTS.items(), start=1):
            tick = _green("✓") if key == proj.key else " "
            print(f"  {_cyan(f'[{i}]')} [{tick}] {p.label:<22} {_dim(p.blurb)}")
        print()
        env_disp = cfg.get("node_env") or _dim(f"{proj.default_env} (project default)")
        port_disp = cfg.get("node_port") or _dim("auto-detect")
        print(f"  {_cyan('[e]')}  Node env   : {_bold(env_disp)}")
        print(f"  {_cyan('[p]')}  Node port  : {_bold(port_disp)}")

        lmk = cfg.get("espnow_lmk") or ""
        if proj.wants_key:
            if len(lmk) == 16:
                shown = _green(lmk) + _dim("  — the collector gets the same 16 bytes")
            elif lmk:
                shown = _red(f"{len(lmk)} characters — both firmwares assert on 16")
            else:
                shown = _yellow("not set — the node would carry its placeholder "
                                "and pair with nothing")
            print(f"  {_cyan('[k]')}  ESP-NOW key: {shown}")
            print(f"  {_cyan('[g]')}  Generate a key  "
                  f"{_dim('16 random characters, from the system CSPRNG')}")
        print()
        print(f"  {_cyan('[b]')}  Back")
        print()

        try:
            ans = input(_bold("  Choice: ")).strip()
        except (KeyboardInterrupt, EOFError):
            print()
            return
        low = ans.lower()
        if low in ("b", "", "q"):
            return
        if low == "e":
            cfg["node_env"] = _prompt("Node env", cfg.get("node_env") or proj.default_env)
            continue
        if low == "p":
            cfg["node_port"] = _prompt("Node port", cfg.get("node_port") or "")
            continue
        if low == "g" and proj.wants_key:
            cfg["espnow_lmk"] = generate_espnow_key()
            print(_green(f"  Generated: {cfg['espnow_lmk']}"))
            time.sleep(1.4)
            continue
        if low == "k" and proj.wants_key:
            key = _prompt("ESP-NOW key (16 characters)", lmk)
            if key and len(key) != 16:
                print(_red(f"  {len(key)} characters — unchanged."))
                time.sleep(1.2)
                continue
            cfg["espnow_lmk"] = key
            continue
        if ans.isdigit() and 1 <= int(ans) <= len(NODE_PROJECTS):
            cfg["node_project"] = list(NODE_PROJECTS)[int(ans) - 1]
            # The env belonged to the old project; keeping it would offer an
            # ESP8266 env for an ESP32 build and fail two steps later.
            cfg["node_env"] = None


def _feature_menu(cfg: dict[str, Any]) -> None:
    """Pick the optional compile-time features this build should carry.

    The list comes from src/setup.h through tools/features.py, so a feature
    added to the firmware shows up here without anyone remembering to add it.

    Every one of them is selectable, in both directions. That is newer than it
    looks: setup.h writes `#ifndef X / #define X`, so a -D flag could only ever
    add, and this menu used to show the off-by-default half and nothing else.
    The tools now pass FEATURE_SET_EXPLICIT, which skips those defaults and
    makes the list they send the whole set — so clearing BME280 or the SD
    driver does what it says.

    The dot in the margin marks what a plain `pio run` would have given you,
    which is the only thing the old split still usefully told anyone.
    """
    while True:
        opts = optional_features()
        chosen = set(m for m in (cfg.get("features") or []) if is_known(m))

        print()
        print(_bold("  ── Build features " + "─" * 44))
        print(_dim("  Compiled in through PLATFORMIO_BUILD_FLAGS. No file is edited."))
        print(_dim("  A dot marks what a plain `pio run` would give you."))
        print()

        index: list[str] = []
        for group, rows in grouped(opts).items():
            print(_bold(f"  {group}"))
            for f in rows:
                index.append(f.macro)
                n = len(index)
                tick = _green("✓") if f.macro in chosen else " "
                dot = "•" if f.enabled else " "
                label = (f.macro.replace("SENSOR_", "").replace("EXPORT_", "")
                                .replace("_ENABLED", ""))
                print(f"  {_cyan(f'[{n:>2}]')} [{tick}]{dot} {label:<22} "
                      f"{_dim(f.summary) if f.summary else ''}".rstrip())
            print()

        # The rule setup.h enforces with an #error, said here in a sentence
        # while there is still something to click. A compiler diagnostic is a
        # fine backstop and a poor first contact.
        if not has_a_reading_source(chosen):
            print(_red("  Nothing to read from.") +
                  _dim("  Pick at least one sensor, or a remote-node feature to"))
            print(_dim("  receive readings from another board. The build refuses "
                       "otherwise."))
            print()

        if "FEATURE_ESPNOW_INGEST" in chosen:
            lmk = cfg.get("espnow_lmk") or ""
            if not lmk:
                shown = _yellow("not set — the build falls back to setup.h's default")
            elif len(lmk) != 16:
                shown = _red(f"{len(lmk)} characters — must be exactly 16")
            else:
                shown = _green("set") + _dim("  (flash the SAME 16 bytes into the node)")
            print(f"  {_cyan('[k]')}  ESP-NOW key   {shown}")
            print(f"  {_cyan('[g]')}  Generate one  "
                  f"{_dim('16 random characters, from the system CSPRNG')}")
            print()

        # A starting point. Now that the tool controls the whole set, a fresh
        # config means thirty cleared boxes, and the first thing anyone wants
        # is "what I would have got anyway, then my changes".
        print(f"  {_cyan('[d]')}  Default set    {_cyan('[c]')}  Clear all"
              f"      {_cyan('[b]')}  Back")
        print()

        try:
            ans = input(_bold("  Choice: ")).strip()
        except (KeyboardInterrupt, EOFError):
            print()
            return

        low = ans.lower()
        if low in ("b", "", "q"):
            return
        if low == "c":
            cfg["features"] = []
            continue
        if low == "d":
            cfg["features"] = [f.macro for f in default_on_features()]
            continue
        if low == "g":
            # Generated rather than invented. A key somebody types is a key
            # somebody can remember, and this one is the only thing between
            # the pipeline and any ESP-NOW frame in radio range.
            cfg["espnow_lmk"] = generate_espnow_key()
            print(_green(f"  Generated: {cfg['espnow_lmk']}"))
            print(_dim("  Saved. Both the collector and the node get it from "
                       "here — flash the node from this tool and neither side "
                       "has to be typed."))
            time.sleep(1.6)
            continue
        if low == "k" and "FEATURE_ESPNOW_INGEST" in chosen:
            print(_dim("  16 characters exactly. Empty leaves the firmware's "
                       "placeholder in place; [g] generates one."))
            try:
                key = input(_bold("  ESP-NOW key: ")).strip()
            except (KeyboardInterrupt, EOFError):
                print()
                continue
            if key and len(key) != 16:
                print(_red(f"  {len(key)} characters — the build asserts on 16. Unchanged."))
                time.sleep(1.2)
                continue
            cfg["espnow_lmk"] = key
            continue

        if not ans.isdigit() or not (1 <= int(ans) <= len(index)):
            continue
        macro = index[int(ans) - 1]
        feats = list(cfg.get("features") or [])
        if macro in feats:
            feats.remove(macro)
        else:
            feats.append(macro)
        cfg["features"] = feats

        # FEATURE_ESPNOW_INGEST implies FEATURE_REMOTE_NODES — setup.h defines
        # it either way. Shown as selected so the menu does not describe a
        # build different from the one that will happen.
        if macro == "FEATURE_ESPNOW_INGEST" and macro in feats:
            if "FEATURE_REMOTE_NODES" not in feats:
                feats.append("FEATURE_REMOTE_NODES")
                cfg["features"] = feats
                print(_dim("  → FEATURE_REMOTE_NODES added: ESP-NOW ingest requires it."))
                time.sleep(1.0)


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

        elif ch == "?":
            _help_screen()

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
            picked = None
            if v.isdigit() and 1 <= int(v) <= len(envs):
                picked = envs[int(v) - 1].name
            elif v in env_names():
                picked = v
            elif v:
                print(_red(f"  No [env:{v}] in platformio.ini — unchanged."))
                time.sleep(1.2)
            # Only re-derive when the environment actually changed. Doing it
            # unconditionally meant backing out of this prompt with a blank
            # line still reset baud, monitor speed and the CDC state — a menu
            # that changes settings when you decline to change anything.
            if picked and picked != cfg.get("env"):
                was_pinned = pinned_keys(cfg)   # measured against the OLD env
                cfg["env"] = picked
                adopt_env_defaults(cfg, was_pinned)

        elif ch == "p":
            # Ports are ranked by the board's own USB IDs, so the right one is
            # first when a node and a collector are both plugged in.
            ranked = ports_for(cfg["env"])
            print()
            if ranked:
                for i, (dev, desc, match) in enumerate(ranked, 1):
                    tag = _green(" ← matches this board") if match else ""
                    print(f"  {_cyan(f'[{i}]')}  {dev:<18} {_dim(desc)}{tag}")
            else:
                print(_dim("  No USB serial ports found (or pyserial is not installed)."))
            v = _prompt("Port (number, a path, or blank for auto-detect)", "").strip()
            if v.isdigit() and 1 <= int(v) <= len(ranked):
                cfg["port"] = ranked[int(v) - 1][0]
            else:
                cfg["port"] = v          # "" means auto-detect on the next load

        elif ch == "i":
            v = _prompt("Device IP", cfg.get("device_ip", "192.168.4.1"))
            if v:
                cfg["device_ip"] = v

        elif ch in ("b", "m"):
            key = "baud" if ch == "b" else "monitor_speed"
            what = "Upload baud" if ch == "b" else "Monitor baud"
            d = defaults_for(cfg["env"])
            src = d["baud_src"] if ch == "b" else d["monitor_src"]
            print()
            print(_dim(f"  {d[key]} from {src}. "
                       f"Blank returns to that; a number pins it here."))
            v = _prompt(what, "").strip()
            if not v:
                cfg[key] = d[key]          # save_cfg writes null, i.e. "follow the env"
                print(_dim(f"  → following the environment: {d[key]}"))
            else:
                try:
                    cfg[key] = int(v)
                    print(_dim(f"  → pinned to {cfg[key]}"))
                except ValueError:
                    print(_red("  Not a number — unchanged."))
            time.sleep(0.8)

        elif choice == "U":          # uppercase U — USB CDC toggle
            # Tested BEFORE the lowercase branch. `ch = choice.lower()` above
            # means `elif ch == "u"` also matches "U", so with that branch
            # first this one could never run and pressing U cycled the upload
            # filter instead. The [W]/[w] pair below is ordered correctly and
            # was the model for this fix. Predates this branch.
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
            print(_dim("    Applied to platformio.ini by the next compile step."))
            time.sleep(0.9)

        elif ch == "u":
            cur = cfg.get("upload_filter", "all")
            nxt = _UPLOAD_FILTERS[((_UPLOAD_FILTERS.index(cur) + 1) % len(_UPLOAD_FILTERS))
                                   if cur in _UPLOAD_FILTERS else 0]
            cfg["upload_filter"] = nxt
            print(_dim(f"  → {_UPLOAD_FILTER_LABELS[nxt]}"))
            time.sleep(0.5)

        elif choice == "F":          # uppercase F — build features
            # Uppercase, and tested before any lowercase branch, for the reason
            # spelled out on [U] above: `ch = choice.lower()` makes a lowercase
            # test match the uppercase key too, so an uppercase action placed
            # after one can never run.
            _feature_menu(cfg)

        elif choice == "N":          # uppercase N — node target
            _node_menu(cfg)

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

    def _confirm() -> bool:
        """Answer a destructive step's question. The step has just logged it.

        Used for the erase steps and for the bootloader step — the latter
        asked for itself, inside flash_bootloader.py, until the GUI (which
        has no stdin to prompt on) turned that prompt into an EOFError. The
        front end asks now, and the script is told --yes.
        """
        try:
            ans = input(_yellow("  Continue? [y/N] ")).strip().lower()
        except (KeyboardInterrupt, EOFError):
            ans = ""
        return ans in ("y", "yes")

    # Run with confirmation callbacks
    success = manager.run_steps(
        steps,
        confirm_erase_callback=_confirm,
        confirm_bootloader_callback=_confirm)

    print()
    # Padded rather than typed: the success line was one column wider than the
    # rule above it, so the box it drew did not close.
    bar = "═" * 46

    # THREE OUTCOMES, NOT TWO. run_steps() returns one bool, and "nothing
    # failed" is not "everything ran": answering no to "Flash bootloader? This
    # overwrites the existing bootloader." is not an error, and it is not the
    # bootloader having been written either. Printing the green box for it is
    # how somebody walks away believing a device was flashed.
    #
    # Which steps were declined goes on its own line rather than inside the
    # box: three declined steps would be wider than the rule, and a box that
    # does not close is what the padding above exists to prevent.
    declined = manager.skipped
    if not success:
        paint, msg = _red, "✗  Some steps failed. See logs above."
    elif declined:
        paint, msg = _yellow, "!  Finished — but some steps were declined."
    else:
        paint, msg = _green, "✓  All steps completed successfully."
    print(paint(f"  ╔{bar}╗"))
    print(paint(f"  ║  {msg.ljust(len(bar) - 2)}║"))
    print(paint(f"  ╚{bar}╝"))
    if success and declined:
        names = ", ".join(f"{n} ({STEP_NAMES.get(n, '?')})" for n in declined)
        print(_yellow(f"     Declined, and did not run: {names}"))

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
