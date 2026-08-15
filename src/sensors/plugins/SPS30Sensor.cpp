#include "SPS30Sensor.h"
#include "../I2CBus.h"
#include <string.h>
#include <math.h>
#include "../../core/BoardProfiles.h"   // R11: validateAttachPin
#include "../SensorManager.h"           // R17: _claimI2cAddress

// CRC-8 per Sensirion: poly=0x31, init=0xFF (same as SCD4x).
uint8_t SPS30Sensor::_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

bool SPS30Sensor::_sendCmd(uint16_t cmd) {
    _wire->beginTransmission(ADDR);
    _wire->write(cmd >> 8);
    _wire->write(cmd & 0xFF);
    return _wire->endTransmission() == 0;
}

// Command + one 16-bit argument word, each followed by its CRC (used to start
// measurement with the float output format).
bool SPS30Sensor::_sendCmdArg(uint16_t cmd, uint16_t arg) {
    uint8_t a[2] = { (uint8_t)(arg >> 8), (uint8_t)(arg & 0xFF) };
    _wire->beginTransmission(ADDR);
    _wire->write(cmd >> 8);
    _wire->write(cmd & 0xFF);
    _wire->write(a[0]);
    _wire->write(a[1]);
    _wire->write(_crc8(a, 2));
    return _wire->endTransmission() == 0;
}

// Command + a 32-bit argument sent as two CRC-tagged words (auto-clean
// interval). Sensirion CRCs each word independently, not the pair.
bool SPS30Sensor::_sendCmdArg32(uint16_t cmd, uint32_t arg) {
    uint8_t hi[2] = { (uint8_t)(arg >> 24), (uint8_t)(arg >> 16) };
    uint8_t lo[2] = { (uint8_t)(arg >> 8),  (uint8_t)(arg & 0xFF) };
    _wire->beginTransmission(ADDR);
    _wire->write(cmd >> 8);
    _wire->write(cmd & 0xFF);
    _wire->write(hi[0]); _wire->write(hi[1]); _wire->write(_crc8(hi, 2));
    _wire->write(lo[0]); _wire->write(lo[1]); _wire->write(_crc8(lo, 2));
    return _wire->endTransmission() == 0;
}

bool SPS30Sensor::_readWords(uint16_t* words, int count) {
    _wire->requestFrom((int)ADDR, count * 3);
    for (int i = 0; i < count; i++) {
        if (_wire->available() < 3) return false;
        uint8_t msb = _wire->read();
        uint8_t lsb = _wire->read();
        uint8_t crc = _wire->read();
        uint8_t buf[2] = { msb, lsb };
        if (_crc8(buf, 2) != crc) return false;
        words[i] = ((uint16_t)msb << 8) | lsb;
    }
    return true;
}

bool SPS30Sensor::_dataReady() {
    if (!_sendCmd(CMD_READ_DATA_READY)) return false;
    delay(1);
    uint16_t status;
    if (!_readWords(&status, 1)) return false;
    return (status & 0x0001) != 0;   // LSB set → a new measurement is ready
}

// ---------------------------------------------------------------------------
// Device status register (0xD206) — two CRC-tagged words, MSW first.
bool SPS30Sensor::_readStatus(uint32_t& raw) {
    if (!_sendCmd(CMD_READ_STATUS)) return false;
    delay(1);
    uint16_t w[2];
    if (!_readWords(w, 2)) return false;
    raw = ((uint32_t)w[0] << 16) | w[1];
    return true;
}

