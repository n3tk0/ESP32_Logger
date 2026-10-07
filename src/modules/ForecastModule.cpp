#include "ForecastModule.h"
#include "ModuleSchemas.h"      // this module's form, gzipped

#ifdef MODULE_FORECAST_ENABLED

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <memory>
#include "../core/Globals.h"        // config.kindle: whether the week strip wants the days
#include "../web/DashboardStrings.h"
#include "../web/KindleDashboard.h"   // kdPx(): the glyphs scale with the page
#include "../core/HeapWatch.h"         // heapActivityBegin/End

ForecastModule forecastModule;

// Open-Meteo over plain HTTP on the C3. A TLS session wants two 16 KB record
// buffers (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN, fixed in the prebuilt core) on
// top of the handshake, and a C3 with an SD card mounted does not have that
// much contiguous heap: the fetch failed with mbedTLS -0x7F00 (alloc failed).
// The request carries only coordinates and the answer is public weather, and
// setInsecure() never checked the server anyway. OpenWeatherMap stays on
// HTTPS because its URL carries the API key. Override with
// -DFORECAST_OPENMETEO_TLS=1 (or 0) to choose on any chip.
#ifndef FORECAST_OPENMETEO_TLS
#  if defined(CONFIG_IDF_TARGET_ESP32C3)
#    define FORECAST_OPENMETEO_TLS 0
#  else
#    define FORECAST_OPENMETEO_TLS 1
#  endif
#endif
#if FORECAST_OPENMETEO_TLS
#  define OPENMETEO_BASE "https://api.open-meteo.com"
#else
#  define OPENMETEO_BASE "http://api.open-meteo.com"
#endif

// Drives the form under Settings → Modules → Weather forecast
// (src/modules/schemas/forecast.json). Without it hasUI() is false and the
// module manager offers only an on/off switch, which left the coordinates
// reachable only by POSTing raw JSON at /api/modules.
//
// The labels stay English: this is the collector's own admin UI, which is
// English throughout, and KINDLE_LANG_BG is about the panel on the shelf.
ModuleSchema ForecastModule::schema() const {
    return {MODULE_SCHEMA_FORECAST_GZ, sizeof(MODULE_SCHEMA_FORECAST_GZ)};
}

// ---------------------------------------------------------------------------
// WMO weather codes → a word that fits the dashboard.
// ---------------------------------------------------------------------------
// Open-Meteo reports WMO 4677. The full table has ~28 entries; collapsing to
// these buckets is intentional — at 16 px on an e-ink panel, "moderate
// drizzle" versus "light drizzle" is a distinction the reader cannot act on,
// and both mean "take a coat".
static const char* wmoSummary(int code) {
    // -1 is the sentinel both fetch paths use for a missing or unmappable
    // condition. It must be rejected before the ranges below, or it satisfies
    // `code <= 2` and an unknown sky renders as a confident "Partly cloudy".
    if (code < 0)                  return KD_T("Unknown",       "Неизвестно");
    if (code == 0)                 return KD_T("Clear",         "Ясно");
    if (code <= 2)                 return KD_T("Partly cloudy", "Променливо");
    if (code == 3)                 return KD_T("Overcast",      "Облачно");
    if (code == 45 || code == 48)  return KD_T("Fog",           "Мъгла");
    if (code >= 51 && code <= 57)  return KD_T("Drizzle",       "Ръмеж");
    if (code >= 61 && code <= 67)  return KD_T("Rain",          "Дъжд");
    if (code >= 71 && code <= 77)  return KD_T("Snow",          "Сняг");
    if (code >= 80 && code <= 82)  return KD_T("Showers",       "Превалявания");
    if (code == 85 || code == 86)  return KD_T("Snow showers",  "Снеговалеж");
    if (code >= 95)                return KD_T("Thunderstorm",  "Гръмотевици");
    return KD_T("Unknown", "Неизвестно");
}

// OpenWeatherMap uses its own ids; map them onto the same WMO buckets so the
// dashboard renders identically whichever provider is configured.
static int owmToWmo(int id) {
    if (id >= 200 && id < 300) return 95;   // thunderstorm
    if (id >= 300 && id < 400) return 51;   // drizzle
    if (id >= 500 && id < 505) return 61;   // rain
    if (id == 511)             return 66;   // freezing rain
    if (id >= 520 && id < 532) return 80;   // showers
    if (id >= 600 && id < 700) return 71;   // snow
    if (id >= 700 && id < 800) return 45;   // atmosphere / fog
    if (id == 800)             return 0;    // clear
    if (id == 801 || id == 802) return 2;   // few / scattered clouds
    if (id == 803 || id == 804) return 3;   // broken / overcast
    return -1;
}


