# ESP32 Logger — Deployment Tools

## Overview

The deployment toolchain provides two interfaces for building, flashing, and deploying firmware to the ESP32 Logger:

- **CLI**: `deploy.py` — Terminal-based interactive menu (traditional)
- **GUI**: `deploy_gui.py` — Modern desktop application (recommended)

Both share identical business logic via `deploy_core.py`, ensuring consistency and easy maintenance.

## Architecture

```
┌──────────────────────────────────────────────────┐
│  deploy.py (CLI)    |    deploy_gui.py (GUI)    │
├──────────────────────────────────────────────────┤
│           deploy_core.py (Shared Logic)           │
│                                                   │
│  • DeployManager class                           │
│  • Configuration management                      │
│  • Step implementations                          │
│  • HTTP upload, WiFi provisioning, etc.          │
├──────────────────────────────────────────────────┤
│      pio_envs.py  (what boards exist at all)     │
│           reads platformio.ini directly           │
└──────────────────────────────────────────────────┘
```

### Adding a board

Add an `[env:…]` to `platformio.ini`. That is the whole procedure — the CLI
list, the GUI dropdown, the chip the bootloader step uses, the upload and
monitor speeds, the partition table, the USB CDC toggle and the USB IDs the
port detector matches on all derive from it through `pio_envs.py`.

    python3 tools/pio_envs.py     # exactly what the tools will see

prints every env with what it resolves to, and the settings the tools adopt
for the default one.

It did not use to be. The board list was written out in four places — the CLI
chip prompt, the GUI dropdowns, `deploy_core`'s `supported_envs` set and
`flash_clean`'s fallback — each listed a different subset, and all four had
gone stale: `xiao_esp32s3` and `esp32s3_n16r8` had been buildable for weeks
while the deploy tools would not offer them. `tools/check_pio_envs.py` runs in
CI to keep the derivation honest, with a negative control proving it can fail.

The optional **compile-time features** are derived the same way and for the
same reason — `tools/features.py` reads them out of `src/setup.h`, so adding a
guarded block there is all it takes for the feature to appear in both the CLI
and the GUI. `tools/check_features.py` compares that parser against a second,
dumber scan on every CI run, because a regex that quietly stops matching leaves
a shorter menu and no error at all.

### Files

- **`deploy_core.py`** — Core business logic, configuration, step implementations
  - `DeployManager` class orchestrates the workflow
  - Callback-based architecture for GUI/CLI integration
  - All HTTP, serial, and PlatformIO operations

- **`deploy.py`** — CLI interface (refactored)
  - Interactive menu with color-coded output
  - Uses DeployManager for all operations
  - Same UX as before, simplified implementation

- **`deploy_gui.py`** — Desktop window (CustomTkinter)
  - Board → port → job → **Run** down the left, always visible
  - Tabs for output, settings, build features, WiFi and help
  - Per-item explanations on hover (a placed label, not a tooltip window)
  - **No pop-up windows at all** — messages and questions appear in the status
    bar at the bottom of the same window
  - Text scales from the header (A− / A+) and the choice is remembered

### Why CustomTkinter — and what about Flet?

The window is a form: six dropdowns, thirty checkboxes, a log pane and a
button. CustomTkinter renders that adequately, `tkinter` ships with CPython on
Windows and macOS, and the only extra dependency is a small pure-Python
package. That is the whole case for it — not that it is pretty.

**Flet** (Python driving a Flutter client) would look better, and its layout
engine is genuinely nicer than tkinter's geometry managers. The costs land
squarely on the three things this project has already paid for:

| | CustomTkinter (today) | Flet |
|---|---|---|
| Binary | one PyInstaller file, ~30 MB | Python **plus** a bundled Flutter client, typically 80–150 MB |
| Packaging | `deploy_gui.spec`, three OSes in CI | `flet pack` — a different pipeline; macOS becomes an app bundle, not one file |
| Antivirus / Gatekeeper | already a documented fight (see below) | a second native executable spawned by the first, and a localhost socket between them |
| Headless test | `drive_deploy_gui.py` clicks real widgets under Xvfb and asserts on the resulting `PLATFORMIO_BUILD_FLAGS` | no Python-side widget driving; you would test the model and stop asserting on the window |
| Dependency | one pure-Python package, stable for years | a versioned native runtime whose API has broken across releases |

