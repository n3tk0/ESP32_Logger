# SensorManager Integration with USB CDC Validation

## Overview

SensorManager automatically integrates USB CDC pin conflict detection through the existing `validateAttachPin()` function. **No changes to sensor plugins are required.**

## Architecture

```
Sensor Plugin        SensorManager      Validation Layer      USB CDC Module
─────────────────────────────────────────────────────────────────────────────
  s->init()
  (from JSON)
       │
       ├─ validateAttachPin(sda, ...)
       │      │
       │      ├─ validatePin(sda)  ◄──── Runtime USB CDC check
       │      │      │
       │      │      └─ isUsbPinLocked(sda)?
       │      │
       │      └─ isPinAllowed()  ◄───── Static board profile checks
       │
       └─ Sensor initialized (or skipped if validation failed)
```

## How It Works

### 1. Plugin Calls validateAttachPin()

Every sensor plugin that uses GPIO/I2C/UART calls `validateAttachPin()`:

```cpp
// In BME280Sensor::init()
int sda = cfg["sda"] | -1;
int scl = cfg["scl"] | -1;

if (!validateAttachPin(sda, "bme280", "sda")) {
    return false;  // Gracefully skip sensor
}
if (!validateAttachPin(scl, "bme280", "scl")) {
    return false;  // Gracefully skip sensor
}
```

### 2. validateAttachPin() Integrates USB CDC Check

In `BoardProfiles.cpp`:

```cpp
bool validateAttachPin(int pin, const char* sensorId, const char* fieldName) {
    // ... existing checks ...

    // ── Centralized pin validation (Pillar 4.2/4.11) ────────────────────
    // Includes USB CDC runtime conflict detection
    String usage = String(sensorId) + "." + String(fieldName);
    if (!validatePin(pin, usage)) {
        // validatePin() already logged the conflict details
        return false;
    }

    // ... more checks ...
    return true;
}
```

### 3. validatePin() Checks USB CDC State

In `Utils.cpp`:

```cpp
bool validatePin(int pin, const String& usage) {
    // Check if pin is locked by USB CDC
    if (usbCdc.isUsbPinLocked(pin)) {
        Serial.printf("[validatePin] CONFLICT: Pin %d reserved for USB CDC (usage: %s)\n",
                      pin, usage.c_str());
        Serial.printf("              USB pins on this board: %s\n",
                      usbCdc.getUsbPins().c_str());
        return false;
    }
    // ... other checks ...
    return true;
}
```

### 4. SensorManager Gracefully Handles Failure

In `SensorManager.cpp`:

```cpp
bool SensorManager::loadAndInit(fs::FS& fs, const char* cfgPath) {
    // ... load JSON ...

    for (JsonObject sensor : arr) {
        ISensor* s = _createPlugin(type);

        s->setId(id);
        if (s->init(sensor)) {  // ◄── Sensor calls validateAttachPin()
            _sensors[_count++] = s;
            initialised++;
            Serial.printf("[SensorManager] Sensor '%s' ready\n", id);
        } else {
            // Validation failed (USB CDC conflict, pin not assigned, etc.)
            Serial.printf("[SensorManager] Sensor '%s' init FAILED\n", id);
            _releaseSerial1(s);
            _releaseI2cClaims(s);
            delete s;
        }
    }

    return initialised > 0;
}
```

## Error Scenarios

### Scenario 1: User Configures I2C on GPIO 18 with USB CDC ON

**platform_config.json:**
```json
{
  "sensors": [
    {
      "id": "env",
      "type": "bme280",
      "enabled": true,
      "sda": 18,
      "scl": 19
    }
  ]
}
```

**Device boots with USB CDC ON:**
```
[SensorManager] Sensor 'env' init FAILED
[validatePin] CONFLICT: Pin 18 reserved for USB CDC (usage: bme280.sda)
              USB pins on this board: 18,19
              Disable USB CDC in deploy tool before using these pins
[SensorManager] 0/1 sensors initialised
```

**Device boots successfully** (no crash), but BME280 is skipped.

**User sees logs and knows:**
1. Which pin caused the conflict (18)
2. Which pins are reserved (18, 19)
3. How to fix it (disable USB CDC in deploy tool)

### Scenario 2: User Disables USB CDC, Recompiles

1. User toggles [U] in deploy tool
2. User recompiles (step 5)
3. Device reboots with USB CDC OFF
4. validatePin(18) checks `usbCdc.isUsbPinLocked(18)` → false
5. Pin validation passes
6. BME280 initializes successfully

```
[validatePin] OK: Pin 18 valid for bme280.sda
[bme280] ready at 0x76
[SensorManager] Sensor 'env' ready
[SensorManager] 1/1 sensors initialised
```

## For Sensor Plugin Authors

### When Writing a New Sensor Plugin

Simply call `validateAttachPin()` for any pins you use:

```cpp
bool MySensor::init(JsonObjectConst cfg) {
    int pin = cfg["my_pin"] | -1;

    // Validate pin (checks USB CDC conflicts + board profile)
    if (!validateAttachPin(pin, "my_sensor", "my_pin")) {
        return false;  // Gracefully fail, SensorManager skips us
    }

    // Safe to initialize hardware
    pinMode(pin, OUTPUT);
    // ... rest of init ...

    return true;
}
```

**That's it.** No need to check USB CDC directly—validateAttachPin() handles it.

### Multiple Pins

