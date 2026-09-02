#include "KindleSlotStore.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <ArduinoJson.h>

static KindleZones s_zones;

KindleZones& kdSlots() { return s_zones; }

// ---------------------------------------------------------------------------
static void copyField(char* dst, size_t cap, JsonVariantConst v) {
    dst[0] = '\0';
    const char* s = v.as<const char*>();
    if (!s) return;
    strncpy(dst, s, cap - 1);
    dst[cap - 1] = '\0';
}

static void readZone(KindleSlot& s, JsonObjectConst o) {
    s = KindleSlot{};
    copyField(s.sensorId, sizeof(s.sensorId), o["sensor"]);
    copyField(s.metric,   sizeof(s.metric),   o["metric"]);
    copyField(s.label,    sizeof(s.label),    o["label"]);
    s.decimals = (uint8_t)(o["decimals"] | (int)KSLOT_DECIMALS_AUTO);
    s.flags    = (uint8_t)(o["flags"]    | (int)KSLOTF_UNIT);
    s.ink      = (uint8_t)(o["ink"]      | (int)KINK_BLACK);
}

// The one earlier shape this file ever had: a "slots" ARRAY of readings that
// packed themselves into rows, from the build before the layout became nine
// named places. Nothing was released on it, but a device that ran it has the
// reader's sensor ids in that file, and those are the tedious part to type
// again — so the array is poured into the places in the order it was written
// rather than thrown away. Sizes are dropped: a place decides its own now.
static void adoptLegacyArray(KindleZones& out, JsonArrayConst arr) {
    static const uint8_t ORDER[] = { KZ_HERO, KZ_BIG, KZ_G1, KZ_G2, KZ_G3,
                                     KZ_G4, KZ_G5, KZ_G6,
                                     KZ_IN1, KZ_IN2, KZ_IN3 };
    int n = 0;
    for (JsonObjectConst o : arr) {
        if (n >= (int)(sizeof(ORDER) / sizeof(ORDER[0]))) break;
        KindleSlot s;
        readZone(s, o);
        if (!s.used()) continue;
        out.z[ORDER[n++]] = s;
    }
    Serial.printf("[kindle] migrated %d reading(s) from the old slot list\n", n);
}

bool kdSlotsLoad(fs::FS& fs, KindleZones& out,
                 const char* outdoorId, const char* indoorId,
                 const char* path) {
    // Whatever happens below, the caller ends up with a usable configuration.
    // Starting from the defaults rather than from empty means a parse failure
    // shows the dashboard somebody had before, not a blank page.
    kdZonesDefault(out, outdoorId, indoorId);

    // Finish a rename interrupted by a power cut mid-save.
    String tmp = String(path) + ".new";
    if (fs.exists(tmp.c_str())) {
        if (!fs.exists(path)) fs.rename(tmp.c_str(), path);
        else                  fs.remove(tmp.c_str());
    }

    if (!fs.exists(path)) return true;   // never configured — defaults stand

    File f = fs.open(path, FILE_READ);
    if (!f) return true;

    // rename() will not overwrite, so an earlier quarantine has to go first or
    // every subsequent bad file is left in place and re-read on the next boot.
    const String bad = String(path) + ".corrupt";

    const size_t sz = f.size();
    if (sz > KINDLE_SLOTS_MAX_BYTES) {
        Serial.printf("[kindle] %s is %u bytes (cap %u) — quarantining\n",
                      path, (unsigned)sz, (unsigned)KINDLE_SLOTS_MAX_BYTES);
        f.close();
        fs.remove(bad.c_str());
        fs.rename(path, bad.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[kindle] %s: %s — quarantining\n", path, err.c_str());
        fs.remove(bad.c_str());
        fs.rename(path, bad.c_str());
        return false;
    }

    JsonObjectConst zones = doc["zones"].as<JsonObjectConst>();
    if (zones.isNull()) {
        JsonArrayConst legacy = doc["slots"].as<JsonArrayConst>();
        if (!legacy.isNull()) {
            out.clear();
            adoptLegacyArray(out, legacy);
            kdZonesClamp(out);
            return true;
        }
        // Valid JSON with neither. Distinct from a corrupt file: it may be a
        // deliberate "show nothing", so it is honoured rather than replaced
        // with the defaults.
        out.clear();
        return true;
    }

    // EVERY PLACE IS RESET FIRST. Reading the file over the top of the defaults
    // would mean a place the reader deliberately emptied came back on the next
    // boot, because the file simply has no key for it.
    out.clear();
    copyField(out.groupOut, sizeof(out.groupOut), doc["groups"]["out"]);
    copyField(out.groupIn,  sizeof(out.groupIn),  doc["groups"]["in"]);

    int loaded = 0;
    for (JsonPairConst kv : zones) {
        const uint8_t z = kdZoneFromKey(kv.key().c_str());
        if (z >= KZ_COUNT) continue;             // a place a later build knows about
        JsonObjectConst o = kv.value().as<JsonObjectConst>();
        if (o.isNull()) continue;
        KindleSlot s;
        readZone(s, o);
        if (!s.used()) continue;                 // stored empty is empty
        out.z[z] = s;
        loaded++;
    }

    kdZonesClamp(out);
    Serial.printf("[kindle] %d place(s) loaded from %s\n", loaded, path);
    return true;
}

// ---------------------------------------------------------------------------
bool kdSlotsSave(fs::FS& fs, const KindleZones& zones, const char* path) {
    // Clamped on a COPY before writing. Storing something a renderer would
    // refuse and then reading it back as though it had been checked is how a
    // bad value becomes permanent.
    KindleZones tidy = zones;
    kdZonesClamp(tidy);

    JsonDocument doc;
    if (tidy.groupOut[0]) doc["groups"]["out"] = tidy.groupOut;
    if (tidy.groupIn[0])  doc["groups"]["in"]  = tidy.groupIn;

    JsonObject zo = doc["zones"].to<JsonObject>();
    for (int i = 0; i < KZ_COUNT; i++) {
        const KindleSlot& s = tidy.z[i];
        if (!s.used()) continue;                  // an empty place writes no key
        JsonObject o = zo[kdZoneKey((uint8_t)i)].to<JsonObject>();
        o["sensor"] = s.sensorId;
        o["metric"] = s.metric;
        if (s.label[0]) o["label"] = s.label;     // omit when it is the default
        o["flags"] = s.flags;
        if (s.decimals != KSLOT_DECIMALS_AUTO) o["decimals"] = s.decimals;
        if (s.ink != KINK_BLACK) o["ink"] = s.ink;   // omit when it is the default
    }

    // /config/ may not exist on a device that has never written a module file.
    if (!fs.exists("/config")) fs.mkdir("/config");

    const String tmp = String(path) + ".new";
    File f = fs.open(tmp.c_str(), FILE_WRITE);
    if (!f) {
        Serial.printf("[kindle] cannot open %s for writing\n", tmp.c_str());
        return false;
    }
    const size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) {
        Serial.println("[kindle] the layout file wrote 0 bytes — leaving the old one");
        fs.remove(tmp.c_str());
        return false;
    }

    // Rename over the top: the old file stays intact until the new one is
    // complete on disk, so a power cut costs the change and not the layout.
    fs.remove(path);
    if (!fs.rename(tmp.c_str(), path)) {
        Serial.printf("[kindle] could not rename %s into place\n", tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void kdSlotsBegin(fs::FS& fs, const char* outdoorId, const char* indoorId) {
    kdSlotsLoad(fs, s_zones, outdoorId, indoorId);
}

#endif  // FEATURE_KINDLE_DASHBOARD
