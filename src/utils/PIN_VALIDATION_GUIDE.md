// ============================================================================
// Pin Validation Integration Guide (Pillar 4.2 / 4.11)
// ============================================================================
//
// ARCHITECTURE:
// Do NOT scatter USB CDC conflict checks across sensor plugins.
// Instead, integrate validatePin() into the central SensorManager.
//
// RATIONALE:
// • Single source of truth for pin validation
// • Board-agnostic: works with all boards without code duplication
// • Graceful degradation: sensors fail cleanly, device boots safely
// • Non-magical: explicit validation logged, no implicit pin remapping
//
// ============================================================================

// Example: SensorManager integration (in SensorManager.cpp)
// ============================================================================

bool SensorManager::initializeSensor(const HardwareConfig& config, const String& sensorName) {
    int pin = config.pin;

    // ── Step 1: Validate pin (checks USB CDC conflicts + range)
    if (!validatePin(pin, sensorName)) {
        Serial.printf("[SensorManager] SKIP: %s (pin %d validation failed)\n", 
                      sensorName.c_str(), pin);
        return false;  // Gracefully skip this sensor
    }

    // ── Step 2: Initialize sensor (only if pin is valid)
    // ... sensor-specific initialization code ...

    Serial.printf("[SensorManager] OK: %s initialized on pin %d\n", 
                  sensorName.c_str(), pin);
    return true;
}

// ============================================================================
// Example: platform_config.json with USB CDC conflict
// ============================================================================

/*
{
  "hardware": {
    "i2c_sda": 18,    <-- USER ERROR: Configured I2C on GPIO 18
    "i2c_scl": 19,    <-- USER ERROR: Configured I2C on GPIO 19
  }
}

At runtime with USB CDC enabled:

[validatePin] CONFLICT: Pin 18 reserved for USB CDC (usage: I2C_SDA)
              USB pins on this board: 18,19
              Disable USB CDC in deploy tool before using these pins

[SensorManager] SKIP: I2C_SDA (pin 18 validation failed)
[SensorManager] SKIP: I2C_SCL (pin 19 validation failed)

Result:
• Device boots normally (no crash)
• User sees clear error in logs explaining the issue
• User can fix config OR toggle USB CDC in deploy tool
• Next compile applies the change and device boots OK
*/

// ============================================================================
// Call sites in SensorManager::begin()
// ============================================================================

void SensorManager::begin() {
    // ... existing code ...

    // Initialize sensors with validation
    if (!initializeSensor(cfg.hardware.i2c_config, "I2C")) {
        // Log is already printed in initializeSensor()
    }

    if (!initializeSensor(cfg.hardware.uart_config, "UART")) {
        // Log is already printed in initializeSensor()
    }

    for (const auto& sensor : cfg.sensors) {
        if (!initializeSensor(sensor, sensor.name)) {
            // Log is already printed in initializeSensor()
        }
    }

    // ... rest of initialization ...
}

// ============================================================================
// Benefits of Centralized Validation
// ============================================================================

✓ NO MAGIC
  • validatePin() rejects invalid pins
  • No automatic remapping (physical wires don't move)
  • User must fix config or change build flags

✓ CENTRALIZED
  • Single function for all pin validation
  • USB CDC check happens once at pin init time
  • Future board-specific checks added in one place

✓ GRACEFUL DEGRADATION
  • Sensor init fails, device boots
  • Clear error message in logs
  • User can fix at runtime (config) or build time (USB CDC)

✓ TESTABLE
  • validatePin() behavior is deterministic
  • Can unit test with different USB CDC states
  • No side effects or global state changes

✓ DOCUMENTABLE
  • Users understand exactly what failed and why
  • Error message includes the solution (disable USB CDC)
  • No hidden assumptions

// ============================================================================
// Future Enhancements (Pillar 4.2)
// ============================================================================

In validatePin(), as your Pillar 4.2 validation framework grows:

1. Add reserved pin list from platform_config.json
2. Add "already used" tracking (prevent pin reuse)
3. Add voltage-level validation (3.3V pins only)
4. Add ADC-specific validation
5. Add GPIO drive-strength checks

All still centralized in validatePin() — no changes to sensor code.