The last two matter most. The widget-driving test is the reason a checkbox
wired to the wrong variable gets caught, and losing it to gain a nicer button
is a bad trade for a tool whose job is to erase flash on demand.

So: **not a replacement, at least not on looks alone.** The reasons that would
justify it are real but absent here — running the flasher in a browser or on a
tablet, or a UI rich enough to need tables, searchable dropdowns and live
charts.

If it is ever tried, the architecture already makes it cheap and reversible:
`deploy_core.py` holds every step, and `deploy_gui.py` is a view over it. A
`deploy_flet.py` can sit beside `deploy_gui.py` against the same core, be
packaged for all three OSes and be measured — size, startup time, whether
Defender flags it, whether the widgets can be driven in CI — before anything
is removed. Decide it on those numbers, not on a screenshot.

## Installation

### CLI Only
No additional dependencies beyond what's already required:
```bash
pip install platformio
```

### GUI (Recommended)
```bash
cd tools
pip install -r requirements.txt
```

Or manually:
```bash
pip install customtkinter platformio pyserial
```

## Usage

### GUI (Recommended)

**Option A: Standalone Executable (No Python needed)**

Build once, distribute to any user:

```bash
# Build the executable
./tools/build_exe.sh          # macOS/Linux
tools\build_exe.bat           # Windows

# Run the executable
./dist/ESP32_Deploy           # macOS/Linux
.\dist\ESP32_Deploy.exe       # Windows
```

This creates a single `.exe` file (Windows) or app bundle (macOS) that includes everything:
- Python interpreter
- All dependencies (customtkinter, pyserial, platformio)
- Your application code
- No installation needed — just run!

**Option B: From Python Source**

```bash
pip install -r tools/requirements.txt
python3 tools/deploy_gui.py
```

### The window

Down the left, in the order you need them, with the last one pinned so it is
never scrolled off:

1. **Board** — the environments read out of `platformio.ini`. Everything else
   (chip, upload speed, partition table, USB IDs) follows from the choice.
2. **Port** — a list of the USB ports that exist, best match for the selected
   board first (★). Still typeable, for a port no scan can see.
3. **What do you want to do?** — the presets, two to a row; hover one for what
   it is for and the steps it ticks. `▸ Customise steps` unfolds the twelve
   individual toggles.
4. **Run** — the button, and under it the steps it is about to run, by name.

On the right: **Run** (progress bar and log), **Settings** (device IP, upload
and monitor baud, HTTP upload filter, USB CDC), **Build** (the feature list
with a filter box, the ESP-NOW key, the node target), **WiFi** (provisioning)
and **Help**.

**Hints on hover.** Rest the pointer on a job button, a step, a feature, or
any heading marked ⓘ, and a balloon says what it is — the preset's sentence
and the steps it ticks, the command a step runs, what a sensor is and on which
bus. Printed under each item, as they used to be, those lines roughly doubled
the height of three panels: the twelve-step list was twenty-four lines long and
the thirty-feature list sixty.

The balloon is **a label placed inside the main window, not a tooltip
window**. That distinction is the whole point: a tooltip is the same object as
the dialogs below — an override-redirect child the window manager places, which
can appear behind its parent and take focus from it. A placed label is clipped
by the window, takes no focus, and vanishes when the pointer leaves. The cost
is that it cannot extend past the window edge, so it clamps and flips above the
widget when there is no room below.

**No pop-up windows.** Everything the tool says lands in the status bar along
the bottom or in the log, and everything it asks — the erase and bootloader
confirmations included — grows buttons in that same bar. This is not cosmetic: a tkinter
dialog can open *behind* its parent under several Linux window managers and on
an unfocused Windows app, and the erase confirmation did exactly that, leaving
a deploy stopped on a question nobody could see. `tests/gui/drive_deploy_gui.py`
asserts that no `messagebox`, `CTkToplevel`, `CTkInputDialog` or `grab_set`
call exists in the file, and that the hint is a `CTkLabel` belonging to the
main window.

**Readability.** Nothing in the window is smaller than 15 px (the old one used
8–10 pt for exactly the labels carrying the warnings). `A −` / `A +` in the
header rescale the whole interface between 85 % and 180 %, and the Dark /
Light / System menu beside them switches theme; both are saved in
`.flash_tool.json` as `ui_scale` and `ui_theme`.

