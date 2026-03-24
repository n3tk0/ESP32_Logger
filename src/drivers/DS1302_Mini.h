// ============================================================================
// DS1302_Mini — Minimal DS1302 RTC driver (no Makuna Rtc dependency)
// Based on Dallas DS1302 datasheet
// Provides: ThreeWire, RtcDateTime, RtcDS1302 — API-compatible replacements
// ============================================================================
#pragma once
#include <Arduino.h>

// ── RtcDateTime — date/time container ─────────────────────────────────────────
class RtcDateTime {
private:
    static constexpr uint32_t SECONDS_PER_DAY    = 86400UL;
    static constexpr uint32_t SECONDS_PER_HOUR   = 3600UL;
    static constexpr uint32_t SECONDS_PER_MINUTE = 60UL;

public:
    RtcDateTime() : _year(2000), _month(1), _day(1), _hour(0), _minute(0), _second(0) {}

    RtcDateTime(uint16_t year, uint8_t month, uint8_t day,
                uint8_t hour, uint8_t minute, uint8_t second)
        : _year(year), _month(month), _day(day),
          _hour(hour), _minute(minute), _second(second) {}

    // Construct from __DATE__ and __TIME__ compile macros
    RtcDateTime(const char* date, const char* time) {
        // __DATE__ = "Mar 24 2026"
        static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = {};
        int d, y, h, m, s;
        sscanf(date, "%3s %d %d", mon, &d, &y);
        sscanf(time, "%d:%d:%d", &h, &m, &s);
        const char* p = strstr(months, mon);
        _month  = p ? ((p - months) / 3 + 1) : 1;
        _year   = y;
        _day    = d;
        _hour   = h;
        _minute = m;
        _second = s;
    }

    uint16_t Year()   const { return _year; }
    uint8_t  Month()  const { return _month; }
    uint8_t  Day()    const { return _day; }
    uint8_t  Hour()   const { return _hour; }
    uint8_t  Minute() const { return _minute; }
    uint8_t  Second() const { return _second; }

    bool IsValid() const {
        if (_year < 2000 || _year > 2099) return false;
        if (_month < 1 || _month > 12) return false;
        if (_hour > 23 || _minute > 59 || _second > 59) return false;
        uint8_t dim = _daysInMonth(_year, _month);
        return _day >= 1 && _day <= dim;
    }

    uint32_t Unix32Time() const {
        if (!IsValid()) return 0;
        uint32_t days = 0;
        for (uint16_t y = 1970; y < _year; y++) {
            days += _isLeapYear(y) ? 366 : 365;
        }
        for (uint8_t m = 1; m < _month; m++) {
            days += _daysInMonth(_year, m);
        }
        days += (uint32_t)(_day - 1);
        return days * SECONDS_PER_DAY + (uint32_t)_hour * SECONDS_PER_HOUR + (uint32_t)_minute * SECONDS_PER_MINUTE + (uint32_t)_second;
    }

    void InitWithUnix32Time(uint32_t ts) {
        uint32_t days = ts / SECONDS_PER_DAY;
        uint32_t rem  = ts % SECONDS_PER_DAY;

        _hour   = (uint8_t)(rem / SECONDS_PER_HOUR);
        rem    %= SECONDS_PER_HOUR;
        _minute = (uint8_t)(rem / SECONDS_PER_MINUTE);
        _second = (uint8_t)(rem % SECONDS_PER_MINUTE);

        uint16_t year = 1970;
        while (true) {
            uint16_t diy = _isLeapYear(year) ? 366 : 365;
            if (days < diy) break;
            days -= diy;
            year++;
        }

        uint8_t month = 1;
        while (true) {
            uint8_t dim = _daysInMonth(year, month);
            if (days < dim) break;
            days -= dim;
            month++;
        }

        _year  = year;
        _month = month;
        _day   = (uint8_t)(days + 1);
    }

private:
    uint16_t _year;
    uint8_t  _month, _day, _hour, _minute, _second;

    static bool _isLeapYear(uint16_t year) {
        return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    }

    static uint8_t _daysInMonth(uint16_t year, uint8_t month) {
        static const uint8_t kDaysInMonth[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (month == 2 && _isLeapYear(year)) return 29;
        return kDaysInMonth[month - 1];
    }
};

// ── ThreeWire — 3-wire serial bus (IO/DAT, SCLK, CE/RST) ────────────────────
class ThreeWire {
public:
    ThreeWire(uint8_t io, uint8_t sclk, uint8_t ce)
        : _io(io), _sclk(sclk), _ce(ce) {}

