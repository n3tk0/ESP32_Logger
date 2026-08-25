#include "ForecastModule.h"

#ifdef MODULE_FORECAST_ENABLED

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>

ForecastModule forecastModule;

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
    if (code < 0)                  return "Unknown";
    if (code == 0)                 return "Clear";
    if (code <= 2)                 return "Partly cloudy";
    if (code == 3)                 return "Overcast";
    if (code == 45 || code == 48)  return "Fog";
    if (code >= 51 && code <= 57)  return "Drizzle";
    if (code >= 61 && code <= 67)  return "Rain";
    if (code >= 71 && code <= 77)  return "Snow";
    if (code >= 80 && code <= 82)  return "Showers";
    if (code == 85 || code == 86)  return "Snow showers";
    if (code >= 95)                return "Thunderstorm";
    return "Unknown";
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

void appendWeatherIcon(String& out, int code, int px) {
    out += F("<svg viewBox=\"0 0 64 64\" width=\""); out += px;
    out += F("\" height=\""); out += px;
    out += F("\" stroke=\"#000\" stroke-width=\"2.4\" stroke-linecap=\"round\" "
             "stroke-linejoin=\"round\" fill=\"none\">");

    if (code < 0) {
        out += F("<circle cx=\"32\" cy=\"32\" r=\"14\" fill=\"#fff\"/>"
                 "<text x=\"32\" y=\"40\" text-anchor=\"middle\" font-size=\"22\" "
                 "font-family=\"Georgia,serif\" stroke=\"none\" fill=\"#000\">?</text>");
    } else if (code == 0) {
        out += FPSTR(SUN_FULL);
    } else if (code <= 2) {
        out += FPSTR(SUN_SMALL); out += FPSTR(CLOUD);
    } else if (code == 3) {
        out += FPSTR(CLOUD);
    } else if (code == 45 || code == 48) {
        out += FPSTR(CLOUD);
        out += F("<line x1=\"16\" y1=\"51\" x2=\"48\" y2=\"51\"/>"
                 "<line x1=\"16\" y1=\"56\" x2=\"48\" y2=\"56\"/>");
    } else if (code >= 51 && code <= 57) {
        out += FPSTR(CLOUD);
        for (int i = 0; i < 3; i++) slant(out, 22 + i * 11, 5);
    } else if (code >= 61 && code <= 67) {
        out += FPSTR(CLOUD);
        for (int i = 0; i < 3; i++) slant(out, 22 + i * 11, 10);
    } else if (code >= 71 && code <= 77) {
        out += FPSTR(CLOUD);
        for (int i = 0; i < 3; i++) flake(out, 22 + i * 13);
    } else if (code >= 80 && code <= 82) {
        out += FPSTR(CLOUD);
        slant(out, 26, 10); slant(out, 37, 10);
    } else if (code == 85 || code == 86) {
        out += FPSTR(CLOUD);
        slant(out, 24, 10); flake(out, 40);
    } else {                       // >= 95, thunderstorm
        out += FPSTR(CLOUD);
        out += F("<path d=\"M34 48L26 58h7l-3 8 10-12h-7l4-6z\" fill=\"#fff\"/>");
    }
    out += F("</svg>");
}