// ---------------------------------------------------------------------------
// Condition glyph
// ---------------------------------------------------------------------------
// Drawn here rather than fetched from the provider. Open-Meteo and OWM both
// publish icon URLs, but serving one would mean the device downloading a
// remote image, holding it in RAM and proxying it to the reader — and their
// icons are colour raster art, which dithers to mush on a 16-level grey panel.
// A stroke-only glyph is a few hundred bytes of markup, costs no traffic, and
// is the same drawing language as the rest of the page.
//
// Cloud is one filled-white path (so the strokes behind it do not show
// through) plus the same path stroked on top; everything else is line work.
static const char CLOUD[] PROGMEM =
    "<path d=\"M20 45C11 45 11 32 20 31C20 19 37 17 40 27C50 25 54 39 45 45Z\" fill=\"#fff\"/>"
    "<path d=\"M20 45C11 45 11 32 20 31C20 19 37 17 40 27C50 25 54 39 45 45\" fill=\"none\"/>";

static const char SUN_FULL[] PROGMEM =
    "<circle cx=\"32\" cy=\"32\" r=\"11\" fill=\"#fff\"/>"
    "<line x1=\"47\" y1=\"32\" x2=\"52\" y2=\"32\"/><line x1=\"43\" y1=\"43\" x2=\"46\" y2=\"46\"/>"
    "<line x1=\"32\" y1=\"47\" x2=\"32\" y2=\"52\"/><line x1=\"21\" y1=\"43\" x2=\"18\" y2=\"46\"/>"
    "<line x1=\"17\" y1=\"32\" x2=\"12\" y2=\"32\"/><line x1=\"21\" y1=\"21\" x2=\"18\" y2=\"18\"/>"
    "<line x1=\"32\" y1=\"17\" x2=\"32\" y2=\"12\"/><line x1=\"43\" y1=\"21\" x2=\"46\" y2=\"18\"/>";

static const char SUN_SMALL[] PROGMEM =
    "<circle cx=\"22\" cy=\"22\" r=\"7\" fill=\"#fff\"/>"
    "<line x1=\"33\" y1=\"22\" x2=\"38\" y2=\"22\"/><line x1=\"30\" y1=\"30\" x2=\"33\" y2=\"33\"/>"
    "<line x1=\"22\" y1=\"33\" x2=\"22\" y2=\"38\"/><line x1=\"14\" y1=\"30\" x2=\"11\" y2=\"33\"/>"
    "<line x1=\"11\" y1=\"22\" x2=\"6\" y2=\"22\"/><line x1=\"14\" y1=\"14\" x2=\"11\" y2=\"11\"/>"
    "<line x1=\"22\" y1=\"11\" x2=\"22\" y2=\"6\"/><line x1=\"30\" y1=\"14\" x2=\"33\" y2=\"11\"/>";

// Six-pointed, not eight: at this size a fourth pair of arms closes the gaps
// and the flake renders as a blob.
static void flake(String& o, int cx) {
    o += F("<line x1=\""); o += cx - 3; o += F("\" y1=\"54\" x2=\""); o += cx + 3; o += F("\" y2=\"54\"/>");
    o += F("<line x1=\""); o += cx - 2; o += F("\" y1=\"51\" x2=\""); o += cx + 2; o += F("\" y2=\"57\"/>");
    o += F("<line x1=\""); o += cx + 2; o += F("\" y1=\"51\" x2=\""); o += cx - 2; o += F("\" y2=\"57\"/>");
}

static void slant(String& o, int x, int len) {
    o += F("<line x1=\""); o += x; o += F("\" y1=\"50\" x2=\""); o += x - (len / 2);
    o += F("\" y2=\""); o += 50 + len; o += F("\"/>");
}

// The WMO code reduced to the one icon that stands for its whole range.
//
// ONE TABLE, TWO RENDERERS. The browser page draws an SVG chosen by the ranges
// below; the panel blits a BMP named after a code, from a set of eleven files.
// The two agreed on the ranges only by accident, and did not: the generator
// makes fc_1_*.bmp for "partly cloudy" and Open-Meteo answers 2 for it about as
// often as 1, so the panel looked for fc_2_52.bmp, did not find it, and fell
// back to fc_-1_52.bmp — the circled question mark. On a dashboard whose
// forecast row is three icons wide, that was three question marks next to a
// browser page showing sun, sun and cloud.
//
// So the collector reduces the code and sends THAT, and the panel does no
// mapping at all. Adding a range here reaches both renderers at once.
int weatherIconCode(int code) {
    if (code < 0)                       return -1;
    if (code == 0)                      return 0;
    if (code <= 2)                      return 1;    // mainly clear, partly cloudy
    if (code == 3)                      return 3;    // overcast
    if (code == 45 || code == 48)       return 45;   // fog, rime fog
    if (code >= 51 && code <= 57)       return 51;   // drizzle, freezing drizzle
    if (code >= 61 && code <= 67)       return 61;   // rain, freezing rain
    if (code >= 71 && code <= 77)       return 71;   // snow, grains
    if (code >= 80 && code <= 82)       return 80;   // rain showers
    if (code == 85 || code == 86)       return 85;   // snow showers
    if (code >= 95)                     return 95;   // thunderstorm
    return -1;                          // a code no range claims
}

