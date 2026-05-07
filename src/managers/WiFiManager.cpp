#include "WiFiManager.h"
#include "../core/Globals.h"
#include "ConfigManager.h"
#include <WiFi.h>
#include <DNSServer.h>            // Captive portal — Pass 5 5.5 phase 2
#include <time.h>

// Captive-portal DNS responder.  Bound to UDP/53 with a wildcard "*" rule
// that resolves every query to the soft-AP IP, so a phone's captive-portal
// probe (Apple `captive.apple.com`, Android `connectivitycheck.gstatic.com`,
// Windows `www.msftconnecttest.com`, …) lands on us regardless of host.
// The probe URL handlers in WebServer.cpp turn each into an HTTP redirect
// to "/" so the OS shows its captive-portal banner.
static DNSServer s_dnsServer;
static bool      s_dnsRunning = false;

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
    Serial.println("WiFi: Safe shutdown before restart...");

    // Stop captive-portal DNS first so no in-flight UDP packet trips up
    // the radio teardown below.
    if (s_dnsRunning) {
        s_dnsServer.stop();
        s_dnsRunning = false;
    }

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

    Serial.println("WiFi: Radio OFF, safe to restart.");
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
        yield();  // Cooperative scheduling (task not registered with ESP-IDF TWDT)
        if (millis() - lastDot >= 250) { Serial.print("."); lastDot = millis(); }
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnectedAsClient = true;
        currentIPAddress      = WiFi.localIP().toString();
        connectedSSID         = config.network.clientSSID;
        Serial.printf("\nWiFi connected: %s\n", currentIPAddress.c_str());
        return true;
    }

    Serial.println("\nWiFi connection failed");
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

    // Use the locally-configured `apIP` instead of WiFi.softAPIP() for the
    // same reason as the DNS bind below — softAPIP() can transiently return
    // 0.0.0.0 right after softAP() while the netif finishes coming up
    // (gemini review PR #48).
    currentIPAddress      = apIP.toString();
    wifiConnectedAsClient = false;
    DBGF("WiFi: AP IP: %s\n", currentIPAddress.c_str());

    // Start the captive-portal DNS responder.  TTL=60s keeps phones from
    // hammering us with re-queries; wildcard "*" matches every label so a
    // probe to captive.apple.com resolves to the AP IP just like any other
    // hostname.  Non-blocking — main loop must call tickCaptivePortalDNS().
    if (s_dnsRunning) {
        s_dnsServer.stop();
        s_dnsRunning = false;
    }
    s_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    s_dnsServer.setTTL(60);
    // Use the locally-configured `apIP` rather than WiFi.softAPIP() — the
    // latter can transiently return 0.0.0.0 right after softAP() while the
    // netif finishes coming up (gemini review PR #48).
    if (s_dnsServer.start(53, "*", apIP)) {
        s_dnsRunning = true;
        DBGLN("WiFi: captive-portal DNS responder started");
    } else {
        DBGLN("WiFi: captive-portal DNS bind failed (port 53 in use?)");
    }
}

void tickCaptivePortalDNS() {
    if (s_dnsRunning) s_dnsServer.processNextRequest();
}

bool isCaptivePortalDNSRunning() {
    return s_dnsRunning;
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
