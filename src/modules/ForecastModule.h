// ============================================================================
// src/modules/ForecastModule.h
//
// Short weather forecast, fetched over HTTPS, for the Kindle dashboard to sit
// beside the measured values.
//
// WHY NOT WEATHER&RADAR
// ---------------------
// Weather&Radar (WetterOnline) has no publicly documented free API — it is a
// B2B product behind a commercial agreement. Two providers that do publish
// one are implemented instead, chosen at runtime:
//
//   PROVIDER_OPEN_METEO  — no account, no key, no quota worth counting.
//   PROVIDER_OWM         — OpenWeatherMap; needs a free API key.
//
// The split is a provider enum rather than two modules because the forecast
// this dashboard shows is four numbers and a condition; everything past
// fetching and parsing is identical.
//
// TEMPERATURE COMES FROM THE SENSORS, NOT FROM HERE
// -------------------------------------------------
// Deliberately. The dashboard's headline figures are what the BME280s
// actually measured, and the forecast contributes only what a sensor cannot
// know: what the sky is about to do. A forecast's "current temperature" is an
// interpolation from a station that may be 20 km away, and showing it next to
// a real reading invites the reader to trust the wrong one.
//
// TLS
// ---
// Uses setInsecure(), consistent with HttpExporter, and for the same reason:
// no CA bundle is shipped yet (R15). On a LAN-scoped home device fetching a
// public forecast this is an accepted risk — the payload is not secret and
// the failure mode of a MITM is a wrong temperature on a bookshelf. It is
// NOT acceptable for anything carrying credentials, which is why the ingest
// path does not reach outward at all.
// ============================================================================
#pragma once

#include "../setup.h"

#ifdef MODULE_FORECAST_ENABLED

#include <Arduino.h>
#include "../core/IModule.h"

class ForecastModule : public IModule {
public:
    enum Provider : uint8_t {
        PROVIDER_OPEN_METEO = 0,
        PROVIDER_OWM        = 1,
    };

    // Cached forecast. Written by tick() on the module task, read by the
    // dashboard renderer on the AsyncTCP task.
    struct Data {
        bool     valid    = false;
        float    tempC    = NAN;   // provider's current temperature
        float    highC    = NAN;   // today's max
        float    lowC     = NAN;   // today's min
        float    windKph  = NAN;
        int      code     = -1;    // WMO code (Open-Meteo) or mapped OWM id
        uint32_t fetchedAt = 0;    // Unix seconds
        char     summary[24] = {0};
    };

    const char* getId()   const override { return "forecast"; }
    const char* getName() const override { return "Weather forecast"; }
    const char* getDescription() const override {
        return "Short forecast from Open-Meteo or OpenWeatherMap";
    }

    bool load(JsonObjectConst cfg) override;
    bool save(JsonObject cfg) const override;
    void tick(uint32_t nowMs) override;
    void statusJson(JsonObject out) const override;

    /// Snapshot of the last successful fetch. Safe to call from any task.
    Data snapshot() const;

private:
    bool _fetch();
    bool _fetchOpenMeteo();
    bool _fetchOwm();

    Provider _provider     = PROVIDER_OPEN_METEO;
    float    _lat          = 0.0f;
    float    _lon          = 0.0f;
    char     _apiKey[40]   = {0};
    uint32_t _intervalMs   = 1800000UL;   // 30 min; forecasts do not move faster

    uint32_t _lastAttempt  = 0;
    uint32_t _failures     = 0;
    Data     _data;

    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

extern ForecastModule forecastModule;

/// Appends the dashboard's forecast section to `out`. Defined here so the
/// Kindle renderer does not need to know the provider details.
void appendForecastSection(String& out);

#endif  // MODULE_FORECAST_ENABLED
