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
└──────────────────────────────────────────────────┘
```

### Files

- **`deploy_core.py`** — Core business logic, configuration, step implementations
  - `DeployManager` class orchestrates the workflow
  - Callback-based architecture for GUI/CLI integration
  - All HTTP, serial, and PlatformIO operations

- **`deploy.py`** — CLI interface (refactored)
  - Interactive menu with color-coded output
  - Uses DeployManager for all operations
  - Same UX as before, simplified implementation

- **`deploy_gui.py`** — Modern GUI application
  - Built with CustomTkinter for native look & feel
  - Real-time logging, progress tracking
  - Settings panel with auto-detection
  - Preset buttons for common workflows
  - Clean tabbed interface

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

Features:
- Modern, professional interface
- Real-time output logging
- Progress tracking
- Keyboard shortcuts (Ctrl+R to run, Ctrl+S to save, Ctrl+L to clear logs)
- Settings panel on left sidebar
- Step toggles and preset buttons
- Persistent configuration

### CLI
```bash
# Interactive menu
python3 tools/deploy.py

# Non-interactive (run saved steps)
python3 tools/deploy.py --run
```

## Workflow Steps

1. **Build web assets** — Minify and gzip `www/` → `data/www/`
2. **Flash bootloader** — Rollback-enabled bootloader via esptool
3. **Erase chip flash** — Full wipe (config, logs, LittleFS)
4. **Clean build artifacts** — Remove old build files
5. **Compile firmware** — Build the ESP32 sketch (pio run)
6. **Flash firmware** — Upload to device (pio run -t upload)
7. **Upload LittleFS** — Flash the web UI filesystem
8. **Upload web via HTTP** — Deploy to running device over WiFi
9. **Open serial monitor** — Watch device logs

## Presets

Quick configurations for common workflows:

| Preset | Steps | Use Case |
|--------|-------|----------|
| **Full flash** | 1,3,5,6,7 | Clean slate + fresh firmware + filesystem |
| **Clean build** | 4,5,6 | Rebuild & recompile without erase |
| **Quick flash** | 5,6 | Fast recompile & flash (no erase) |
| **HTTP deploy** | 1,8 | Rebuild web + push to running device |
| **All steps** | 1-9 | Everything including serial monitor |

## Configuration

Settings are saved to `.flash_tool.json` in the project root:

```json
{
  "env": "esp32c3_supermini",
  "port": "/dev/ttyUSB0",
  "chip": "esp32c3",
  "baud": 921600,
  "device_ip": "192.168.4.1",
  "steps": [1, 3, 5, 6, 7],
  "upload_filter": "all",
  "wipe_before_upload": false
}
```

### Settings

- **env** — PlatformIO environment (auto-detected from platformio.ini)
- **port** — Serial port for USB connection (auto-detected)
- **chip** — Device type (esp32c3, esp32c3_supermini, esp32)
- **baud** — Serial baud rate (default: 921600)
- **device_ip** — Device IP for HTTP deploy (default: 192.168.4.1)
- **steps** — Selected workflow steps
- **upload_filter** — Which files to upload (all/gz/plain)
- **wipe_before_upload** — Delete /www before uploading (safety)
- **usb_cdc_on_boot** — USB CDC (serial over USB) on boot for ESP32-C3
  - **ON** (default) — USB pins locked for serial communication
  - **OFF** — USB pins (GPIO 18/19) available for general use

#### USB CDC on Boot (ESP32-C3, ESP32-S3)

Several ESP32 boards support configurable USB CDC (serial over USB). You can choose to enable or disable it to control which GPIO pins are available.

**Supported Boards:**
- ESP32-C3 SuperMini (GPIO 18/19) ← Most common
- XIAO ESP32-C3 (GPIO 18/19)
- Generic ESP32-C3 (GPIO 18/19)
- ESP32-S3 (GPIO 19/20)

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

## Building a Standalone Executable

### Quick Start

```bash
# Install PyInstaller
pip install pyinstaller

# Build the executable
./tools/build_exe.sh          # macOS/Linux
tools\build_exe.bat           # Windows
```

The executable will be in `dist/ESP32_Deploy.exe` (Windows) or `dist/ESP32_Deploy` (macOS/Linux).

### What's Included

The standalone executable bundles:
- Python interpreter (embedded)
- CustomTkinter (GUI framework)
- PySerial (serial communication)
- PlatformIO (firmware compilation)
- All application code

**Result:** A single file users can just run. No Python installation required.

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
- **Run from source instead** (no exe): `python tools/deploy_gui.py`. This never
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
```

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