// How old the forecast is, in the words the reader chose.
//
// ONE FORMATTER, TWO RENDERERS — the same rule weatherIconCode() exists for,
// and the same drift if it is written twice: change the sixty-minute threshold
// or the wording in one place and the browser page and the panel print
// different ages for the same snapshot. Empty when there is no fetch to
// measure from, which is what both callers test.
void forecastAgeText(char* out, size_t cap, uint32_t fetchedAt, uint32_t now) {
    if (out == nullptr || cap == 0) return;
    out[0] = '\0';
    if (fetchedAt == 0 || now <= fetchedAt) return;
    const uint32_t mins = (now - fetchedAt) / 60u;
    if (mins < 60) snprintf(out, cap, "%u%s", (unsigned)mins, kdT(" min old", " мин"));
    else           snprintf(out, cap, "%u%s", (unsigned)(mins / 60), kdT(" h old", " ч"));
}

void appendWeatherIcon(String& out, int code, int px) {
    out += F("<svg viewBox=\"0 0 64 64\" width=\""); out += px;
    out += F("\" height=\""); out += px;
    out += F("\" stroke=\"#000\" stroke-width=\"2.4\" stroke-linecap=\"round\" "
             "stroke-linejoin=\"round\" fill=\"none\">");

    // Through the same reduction the panel is sent, so the two cannot drift.
    switch (weatherIconCode(code)) {
    case 0:
        out += FPSTR(SUN_FULL);
        break;
    case 1:
        out += FPSTR(SUN_SMALL); out += FPSTR(CLOUD);
        break;
    case 3:
        out += FPSTR(CLOUD);
        break;
    case 45:
        out += FPSTR(CLOUD);
        out += F("<line x1=\"16\" y1=\"51\" x2=\"48\" y2=\"51\"/>"
                 "<line x1=\"16\" y1=\"56\" x2=\"48\" y2=\"56\"/>");
        break;
    case 51:
        out += FPSTR(CLOUD);
        for (int i = 0; i < 3; i++) slant(out, 22 + i * 11, 5);
        break;
    case 61:
        out += FPSTR(CLOUD);
        for (int i = 0; i < 3; i++) slant(out, 22 + i * 11, 10);
        break;
    case 71:
        out += FPSTR(CLOUD);
        for (int i = 0; i < 3; i++) flake(out, 22 + i * 13);
        break;
    case 80:
        out += FPSTR(CLOUD);
        slant(out, 26, 10); slant(out, 37, 10);
        break;
    case 85:
        out += FPSTR(CLOUD);
        slant(out, 24, 10); flake(out, 40);
        break;
    case 95:
        out += FPSTR(CLOUD);
        out += F("<path d=\"M34 48L26 58h7l-3 8 10-12h-7l4-6z\" fill=\"#fff\"/>");
        break;
    default:                       // -1, and anything no range claims
        out += F("<circle cx=\"32\" cy=\"32\" r=\"14\" fill=\"#fff\"/>"
                 "<text x=\"32\" y=\"40\" text-anchor=\"middle\" font-size=\"22\" "
                 "font-family=\"Georgia,serif\" stroke=\"none\" fill=\"#000\">?</text>");
        break;
    }
    out += F("</svg>");
}


