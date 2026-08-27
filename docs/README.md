# Documentation index

Every document in the repository, including the ones that live next to the code
they describe. If you add a document, add it here — this file is the index the
[root README](../README.md) points at, and an unindexed document is one nobody
finds.

## Start here

| | |
|---|---|
| [README.md](../README.md) | What the project is, supported boards, quick start, flash budget |
| [INSTRUCTIONS.md](INSTRUCTIONS.md) | Operating the device after first boot: the wizard, operating modes, sensors, exporters, OTA, safe mode, diagnostics, troubleshooting |

## Architecture and conventions

| | |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Module layout, FreeRTOS task model, data pipeline, and the API route reference (§6) |
| [REFACTORING_GUIDELINES.md](REFACTORING_GUIDELINES.md) | Architecture invariants and the SOPs a code change has to satisfy |
| [AUDIT_LOG.md](AUDIT_LOG.md) | Append-only security and architecture audit record, R1–R27. Its line numbers are evidence of what was seen at the time and are deliberately not maintained |

## Build, flash, deploy

| | |
|---|---|
| [../tools/DEPLOY.md](../tools/DEPLOY.md) | The flash/deploy tools: `deploy_gui.py`, `deploy.py`, bootloader flashing |
| [CORE3_MIGRATION.md](CORE3_MIGRATION.md) | What moving to Arduino core 3.x would cost, measured rather than estimated. Conclusion: not for flash |

## Features

| | |
|---|---|
| [KINDLE_DASHBOARD.md](KINDLE_DASHBOARD.md) | The JavaScript-free e-ink dashboard at `GET /kindle` |
| [../tools/kindle_preview/README.md](../tools/kindle_preview/README.md) | Rendering that dashboard on a desktop to iterate on the layout |
| [../src/modules/README_USB_CDC.md](../src/modules/README_USB_CDC.md) | The USB CDC module: board detection, first-run setup, NVS storage |
| [../src/sensors/SENSORMANAGER_INTEGRATION.md](../src/sensors/SENSORMANAGER_INTEGRATION.md) | Adding a sensor plugin to `SensorManager` |
| [../src/utils/PIN_VALIDATION_GUIDE.md](../src/utils/PIN_VALIDATION_GUIDE.md) | Board profiles, `PinPurpose`, and how a GPIO assignment is validated |

## Satellite hardware

| | |
|---|---|
| [../node/README.md](../node/README.md) | The ESP8266 sensor node that pushes readings into `POST /api/ingest` |
| [../schematics/README.md](../schematics/README.md) | Wiring diagrams |

## Tests

| | |
|---|---|
| [../tests/host/README.md](../tests/host/README.md) | Host-side unit tests — what they cover and how to run them |
| [../tests/chaos/README.md](../tests/chaos/README.md) | The Wokwi resilience build (`chaos_simulator`) |

---

## House rules for these documents

**Never cite a line number.** Name the symbol instead:

```
BAD   the OTA guard (`src/web/WebServer.cpp:<line>`)
GOOD  the OTA guard (`sendOtaDisabled()` in `src/web/WebServer.cpp`)
```

Line citations rot silently, and a reader cannot tell a stale one from their own
confusion — they follow it, land somewhere unrelated, and assume they misread.
When this rule was introduced every one of the 53 `file:line` references in
these documents was already wrong, one of them written two days earlier.

`tools/check_doc_refs.py` enforces this in CI, along with checking that every
source path a document names actually exists. `AUDIT_LOG.md` is exempt, for the
reason given in its row above.
