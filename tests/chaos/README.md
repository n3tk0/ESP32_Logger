# Phase 3 — Chaos Engineering / resilience testing

Software-in-the-loop fault injection + structured-telemetry validation for the
ESP32_Logger firmware, runnable in the Wokwi emulator under GitHub Actions.

## Components

| File | Role |
|------|------|
| `src/chaos/ChaosTelemetry.h` | Emits machine-parseable `@TLM` survival snapshots (heap, WiFi, safe-mode, reset reason). Gated by `ENABLE_CHAOS_TELEMETRY`. |
| `src/chaos/ChaosMonkey.h` | Seeded fault injector: WiFi flapping, mutex starvation, heap pressure. Gated by `ENABLE_CHAOS_MONKEY`. |
| `tests/chaos/validate_chaos.py` | Parses a serial capture and asserts survival invariants (no crash, heap floor, post-drop WiFi survival, no permanent safe-mode, no boot loop). `--wifi-can-connect` upgrades the WiFi check to require a *stable reconnect* (real HIL with an AP); omit it for Wokwi, which has no AP. |
| `tests/chaos/fixtures/` | `good.log` / `crash.log` — validator self-test corpus. |
| `wokwi.toml`, `diagram.json` (repo root) | Minimal ESP32-C3 Wokwi setup (root so `wokwi-cli .` finds them). |
| `.github/workflows/chaos-test.yml` | Gating validator self-test + non-gating Wokwi sim job. |

## Safety model

Both headers are **fully compiled out** unless their macro is defined, and they
expose always-defined no-op macros (`CHAOS_*_BEGIN/TICK`) so call sites compile
unconditionally. A production `.bin` contains none of this code.

Determinism: `ChaosMonkey` uses a seeded xorshift PRNG (`CHAOS_SEED`, logged at
start-up), so a red CI run is replayable with the same fault schedule.

## Status

- ✅ Self-contained pieces (headers, validator, fixtures, Wokwi config, workflow) — committed.
- ✅ `validate_chaos.py` verified locally: `good.log` → pass, `crash.log` → fail.
- ✅ **Wired into the firmware build.** The 3 integration edits below are applied:
  `[env:chaos_simulator]` exists in `platformio.ini`, and `ESP_Logger.ino`
  includes the chaos headers + calls the hooks. Normal builds are unaffected
  (the hooks expand to no-ops). Kept in this README as a reference / changelog.
- ✅ Wokwi job auto-enables once `WOKWI_CLI_TOKEN` is set; it skips cleanly
  (no red ❌) when the secret is absent (free token: https://wokwi.com/ci).

## Integration — the 3 edits (already applied; kept for reference)

### 1. `platformio.ini` — add the chaos build env

```ini
[env:chaos_simulator]
extends = env:xiao_esp32c3
build_flags =
    ${env:xiao_esp32c3.build_flags}
    -DENABLE_CHAOS_TELEMETRY=1
    -DENABLE_CHAOS_MONKEY=1
    ; optional: -DCHAOS_SEED=0x5eed1234  -DCHAOS_DURATION_MS=150000
```

### 2. `ESP_Logger.ino` — include + hook (3 lines)

Near the other `#include`s (headers self-guard, so include unconditionally):

```cpp
#include "src/chaos/ChaosTelemetry.h"
#include "src/chaos/ChaosMonkey.h"
```

At the **end of `setup()`**:

```cpp
    CHAOS_TELEMETRY_BEGIN();
    CHAOS_MONKEY_BEGIN();
```

Once per **`loop()`** iteration (e.g. near the top, after `OtaManager::tick`):

```cpp
    CHAOS_TELEMETRY_TICK();
```

In a normal build all three lines expand to no-ops, so they are safe to commit.

### 3. Add the Wokwi token

Create a free token at https://wokwi.com/ci and add it as the `WOKWI_CLI_TOKEN`
repository secret. That's it — the workflow's `gate` job detects the secret and
the `chaos-sim` job switches itself on (it **skips cleanly** when the secret is
absent, so it never shows a red ❌ before then).

## Run locally

```bash
# validator self-test (no board needed)
python3 tests/chaos/validate_chaos.py --input tests/chaos/fixtures/good.log --require-chaos
python3 tests/chaos/validate_chaos.py --input tests/chaos/fixtures/crash.log   # exits non-zero

# full sim (after edits above + `pip install platformio` + wokwi-cli)
pio run -e chaos_simulator
wokwi-cli . --timeout 180000 --serial-log-file chaos-serial.log
python3 tests/chaos/validate_chaos.py --input chaos-serial.log --require-chaos
```

## Tuning knobs (compile-time defines)

| Macro | Default | Meaning |
|-------|---------|---------|
| `CHAOS_SEED` | `0x5eed1234` | PRNG seed (reproducible fault schedule) |
| `CHAOS_DURATION_MS` | `150000` | How long ChaosMonkey injects before going quiet |
| `CHAOS_TLM_PERIOD_MS` | `1000` | Telemetry snapshot interval |