// Weekday abbreviation for `daysAhead` from today, taken from the collector's
// own clock rather than by parsing the provider's date strings. The daily
// arrays are always anchored on today, so the offset is all that is needed and
// there is no timezone of theirs to reconcile with ours.
static void weekdayLabel(ForecastModule::Period& p, int daysAhead) {
    const time_t now = (time_t)time(nullptr);
    struct tm tmv;
    p.wday = -1;
    if (now < 1000000000 || localtime_r(&now, &tmv) == nullptr) {
        snprintf(p.label, sizeof(p.label), "+%dd", daysAhead);
        return;
    }
    // THE NUMBER, NOT THE NAME. A forecast is fetched every few hours and read
    // every few minutes, and the language is a setting now — so a name written
    // down here is a name in whatever language was set when the provider was
    // last polled. Switching to Bulgarian would leave three English weekdays
    // sitting under a Bulgarian page until the next poll, up to six hours
    // later, which reads as a translation somebody gave up on.
    //
    // `label` is still filled in, because an hourly column has no weekday and
    // because an older reader of this snapshot has nothing else to print.
    p.wday = (int8_t)((tmv.tm_wday + daysAhead) % 7);
    snprintf(p.label, sizeof(p.label), "%s", kdWeekdayAhead(tmv.tm_wday, daysAhead));
}

/// What an outlook column is captioned, resolved at the moment it is drawn.
const char* forecastPeriodLabel(const ForecastModule::Period& p) {
    return (p.wday >= 0 && p.wday < 7) ? kdWeekdayAhead(p.wday, 0) : p.label;
}

// ---------------------------------------------------------------------------
bool ForecastModule::load(JsonObjectConst cfg) {
    const char* prov = cfg["provider"] | "open-meteo";
    _provider = (strcmp(prov, "owm") == 0) ? PROVIDER_OWM : PROVIDER_OPEN_METEO;

    _lat = cfg["lat"] | 0.0f;
    _lon = cfg["lon"] | 0.0f;

    const char* key = cfg["api_key"] | "";
    strncpy(_apiKey, key, sizeof(_apiKey) - 1);
    _apiKey[sizeof(_apiKey) - 1] = '\0';

    const char* ol = cfg["outlook"] | "hourly";
    _outlook = (strcmp(ol, "daily") == 0) ? OUTLOOK_DAILY : OUTLOOK_HOURLY;

    uint32_t mins = cfg["interval_min"] | 30UL;
    if (mins < 10)   mins = 10;      // a forecast that changes faster than this
    if (mins > 360)  mins = 360;     // does not exist; the floor protects the API
    _intervalMs = mins * 60000UL;

    // A coordinate pair of exactly 0,0 is Null Island, not a location anyone
    // configured — treat it as "not set up yet" rather than fetching for it.
    if (_lat == 0.0f && _lon == 0.0f) {
        Serial.println("[forecast] no coordinates configured");
        return true;   // valid config, just inert
    }
    if (_provider == PROVIDER_OWM && _apiKey[0] == '\0') {
        Serial.println("[forecast] OWM selected but no api_key set");
    }
    return true;
}

bool ForecastModule::save(JsonObject cfg) const {
    cfg["provider"]     = (_provider == PROVIDER_OWM) ? "owm" : "open-meteo";
    cfg["lat"]          = _lat;
    cfg["lon"]          = _lon;
    cfg["api_key"]      = _apiKey;
    cfg["interval_min"] = _intervalMs / 60000UL;
    cfg["outlook"]      = (_outlook == OUTLOOK_DAILY) ? "daily" : "hourly";
    return true;
}

void ForecastModule::tick(uint32_t nowMs) {
    if (!isEnabled())                    return;
    if (_lat == 0.0f && _lon == 0.0f)    return;
    if (WiFi.status() != WL_CONNECTED)   return;

    // The first fetch waits FIRST_FETCH_DELAY_MS after boot. The TLS
    // handshake takes ~40 KB of heap, and on a C3 the first minute is when
    // the web UI is opened too: both at once took min_free_heap to ~5 KB.
    // A refresh button still fetches at once.
    if (_lastAttempt == 0 && nowMs < FIRST_FETCH_DELAY_MS &&
        !_refreshRequested) return;

    // Unsigned subtraction handles the millis() wrap.
    if (_lastAttempt != 0 && (nowMs - _lastAttempt) < _intervalMs &&
        !_refreshRequested) return;
    _lastAttempt = nowMs ? nowMs : 1;
    _fetching = true;
    _refreshRequested = false;

    heapActivityBegin(HA_FORECAST, "fetch");
    const bool ok = _fetch();
    heapActivityEnd(HA_FORECAST);
    _fetching = false;
    if (ok) {
        _failures = 0;
    } else {
        _failures++;
        // The cached forecast is deliberately NOT invalidated on failure. A
        // three-hour-old forecast is still broadly right, and blanking the
        // panel because one HTTPS request timed out trades useful for
        // nothing. Age is shown on the dashboard so the reader can judge.
        Serial.printf("[forecast] fetch failed (%lu consecutive)\n",
                      (unsigned long)_failures);
    }
}

