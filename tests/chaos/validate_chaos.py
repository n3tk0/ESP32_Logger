#!/usr/bin/env python3
"""
validate_chaos.py — parse the (Wokwi) serial capture from a chaos / boot-smoke
run and assert survival INVARIANTS.  Exit 0 = passed, non-zero = failed.

Design (Phase 3): we validate STRUCTURED telemetry (@TLM / @CHAOS lines emitted
by src/chaos/*) plus crash markers, rather than grepping human log prose.  That
makes the checks robust to log-wording changes and lets us assert real
properties (heap floor, WiFi recovery, no panic, no permanent safe-mode).

Usage:
    validate_chaos.py --input serial.log [--min-heap 20000] [--require-chaos]
    cat serial.log | validate_chaos.py
"""
import argparse
import json
import re
import sys

# Espressif crash / panic / reboot-loop markers.
CRASH_MARKERS = [
    "Guru Meditation Error",
    "panic'ed",
    "Backtrace:",
    "abort() was called",
    "assert failed",
    "CORRUPT HEAP",
    "Stack canary watchpoint",
    "StoreProhibited",
    "LoadProhibited",
    "InstrFetchProhibited",
]

TLM_RE  = re.compile(r"@TLM\s+(\{.*\})\s*$")
BOOT_RE = re.compile(r"@TLM_BOOT\s+(\{.*\})\s*$")
# Bootloader reset banner ("rst:0x... boot:...") — many of these = boot loop.
RST_RE  = re.compile(r"^\s*rst:0x", re.IGNORECASE)


def parse(lines):
    boots, tlm, crashes, chaos = [], [], [], []
    # Chronological event stream so checks can reason about ORDER (e.g. how much
    # telemetry came *after* a chaos fault), not just global counts.
    events = []
    rst_count = 0
    for ln in lines:
        for m in CRASH_MARKERS:
            if m in ln:
                crashes.append(ln.strip())
                break
        if RST_RE.search(ln):
            rst_count += 1
        if "@CHAOS" in ln:
            chaos.append(ln.strip())
            events.append(("chaos", ln.strip()))
        mb = BOOT_RE.search(ln)
        if mb:
            try:
                val = json.loads(mb.group(1))
                boots.append(val)
                events.append(("boot", val))
            except json.JSONDecodeError:
                pass
            continue
        mt = TLM_RE.search(ln)
        if mt:
            try:
                val = json.loads(mt.group(1))
                tlm.append(val)
                events.append(("tlm", val))
            except json.JSONDecodeError:
                pass
    return boots, tlm, crashes, chaos, rst_count, events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", help="serial log file (default: stdin)")
    ap.add_argument("--min-heap", type=int, default=15000,
                    help="fail if free heap ever drops below this (bytes)")
    ap.add_argument("--max-resets", type=int, default=2,
                    help="fail if more than this many bootloader resets seen")
    ap.add_argument("--require-chaos", action="store_true",
                    help="also require @CHAOS markers and WiFi survival after a drop")
    ap.add_argument("--wifi-can-connect", action="store_true",
                    help="environment has a reachable AP (real HIL): require WiFi "
                         "to stably RECONNECT after a drop.  Omit for Wokwi, which "
                         "has no AP — then only post-drop SURVIVAL is checked.")
    ap.add_argument("--strict-safe-mode", action="store_true",
                    help="fully-provisioned image (FS present): fail if the device "
                         "is in safe mode at the end.  Omit for a bare Wokwi image "
                         "with no LittleFS partition — then only a chaos-induced "
                         "healthy->safe transition fails.")
    args = ap.parse_args()

    # Serial captures routinely contain non-UTF-8 noise (boot garbage, partial
    # frames, line glitches); decode leniently so the validator never crashes
    # with UnicodeDecodeError mid-run.
    if args.input:
        with open(args.input, "r", encoding="utf-8", errors="replace") as f:
            boots, tlm, crashes, chaos, rst_count, events = parse(f)
    else:
        if hasattr(sys.stdin, "reconfigure"):
            sys.stdin.reconfigure(errors="replace")
        boots, tlm, crashes, chaos, rst_count, events = parse(sys.stdin)

    failures = []

    # 1. No crash / panic.
    if crashes:
        failures.append(f"crash markers seen ({len(crashes)}): {crashes[:3]}")

    # 2. Booted and reached the telemetry loop.
    if not tlm:
        failures.append("no @TLM telemetry seen — device never reached loop()")

    # 3. Not a boot loop.
    if rst_count > args.max_resets:
        failures.append(f"boot loop: {rst_count} resets > {args.max_resets}")

    # 4. Heap floor held.
    heaps = [t["heap"] for t in tlm if "heap" in t]
    if heaps:
        lo = min(heaps)
        if lo < args.min_heap:
            failures.append(f"heap floor breached: min {lo} < {args.min_heap}")

    # 5. Safe mode: the regression we care about is the device being driven
    #    HEALTHY -> SAFE by the injected chaos.  A device that is ALREADY in
    #    safe mode from boot (e.g. the Wokwi image has no LittleFS partition to
    #    mount) and stays stable is a known environment limitation, not a
    #    resilience failure — only flag a chaos-induced transition into safe
    #    mode.  (--strict-safe-mode restores the "never safe at the end" check
    #    for a fully-provisioned image.)
    safes = [t.get("safe", 0) for t in tlm]
    if safes:
        booted_safe = safes[0] == 1
        if args.strict_safe_mode:
            if len(safes) >= 5 and all(safes[-5:]):
                failures.append("device ended STUCK in safe mode (last 5 samples safe=1)")
        elif not booted_safe and any(safes):
            failures.append("chaos drove the device from healthy into SAFE mode")

    # 6. Chaos-specific.
    if args.require_chaos:
        if not chaos:
            failures.append("--require-chaos set but no @CHAOS markers seen")

        if args.wifi_can_connect:
            # Real HIL with a reachable AP: WiFi must STABLY reconnect after a
            # drop (last 5 samples all WL_CONNECTED == 3).
            dropped = any("wifi_drop" in c for c in chaos)
            recovered = (len(tlm) >= 5 and all(t.get("wifi") == 3 for t in tlm[-5:]))
            if dropped and not recovered:
                failures.append("WiFi dropped by chaos but not stably reconnected "
                                "at end of run (last 5 samples not all wifi==3)")
        else:
            # Emulator (Wokwi) has no AP, so the device can never show
            # WL_CONNECTED — "reconnect" is unverifiable here.  The meaningful
            # property is SURVIVAL: the firmware keeps emitting telemetry AFTER
            # a wifi_drop and isn't crashed/reset by it.  Count telemetry that
            # came *after* the drop in the chronological stream — using the
            # total tlm count would pass even on an immediate post-drop crash,
            # since many samples were emitted before the drop.
            drop_idx = next((i for i, ev in enumerate(events)
                             if ev[0] == "chaos" and "wifi_drop" in ev[1]), None)
            if drop_idx is not None:
                post_drop_tlm = sum(1 for ev in events[drop_idx:] if ev[0] == "tlm")
                if post_drop_tlm < 3:
                    failures.append("WiFi dropped by chaos but device produced fewer "
                                    "than 3 telemetry updates afterward (did not "
                                    "survive the drop)")

    # --- report ---
    print(f"parsed: boots={len(boots)} tlm={len(tlm)} chaos={len(chaos)} "
          f"resets={rst_count} crashes={len(crashes)}"
          + (f" heap_min={min(heaps)}" if heaps else ""))
    if failures:
        print("\nFAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nPASSED: all survival invariants held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