// Poll the status register when due and fold the raw bits into _status.
// Never clears the register (0xD210): FAN and LASER are latched fault bits,
// and clearing them here would hide a fault that occurred between polls.
void SPS30Sensor::_pollStatus() {
    if ((int32_t)(millis() - _nextStatusMs) < 0) return;
    _nextStatusMs = millis() + _statusIntervalMs;

    uint32_t raw = 0;
    if (!_readStatus(raw)) {
        _status = STATUS_READ_FAILED;
        Serial.println("[SPS30] status register read failed");
        return;
    }

    uint8_t s = STATUS_OK;
    if (raw & (1UL << RAW_BIT_FAN_ERROR))   s |= STATUS_FAN_ERROR;
    if (raw & (1UL << RAW_BIT_LASER_ERROR)) s |= STATUS_LASER_ERROR;
    if (raw & (1UL << RAW_BIT_FAN_SPEED))   s |= STATUS_FAN_SPEED;

    // Log only on change — this runs once a minute for the life of the device.
    if (s != _status) {
        if (s == STATUS_OK) {
            Serial.println("[SPS30] device status recovered — OK");
        } else {
            Serial.printf("[SPS30] device status 0x%02X (raw 0x%08lX)%s%s%s\n",
                          s, (unsigned long)raw,
                          (s & STATUS_FAN_ERROR)   ? " FAN"        : "",
                          (s & STATUS_LASER_ERROR) ? " LASER"      : "",
                          (s & STATUS_FAN_SPEED)   ? " FAN_SPEED"  : "");
        }
    }
    _status = s;
}

// ---------------------------------------------------------------------------
// Auto-clean interval. Writing 0x8004 resets the device's internal runtime
// counter, so read the stored value first and write only on a real change —
// otherwise every reboot would push the next clean out by a full interval and
// a device that reboots more often than it cleans would never clean at all.
void SPS30Sensor::_applyAutoCleanInterval(int32_t wantSeconds) {
    if (wantSeconds < 0) return;    // caller asked to leave the setting alone

    uint32_t current = 0;
    bool haveCurrent = false;
    if (_sendCmd(CMD_AUTOCLEAN_INTV)) {
        delay(5);
        uint16_t w[2];
        if (_readWords(w, 2)) {
            current     = ((uint32_t)w[0] << 16) | w[1];
            haveCurrent = true;
        }
    }

    if (haveCurrent && current == (uint32_t)wantSeconds) {
        Serial.printf("[SPS30] auto-clean interval already %lus — counter preserved\n",
                      (unsigned long)current);
        return;
    }

    if (_sendCmdArg32(CMD_AUTOCLEAN_INTV, (uint32_t)wantSeconds)) {
        delay(20);
        Serial.printf("[SPS30] auto-clean interval set to %lus (was %s)\n",
                      (unsigned long)wantSeconds,
                      haveCurrent ? String((unsigned long)current).c_str() : "unknown");
    } else {
        Serial.println("[SPS30] failed to write auto-clean interval");
    }
}

// ---------------------------------------------------------------------------
bool SPS30Sensor::init(JsonObjectConst cfg) {
    _enabled          = cfg["enabled"]            | true;
    _intervalMs       = cfg["read_interval_ms"]   | 5000;
    _statusIntervalMs = cfg["status_interval_ms"] | 60000;
    if (_statusIntervalMs < 1000) _statusIntervalMs = 1000;   // don't flood the bus

    int sda = cfg["sda"] | -1;
    int scl = cfg["scl"] | -1;
    // I2C bus selection. acquire() validates both pins against the board
    // profile, rejects a bus this chip does not have, and refuses a bus
    // already brought up on different pins.
    _bus  = (uint8_t)(cfg["bus"] | 0);
    _wire = I2CBus::acquire(_bus, sda, scl, "sps30");
    if (!_wire) return false;
    if (!_claimI2cAddress(_bus, ADDR, this)) {
        Serial.printf("[SPS30] I2C address 0x%02X already claimed on bus %u — refusing init\n", ADDR, (unsigned)_bus);
        return false;
    }

    JsonObjectConst cal = cfg["calibration"];
    _calPm1.load(cal,  "pm1");
    _calPm25.load(cal, "pm25");
    _calPm4.load(cal,  "pm4");
    _calPm10.load(cal, "pm10");

    // Wake the sensor in case a previous session left it asleep (while idle the
    // wake-up command is simply NACKed — harmless), then stop any running
    // measurement for a clean state.
    _sendCmd(CMD_WAKE);
    delay(5);
    _sendCmd(CMD_STOP_MEASURE);
    delay(20);

    // Auto-clean interval is configured in idle mode, before measurement starts.
    int32_t autoClean = cfg["auto_clean_interval_s"] | -1;
    if (autoClean > 0 && (autoClean < 10 || autoClean > 604800)) {
        Serial.printf("[SPS30] auto_clean_interval_s %ld out of range (10..604800) — ignored\n",
                      (long)autoClean);
        autoClean = -1;
    }
    _applyAutoCleanInterval(autoClean);

    // Start measurement in IEEE-754 float output format.
    if (!_sendCmdArg(CMD_START_MEASURE, ARG_FLOAT_FORMAT)) {
        Serial.println("[SPS30] Not found / no ACK at 0x69");
        return false;
    }

    // Defer the fan spin-up out of init(); readAll() gates on _warmupUntilMs.
    // SPS30 yields a new sample every ~1 s, but the first few seconds are
    // unstable until the fan reaches speed.
    _warmupUntilMs = millis() + 8000;

    // Optional boot-time fan clean — only valid in measurement mode. Extends
    // the blackout so the high-speed cleaning burst is never sampled.
    if (cfg["clean_on_boot"] | false) {
        delay(100);
        if (_sendCmd(CMD_START_FAN_CLEAN)) {
            _warmupUntilMs = millis() + FAN_CLEAN_MS + 8000;
            Serial.println("[SPS30] fan cleaning started (boot)");
        } else {
            Serial.println("[SPS30] fan clean command refused");
        }
    }

    // First status poll one interval in, not immediately: the register can
    // still carry start-up transients while the fan ramps.
    _nextStatusMs = millis() + _statusIntervalMs;
    _status       = STATUS_OK;
    _ready        = true;
    Serial.println("[SPS30] Started — first reading in ~8s");
    return true;
}