ForecastModule::Refresh ForecastModule::requestRefresh(uint32_t nowMs,
                                                       uint32_t& waitS) {
    waitS = 0;
    if (!isEnabled() || (_lat == 0.0f && _lon == 0.0f)) return REFRESH_OFF;
    // Same gate as tick(): in AP mode, or with the station link down, the
    // export task skips the fetch, so a raised flag would only sit there.
    if (WiFi.status() != WL_CONNECTED) return REFRESH_OFFLINE;
    constexpr uint32_t MIN_GAP_MS = 60000UL;
    const uint32_t last = _lastAttempt;
    if (last != 0 && (nowMs - last) < MIN_GAP_MS) {
        waitS = (MIN_GAP_MS - (nowMs - last) + 999UL) / 1000UL;
        return REFRESH_WAIT;
    }
    _refreshRequested = true;
    return REFRESH_QUEUED;
}

bool ForecastModule::_fetch() {
    return (_provider == PROVIDER_OWM) ? _fetchOwm() : _fetchOpenMeteo();
}

// Shared GET, HTTPS or plain by the URL's scheme. Returns the body, or an
// empty String on failure. The TLS client is only constructed for https://,
// so a plain fetch never touches mbedTLS.
static String httpGet(const char* url) {
    const bool tls = strncmp(url, "https://", 8) == 0;
    WiFiClient plain;
    std::unique_ptr<WiFiClientSecure> secure;
    if (tls) {
        secure.reset(new WiFiClientSecure);
        // No CA bundle shipped yet (R15) — same posture as HttpExporter.
        secure->setInsecure();
    }

    HTTPClient http;   // declared after the clients: destroyed before them
    if (!http.begin(tls ? static_cast<WiFiClient&>(*secure) : plain, url)) return String();
    // Every task stamps a watchdog heartbeat at the top of its loop and
    // TaskManager reboots after 30 s of silence, so this blocking call must
    // finish comfortably inside that. Redirect following is NOT enabled: the
    // default limit is 10 hops, and 10 x this timeout is several times the
    // watchdog window. Neither provider redirects, so the multiplier bought
    // nothing and risked a reboot loop on a misbehaving one.
    http.setTimeout(6000);

    String body;
    const int code = http.GET();
    if (code == 200) {
        // getString() buffers the WHOLE body before the filtered parse gets to
        // discard most of it, so the heap cost is the endpoint's choice, not
        // ours. A forecast response is 2-8 KB; refusing anything past 32 KB
        // keeps a misbehaving or redirected endpoint from taking the C3's heap
        // (and with it the sensor pipeline) rather than merely failing a fetch.
        constexpr int MAX_BODY_BYTES = 32 * 1024;
        const int len = http.getSize();      // -1 when chunked / unknown
        if (len > MAX_BODY_BYTES) {
            Serial.printf("[forecast] body too large (%d B) — refused\n", len);
        } else {
            body = http.getString();
            if (len < 0 && body.length() > (size_t)MAX_BODY_BYTES) {
                // Chunked: the length was unknown until it arrived. Drop it
                // rather than parse it, and free the String on the way out.
                Serial.printf("[forecast] chunked body too large (%u B) — refused\n",
                              (unsigned)body.length());
                body = String();
            }
        }
    }
    else Serial.printf("[forecast] HTTP %d\n", code);
    http.end();
    return body;
}

