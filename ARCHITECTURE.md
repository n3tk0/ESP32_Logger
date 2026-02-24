# 💧 ESP32 Water Logger – Модулна структура v4.1.5 (Web/UI refactor)

## 📁 Файлова структура

```text
Water_logger/
├── Logger.ino               ← setup()/loop(), state machine, deep-sleep логика
├── full_logger.ino          ← исторически монолитен вариант (референтен)
├── changelog.txt            ← release notes
├── README.md
├── ARCHITECTURE.md
├── www/                     ← SPA UI файлове (качват се в LittleFS под /www/)
│   ├── index.html
│   ├── web.js
│   ├── style.css
│   └── chart.min.js
└── src/
    ├── core/
    │   ├── Config.h         ← версии, enum-и, конфигурационни struct-ове
    │   ├── Globals.h
    │   └── Globals.cpp
    ├── managers/
    │   ├── ConfigManager.h/.cpp
    │   ├── WiFiManager.h/.cpp
    │   ├── StorageManager.h/.cpp
    │   ├── RtcManager.h/.cpp
    │   ├── HardwareManager.h/.cpp
    │   └── DataLogger.h/.cpp
    ├── web/
    │   └── WebServer.h/.cpp ← API + static/failsafe routing
    └── utils/
        ├── Utils.h
        └── Utils.cpp
```

---

## 🌐 Web архитектура (v4.1.5)

### 1) SPA от `/www/` в LittleFS
- При наличен `/www/index.html`: `serveStatic("/", LittleFS, "/www/")`.
- Всички legacy страници (`/dashboard`, `/files`, `/settings*`, `/data`, `/live`) се пренасочват към `/`.
- UI логиката е централизирана в `www/web.js`.

### 2) Failsafe recovery режим
- Ако `/www/index.html` липсва: root (`/`) връща вграден failsafe HTML.
- Маршрутът `/setup` винаги е наличен и връща failsafe страницата, независимо от състоянието на SPA.
- Цел: възстановяване чрез качване на липсващи/повредени UI файлове без повторно флашване.

### 3) API модел: runtime + config разделяне
- `/api/status` → live runtime статус (IP, mode, counters и др.).
- `/export_settings` → пълна конфигурация за UI forms.
- SPA зарежда двата endpoint-а паралелно и кешира резултатите (`ST` и `CFG`).

### 4) Защитено OTA качване
- Преди OTA upload се валидират:
  - `.bin` разширение
  - минимален размер (10 KB)
  - ESP magic byte `0xE9`
- Невалидни файлове се отхвърлят преди стартиране на update процеса.

---

## 🔁 Рестарт и WiFi cleanup

`safeWiFiShutdown()` в `WiFiManager.cpp` се извиква преди `ESP.restart()` при restart-trigger операции.

Полза:
- избягва остатъчно WiFi състояние след scan/AP/client
- намалява фалшив AP trigger при следващ boot
- по-стабилно връщане към logging режим

---

## 💾 Конфигурация и миграция

- Конфигурацията е бинарна (`/config.bin`) с `CONFIG_STRUCT_MAGIC` и `CONFIG_VERSION`.
- `ConfigManager` поддържа migration при промени в struct-овете.
- В datalog има post-correction параметри:
  - `postCorrectionEnabled`
  - `pfToFfThreshold`
  - `ffToPfThreshold`
  - `manualPressThresholdMs`

---

## 🚀 Upload последователност

1. Качи LittleFS съдържание (вкл. `/www/index.html`, `/www/web.js`, `/www/style.css`, `/www/changelog.txt`).
2. Качи firmware (`Logger.ino`).
3. При проблем с UI отвори `http://<device-ip>/setup` за recovery upload.

---

## 📦 Основни зависимости

- ESPAsyncWebServer
- AsyncTCP
- ArduinoJson
- LittleFS
- SD
- RtcDS1302 (Makuna)
- FlowSensor

---

## 👨‍💻 Автор

**Petko Georgiev** – Villeroy & Boch Bulgaria, Севлиево