```cpp
bool MyI2cSensor::init(JsonObjectConst cfg) {
    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;

    // Validate both pins
    if (!validateAttachPin(sda, "my_i2c", "sda")) return false;
    if (!validateAttachPin(scl, "my_i2c", "scl")) return false;

    // Both pins are valid
    Wire.begin(sda, scl);
    // ...
}
```

### UART Pins (Serial1)

```cpp
bool MyUartSensor::init(JsonObjectConst cfg) {
    int rx = cfg["rx"] | -1;
    int tx = cfg["tx"] | -1;

    // Validate pins (includes USB CDC check)
    if (!validateAttachPin(rx, "uart_sensor", "rx")) return false;
    if (!validateAttachPin(tx, "uart_sensor", "tx")) return false;

    // Safe to use pins
    Serial1.begin(9600, SERIAL_8N1, rx, tx);
    // ...
}
```

### ADC/GPIO Pins

```cpp
bool MySoilSensor::init(JsonObjectConst cfg) {
    int adcPin = cfg["adc_pin"] | -1;

    // Validate pin
    if (!validateAttachPin(adcPin, "soil_sensor", "adc")) return false;

    // Safe to initialize
    analogRead(adcPin);
    // ...
}
```

## Error Handling Best Practices

### ✓ DO: Log and Return False

```cpp
if (!validateAttachPin(pin, "my_sensor", "pin")) {
    return false;  // SensorManager logs and cleans up
}
```

### ✗ DON'T: Try to Recover

```cpp
// Don't try to automatically pick a different pin
if (!validateAttachPin(pin, "sensor", "pin")) {
    pin = 4;  // WRONG: physical wires don't move!
    // ...
}
```

### ✗ DON'T: Ignore Validation

```cpp
int pin = cfg["pin"] | -1;
digitalWrite(pin, HIGH);  // WRONG: might crash if pin -1 or locked
```

### ✗ DON'T: Duplicate Checks

```cpp
if (!validateAttachPin(pin, "sensor", "pin")) return false;
if (pin == 18 || pin == 19) {  // WRONG: already checked!
    return false;
}
```

## Graceful Degradation Examples

### Example 1: Optional Environmental Sensor

```cpp
// Sensor is not critical — if pins conflict, device still works

for (JsonObject sensor_cfg : cfg["sensors"]) {
    ISensor* s = createSensor(sensor_cfg);
    if (!s->init(sensor_cfg)) {
        // Validation failed (USB CDC, unassigned pin, etc.)
        // Log is already printed by validateAttachPin()
        delete s;
        continue;  // Skip to next sensor
    }
    // Sensor initialized successfully
    sensorManager.add(s);
}
```

### Example 2: Network of Multiple Sensors

```cpp
int ok = 0, failed = 0;

for (JsonObject sensor_cfg : cfg["sensors"]) {
    ISensor* s = createSensor(sensor_cfg);
    if (s->init(sensor_cfg)) {
        sensorManager.add(s);
        ok++;
    } else {
        // Validation or init failed
        failed++;
        delete s;
    }
}

Serial.printf("[setup] %d sensors OK, %d failed\n", ok, failed);
if (ok > 0) {
    // At least some sensors working — proceed
} else {
    // All sensors failed — might want to alert user
}
```

## Debugging USB CDC Conflicts

### Check Device Logs

```
[validatePin] CONFLICT: Pin 18 reserved for USB CDC (usage: bme280.sda)
              USB pins on this board: 18,19
              Disable USB CDC in deploy tool before using these pins
```

### Find USB Pins on Your Board

Run the device and watch boot logs:

```
┌─ USB Configuration ─────────────────────────────────────
│ Board:            ESP32-C3 SuperMini
│ USB CDC on boot:  ON — locked for serial
│ Affected pins:    GPIO 18, 19
└─────────────────────────────────────────────────────────
```

### Toggle USB CDC

```bash
# In deploy tool menu
[U] USB CDC on boot: ON — GPIO 18/19 locked for serial
    (press U to toggle)

# Or in GUI Settings
☑ USB CDC on boot
  Controls GPIO 18/19 for USB
```

## Testing

### Unit Test Pattern

```cpp
// Test that validatePin rejects USB CDC pins when enabled
void test_usb_cdc_conflict() {
    usbCdc.setEnabled(true);
    
    // These should fail (USB CDC enabled)
    assert(!validatePin(18, "test"));
    assert(!validatePin(19, "test"));
    
    // These should pass
    assert(validatePin(4, "test"));
    assert(validatePin(5, "test"));
}
```

### Integration Test Pattern

```cpp
// Test sensor gracefully skips if pins conflict
void test_bme280_with_conflict() {
    JsonDocument cfg_json;
    cfg_json["type"] = "bme280";
    cfg_json["sda"] = 18;  // Conflict with USB CDC ON
    cfg_json["scl"] = 19;
    
    ISensor* sensor = new BME280Sensor();
    bool ok = sensor->init(cfg_json);
    
    assert(!ok);  // Should fail validation
    delete sensor;
}
```

## Summary

- **No plugin changes needed** — existing validateAttachPin() integration is automatic
- **Clear error messages** — users know exactly what went wrong and how to fix it
- **Graceful degradation** — sensors skip cleanly, device boots
- **Centralized logic** — single validatePin() function maintains all rules
- **Pillar 4.2 compliant** — follows architectural standards

The system is production-ready and requires no additional work from sensor plugin developers.