// Weekday abbreviation for `daysAhead` from today, taken from the collector's
// own clock rather than by parsing the provider's date strings. The daily
// arrays are always anchored on today, so the offset is all that is needed and
// there is no timezone of theirs to reconcile with ours.
static void weekdayLabel(char* out, size_t n, int daysAhead) {
    static const char* NAMES[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    const time_t now = (time_t)time(nullptr);
    struct tm tmv;
    if (now < 1000000000 || localtime_r(&now, &tmv) == nullptr) {
        snprintf(out, n, "+%dd", daysAhead);
        return;
    }
    snprintf(out, n, "%s", NAMES[(tmv.tm_wday + daysAhead) % 7]);
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

    // Unsigned subtraction handles the millis() wrap. _lastAttempt starts at
    // 0, so the first tick after boot fetches immediately.
    if (_lastAttempt != 0 && (nowMs - _lastAttempt) < _intervalMs) return;
    _lastAttempt = nowMs ? nowMs : 1;

    if (_fetch()) {
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

bool ForecastModule::_fetch() {
    return (_provider == PROVIDER_OWM) ? _fetchOwm() : _fetchOpenMeteo();
}

// Shared HTTPS GET. Returns the body, or an empty String on failure.
static String httpsGet(const char* url) {
    WiFiClientSecure client;
    // No CA bundle shipped yet (R15) — same posture as HttpExporter.
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) return String();
    // Every task stamps a watchdog heartbeat at the top of its loop and
    // TaskManager reboots after 30 s of silence, so this blocking call must
    // finish comfortably inside that. Redirect following is NOT enabled: the
    // default limit is 10 hops, and 10 x this timeout is several times the
    // watchdog window. Neither provider redirects, so the multiplier bought
    // nothing and risked a reboot loop on a misbehaving one.
    http.setTimeout(6000);

    String body;
    const int code = http.GET();
    if (code == 200) body = http.getString();
    else Serial.printf("[forecast] HTTP %d\n", code);
    http.end();
    return body;
}

bool ForecastModule::_fetchOpenMeteo() {
    // One request covers current conditions and whichever outlook is
    // configured. forecast_hours anchors the hourly array on the CURRENT hour
    // rather than on local midnight, which is what makes index 3/6/9 mean
    // +3/+6/+9 h without any date arithmetic here.
    char url[320];
    if (_outlook == OUTLOOK_DAILY) {
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast"
                 "?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,weather_code,wind_speed_10m"
                 "&daily=temperature_2m_max,temperature_2m_min,weather_code"
                 "&timezone=auto&forecast_days=4",
                 (double)_lat, (double)_lon);
    } else {
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast"
                 "?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,weather_code,wind_speed_10m"
                 "&daily=temperature_2m_max,temperature_2m_min"
                 "&hourly=temperature_2m,weather_code"
                 "&timezone=auto&forecast_days=1&forecast_hours=12",
                 (double)_lat, (double)_lon);
    }

    const String body = httpsGet(url);
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
    if (_outlook == OUTLOOK_DAILY) {
        filter["daily"]["weather_code"]   = true;
    } else {
        filter["hourly"]["time"]           = true;
        filter["hourly"]["temperature_2m"] = true;
        filter["hourly"]["weather_code"]   = true;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
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

    if (_outlook == OUTLOOK_DAILY) {
        for (int i = 0; i < 3; i++) {
            // Index 0 is today, so the three columns are +1..+3 days.
            JsonVariantConst hi = doc["daily"]["temperature_2m_max"][i + 1];
            if (hi.isNull()) break;
            d.outlook[i].valid = true;
            d.outlook[i].tempC = hi | NAN;
            d.outlook[i].lowC  = doc["daily"]["temperature_2m_min"][i + 1] | NAN;
            d.outlook[i].code  = doc["daily"]["weather_code"][i + 1] | -1;
            weekdayLabel(d.outlook[i].label, sizeof(d.outlook[i].label), i + 1);
        }
    } else {
        for (int i = 0; i < 3; i++) {
            const int idx = (i + 1) * 3;             // +3 h, +6 h, +9 h
            JsonVariantConst t = doc["hourly"]["temperature_2m"][idx];
            if (t.isNull()) break;
            d.outlook[i].valid = true;
            d.outlook[i].tempC = t | NAN;
            d.outlook[i].code  = doc["hourly"]["weather_code"][idx] | -1;
            // "2026-08-25T21:00" — take the clock face out of the middle
            // rather than reformatting it; it is already local, because the
            // request asked for timezone=auto.
            const char* iso = doc["hourly"]["time"][idx] | "";
            if (strlen(iso) >= 16) {
                memcpy(d.outlook[i].label, iso + 11, 5);
                d.outlook[i].label[5] = '\0';
            }
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

    const String body = httpsGet(url);
    if (body.isEmpty()) return false;

    JsonDocument filter;
    filter["main"]["temp"]     = true;
    filter["main"]["temp_max"] = true;
    filter["main"]["temp_min"] = true;
    filter["wind"]["speed"]    = true;
    filter["weather"][0]["id"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
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
    _fetchOwmOutlook(d);

    taskENTER_CRITICAL(&_mux);
    _data = d;
    taskEXIT_CRITICAL(&_mux);
    return true;
}

bool ForecastModule::_fetchOwmOutlook(Data& d) {
    // cnt=24 is three days of 3-hourly slots — enough for either mode and a
    // hard bound on how much JSON lands in heap on a 4 MB C3.
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/2.5/forecast"
             "?lat=%.4f&lon=%.4f&units=metric&cnt=24&appid=%s",
             (double)_lat, (double)_lon, _apiKey);

    const String body = httpsGet(url);
    if (body.isEmpty()) return false;

    JsonDocument filter;
    JsonObject item = filter["list"].add<JsonObject>();
    item["dt"] = true;
    item["main"]["temp"] = true;
    item["weather"][0]["id"] = true;
    filter["city"]["timezone"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        Serial.println("[forecast] owm: bad forecast json");
        return false;
    }
    JsonArrayConst list = doc["list"];
    if (list.isNull() || list.size() == 0) return false;

    if (_outlook == OUTLOOK_HOURLY) {
        // list[0] is the next slot, so the first three are +3/+6/+9 h.
        const long tz = doc["city"]["timezone"] | 0L;
        for (int i = 0; i < 3 && i < (int)list.size(); i++) {
            JsonObjectConst e = list[i];
            d.outlook[i].valid = true;
            d.outlook[i].tempC = e["main"]["temp"] | NAN;
            d.outlook[i].code  = owmToWmo(e["weather"][0]["id"] | -1);
            // dt is UTC; city.timezone is the location's offset in seconds.
            const time_t local = (time_t)((long)(e["dt"] | 0L) + tz);
            struct tm tmv;
            if (gmtime_r(&local, &tmv) != nullptr) {
                snprintf(d.outlook[i].label, sizeof(d.outlook[i].label),
                         "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
        }
        return true;
    }

    // Daily mode. The free tier has no daily endpoint, so the days are
    // aggregated out of the same 3-hourly list: max and min per local day,
    // and the condition taken from the slot nearest midday, which is the one
    // that describes the day a reader would recognise. An early-hours shower
    // should not make a sunny day render as rain.
    const long tz = doc["city"]["timezone"] | 0L;
    const time_t nowLocal = (time_t)((long)time(nullptr) + tz);
    struct tm nowTm;
    if (nowLocal < 1000000000 || gmtime_r(&nowLocal, &nowTm) == nullptr) return false;
    const int today = nowTm.tm_yday;

    struct Acc { bool used = false; float hi = -1e9f, lo = 1e9f; int code = -1; int bestGap = 99; };
    Acc acc[3];

    for (JsonObjectConst e : list) {
        const time_t local = (time_t)((long)(e["dt"] | 0L) + tz);
        struct tm tmv;
        if (gmtime_r(&local, &tmv) == nullptr) continue;

        int ahead = tmv.tm_yday - today;
        if (ahead < 0) ahead += 365;              // year wrap
        if (ahead < 1 || ahead > 3) continue;     // only tomorrow .. +3 days

        Acc& a = acc[ahead - 1];
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

    for (int i = 0; i < 3; i++) {
        if (!acc[i].used) continue;
        d.outlook[i].valid = true;
        d.outlook[i].tempC = acc[i].hi;
        d.outlook[i].lowC  = acc[i].lo;
        d.outlook[i].code  = acc[i].code;
        weekdayLabel(d.outlook[i].label, sizeof(d.outlook[i].label), i + 1);
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
    if (d.valid) {
        out["tempC"]   = d.tempC;
        out["summary"] = d.summary;
    }
}

// ---------------------------------------------------------------------------
// Dashboard section
// ---------------------------------------------------------------------------
void appendForecastSection(String& out) {
    const ForecastModule::Data d = forecastModule.snapshot();
    if (!d.valid) return;

    // Left: what it is doing now — glyph, word, today's range. Right: three
    // columns stepping forward. Reading order matches the question order,
    // "what is it like" then "what is coming", and keeps the outlook from
    // competing with the measured temperatures higher up the page.
    out += F("<div class=\"rule\"></div><div class=\"sec\">Forecast</div>"
             "<table><tr><td width=\"56\" class=\"ico\">");
    appendWeatherIcon(out, d.code, 52);
    out += F("</td><td class=\"fc\">");
    out += d.summary;
    if (isfinite(d.highC) && isfinite(d.lowC)) {
        out += F("<div class=\"fc-t\">");
        out += (int)lroundf(d.highC);
        out += F("&deg; / ");
        out += (int)lroundf(d.lowC);
        out += F("&deg;</div>");
    }
    out += F("<div class=\"sub\">");
    if (isfinite(d.windKph)) {
        out += F("wind ");
        out += (int)(d.windKph + 0.5f);
        out += F(" km/h");
    }
    // Age, not the fetch time: it answers "should I believe this?" without the
    // reader doing arithmetic against a clock.
    const uint32_t now = (uint32_t)time(nullptr);
    if (d.fetchedAt > 0 && now > d.fetchedAt) {
        const uint32_t ageMin = (now - d.fetchedAt) / 60u;
        if (isfinite(d.windKph)) out += F(" &middot; ");
        out += F("<span class=\"dim\">");
        if (ageMin < 60) { out += ageMin; out += F(" min old"); }
        else             { out += (ageMin / 60); out += F(" h old"); }
        out += F("</span>");
    }
    out += F("</div></td>");

    for (int i = 0; i < 3; i++) {
        const ForecastModule::Period& pd = d.outlook[i];
        out += F("<td class=\"per\">");
        if (pd.valid) {
            out += F("<div class=\"per-l\">"); out += pd.label; out += F("</div>");
            appendWeatherIcon(out, pd.code, 34);
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