Keyboard: `Ctrl+R` run · `Ctrl+S` save · `Ctrl+L` clear the log · `F5` rescan
ports · `Ctrl++` / `Ctrl+-` / `Ctrl+0` text size.

### CLI

`[?]` in the menu explains every preset and step in plain words, so the dense
one-screen menu does not have to.

```bash
# Interactive menu
python3 tools/deploy.py

# Non-interactive (run saved steps)
python3 tools/deploy.py --run
```

## Workflow Steps

> **Charts (uPlot):** the dashboard/log charts load `uPlot.iife.min.js` +
> `uPlot.min.css` from LittleFS first and only fall back to the jsDelivr CDN.
> Both files are vendored in `www/` (pinned to the `CDN_VERSION` in
> `www/js/pages.js`), so charts render even in offline / AP-only mode. To
> refresh or re-pin them run `tools/fetch_uplot.sh [version]`; the build-web
> step below then produces their `.gz` siblings, which the ESP serves with
> `Content-Encoding: gzip`.

1. **Build web assets** — Minify and gzip `www/` → `data/www/`
2. **Flash bootloader** — Rollback-enabled bootloader via esptool (asks first)
3. **Erase chip flash** — Full wipe (config, logs, LittleFS)
4. **Clean build artifacts** — Remove old build files
5. **Compile firmware** — Build the ESP32 sketch (pio run)
6. **Flash firmware** — Upload to device (pio run -t upload)
7. **Upload LittleFS** — Flash the web UI filesystem
8. **Upload web via HTTP** — Deploy to running device over WiFi
9. **Open serial monitor** — Watch device logs
10. **Erase node flash** — Full wipe of the satellite board (`pio run -d node… -t erase`)
11. **Compile node firmware** — Build the satellite project
12. **Flash node firmware** — Upload it, on the node's own port

## Presets

Quick configurations for common workflows:

| Preset | Steps | Use Case |
|--------|-------|----------|
| **Full flash** | 1,3,5,6,7 | Clean slate + fresh firmware + filesystem |
| **Clean build** | 4,5,6 | Rebuild & recompile without erase |
| **Quick flash** | 5,6 | Fast recompile & flash (no erase) |
| **HTTP deploy** | 1,8 | Rebuild web + push to running device |
| **Node flash** | 11,12 | The satellite node board, on its own port |
| **All steps** | 1-9 | Everything including serial monitor |

The GUI shows the same one-line explanation when you hover a preset button, and
the CLI's `[?]` screen lists them; both read `PRESET_BLURBS` in
`deploy_core.py`, so there is one copy of the wording.

## Configuration

Settings are saved to `.flash_tool.json` in the project root:

```json
{
  "env": "xiao_esp32c3",
  "port": null,
  "baud": null,
  "monitor_speed": null,
  "device_ip": "192.168.4.1",
  "steps": [1, 3, 5, 6, 7],
  "upload_filter": "all",
  "wipe_before_upload": false,
  "ui_scale": 1.0,
  "ui_theme": "Dark"
}
```

**`null` means "take it from `platformio.ini`".** That is the normal state, and
it is why the file is nearly empty: the environment already states the upload
speed, the monitor speed, the chip, the partition table and the USB CDC flag,
so the tools read them instead of keeping a second copy. Put a number in and it
is pinned — the menu then shows `921600 (pinned; env says 460800)` so an
override never looks like a default. Clearing the field (or entering nothing at
the CLI prompt) hands it back to the environment.

`chip` and `usb_cdc_on_boot` are not written at all. Both are read from the
project on every load, because a stale copy of the first flashes the wrong
bootloader and a stale copy of the second puts a checkbox on screen describing
a build that will not happen.

Switching board therefore carries that board's settings with it, and only
values you pinned survive the switch.

### Settings

- **env** — PlatformIO environment. Picked from a list read out of
  `platformio.ini`, so adding an `[env:…]` there is all it takes for a board
  to appear in both the CLI and the GUI. Defaults to `[platformio]
  default_envs`.
- **port** — Serial port. Auto-detection ranks ports by the board definition's
  own USB VID:PID, so with a collector and an ESP8266 node both plugged in the
  right one comes first instead of whichever enumerated first. Non-USB ports
  (`COM1`, `/dev/ttyS0`) are never offered — no board is behind one.
