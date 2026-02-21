#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <functional>

/**
 * WebServer модул – архитектура на обслужването
 *
 * HTML страниците се зареждат от LittleFS /www/:
 *   GET /         → /www/index.html    (dashboard)
 *   GET /settings → /www/settings.html (настройки)
 *   GET /files    → /www/files.html    (файлов мениджър)
 *   GET /data     → /www/data.html     (данни / графика)
 *
 * Ако даден файл липсва, се сервира вграден failsafe HTML,
 * който позволява качване на /www/ файловете чрез бразура.
 *
 * JSON API (винаги в firmware, не изискват /www/ файлове):
 *   GET  /api/status        – runtime статус
 *   GET  /api/config        – пълна конфигурация
 *   GET  /api/files?dir=/   – JSON списък на файловете
 *   POST /save_hardware     – записва HW настройки (→ restart)
 *   POST /save_network      – записва мрежови настройки (→ restart)
 *   POST /save_datalog      – записва datalog настройки
 *   POST /save_flowmeter    – записва flowmeter настройки
 *   POST /save_theme        – записва theme настройки
 *   POST /upload?dir=/www/  – качване на файл в LittleFS
 *   GET  /download?path=... – сваляне на файл
 *   GET  /delete?path=...   – изтриване
 *   POST /mkdir?dir=...     – създаване на директория
 *   POST /set_time          – ръчно задаване на RTC времето
 *   POST /sync_ntp          – NTP синхронизация
 *   POST /restart           – рестарт
 *   POST /factory_reset     – нулиране до фабрични настройки
 */

void setupWebServer();

// Helpers използвани и от Logger.ino / other modules
String getModeDisplay();
String getNetworkDisplay();

void sendJsonResponse(AsyncWebServerRequest* r, JsonDocument& doc);
void sendRestartPage(AsyncWebServerRequest* r, const char* message);