bool ForecastModule::_fetchOpenMeteo() {
    // One request covers current conditions, the hours and the week, whichever
    // outlook is configured: the Kindle's week strip wants the days while its
    // outlook columns keep the hours. forecast_hours anchors the hourly array
    // on the CURRENT hour rather than on local midnight, which is what makes
    // index 3/6/9 mean +3/+6/+9 h without any date arithmetic here; the daily
    // arrays start today.
    char url[320];
    snprintf(url, sizeof(url),
             OPENMETEO_BASE "/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,wind_speed_10m"
             "&daily=temperature_2m_max,temperature_2m_min,weather_code"
             "&hourly=temperature_2m,weather_code"
             "&timezone=auto&forecast_days=%d&forecast_hours=16",
             (double)_lat, (double)_lon, DAYS_N);

    const String body = httpGet(url);
    if (body.isEmpty()) return false;

    // Filtered parse: the response carries units and metadata this never
    // reads, and on a C3 the difference between parsing all of it and only
    // these fields is several kilobytes of heap.
    JsonDocument filter;
    filter["current"]["temperature_2m"]   = true;
    filter["current"]["weather_code"]     = true;
    filter["current"]["wind_speed_10m"]   = true;
    filter["daily"]["temperature_2m_max"] = true;
    filter["daily"]["temperature_2m_min"] = true;
    filter["daily"]["weather_code"]       = true;
    filter["hourly"]["time"]              = true;
    filter["hourly"]["temperature_2m"]    = true;
    filter["hourly"]["weather_code"]      = true;

    JsonDocument doc;
    if (deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter))) {
        Serial.println("[forecast] open-meteo: bad json");
        return false;
    }

    JsonObjectConst cur = doc["current"];
    if (cur.isNull()) return false;

    Data d;
    d.valid     = true;
    d.tempC     = cur["temperature_2m"] | NAN;
    d.code      = cur["weather_code"]   | -1;
    d.windKph   = cur["wind_speed_10m"] | NAN;   // API already returns km/h
    d.highC     = doc["daily"]["temperature_2m_max"][0] | NAN;
    d.lowC      = doc["daily"]["temperature_2m_min"][0] | NAN;
    d.fetchedAt = (uint32_t)time(nullptr);
    strncpy(d.summary, wmoSummary(d.code), sizeof(d.summary) - 1);

    d.daily = (_outlook == OUTLOOK_DAILY);
    for (int i = 0; i < DAYS_N; i++) {
        // Index 0 is today; the outlook's daily columns are 1..5.
        JsonVariantConst hi = doc["daily"]["temperature_2m_max"][i];
        if (hi.isNull()) break;
        d.days[i].valid = true;
        d.days[i].tempC = hi | NAN;
        d.days[i].lowC  = doc["daily"]["temperature_2m_min"][i] | NAN;
        d.days[i].code  = doc["daily"]["weather_code"][i] | -1;
        weekdayLabel(d.days[i], i);
    }
    for (int i = 0; i < OUTLOOK_N; i++) {
        const int idx = (i + 1) * 3;             // +3 h, +6 h ... +15 h
        JsonVariantConst t = doc["hourly"]["temperature_2m"][idx];
        if (t.isNull()) break;
        d.hours[i].valid = true;
        d.hours[i].tempC = t | NAN;
        d.hours[i].code  = doc["hourly"]["weather_code"][idx] | -1;
        // "2026-08-25T21:00" — take the clock face out of the middle
        // rather than reformatting it; it is already local, because the
        // request asked for timezone=auto.
        const char* iso = doc["hourly"]["time"][idx] | "";
        if (strlen(iso) >= 16) {
            memcpy(d.hours[i].label, iso + 11, 5);
            d.hours[i].label[5] = '\0';
        }
    }

    taskENTER_CRITICAL(&_mux);
    _data = d;
    taskEXIT_CRITICAL(&_mux);
    return true;
}

// OWM's free tier splits what Open-Meteo serves in one response: /weather for
// current conditions, /forecast for the 3-hourly outlook. Two requests, run
// back to back — together roughly 12 s worst case against a 30 s watchdog on
// ExportTask, which is why the per-request timeout is 6 s and redirects are
// off.
bool ForecastModule::_fetchOwm() {
    if (_apiKey[0] == '\0') return false;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/weather"
             "?lat=%.4f&lon=%.4f&units=metric&appid=%s",
             (double)_lat, (double)_lon, _apiKey);

    const String body = httpGet(url);
    if (body.isEmpty()) return false;

    JsonDocument filter;
    filter["main"]["temp"]     = true;
    filter["main"]["temp_max"] = true;
    filter["main"]["temp_min"] = true;
    filter["wind"]["speed"]    = true;
    filter["weather"][0]["id"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter))) {
        Serial.println("[forecast] owm: bad json");
        return false;
    }

    Data d;
    d.valid = true;
    d.tempC = doc["main"]["temp"] | NAN;
    // NOTE: on the free current-weather endpoint temp_min/temp_max are the
    // spread across nearby stations at this moment, NOT today's high and low.
    // They are reported as-is rather than relabelled, because inventing a
    // daily range the API did not supply would be worse than a narrow one.
    d.highC = doc["main"]["temp_max"] | NAN;
    d.lowC  = doc["main"]["temp_min"] | NAN;

    const float ms = doc["wind"]["speed"] | NAN;   // metric units → m/s
    d.windKph = isfinite(ms) ? ms * 3.6f : NAN;

    const int owmId = doc["weather"][0]["id"] | -1;
    d.code      = owmToWmo(owmId);
    d.fetchedAt = (uint32_t)time(nullptr);
    strncpy(d.summary, wmoSummary(d.code), sizeof(d.summary) - 1);

    // A failed outlook leaves the current conditions usable rather than
    // discarding the whole fetch: three empty columns are a smaller loss than
    // a blank forecast block.
    d.daily = (_outlook == OUTLOOK_DAILY);
    _fetchOwmOutlook(d);

    taskENTER_CRITICAL(&_mux);
    _data = d;
    taskEXIT_CRITICAL(&_mux);
    return true;
}