- **monitor_speed** — Serial monitor / WiFi-provisioning speed, resolved for
  the selected env (its own value, then what it `extends`, then `[env]`). The
  provisioner used to take the first `monitor_speed` anywhere in the file,
  which is right only until one env overrides the shared one.
- **chip** — **Derived from the env, and not stored in this file at all.** It
  is re-resolved from the board definition on every load; adding it by hand
  has no effect, because `save_cfg()` drops it again. It used to be a
  free-text field beside the environment, which let a saved config say
  `esp32s3` for the env and `esp32c3` for the chip — and step 2 would then
  write a C3 bootloader to an S3.
- **baud** — Upload speed, from the env's `upload_speed` (falling back to
  the board definition's `upload.speed`). Used by the bootloader step;
  `pio run -t upload` reads the same value from the ini directly.
- **device_ip** — Device IP for HTTP deploy (default: 192.168.4.1)
- **steps** — Selected workflow steps
- **upload_filter** — Which files to upload (all/gz/plain)
- **wipe_before_upload** — Delete /www before uploading (safety)
- **ui_scale**, **ui_theme**, **steps_panel_open** — GUI only: interface scale
  (0.85–1.8), `Dark`/`Light`/`System`, and whether the step list is unfolded.
  The CLI ignores them.
- **usb_cdc_on_boot** — USB CDC (serial over USB) on boot for ESP32-C3
  - **ON** (default) — USB pins locked for serial communication
  - **OFF** — USB pins (GPIO 18/19) available for general use
- **features** — Optional compile-time features, by macro name. See below.
- **espnow_lmk** — The 16-character key shared with an ESP-NOW battery node.
  Only used when `FEATURE_ESPNOW_INGEST` is selected.

#### Build features

The `[F]` menu entry in the CLI and the checkbox list in the GUI's **Build**
tab (with a filter box over it — thirty checkboxes is a list you search, not
one you read). Both read `src/setup.h` through `tools/features.py`, so there is no list
here to keep in step.

Selected features are passed as `PLATFORMIO_BUILD_FLAGS` and **no project file
is edited**. That is a deliberate difference from the USB CDC toggle, which has
to rewrite `platformio.ini` because the flag it changes lives there — and the
comments on `_configure_usb_cdc()` are a catalogue of what goes wrong when a
tool edits the project's own source: sections that ran to the end of the file,
a duplicate flag appended on every single run, a build comment rewritten into
nonsense. Nothing in the feature path touches a file, so a deploy you abandon
halfway leaves the checkout exactly as it was.

**Every feature is selectable, in both directions**, and that took a change in
the firmware to make true. `setup.h` writes each default as
`#ifndef X / #define X`, so `-UX` achieves nothing — the preprocessor applies
it before the header, and the header defines the macro again a line later. A
`-D` flag could therefore only ever *add*, and eight features had no switch at
all: among them BME280/BMP280, which somebody went looking for and concluded
was unsupported, and the SD driver, which is ~34 KB of flash on a device that
may never have a card in it.

`FEATURE_SET_EXPLICIT` is the switch. With it defined, `setup.h` skips its
default block entirely and the build carries exactly the macros it was passed.
The tools always pass it, so a cleared checkbox means what it says. A hand
build is unaffected: no flag, same defaults as always.

Two rules sit outside that guard because they hold either way — the
`FEATURE_ESPNOW_INGEST → FEATURE_REMOTE_NODES` implication, and an `#error`
when nothing in the set can produce a reading. The tools refuse that set first,
in a sentence, while there is still something to click; the `#error` is the
backstop for a hand build.

Both tools group the list by type and mark the default set with a dot, with a
**Default set** control to restore it. Measured on `xiao_esp32c3`: the default
set is 1,322,692 bytes; one sensor and nothing else is 1,109,610. 213 KB is
what the SD driver, the second sensor and the five exporters cost when they are
not wanted.

#### The nodes

Steps **10** and **11** compile and flash a satellite board — `node_espnow/`
(the deep-sleeping ESP32-C3) or `node/` (the mains ESP8266) — with
`pio run -d <project>`. Each gets its own env and its own port, because a node
is a different board on a different USB device and borrowing the collector's
is the shortest path to flashing an ESP8266 image at an ESP32-C3. The `D`
preset runs both steps.

#### The ESP-NOW key

