#include "IaqBaselineStore.h"
#include "../core/LogRing.h"   // Log: Serial + the RTC log ring (/api/log)

#include <Arduino.h>
#include <LittleFS.h>
#include "AtomicWrite.h"
#include "MutexGuard.h"
#include "../pipeline/DataPipeline.h"   // fsMutex

namespace {
struct IaqBaselineFile {
    uint32_t magic;          // 'IAQ1'
    int16_t  heaterTemp;
    int16_t  heaterDurMs;
    float    baseline;       // Ω
};
constexpr uint32_t IAQ_MAGIC = 0x31514149u;   // "IAQ1"
}

namespace IaqBaselineStore {

void path(const char* prefix, const char* id, char* out, size_t len) {
    uint32_t h = 2166136261u;
    for (const char* p = id; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    snprintf(out, len, "/config/%s_%08lx.bin", prefix, (unsigned long)h);
}

float load(const char* path, int heaterTemp, int heaterDurMs, const char* tag) {
    // Asked first: opening a missing file for reading logs an error on ESP32.
    if (!LittleFS.exists(path)) return 0.0f;
    File f = LittleFS.open(path, FILE_READ);
    if (!f) return 0.0f;
    IaqBaselineFile rec{};
    const bool ok = f.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
    f.close();
    if (!ok || rec.magic != IAQ_MAGIC) return 0.0f;
    if (rec.heaterTemp != heaterTemp || rec.heaterDurMs != heaterDurMs) {
        // Drop it too: going back to the old settings later must not bring
        // back a baseline that has been out of date ever since.
        Log.printf("[%s] IAQ baseline from other heater settings, starting over\n", tag);
        MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
        if (g.isLocked()) LittleFS.remove(path);
        return 0.0f;
    }
    if (!(rec.baseline >= 1.0f && rec.baseline < 1e9f)) return 0.0f;
    Log.printf("[%s] IAQ baseline %.0f Ohm restored\n", tag, rec.baseline);
    return rec.baseline;
}

bool save(const char* path, int heaterTemp, int heaterDurMs, float baseline,
          uint32_t waitMs) {
    IaqBaselineFile rec{ IAQ_MAGIC, (int16_t)heaterTemp, (int16_t)heaterDurMs, baseline };
    if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
    return atomicWrite(LittleFS, path, [&](File& f) -> bool {
        return f.write((const uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
    }, fsMutex, pdMS_TO_TICKS(waitMs));
}

} // namespace IaqBaselineStore
