#include "RtcManager.h"
#include "../core/Globals.h"
#include "../utils/AtomicWrite.h"
#include "../pipeline/DataPipeline.h"
#include <LittleFS.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <time.h>
#include <new>

void initRtc() {
    DBGLN("Init RTC...");
    bool pinsValid = true;

    // Bounds PER TARGET, matching ConfigManager::sanitizeWakeConfig()'s
    // isSafePin. This was a C3-only range (0..21 minus the 11-17 flash bus),
    // which is the exact mistake HardwareManager::initHardware warns about in
    // its own comment: on a classic ESP32 or an S3 it refuses GPIOs the chip
    // and the sanitiser both accept, so a DS1302 wired to GPIO25/26/27 — the
    // ordinary choice on a devkit — reported "RTC pins invalid!" and left
    // rtcValid false with no way to fix it from the UI.
    auto isPinSafe = [](int p) {
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
        return p >= 0 && p <= 21 && !(p >= 11 && p <= 17);
#elif CONFIG_IDF_TARGET_ESP32
        // Internal flash occupies 6-11; 34-39 are input-only, and the DS1302
        // needs to drive CE/SCLK and both directions on IO.
        return p >= 0 && p <= 33 && !(p >= 6 && p <= 11);
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
        // 26-37 is flash + octal PSRAM on the parts this project builds for.
        return p >= 0 && p <= 48 && !(p >= 26 && p <= 37);
#else
        return p >= 0 && p <= 48;
#endif
    };

    if (!isPinSafe(config.hardware.pinRtcCE) ||
        !isPinSafe(config.hardware.pinRtcIO) ||
        !isPinSafe(config.hardware.pinRtcSCLK)) {
        DBGLN("WARNING: RTC pins invalid!");
        pinsValid = false;
    }

    if (!pinsValid) { rtcValid = false; return; }

    if (Rtc)     { delete Rtc;     Rtc     = nullptr; }
    if (rtcWire) { delete rtcWire; rtcWire = nullptr; }

    // R28 / AUDIT 8.12: nothrow allocation.  On heap-pressured boot a throwing
    // `new` aborts — preferable to keep going with rtcValid=false so the rest
    // of the system can still run (NTP fallback path covers timekeeping).
    rtcWire = new(std::nothrow) ThreeWire(config.hardware.pinRtcIO,
                                          config.hardware.pinRtcSCLK,
                                          config.hardware.pinRtcCE);
    if (!rtcWire) {
        DBGLN("RTC: alloc ThreeWire failed");
        statusMessage = "RTC alloc failed (low heap)";
        rtcValid = false;
        return;
    }
    Rtc = new(std::nothrow) RtcDS1302<ThreeWire>(*rtcWire);
    if (!Rtc) {
        DBGLN("RTC: alloc RtcDS1302 failed");
        statusMessage = "RTC alloc failed (low heap)";
        delete rtcWire; rtcWire = nullptr;
        rtcValid = false;
        return;
    }
    Rtc->Begin();

    if (Rtc->GetIsWriteProtected()) Rtc->SetIsWriteProtected(false);
    if (!Rtc->GetIsRunning())       Rtc->SetIsRunning(true);

    RtcDateTime test = Rtc->GetDateTime();
    bool timeOk = (test.Year() >= 2020 && test.Year() <= 2100 &&
                   test.Month() >= 1   && test.Month() <= 12  &&
                   test.Day()   >= 1   && test.Day()   <= 31);

    if (!timeOk) {
        DBGLN("RTC: Time invalid, setting baseline 2024-01-01...");
        statusMessage = "RTC time invalid — set via web UI";
        RtcDateTime compiled = RtcDateTime(2024, 1, 1, 0, 0, 0);
        for (int i = 0; i < 3 && !timeOk; i++) {
            Rtc->SetIsWriteProtected(false);
            delay(10);
            Rtc->SetIsRunning(true);
            delay(10);
            Rtc->SetDateTime(compiled);
            delay(100);
            RtcDateTime v = Rtc->GetDateTime();
            timeOk = (v.Year() >= 2020 && v.Month() >= 1);
        }
    }

    rtcValid = timeOk;
    if (rtcValid) {
        RtcDateTime now = Rtc->GetDateTime();
        DBGF("RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
             now.Year(), now.Month(), now.Day(),
             now.Hour(), now.Minute(), now.Second());
    } else {
        DBGLN("RTC: Could not set time. Use web UI.");
    }

    // R22 / AUDIT 8.5: re-enable write protection after init. Without this,
    // the RTC stays writable forever and any stray Rtc->SetDateTime path
    // would succeed silently. /set_time + WiFiManager NTP path each wrap
    // their writes in their own unprotect/write/protect cycle, so this
    // post-init protect doesn't break them.
    Rtc->SetIsWriteProtected(true);
}