Generated, not invented: **Generate** in the GUI, `[g]` in the CLI, 16
characters from the system CSPRNG. Letters and digits only — the key reaches
the compiler as `-DESPNOW_LMK=\"…\"` through a shell, and ambiguous glyphs
(`0`/`O`, `1`/`l`/`I`) are dropped because somebody reads it off one screen and
types it into another.

It is stored once and carried into **whichever target is built**, collector or
battery node, so both sides hold the same 16 bytes without either being typed
twice. `node_espnow/platformio.ini` no longer hardcodes a key: `node_config.h`
declares it with `#ifndef` and a placeholder, which is what lets the tool
override it without redefining a macro the ini had already defined.

`tests/gui/drive_deploy_gui.py` asserts all of it against the running window —
that every feature has a checkbox, that clearing everything raises the
"nothing to read from" warning, that a generated key is 16 characters and
contains nothing that would break a `-D` flag, and that the battery node is
built with the same key as the collector while the ESP8266 node is not handed
one it cannot use.

The flags go to **every** `pio run`, not only the compile step. `pio run -t
upload` relinks before it flashes, so an upload that did not carry them would
quietly rebuild the firmware *without* the selected features and flash that —
a board that boots fine, missing exactly what was asked for, with the
successful compile step scrolled off the screen above it.

##### The ESP-NOW key

`FEATURE_ESPNOW_INGEST` needs a 16-character shared secret, and the **same 16
characters must be flashed into the node** (`node_espnow/platformio.ini`) or
nothing pairs and nothing decrypts. Both tools say so, and both warn when the
field is empty — in which case the build falls back to the placeholder in
`setup.h`, which is fine on a bench and not fine on a shared network.

The battery node itself is a separate PlatformIO project and is **not** flashed
by these tools; it is built with `cd node_espnow && pio run -t upload`. Same for
the ESP8266 node in `node/`. Both have their own `platformio.ini`, which is why
`pio_envs.py` — which reads the root one — does not see them.

#### USB CDC on Boot (ESP32-C3, ESP32-S3)

The USB Serial/JTAG pins are either the serial console or general GPIO, never
both, and which one is decided at compile time. The toggle rewrites
`-DARDUINO_USB_CDC_ON_BOOT` in the env's own `build_flags` before the build.

**Which pins** comes from the chip family, not from a list of boards:

| Chip | USB D- | USB D+ |
|---|---|---|
| ESP32-C3 (XIAO C3, Super Mini, LOLIN C3 PICO) | GPIO 18 | GPIO 19 |
| ESP32-S3 (DevKitC-1, XIAO S3, N16R8) | GPIO 19 | GPIO 20 |

**Which envs can be toggled** is read from `platformio.ini`: the env needs its
own `-DARDUINO_USB_CDC_ON_BOOT` line, because the flag is rewritten in place.
`esp32s3_n16r8` inherits its flag through `extends`, so the tool says so and
declines rather than editing `esp32s3` — which would have changed a second
board silently. Toggle it in the parent env instead.

**Configuration Options:**

- **USB CDC ON** (Default) — Uses GPIO 18/19 (or 19/20 on S3) for USB serial
  - ✓ Easy serial debugging via USB cable
  - ✓ Python scripts can read/write via serial port
  - ✗ GPIO pins locked for USB communication

- **USB CDC OFF** — Frees GPIO 18/19 (or 19/20) for general use
  - ✓ GPIO pins available for sensors/I2C/SPI/etc.
  - ✓ More GPIO expansion options
  - ✗ Can't use USB serial (must use HTTP logs or UART)

**Workflow:**

1. **Deploy Tool** — Toggle [U] in CLI menu or checkbox in GUI before compilation
2. **Compilation** — Build flags automatically applied to platformio.ini
3. **Firmware Runtime** — On first boot, device detects configuration and prompts for confirmation
4. **Non-volatile Storage** — User preference saved to device NVS (persists across reboots)

**Firmware Features:**

- **First-run Setup** — Interactive menu on device's first boot
- **Board Detection** — Automatically detects which board is running
- **Pin Information** — Shows affected GPIO pins for each board
- **Persistent Configuration** — Setting survives firmware updates and reboots
- **Runtime Status** — Firmware prints current USB CDC configuration at boot

## The standalone executable

### Download it — you do not have to build it

