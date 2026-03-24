#include "WiFiManager.h"
#include "../core/Globals.h"
#include "ConfigManager.h"
#include <WiFi.h>
#include <time.h>
#include <esp_task_wdt.h>

// ============================================================================
// safeWiFiShutdown – КЛЮЧОВА ПОПРАВКА за проблема с рестарта
//
// Проблемът: ESP.restart() не почиства WiFi hardware state.
// При следващ boot earlyGPIO snapshot вижда WiFi пина HIGH (ако е бил активен),
// или onlineLoggerMode/apModeTriggered се задава грешно.
//
// Решение: преди всеки рестарт:
//   1. Спираме async web server tasks (те се спират автоматично при WiFi stop)
//   2. WiFi.scanDelete() – почиства незавършени scan
//   3. WiFi.disconnect(true) – disconnect + изчистване на credentials от RAM
//   4. WiFi.softAPdisconnect(true) – спира AP
//   5. WiFi.mode(WIFI_OFF) – изключва радиото напълно
//   6. delay(200) – дава време на радио стека да се изчисти
//
// Резултат: след рестарт GPIO пиновете са в чисто pull-down/pull-up
//           и earlyGPIO snapshot чете само реалния физически бутон.
// ============================================================================
void safeWiFiShutdown() {
    DBGLN("WiFi: Safe shutdown before restart...");

    // Изчисти незавършен WiFi scan (оставен от /wifi_scan_start endpoint)
    WiFi.scanDelete();

    // Disconnect от AP/Client, изчисти запазените credentials в RAM
    WiFi.disconnect(true /*wifioff=false*/);
    delay(50);

    // Спри SoftAP ако е активен
    WiFi.softAPdisconnect(true);
    delay(50);

    // Изключи WiFi радиото напълно
    // ВАЖНО: това е единственото сигурно средство срещу "phantom WiFi pin"
    WiFi.mode(WIFI_OFF);
    delay(200);   // Дай на радио стека да се flush-не

    DBGLN("WiFi: Radio OFF, safe to restart.");
}

bool connectToWiFi() {
    if (config.network.wifiMode != WIFIMODE_CLIENT ||
        strlen(config.network.clientSSID) == 0) {
        return false;
    }

    DBGF("WiFi: Connecting to %s...\n", config.network.clientSSID);
    WiFi.mode(WIFI_STA);

    if (config.network.useStaticIP) {
        IPAddress ip(config.network.staticIP[0], config.network.staticIP[1],
                     config.network.staticIP[2], config.network.staticIP[3]);
        IPAddress gw(config.network.gateway[0], config.network.gateway[1],
                     config.network.gateway[2], config.network.gateway[3]);
        IPAddress sn(config.network.subnet[0],  config.network.subnet[1],
                     config.network.subnet[2],  config.network.subnet[3]);
        IPAddress dns(config.network.dns[0],    config.network.dns[1],
                      config.network.dns[2],    config.network.dns[3]);
        WiFi.config(ip, gw, sn, dns);
    }

    WiFi.begin(config.network.clientSSID, config.network.clientPassword);

    unsigned long start = millis();
    unsigned long lastDot = 0;
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(100);
        esp_task_wdt_reset();
        if (millis() - lastDot >= 250) { DBG("."); lastDot = millis(); }
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnectedAsClient = true;
        currentIPAddress      = WiFi.localIP().toString();
        connectedSSID         = config.network.clientSSID;
        DBGF("\nWiFi connected: %s\n", currentIPAddress.c_str());
        return true;
    }

    DBGLN("\nWiFi connection failed");
    return false;
}

void startAPMode() {
    String apName = strlen(config.network.apSSID) > 0
                    ? config.network.apSSID
                    : config.deviceName;

    DBGF("WiFi: Starting AP '%s'\n", apName.c_str());
    WiFi.mode(WIFI_AP);

    IPAddress apIP    (config.network.apIP[0],      config.network.apIP[1],
                       config.network.apIP[2],      config.network.apIP[3]);
    IPAddress apGW    (config.network.apGateway[0], config.network.apGateway[1],
                       config.network.apGateway[2], config.network.apGateway[3]);
    IPAddress apSubnet(config.network.apSubnet[0],  config.network.apSubnet[1],
                       config.network.apSubnet[2],  config.network.apSubnet[3]);
    WiFi.softAPConfig(apIP, apGW, apSubnet);
    WiFi.softAP(apName.c_str(), config.network.apPassword);

    currentIPAddress      = WiFi.softAPIP().toString();
    wifiConnectedAsClient = false;
    DBGF("WiFi: AP IP: %s\n", currentIPAddress.c_str());
}

bool syncTimeFromNTP() {
    if (!wifiConnectedAsClient) { DBGLN("NTP: No WiFi"); return false; }

    configTime(config.network.timezone * 3600, config.network.dstOffsetHours * 3600, config.network.ntpServer);

    time_t now = 0;
    struct tm ti = {0};
    int retry = 0;
    while (ti.tm_year < (2020 - 1900) && retry < 20) {
        delay(500);
        time(&now);
        localtime_r(&now, &ti);
        retry++;
    }
    if (ti.tm_year < (2020 - 1900)) { DBGLN("NTP: Failed"); return false; }

    if (Rtc) {
        Rtc->SetIsWriteProtected(false);
        Rtc->SetIsRunning(true);
        RtcDateTime dt(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                        ti.tm_hour, ti.tm_min, ti.tm_sec);
        Rtc->SetDateTime(dt);
        DBGF("NTP: RTC set to %04d-%02d-%02d %02d:%02d:%02d\n",
             dt.Year(), dt.Month(), dt.Day(), dt.Hour(), dt.Minute(), dt.Second());
    }
    return true;
}
