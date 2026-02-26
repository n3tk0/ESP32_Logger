#include "RtcManager.h"
#include "../core/Globals.h"
#include <LittleFS.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

void initRtc() {
    DBGLN("Init RTC...");
    bool pinsValid = true;

    auto isPinSafe = [](int p) {
        if (p < 0 || p > 21) return false;
        if (p >= 11 && p <= 17) return false;
        return true;
    };

    if (!isPinSafe(config.hardware.pinRtcCE) ||
        !isPinSafe(config.hardware.pinRtcIO) ||
        !isPinSafe(config.hardware.pinRtcSCLK)) {
        DBGLN("WARNING: RTC pins invalid!");
        pinsValid = false;
    }

    if (!pinsValid) { rtcValid = false; return; }

    if (rtcWire) delete rtcWire;
    if (Rtc)     delete Rtc;

    rtcWire = new ThreeWire(config.hardware.pinRtcIO,
                            config.hardware.pinRtcSCLK,
                            config.hardware.pinRtcCE);
    Rtc = new RtcDS1302<ThreeWire>(*rtcWire);
    Rtc->Begin();

    if (Rtc->GetIsWriteProtected()) Rtc->SetIsWriteProtected(false);
    if (!Rtc->GetIsRunning())       Rtc->SetIsRunning(true);

    RtcDateTime test = Rtc->GetDateTime();
    bool timeOk = (test.Year() >= 2020 && test.Year() <= 2100 &&
                   test.Month() >= 1   && test.Month() <= 12  &&
                   test.Day()   >= 1   && test.Day()   <= 31);

    if (!timeOk) {
        DBGLN("RTC: Time invalid, setting compile time...");
        RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
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
}

void backupBootCount() {
    if (Rtc) {
        Rtc->SetMemory((uint8_t)RTC_RAM_MAGIC_ADDR, (uint8_t)RTC_RAM_MAGIC_VALUE);
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR),     (uint8_t)((bootCount >> 24) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 1), (uint8_t)((bootCount >> 16) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 2), (uint8_t)((bootCount >>  8) & 0xFF));
        Rtc->SetMemory((uint8_t)(RTC_RAM_BOOTCOUNT_ADDR + 3), (uint8_t)( bootCount        & 0xFF));
    }
    File f = LittleFS.open(BOOTCOUNT_BACKUP_FILE, "w");
    if (f) { f.write((uint8_t*)&bootCount, sizeof(bootCount)); f.close(); }
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
    if (f) { f.read((uint8_t*)&bootCount, sizeof(bootCount)); f.close(); }
    DBGF("Bootcount from flash: %d\n", bootCount);
}

String getRtcTimeString() {
    if (!Rtc) return "No RTC";
    RtcDateTime now = Rtc->GetDateTime();
    if (now.Year() < 2020 || now.Month() == 0) return "Set Time";
    char buf[10];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
             now.Hour(), now.Minute(), now.Second());
    return String(buf);
}

String getRtcDateTimeString() {
    if (!Rtc) return "No RTC";
    RtcDateTime now = Rtc->GetDateTime();
    if (now.Year() < 2020 || now.Month() == 0) return "Not Set - Use Manual Set";
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
             now.Year(), now.Month(), now.Day(),
             now.Hour(), now.Minute(), now.Second());
    return String(buf);
}

void configureWakeup() {
    auto isRtcWakePinC3 = [](uint8_t pin) -> bool {
        return pin <= 5; // ESP32-C3 deep-sleep GPIO wake capable pins
    };

    const uint8_t ffPin   = config.hardware.pinWakeupFF;
    const uint8_t pfPin   = config.hardware.pinWakeupPF;
    const uint8_t wifiPin = config.hardware.pinWifiTrigger;

    if (!isRtcWakePinC3(ffPin) || !isRtcWakePinC3(pfPin) || !isRtcWakePinC3(wifiPin)) {
        Serial.printf("WAKEUP CONFIG ERROR: C3 wake pins must be GPIO0..GPIO5 (FF=%u PF=%u WIFI=%u)\n",
                      ffPin, pfPin, wifiPin);
        return;
    }

    if (ffPin == pfPin || ffPin == wifiPin || pfPin == wifiPin) {
        Serial.printf("WAKEUP CONFIG ERROR: duplicate wake pins (FF=%u PF=%u WIFI=%u)\n",
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

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(mask, mode);
    if (err != ESP_OK) {
        Serial.printf("WAKEUP CONFIG ERROR: esp_deep_sleep_enable_gpio_wakeup failed (%d)\n", (int)err);
    }
}

String getWakeupReason() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        int expectedState = (config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH) ? HIGH : LOW;

        if (earlyGPIO_captured) {
            bool ffEarly  = (bool)((earlyGPIO_bitmask >> config.hardware.pinWakeupFF)    & 1);
            bool pfEarly  = (bool)((earlyGPIO_bitmask >> config.hardware.pinWakeupPF)    & 1);
            bool wifiEarly= (bool)((earlyGPIO_bitmask >> config.hardware.pinWifiTrigger) & 1);
            
            if (expectedState == LOW) {
                ffEarly   = !ffEarly;
                pfEarly   = !pfEarly;
                wifiEarly = !wifiEarly;
            }

            Serial.printf("GPIO early: FF=%d PF=%d WIFI=%d (bitmask=0x%08X)\n",
                          ffEarly, pfEarly, wifiEarly, earlyGPIO_bitmask);
            if (ffEarly)   return "FF_BTN";
            if (pfEarly)   return "PF_BTN";
            if (wifiEarly) return "WIFI";
        }

        // Fallback
        delay(config.hardware.debounceMs);
        bool ffNow   = (digitalRead(config.hardware.pinWakeupFF)    == expectedState);
        bool pfNow   = (digitalRead(config.hardware.pinWakeupPF)    == expectedState);
        bool wifiNow = (digitalRead(config.hardware.pinWifiTrigger) == expectedState);
        if (ffNow)   return "FF_BTN";
        if (pfNow)   return "PF_BTN";
        if (wifiNow) return "WIFI";
        return "GPIO";
    }
    return (cause == ESP_SLEEP_WAKEUP_TIMER) ? "TIMER" : "PWR_ON";
}
