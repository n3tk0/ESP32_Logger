# USB CDC Module

## Overview

The `UsbCdcModule` manages USB CDC (Communications Device Class) configuration for boards that support multiplexed USB pins.

**What is USB CDC?**
- USB CDC is a virtual serial port over the USB interface
- On ESP32-C3 and ESP32-S3, USB pins can be used for either serial communication (CDC) or general GPIO
- The choice is made at build time via the `-DARDUINO_USB_CDC_ON_BOOT=1|0` flag

## Supported Boards

| Board | USB Pins | Deploy Tool Env |
|-------|----------|-----------------|
| ESP32-C3 SuperMini | GPIO 18/19 | `esp32c3_supermini` |
| XIAO ESP32-C3 | GPIO 18/19 | `xiao_esp32c3` |
| Generic ESP32-C3 | GPIO 18/19 | (custom) |
| ESP32-S3 | GPIO 19/20 | `esp32s3` |

## Features

### First-Run Setup
When the firmware runs on a device for the first time, the USB CDC module:
1. Detects first-run status via NVS (Non-Volatile Storage)
2. Displays the board type and affected pins
3. Prompts the user to choose USB CDC on/off
4. Saves the preference to NVS for future boots

### Board Detection
Automatically detects the board type from PlatformIO build macros:
- `ARDUINO_SEEED_XIAO_ESP32C3`
- `ARDUINO_ESP32C3_DEV`
- `ARDUINO_ESP32S3_DEV`

### Status Reporting
Prints a clear status banner at boot showing:
- Detected board name
- Current USB CDC configuration
- Affected GPIO pins
- Benefits and limitations of current setting

### Non-Volatile Storage
- Uses ESP32 Preferences (NVS wrapper)
- Namespace: `usb_cdc`
- Keys:
  - `enabled` — USB CDC on/off setting
  - `first_run` — First-run flag
- **Persists across reboots and firmware updates**

## Integration

### In your sketch (Logger.ino or setup.h)

```cpp
#include "src/modules/UsbCdcModule.h"

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);  // Wait for USB serial (if enabled)

    // Initialize USB CDC module (must be after Serial.begin)
    usbCdc.begin();

    // Rest of setup...
}
```

### Checking USB CDC Status

```cpp
// Check if USB CDC is enabled
if (usbCdc.isUsbCdcEnabled()) {
    Serial.println("USB serial is available");
} else {
    Serial.println("GPIO 18/19 are available for sensors");
}

// Get current configuration
Serial.print("Board: ");
Serial.println(usbCdc.getBoardName());

Serial.print("Affected pins: ");
Serial.println(usbCdc.getAffectedPins());
```

### Using with Sensor/GPIO Code

When planning sensor connections:

```cpp
#if ARDUINO_USB_CDC_ON_BOOT == 1
  // GPIO 18/19 are locked for USB, use other pins
  const int I2C_SDA = 4;
  const int I2C_SCL = 5;
#else
  // GPIO 18/19 are available for use
  const int I2C_SDA = 18;
  const int I2C_SCL = 19;
#endif
```

Or better yet, use the runtime check:

```cpp
if (!usbCdc.isUsbCdcEnabled()) {
    // Safe to use GPIO 18/19
    Wire.begin(18, 19);  // SDA, SCL
} else {
    // Use alternate pins
    Wire.begin(4, 5);
}
```

## Deploy Tool Integration

The deploy tool (`deploy.py` or `deploy_gui.py`) handles USB CDC configuration:

### CLI
```
Menu → [U] USB CDC on boot
```
Shows:
```
[U] USB CDC on boot: ON — GPIO 18/19 locked for serial
    (press U to toggle)
[U] USB CDC on boot: OFF — GPIO 18/19 available as GPIO
```

### GUI
Settings panel → Check/uncheck "USB CDC on boot"

Shows board-specific info:
```
USB CDC on boot ☑
Controls GPIO 18/19 for USB
```

## Workflow

1. **Deploy Tool** — Toggle USB CDC setting before compilation
2. **Build** — Deploy tool applies flag to `platformio.ini`
3. **Flash** — Upload firmware with flag applied
4. **First Boot** — Device prompts user to confirm USB CDC preference
5. **Persistent** — Setting saved to NVS, survives updates

## First-Run Interactive Setup

Example output on first boot:

```
═══════════════════════════════════════════════════════
                   FIRST RUN SETUP
═══════════════════════════════════════════════════════

Board detected: ESP32-C3 SuperMini
USB pins: GPIO 18, 19

Choose USB CDC configuration:
  [1] Enable USB CDC (default)
      ✓ Easy serial debugging via USB cable
      ✗ USB pins locked for communication

  [2] Disable USB CDC
      ✓ USB pins available for GPIO/sensors
      ✗ No USB serial (use HTTP or UART logs)

Current setting: USB CDC ON (build time)
Enter 1 or 2 (default 1): 2

✓ USB CDC disabled. Pins 18/19 are available for GPIO.
  NOTE: Next firmware compilation must have USB CDC OFF.

┌─ USB Configuration ─────────────────────────────────
│ Board:            ESP32-C3 SuperMini
│ USB CDC on boot:  OFF (GPIO available)
│ Affected pins:    GPIO 18, 19
│
│ ✓ GPIO pins available for sensors/IO
│ ✗ No USB serial (use HTTP or UART for logs)
└─────────────────────────────────────────────────────
```

## How It Works

### Build Time
1. PlatformIO applies `-DARDUINO_USB_CDC_ON_BOOT=0|1` flag
2. ESP32 bootloader configures USB based on flag
3. Firmware compiles with knowledge of setting

### First Run
1. Module checks NVS for `first_run` key
2. If first run, displays interactive setup menu
3. Saves user choice to NVS
4. Prints status banner

### Subsequent Boots
1. Module loads preference from NVS
2. Prints status banner for confirmation
3. Application can query status via `usbCdc.isUsbCdcEnabled()`

## Technical Details

### NVS Storage
```cpp
Preferences prefs;
prefs.begin("usb_cdc", false);  // read-write mode
prefs.putBool("enabled", true);
prefs.putBool("first_run", false);
prefs.end();
```

### Board Detection
Uses PlatformIO build macros defined in `platformio.ini`:
```ini
-DARDUINO_SEEED_XIAO_ESP32C3
-DARDUINO_ESP32C3_DEV
-DARDUINO_ESP32S3_DEV
```

### Pin Mappings

**ESP32-C3 (all variants)**
- USB D+ (DP): GPIO 19
- USB D- (DM): GPIO 18

**ESP32-S3**
- USB D+ (DP): GPIO 20
- USB D- (DM): GPIO 19

## Troubleshooting

### "USB serial not detected"
- Check that `-DARDUINO_USB_CDC_ON_BOOT=1` was applied during build
- Use deploy tool to toggle setting and recompile
- Verify device cable is USB data capable (not power-only)

### "GPIO 18/19 don't work for sensors"
- Check if USB CDC is enabled
- Run `usbCdc.printStatus()` to confirm
- Recompile with `-DARDUINO_USB_CDC_ON_BOOT=0`

### "Settings not persisting after reboot"
- Check that NVS partition has enough space
- Run `nvs_flash_erase()` in setup to reset NVS
- Verify first-run setup completed successfully

## See Also

- `src/modules/UsbCdcModule.h` — Header with full API
- `src/modules/UsbCdcModule.cpp` — Implementation
- `tools/deploy.py` — Deploy tool for toggling USB CDC
- `platformio.ini` — Build flag configuration
