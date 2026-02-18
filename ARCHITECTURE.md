# 💧 ESP32 Water Logger – Модулна структура v4.1.4

## 📁 Файлова структура

```
WaterLogger/
├── WaterLogger.ino          ← Само setup() и loop()
├── Config.h                 ← Всички struct-ове, enum-и, константи
├── Globals.h                ← extern декларации на глобални променливи
├── Globals.cpp              ← Дефиниции на глобалните променливи
├── Utils.h / Utils.cpp      ← formatFileSize, sanitize, buildPath, deleteRecursive
├── ConfigManager.h/.cpp     ← loadConfig, saveConfig, loadDefaultConfig, migrateConfig
├── WiFiManager.h/.cpp       ← connectToWiFi, startAPMode, safeWiFiShutdown, syncTimeFromNTP
├── StorageManager.h/.cpp    ← initStorage, getActiveDatalogFile, getStorageInfo, ...
├── RtcManager.h/.cpp        ← initRtc, backupBootCount, restoreBootCount, getRtcTimeString, ...
├── HardwareManager.h/.cpp   ← initHardware, debounceButton, ISR handlers
├── DataLogger.h/.cpp        ← addLogEntry, flushLogBufferToFS
└── WebServer.h / WebServer.cpp ← setupWebServer + всички web handlers (от оригиналния Logger.ino)
```

---

## 🔧 Поправка на рестарт проблема

### Проблемът
При рестарт след активен WiFi (AP scan, client mode), ESP.restart() **не почиства**
WiFi hardware state. При следващ boot:
- `earlyGPIO_bitmask` може да хване GPIO 2 (WiFi trigger pin) като HIGH
- `apModeTriggered` се задава TRUE погрешно
- Устройството влиза в Web Server режим вместо Logging режим

### Решението – `safeWiFiShutdown()`

Нова функция в `WiFiManager.cpp`:

```cpp
void safeWiFiShutdown() {
    WiFi.scanDelete();           // Изчиства незавършен scan
    WiFi.disconnect(true);       // Disconnect + изчиства RAM credentials
    delay(50);
    WiFi.softAPdisconnect(true); // Спира SoftAP
    delay(50);
    WiFi.mode(WIFI_OFF);         // Изключва радиото напълно ← КЛЮЧОВО
    delay(200);                  // Flush на радио стека
}
```

### Кога се използва

**В `WaterLogger.ino` loop():**
```cpp
if (shouldRestart && millis() - restartTimer > 2000) {
    safeWiFiShutdown();   // ← добавено
    ESP.restart();
}
```

**В `/save_hardware` и `/save_network` handlers (WebServer.cpp):**
```cpp
// Вместо:
delay(500);
ESP.restart();

// Сега:
sendRestartPage(r, "...");
safeWiFiShutdown();
delay(100);
ESP.restart();
```

---

## 📦 Зависимости (libraries)

| Library | Версия |
|---------|--------|
| ESPAsyncWebServer | latest |
| AsyncTCP | latest |
| RtcDS1302 (Makuna) | latest |
| ArduinoJson | ^7.x |
| FlowSensor | custom |
| LittleFS | built-in |

---

## 🚀 Upload последователност

1. **Filesystem image:** LittleFS с `/style.css`, `/changelog.txt`, `/favicon.ico`
2. **Firmware:** `WaterLogger.ino`

---

## ⚙️ Важни бележки за WebServer.cpp

При преместване на код от `Logger.ino` в `WebServer.cpp`:

1. Добави `#include "WiFiManager.h"` за `safeWiFiShutdown()`
2. Замени всички директни `ESP.restart()` в web handlers с:
   ```cpp
   safeWiFiShutdown();
   delay(100);
   ESP.restart();
   ```
3. Handlers, които само задават `shouldRestart = true`, са OK.

---

## 👨‍💻 Автор

**Petko Georgiev** – Villeroy & Boch Bulgaria, Севлиево