    void begin() {
        pinMode(_ce,   OUTPUT);
        pinMode(_sclk, OUTPUT);
        digitalWrite(_ce,   LOW);
        digitalWrite(_sclk, LOW);
    }

    void beginTransaction() {
        digitalWrite(_ce, HIGH);
        delayMicroseconds(4);
    }

    void endTransaction() {
        digitalWrite(_ce, LOW);
        delayMicroseconds(4);
    }

    void writeByte(uint8_t val) {
        pinMode(_io, OUTPUT);
        for (int i = 0; i < 8; i++) {
            digitalWrite(_io, (val >> i) & 1);
            delayMicroseconds(1);
            digitalWrite(_sclk, HIGH);
            delayMicroseconds(1);
            digitalWrite(_sclk, LOW);
        }
    }

    uint8_t readByte() {
        pinMode(_io, INPUT);
        uint8_t val = 0;
        for (int i = 0; i < 8; i++) {
            if (digitalRead(_io)) val |= (1 << i);
            delayMicroseconds(1);
            digitalWrite(_sclk, HIGH);
            delayMicroseconds(1);
            digitalWrite(_sclk, LOW);
        }
        return val;
    }

private:
    uint8_t _io, _sclk, _ce;
    friend class RtcDS1302_Base;
};

// ── RtcDS1302 — DS1302 real-time clock driver ────────────────────────────────
// Templated to match Makuna API: RtcDS1302<ThreeWire>
template <typename TWire>
class RtcDS1302 {
public:
    RtcDS1302(TWire& wire) : _w(wire) {}

    void Begin() {
        _w.begin();
    }

    bool GetIsWriteProtected() {
        return (_readReg(0x8F) & 0x80) != 0;
    }

    void SetIsWriteProtected(bool wp) {
        _writeReg(0x8E, wp ? 0x80 : 0x00);
    }

    bool GetIsRunning() {
        return (_readReg(0x81) & 0x80) == 0;  // CH bit = 0 means running
    }

    void SetIsRunning(bool run) {
        uint8_t sec = _readReg(0x81);
        if (run)
            sec &= ~0x80;  // clear CH bit
        else
            sec |= 0x80;   // set CH bit
        _writeReg(0x80, sec);
    }

    RtcDateTime GetDateTime() {
        uint8_t buf[7];
        // Burst read: command 0xBF
        _w.beginTransaction();
        _w.writeByte(0xBF);
        for (int i = 0; i < 7; i++) buf[i] = _w.readByte();
        _w.endTransaction();

        return RtcDateTime(
            _bcd2dec(buf[6]) + 2000,  // year
            _bcd2dec(buf[4]),          // month
            _bcd2dec(buf[3]),          // day
            _bcd2dec(buf[2] & 0x3F),  // hour (24h)
            _bcd2dec(buf[1]),          // minute
            _bcd2dec(buf[0] & 0x7F)   // second (mask CH bit)
        );
    }

    void SetDateTime(const RtcDateTime& dt) {
        // Burst write: command 0xBE
        _w.beginTransaction();
        _w.writeByte(0xBE);
        _w.writeByte(_dec2bcd(dt.Second()));
        _w.writeByte(_dec2bcd(dt.Minute()));
        _w.writeByte(_dec2bcd(dt.Hour()));
        _w.writeByte(_dec2bcd(dt.Day()));
        _w.writeByte(_dec2bcd(dt.Month()));
        _w.writeByte(0x01); // day-of-week (not used but required)
        _w.writeByte(_dec2bcd(dt.Year() - 2000));
        _w.writeByte(0x00); // write-protect off
        _w.endTransaction();
    }

    // 31-byte battery-backed SRAM (addresses 0..30)
    uint8_t GetMemory(uint8_t addr) {
        if (addr > 30) return 0;
        return _readReg(0xC1 + (addr * 2));
    }

    void SetMemory(uint8_t addr, uint8_t val) {
        if (addr > 30) return;
        _writeReg(0xC0 + (addr * 2), val);
    }

private:
    TWire& _w;

    void _writeReg(uint8_t cmd, uint8_t val) {
        _w.beginTransaction();
        _w.writeByte(cmd);
        _w.writeByte(val);
        _w.endTransaction();
    }

    uint8_t _readReg(uint8_t cmd) {
        _w.beginTransaction();
        _w.writeByte(cmd);
        uint8_t val = _w.readByte();
        _w.endTransaction();
        return val;
    }

    static uint8_t _bcd2dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
    static uint8_t _dec2bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }
};