CI builds the binary on every change under `tools/`, so the published one never
lags the scripts. Grab it from the **Build Deploy GUI** workflow: open the run
for the commit you want on the [Actions
tab](../../actions/workflows/build-gui-tools.yml) and take the artifact from
its Summary page.

| Artifact | Contains |
|---|---|
| `ESP32_Deploy-windows-<sha>` | `ESP32_Deploy-windows.exe` |
| `ESP32_Deploy-linux-<sha>` | `ESP32_Deploy-linux` |
| `ESP32_Deploy-macos-<sha>` | `ESP32_Deploy-macos` |

Each also carries `selftest-<os>.txt` — what that exact binary reported about
itself when CI checked it. The commit sha is in the artifact name so it is
obvious which version of the deploy scripts you are holding.

Releases get the three binaries attached as assets as well.

**Put it in your `ESP32_Logger` folder** (any subdirectory works) or start it
from there. It reads `platformio.ini` at runtime rather than bundling it —
that is what makes the board list follow the project instead of the binary's
build date. Started somewhere with no project above it, it says so in its own
status bar and log instead of showing an empty board list.

On Linux: `chmod +x ESP32_Deploy-linux`. On macOS, the binary is unsigned, so
Gatekeeper quarantines it: `xattr -d com.apple.quarantine ESP32_Deploy-macos`.

Check any copy with:

```bash
./ESP32_Deploy --selftest
```

which prints the project it found, whether pyserial made it into the bundle,
and every environment it can offer — then exits without opening a window. On
Windows the same report is written to `deploy_selftest.txt` next to the
executable, because a windowed build there has no console to print to.

### Building it yourself

```bash
pip install pyinstaller customtkinter pyserial
./tools/build_exe.sh          # macOS/Linux
tools\build_exe.bat           # Windows
```

Output: `dist/ESP32_Deploy.exe` (Windows) or `dist/ESP32_Deploy` (macOS and
Linux — the spec has no `BUNDLE` section, so macOS gets a plain binary, not an
`.app`). Linux also needs `python3-tk`, which customtkinter sits on top of.

### What is and is not in it

Bundled: the Python interpreter, CustomTkinter, pyserial, and all of
`tools/` — `deploy_gui.py`, `deploy_core.py` and `pio_envs.py`.

**Not bundled: PlatformIO.** It is a ~500 MB toolchain the spec explicitly
excludes, and the tool shells out to it. Install it separately
(`pip install platformio`). An earlier version of this document listed it as
included; it never was.

Also not bundled: your project. See above — that is deliberate.

### Customization

Edit `tools/deploy_gui.spec` to:
- Add an application icon: `icon='path/to/icon.ico'`
- Show/hide console window: `console=True/False`
- Change app name: `name='YourName'`
- Optimize for size: Add `--onefile` for single-file build

### File Size

- Executable: ~150-200 MB
- Compressed: ~50-70 MB

### Distribution

1. **Single File**: Share `dist/ESP32_Deploy.exe` directly
2. **Installer**: Wrap with NSIS or Inno Setup for auto-updates
3. **GitHub Releases**: Upload to releases page for one-click downloads
4. **Portable**: Users can run from USB without installation

### Troubleshooting

**Antivirus false positive (e.g. `Behavior:Win32/DefenseEvasion.A!ml`):**

PyInstaller `.exe` files are commonly flagged by Windows Defender and other
antivirus engines. This is a **false positive** — the build is not malware. The
heuristics trigger because:
- PyInstaller's onefile bootloader unpacks itself to a temp dir at runtime
  (looks like "self-extracting" behavior), and
- the tool legitimately spawns subprocesses (`pio`, `esptool`).

The spec file already minimizes this by:
- **disabling UPX compression** (`upx=False`) — packed executables are the
  single biggest heuristic trigger, and
- **embedding Windows version metadata** (`version_info.txt`) so the binary
  looks like a published app rather than an anonymous stub.

If it is still flagged:
- **Restore from quarantine** and add an exclusion: Windows Security →
  Virus & threat protection → Manage settings → Exclusions → add the `.exe`.
- **Report the false positive** to Microsoft:
  https://www.microsoft.com/en-us/wdsi/filesubmission — getting it whitelisted
  upstream helps all users.
- **Run from source instead** (no exe): `python3 tools/deploy_gui.py`. This never
  trips antivirus because it's plain Python.
