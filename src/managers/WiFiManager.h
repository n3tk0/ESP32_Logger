#pragma once
#include <Arduino.h>

bool connectToWiFi();
void startAPMode();

/**
 * Правилно спира WiFi преди рестарт:
 *  1. Изчаква активни трансфери
 *  2. Disconnects / stops softAP
 *  3. Поставя WiFi в режим WIFI_OFF
 *  4. Кратък delay за flush на радио стека
 */
void safeWiFiShutdown();

bool syncTimeFromNTP();

// Pass 5 5.5 phase 2 — captive-portal DNS responder.
//
// startAPMode() spins up a wildcard DNS responder bound to the AP IP so
// any phone joining the WaterLogger AP gets its OS-level captive-portal
// probe redirected to the SPA, prompting the OS to auto-open it.  The
// responder is non-blocking: tickCaptivePortalDNS() must be called from
// the main loop so DNS queries actually get serviced.
void tickCaptivePortalDNS();
bool isCaptivePortalDNSRunning();