void backupBootCount() {
    if (Rtc) {
        // R22 follow-up (Gemini HIGH + Codex P2 on PR #99): initRtc now
        // re-enables write protection, so SetMemory needs its own
        // unprotect/write/protect cycle — same pattern /set_time and
        // syncTimeFromNTP use. Without this, SetMemory silently no-ops
        // and bootCount drifts away from the RTC RAM copy.
        Rtc->SetIsWriteProtected(false);
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR),     (uint8_t)((bootCount >> 24) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 1), (uint8_t)((bootCount >> 16) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 2), (uint8_t)((bootCount >>  8) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 3), (uint8_t)( bootCount        & 0xFF));
        Rtc->SetMemory((uint8_t)RTC_RAM_MAGIC_ADDR, (uint8_t)RTC_RAM_MAGIC_VALUE);
        Rtc->SetIsWriteProtected(true);
    }
    atomicWrite(LittleFS, BOOTCOUNT_BACKUP_FILE, [](File& f) -> bool {
        return f.write((uint8_t*)&bootCount, sizeof(bootCount)) == sizeof(bootCount);
    }, fsMutex);
}

void restoreBootCount() {
    if (Rtc) {
        uint8_t magic = Rtc->GetMemory((uint8_t)RTC_RAM_MAGIC_ADDR);
        if (magic == RTC_RAM_MAGIC_VALUE) {
            bootCount = ((uint32_t)Rtc->GetMemory((uint8_t) RTC_RAM_BOOTCOUNT_ADDR)     << 24) |
                        ((uint32_t)Rtc->GetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 1)) << 16) |
                        ((uint32_t)Rtc->GetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 2)) <<  8) |
                                   Rtc->GetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 3));
            DBGF("Bootcount from RTC RAM: %d\n", bootCount);
            return;
        }
    }
    File f = LittleFS.open(BOOTCOUNT_BACKUP_FILE, "r");
    if (f) {
        size_t n = f.read((uint8_t*)&bootCount, sizeof(bootCount));
        if (n != sizeof(bootCount)) bootCount = 0;
        f.close();
    }
    DBGF("Bootcount from flash: %d\n", bootCount);
}

// ---------------------------------------------------------------------------
// Shared helper: read system clock into tm struct; returns false if not set.
static bool _sysClockTm(struct tm* out) {
    time_t t = time(nullptr);
    if (t <= 1000000000L) return false;
    return localtime_r(&t, out) != nullptr;
}

String getRtcDateTimeString() {
    if (Rtc) {
        RtcDateTime now = Rtc->GetDateTime();
        if (now.Year() >= 2020 && now.Month() != 0) {
            // RTC stores UTC (after NTP sync); convert to local for display.
            time_t epoch = (time_t)now.Unix32Time();
            struct tm ti = {0};
            localtime_r(&epoch, &ti);
            char buf[32];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
            return String(buf);
        }
        return "Not Set - Use Manual Set";
    }
    // No hardware RTC — fall back to system clock
    struct tm ti;
    if (_sysClockTm(&ti)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        return String(buf);
    }
    return "No RTC - Set time via NTP";
}

