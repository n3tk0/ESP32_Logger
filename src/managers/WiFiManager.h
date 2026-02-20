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