- **For public distribution, code-sign the executable.** An Authenticode (ideally
  EV) certificate is the only thing that reliably stops Defender/SmartScreen
  warnings. Sign in CI with `signtool` using a cert stored in GitHub secrets.

**Serial port not detected:**
- Ensure pyserial is included (it is by default)
- On Linux, user may need: `sudo usermod -a -G dialout $USER`

**Large file size:**
- Normal for PyInstaller (includes Python + all deps)
- UPX compression is intentionally **disabled** to avoid antivirus false
  positives (see above) — this trades a larger file for fewer AV problems
- Consider distributing as an installer instead

## Extending the System

### Adding a New Step

1. Add to `STEP_NAMES` in `deploy_core.py`
2. Implement as a method in `DeployManager` class (e.g., `s10_example()`)
3. Register in the `dispatch` dict in `run_steps()`
4. Both CLI and GUI will automatically support it

### Callbacks

`DeployManager` supports callbacks for integration:

```python
manager = DeployManager(cfg)
manager.on_step_start = lambda step, name: print(f"Starting {name}")
manager.on_step_output = lambda msg: print(msg)
manager.on_step_complete = lambda step, rc: print(f"Step {step} done")
manager.on_error = lambda msg: print(f"ERROR: {msg}")

manager.run_steps(
    steps,
    confirm_erase_callback=ask,        # steps 3 and 10 — wipes the flash
    confirm_bootloader_callback=ask,   # step 2 — overwrites the bootloader
)
```

### A step must never prompt on stdin

The destructive steps are confirmed by the FRONT END, through the callbacks
above, and the answer is passed to the helper script as a flag
(`flash_bootloader.py --yes`, `flash_clean.py -y`). A helper that asks for
itself works from a terminal and cannot work from the GUI: the windowed build
has no console behind it and therefore no stdin, so `input()` there raises

```
EOFError: EOF when reading a line
```

and the step fails on a question nobody was shown. `deploy_core._run_cmd()`
now gives every step `stdin=DEVNULL` (the serial monitor excepted) so a new
one cannot reintroduce this quietly — it fails at the prompt in testing rather
than in someone's hands.

## Troubleshooting

### GUI won't start
```bash
pip install customtkinter
# Or if that fails
pip install --upgrade customtkinter
```

### PlatformIO not found
```bash
pip install platformio
pio --version
```

### Serial port not detected
- Check device is connected: `ls /dev/tty*` (Linux) or Device Manager (Windows)
- Try setting port manually in settings
- On Linux, you may need: `sudo usermod -a -G dialout $USER`

### HTTP upload fails
- Ensure device is connected to WiFi with correct IP
- Device must be reachable: `ping <device_ip>`
- Run step 1 (Build web) first to ensure data/www/ exists

### `EOFError: EOF when reading a line` during a step

Fixed. A helper script was prompting on a stdin the windowed GUI does not
have; the confirmations are asked in the window now and passed on as `--yes`.
If you see it again, a step is prompting where it must not — see
[A step must never prompt on stdin](#a-step-must-never-prompt-on-stdin).

### A console window flashes up on every step (Windows)

Also fixed, and for the same underlying reason: the GUI is built windowed
(`console=False`), so Windows handed each console program it launched
(`python.exe`, `pio.exe`, `esptool`) a console window of its own. They were
always empty — the output goes down a pipe into the GUI's log — and the steps
are launched with `CREATE_NO_WINDOW` now. The serial monitor keeps its console
when there is one to keep, since it is the one step with a keyboard.

## Migration from Old Tools

The old separate scripts still work:
- `build_web.py` — Still works independently
- `flash_bootloader.py` — Still works independently
- `flash_clean.py` — Still works independently
- `upload_www.py` — Still works independently

But you get more benefits using the unified deploy tool:
- Single consistent interface
- Configuration persistence
- Better error handling
- Progress tracking
- Both CLI and GUI options

## Development

### Testing changes
```bash
# Test CLI
python3 tools/deploy.py --help

# Test GUI
python3 tools/deploy_gui.py

# Run tests
cd tests
pytest
```

### Code structure
- `deploy_core.py` — Pure logic, no UI dependencies
- `deploy.py` — CLI presentation layer
- `deploy_gui.py` — GUI presentation layer (CustomTkinter)

This separation makes it easy to add new interfaces (web dashboard, REST API, etc.) in the future.

## License

Same as ESP32 Logger project.