void configureWakeup() {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
    // ── ESP32-C3 / C6: deep-sleep GPIO wake on pins 0..5 ─────────────────
    auto isRtcWakePinC3 = [](uint8_t pin) -> bool {
        return pin <= 5; // ESP32-C3 deep-sleep GPIO wake capable pins
    };

    const uint8_t ffPin   = config.hardware.pinWakeupFF;
    const uint8_t pfPin   = config.hardware.pinWakeupPF;
    const uint8_t wifiPin = config.hardware.pinWifiTrigger;

    if (!isRtcWakePinC3(ffPin) || !isRtcWakePinC3(pfPin) || !isRtcWakePinC3(wifiPin)) {
        DBGF("WAKEUP CONFIG ERROR: C3 wake pins must be GPIO0..GPIO5 (FF=%u PF=%u WIFI=%u)\n",
                      ffPin, pfPin, wifiPin);
        return;
    }

    if (ffPin == pfPin || ffPin == wifiPin || pfPin == wifiPin) {
        DBGF("WAKEUP CONFIG ERROR: duplicate wake pins (FF=%u PF=%u WIFI=%u)\n",
                      ffPin, pfPin, wifiPin);
        return;
    }

    if (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) {
        pinMode(ffPin, INPUT_PULLDOWN);
        pinMode(pfPin, INPUT_PULLDOWN);
        pinMode(wifiPin, INPUT_PULLDOWN);
    } else {
        pinMode(ffPin, INPUT_PULLUP);
        pinMode(pfPin, INPUT_PULLUP);
        pinMode(wifiPin, INPUT_PULLUP);
    }

    uint64_t mask = 0;
    mask |= (1ULL << ffPin);
    mask |= (1ULL << pfPin);
    mask |= (1ULL << wifiPin);

    esp_deepsleep_gpio_wake_up_mode_t mode =
        (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH)
        ? ESP_GPIO_WAKEUP_GPIO_HIGH
        : ESP_GPIO_WAKEUP_GPIO_LOW;

    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(mask, mode);
    if (err != ESP_OK) {
        DBGF("WAKEUP CONFIG ERROR: esp_deep_sleep_enable_gpio_wakeup failed (%d)\n", (int)err);
    }
#elif CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    // ── ESP32 / S2 / S3: use EXT1 wakeup on RTC GPIOs ────────────────────
    // Note: EXT1 ANY_HIGH wakes if any pin goes HIGH; ALL_LOW requires all
    // pins LOW simultaneously. ACTIVE_LOW button semantics don't map cleanly
    // to ALL_LOW, so we only support ACTIVE_HIGH on these targets.
    const uint8_t ffPin   = config.hardware.pinWakeupFF;
    const uint8_t pfPin   = config.hardware.pinWakeupPF;
    const uint8_t wifiPin = config.hardware.pinWifiTrigger;

    if (config.hardware.wakeupMode != WAKEUP_GPIO_ACTIVE_HIGH) {
        DBGLN("WAKEUP CONFIG: EXT1 wake requires ACTIVE_HIGH on ESP32/S2/S3");
        statusMessage = "EXT1 wake requires ACTIVE_HIGH on this chip";
        return;
    }

    pinMode(ffPin,   INPUT_PULLDOWN);
    pinMode(pfPin,   INPUT_PULLDOWN);
    pinMode(wifiPin, INPUT_PULLDOWN);

    uint64_t mask = (1ULL << ffPin) | (1ULL << pfPin) | (1ULL << wifiPin);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (err != ESP_OK) {
        DBGF("WAKEUP CONFIG ERROR: esp_sleep_enable_ext1_wakeup failed (%d)\n", (int)err);
    }
#else
    DBGLN("WAKEUP CONFIG: deep-sleep GPIO wake not implemented for this target");
#endif
}

String getWakeupReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        int expectedState = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;

        // An unassigned pin (PIN_UNSET = 255) must not reach either of the two
        // reads below: `bitmask >> 255` is undefined for a 32-bit value, and
        // digitalRead(255) is an out-of-range GPIO. A device with no buttons
        // wired cannot have woken on one, so unset reads as not-triggered.
        auto bitOf = [](uint32_t mask, uint8_t pin) -> bool {
            return pin < 32 && ((mask >> pin) & 1u);
        };
        auto readIs = [](uint8_t pin, int expected) -> bool {
            return pin != PIN_UNSET && pin <= 48 && digitalRead(pin) == expected;
        };

        if (earlyGPIO_captured) {
            bool ffEarly  = bitOf(earlyGPIO_bitmask, config.hardware.pinWakeupFF);
            bool pfEarly  = bitOf(earlyGPIO_bitmask, config.hardware.pinWakeupPF);
            bool wifiEarly= bitOf(earlyGPIO_bitmask, config.hardware.pinWifiTrigger);
            
            if (expectedState == LOW) {
                ffEarly   = !ffEarly;
                pfEarly   = !pfEarly;
                wifiEarly = !wifiEarly;
            }

            DBGF("GPIO early: FF=%d PF=%d WIFI=%d (bitmask=0x%08X)\n",
                          ffEarly, pfEarly, wifiEarly, earlyGPIO_bitmask);
            if (ffEarly)   return "FF_BTN";
            if (pfEarly)   return "PF_BTN";
            if (wifiEarly) return "WIFI";
        }

        // Fallback
        delay(config.hardware.debounceMs);
        bool ffNow   = readIs(config.hardware.pinWakeupFF,    expectedState);
        bool pfNow   = readIs(config.hardware.pinWakeupPF,    expectedState);
        bool wifiNow = readIs(config.hardware.pinWifiTrigger, expectedState);
        if (ffNow)   return "FF_BTN";
        if (pfNow)   return "PF_BTN";
        if (wifiNow) return "WIFI";
        return "GPIO";
    }
    return (cause == ESP_SLEEP_WAKEUP_TIMER) ? "TIMER" : "PWR_ON";
}