bool ForecastModule::_fetchOwmOutlook(Data& d) {
    // As few 3-hourly slots as are used, which is also the bound on how much
    // JSON lands in heap on a 4 MB C3: the hourly columns are the first five
    // of eight. The days — the daily outlook, or the Kindle's week strip
    // holding the forecast — are aggregated out of the whole of the free
    // tier's 40: today and the next four days, the fifth only partly
    // covered. The free tier has no daily endpoint and nothing past five
    // days, so the last cells of the Kindle's week strip stay empty here.
    bool days = (_outlook == OUTLOOK_DAILY);
#ifdef FEATURE_KINDLE_DASHBOARD
    days = days || (config.kindle.weekStyle & KWEEK_FORECAST);
#endif
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/forecast"
             "?lat=%.4f&lon=%.4f&units=metric&cnt=%d&appid=%s",
             (double)_lat, (double)_lon, days ? 40 : 8, _apiKey);

    const String body = httpGet(url);
    if (body.isEmpty()) return false;

    JsonDocument filter;
    JsonObject item = filter["list"].add<JsonObject>();
    item["dt"] = true;
    item["main"]["temp"] = true;
    item["weather"][0]["id"] = true;
    filter["city"]["timezone"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body.c_str(), body.length(), DeserializationOption::Filter(filter))) {
        Serial.println("[forecast] owm: bad forecast json");
        return false;
    }
    JsonArrayConst list = doc["list"];
    if (list.isNull() || list.size() == 0) return false;

    // list[0] is the next slot, so the first five are +3 .. +15 h.
    const long tz = doc["city"]["timezone"] | 0L;
    for (int i = 0; i < OUTLOOK_N && i < (int)list.size(); i++) {
        JsonObjectConst e = list[i];
        d.hours[i].valid = true;
        d.hours[i].tempC = e["main"]["temp"] | NAN;
        d.hours[i].code  = owmToWmo(e["weather"][0]["id"] | -1);
        // dt is UTC; city.timezone is the location's offset in seconds.
        const time_t local = (time_t)((long)(e["dt"] | 0L) + tz);
        struct tm tmv;
        if (gmtime_r(&local, &tmv) != nullptr) {
            snprintf(d.hours[i].label, sizeof(d.hours[i].label),
                     "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        }
    }

    // The days: max and min per local day, and the condition taken from the
    // slot nearest midday, which is the one that describes the day a reader
    // would recognise. An early-hours shower should not make a sunny day
    // render as rain.
    const time_t nowLocal = (time_t)((long)time(nullptr) + tz);
    struct tm nowTm;
    if (nowLocal < 1000000000 || gmtime_r(&nowLocal, &nowTm) == nullptr) return true;

    struct Acc { bool used = false; float hi = -1e9f, lo = 1e9f; int code = -1; int bestGap = 99; };
    Acc acc[DAYS_N];

    for (JsonObjectConst e : list) {
        const time_t local = (time_t)((long)(e["dt"] | 0L) + tz);
        struct tm tmv;
        if (gmtime_r(&local, &tmv) == nullptr) continue;

        // Whole local days apart, which knows nothing of years, leap or not.
        const int ahead = (int)(local / 86400 - nowLocal / 86400);
        if (ahead < 0 || ahead >= DAYS_N) continue;

        Acc& a = acc[ahead];
        const float t = e["main"]["temp"] | NAN;
        if (!isfinite(t)) continue;
        a.used = true;
        if (t > a.hi) a.hi = t;
        if (t < a.lo) a.lo = t;

        const int gap = abs(tmv.tm_hour - 13);
        if (gap < a.bestGap) {
            a.bestGap = gap;
            a.code    = owmToWmo(e["weather"][0]["id"] | -1);
        }
    }

    // Today with no slot left in it (late evening): what it is doing now.
    if (!acc[0].used && isfinite(d.tempC)) {
        acc[0].used = true;
        acc[0].hi = acc[0].lo = d.tempC;
        acc[0].code = d.code;
    }

    for (int i = 0; i < DAYS_N; i++) {
        if (!acc[i].used) continue;
        d.days[i].valid = true;
        d.days[i].tempC = acc[i].hi;
        d.days[i].lowC  = acc[i].lo;
        d.days[i].code  = acc[i].code;
        weekdayLabel(d.days[i], i);
    }
    return true;
}

ForecastModule::Data ForecastModule::snapshot() const {
    taskENTER_CRITICAL(&_mux);
    Data d = _data;
    taskEXIT_CRITICAL(&_mux);
    return d;
}

void ForecastModule::statusJson(JsonObject out) const {
    const Data d = snapshot();
    out["provider"]  = (_provider == PROVIDER_OWM) ? "owm" : "open-meteo";
    out["valid"]     = d.valid;
    out["fetchedAt"] = d.fetchedAt;
    out["failures"]  = _failures;
    // Queued OR in flight: the pages poll this until it drops.
    out["pending"]   = refreshPending();
    #ifdef FEATURE_KINDLE_DASHBOARD
    // Where the refresh button posts. Only /kindle serves it, so a build
    // without the dashboard leaves the key out and the page shows no button.
    out["refresh"]   = "/kindle/forecast";
    #endif
    if (d.valid) {
        out["tempC"]   = d.tempC;
        // String(), not the bare buffer. `d` is a snapshot living on THIS
        // stack frame, and ArduinoJson links a const char* instead of copying
        // it — the document then holds a pointer into a frame that is gone by
        // the time sendJsonResponse() serialises it. What got written out was
        // whatever had since been pushed over those bytes: garbage that
        // changed between requests, and, when it happened to contain a raw
        // control byte, a response the browser could not parse at all. That
        // took out /api/modules and /api/modules/forecast for everyone.
        //
        // The other fields above are scalars, copied by value; only the char
        // buffer is affected. Every other module's statusJson() assigns a
        // String already — this was the one raw buffer.
        out["summary"] = String(d.summary);
    }
}

const char* forecastSummary(const ForecastModule::Data& d) {
    return wmoSummary(d.code);
}

// ---------------------------------------------------------------------------
// Dashboard section
// ---------------------------------------------------------------------------
void appendForecastSection(String& out, int columns, bool hourly) {
    if (columns < 1) columns = 1;
    if (columns > ForecastModule::OUTLOOK_N) columns = ForecastModule::OUTLOOK_N;
    const ForecastModule::Data d = forecastModule.snapshot();
    if (!d.valid) return;

    // Left: what it is doing now — glyph, word, today's range. Right: three
    // columns stepping forward. Reading order matches the question order,
    // "what is it like" then "what is coming", and keeps the outlook from
    // competing with the measured temperatures higher up the page.
    out += F("<div class=\"rule\"></div><div class=\"sec\">");
    out += kdT("Forecast", "Прогноза");
    out += F("</div><table><tr><td width=\"56\" class=\"ico\">");
    appendWeatherIcon(out, d.code, kdPx(52));
    out += F("</td><td class=\"fc\">");
    out += forecastSummary(d);
    if (isfinite(d.highC) && isfinite(d.lowC)) {
        out += F("<div class=\"fc-t\">");
        out += (int)lroundf(d.highC);
        out += F("&deg; / ");
        out += (int)lroundf(d.lowC);
        out += F("&deg;</div>");
    }
    out += F("<div class=\"sub\">");
    if (isfinite(d.windKph)) {
        out += kdT("wind ", "вятър ");
        out += (int)(d.windKph + 0.5f);
        out += F(" km/h");
    }
    // Age, not the fetch time: it answers "should I believe this?" without the
    // reader doing arithmetic against a clock.
    char age[16];
    forecastAgeText(age, sizeof(age), d.fetchedAt, (uint32_t)time(nullptr));
    if (age[0] != '\0') {
        if (isfinite(d.windKph)) out += F(" &middot; ");
        out += F("<span class=\"dim\">");
        out += age;
        out += F("</span>");
    }
    out += F("</div></td>");

    for (int i = 0; i < columns; i++) {
        const ForecastModule::Period& pd = d.outlookAt(i, hourly);
        out += F("<td class=\"per\">");
        if (pd.valid) {
            out += F("<div class=\"per-l\">");
            out += forecastPeriodLabel(pd);
            out += F("</div>");
            appendWeatherIcon(out, pd.code, kdPx(34));
            out += F("<div class=\"per-t\">");
            out += (int)lroundf(pd.tempC);
            out += F("&deg;");
            // Daily columns carry a range; an hour has none, and printing
            // "11 / 11" would imply a precision the slot does not have.
            if (isfinite(pd.lowC)) {
                out += F("<span class=\"dim\">/");
                out += (int)lroundf(pd.lowC);
                out += F("&deg;</span>");
            }
            out += F("</div>");
        }
        out += F("</td>");
    }
    out += F("</tr></table>");
}

#endif  // MODULE_FORECAST_ENABLED
