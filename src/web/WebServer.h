#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <functional>

/**
 * Инициализира и стартира AsyncWebServer с всички endpoint-и.
 *
 * NOTE: Цялото HTML/JS генериране е запазено идентично с оригиналния
 *       Logger.ino. При рефакториране то може да се раздели в WebPages.h/.cpp.
 *
 * ВАЖНА ПРОМЯНА относно рестарта:
 *   Всеки endpoint, който извиква ESP.restart() директно (напр. /save_hardware,
 *   /save_network), ТРЯБВА да извика safeWiFiShutdown() ПРЕДИ рестарта:
 *
 *       #include "../managers/WiFiManager.h"
 *       ...
 *       safeWiFiShutdown();
 *       delay(100);
 *       ESP.restart();
 *
 *   Handlers, които само задават shouldRestart = true, са OK –
 *   главния loop() в Logger.ino ще извика safeWiFiShutdown() автоматично.
 */
void setupWebServer();

// Helper функции използвани от web handlers
String getModeDisplay();
String getNetworkDisplay();
String getThemeClass();
String icon(const char* emoji);

// HTML page helpers
void writeSidebar(Print& out, const char* currentPage);
void writeBottomNav(Print& out, const char* currentPage);
void sendChunkedHtml(AsyncWebServerRequest* r, const char* title,
                     std::function<void(Print&)> bodyWriter);
void sendJsonResponse(AsyncWebServerRequest* r, JsonDocument& doc);
void sendRestartPage(AsyncWebServerRequest* r, const char* message);
void writeFileList(Print& out, const String& dir, bool editMode = false);