bool SPS30Sensor::read(SensorReading& out) {
    SensorReading buf[5];
    if (readAll(buf, 5) < 2) return false;
    out = buf[1];   // PM2.5 — the most-used single metric
    return true;
}

int SPS30Sensor::readAll(SensorReading* out, int maxOut) {
    if (!_ready || maxOut < 5) return 0;
    if ((int32_t)(millis() - _warmupUntilMs) < 0) return 0;   // warm-up / fan clean

    if (!_dataReady()) return 0;

    if (!_sendCmd(CMD_READ_MEASURED)) return 0;
    delay(1);

    // Float output format streams 10 floats; the four mass concentrations
    // (PM1.0, PM2.5, PM4.0, PM10) come first. Each float is two CRC-checked
    // 16-bit words, so read the first 8 words and drop the rest of the frame.
    uint16_t w[8];
    if (!_readWords(w, 8)) return 0;

    auto toFloat = [](uint16_t hi, uint16_t lo) -> float {
        uint32_t bits = ((uint32_t)hi << 16) | lo;   // big-endian IEEE-754 word
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    };
    float pm1  = toFloat(w[0], w[1]);
    float pm25 = toFloat(w[2], w[3]);
    float pm4  = toFloat(w[4], w[5]);
    float pm10 = toFloat(w[6], w[7]);

    // Reject NaN/inf or negative values — a CRC-passed-but-corrupt/stale frame.
    if (!isfinite(pm1) || !isfinite(pm25) || !isfinite(pm4) || !isfinite(pm10)) return 0;
    if (pm1 < 0.0f || pm25 < 0.0f || pm4 < 0.0f || pm10 < 0.0f) return 0;

    // Health poll last: a failure here must not cost us an otherwise good PM
    // sample, it only sets the status bits published alongside it.
    _pollStatus();

    out[0] = SensorReading::make(0, _id, getType(), "pm1",  _calPm1.apply(pm1),   "ug/m3");
    out[1] = SensorReading::make(0, _id, getType(), "pm25", _calPm25.apply(pm25), "ug/m3");
    out[2] = SensorReading::make(0, _id, getType(), "pm4",  _calPm4.apply(pm4),   "ug/m3");
    out[3] = SensorReading::make(0, _id, getType(), "pm10", _calPm10.apply(pm10), "ug/m3");
    // Published as QUALITY_GOOD even when non-zero: the metric's whole job is
    // to carry the fault code. Marking it QUALITY_ERROR would make
    // ProcessingTask drop it from the ring buffer and skip alert evaluation —
    // exactly when it matters most.
    out[4] = SensorReading::make(0, _id, getType(), "device_status", (float)_status, "");
    return 5;
}
