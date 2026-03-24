// ============================================================================
// DS18B20_Mini — Minimal 1-Wire DS18B20 driver (no OneWire/DallasTemperature)
// Based on Maxim DS18B20 datasheet (19-7487 Rev 6)
// Supports: multiple sensors on one bus, configurable resolution (9-12 bit)
// ============================================================================
#pragma once
#include <Arduino.h>

class DS18B20_Mini {
public:
    static constexpr int MAX_SENSORS = 8;
    static constexpr float DISCONNECTED = -127.0f;

    bool begin(uint8_t pin, uint8_t resolution = 12) {
        _pin = pin;
        _resolution = constrain(resolution, 9, 12);
        pinMode(_pin, INPUT_PULLUP);

        // Enumerate devices on bus
        _count = 0;
        uint8_t addr[8];
        _searchReset();
        while (_count < MAX_SENSORS && _search(addr)) {
            if (addr[0] != 0x28 && addr[0] != 0x10) continue;  // DS18B20=0x28, DS18S20=0x10
            if (_crc8(addr, 7) != addr[7]) continue;
            memcpy(_romCodes[_count], addr, 8);
            _count++;
        }

        // Set resolution on all devices
        if (_count > 0) {
            _reset();
            _write(0xCC);  // Skip ROM (broadcast)
            _write(0x4E);  // Write Scratchpad
            _write(0x00);  // TH (unused)
            _write(0x00);  // TL (unused)
            // Config register: bits 6:5 = resolution - 9
            _write(((_resolution - 9) & 0x03) << 5 | 0x1F);
        }

        return _count > 0;
    }

    int deviceCount() const { return _count; }

    void requestTemperatures() {
        _reset();
        _write(0xCC);  // Skip ROM (all devices)
        _write(0x44);  // Convert T
    }

    // Conversion time in ms for current resolution
    uint32_t conversionTimeMs() const {
        switch (_resolution) {
            case 9:  return 95;
            case 10: return 190;
            case 11: return 380;
            default: return 760;
        }
    }

    float getTempC(int index) {
        if (index < 0 || index >= _count) return DISCONNECTED;

        _reset();
        _write(0x55);  // Match ROM
        for (int i = 0; i < 8; i++) _write(_romCodes[index][i]);
        _write(0xBE);  // Read Scratchpad

        uint8_t data[9];
        for (int i = 0; i < 9; i++) data[i] = _readByte();

        if (_crc8(data, 8) != data[8]) return DISCONNECTED;

        int16_t raw = (int16_t)(data[1] << 8 | data[0]);

        // DS18S20 (family 0x10) uses 9-bit only
        if (_romCodes[index][0] == 0x10) {
            raw = raw << 3;  // 12-bit equivalent
            if (data[7] == 0x10) {
                raw = (raw & 0xFFF0) + 12 - data[6];
            }
        } else {
            // Mask unused bits per resolution
            switch (_resolution) {
                case 9:  raw &= ~7; break;
                case 10: raw &= ~3; break;
                case 11: raw &= ~1; break;
            }
        }

        return (float)raw / 16.0f;
    }

private:
    uint8_t _pin = 0;
    uint8_t _resolution = 12;
    int     _count = 0;
    uint8_t _romCodes[MAX_SENSORS][8];

    // Search state
    int     _lastDiscrep = 0;
    bool    _lastDone = false;
    uint8_t _lastROM[8];

    // --- 1-Wire low-level timing ---
    // All timing per DS18B20 datasheet Table 2 (standard speed)

    bool _reset() {
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        delayMicroseconds(480);
        pinMode(_pin, INPUT_PULLUP);
        delayMicroseconds(70);
        bool present = (digitalRead(_pin) == LOW);
        delayMicroseconds(410);
        return present;
    }

    void _writeBit(bool bit) {
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        if (bit) {
            delayMicroseconds(6);
            pinMode(_pin, INPUT_PULLUP);
            delayMicroseconds(64);
        } else {
            delayMicroseconds(60);
            pinMode(_pin, INPUT_PULLUP);
            delayMicroseconds(10);
        }
    }

    bool _readBit() {
        pinMode(_pin, OUTPUT);
        digitalWrite(_pin, LOW);
        delayMicroseconds(3);
        pinMode(_pin, INPUT_PULLUP);
        delayMicroseconds(10);
        bool bit = digitalRead(_pin);
        delayMicroseconds(53);
        return bit;
    }

    void _write(uint8_t byte) {
        for (int i = 0; i < 8; i++) {
            _writeBit(byte & 0x01);
            byte >>= 1;
        }
    }

    uint8_t _readByte() {
        uint8_t byte = 0;
        for (int i = 0; i < 8; i++) {
            if (_readBit()) byte |= (1 << i);
        }
        return byte;
    }

    // --- ROM search algorithm (from Maxim AN187) ---
    void _searchReset() {
        _lastDiscrep = 0;
        _lastDone = false;
        memset(_lastROM, 0, 8);
    }

    bool _search(uint8_t* addr) {
        if (_lastDone) return false;

        if (!_reset()) {
            _searchReset();
            return false;
        }

        _write(0xF0);  // Search ROM command

        int lastZero = 0;
        uint8_t romByte = 0;

        for (int i = 1; i <= 64; i++) {
            bool id_bit      = _readBit();
            bool cmp_id_bit  = _readBit();

            if (id_bit && cmp_id_bit) {
                // No devices
                _searchReset();
                return false;
            }

            bool dir;
            if (id_bit != cmp_id_bit) {
                dir = id_bit;
            } else {
                // Discrepancy
                if (i < _lastDiscrep) {
                    dir = ((_lastROM[(i - 1) / 8] >> ((i - 1) % 8)) & 1);
                } else {
                    dir = (i == _lastDiscrep);
                }
                if (!dir) lastZero = i;
            }

            if (dir) {
                romByte |= (1 << ((i - 1) % 8));
            } else {
                romByte &= ~(1 << ((i - 1) % 8));
            }

            _writeBit(dir);

            if ((i % 8) == 0) {
                _lastROM[(i / 8) - 1] = romByte;
                romByte = 0;
            }
        }

        _lastDiscrep = lastZero;
        if (_lastDiscrep == 0) _lastDone = true;

        memcpy(addr, _lastROM, 8);
        return true;
    }

    // Dow/Dallas CRC-8
    static uint8_t _crc8(const uint8_t* data, uint8_t len) {
        uint8_t crc = 0;
        for (uint8_t i = 0; i < len; i++) {
            uint8_t inbyte = data[i];
            for (uint8_t j = 0; j < 8; j++) {
                uint8_t mix = (crc ^ inbyte) & 0x01;
                crc >>= 1;
                if (mix) crc ^= 0x8C;
                inbyte >>= 1;
            }
        }
        return crc;
    }
};
