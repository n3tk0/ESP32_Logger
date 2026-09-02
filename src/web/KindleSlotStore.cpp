#include "KindleSlotStore.h"

#ifdef FEATURE_KINDLE_DASHBOARD

#include <Arduino.h>
#include <ArduinoJson.h>

static KindleSlotList s_slots;

KindleSlotList& kdSlots() { return s_slots; }

// ---------------------------------------------------------------------------
static void copyField(char* dst, size_t cap, JsonVariantConst v) {
    dst[0] = '\0';
    const char* s = v.as<const char*>();
    if (!s) return;
    strncpy(dst, s, cap - 1);
    dst[cap - 1] = '\0';
}

bool kdSlotsLoad(fs::FS& fs, KindleSlotList& out,
                 const char* outdoorId, const char* indoorId,
                 const char* path) {
    // Whatever happens below, the caller ends up with a usable list. Starting
    // from the defaults rather than from empty means a parse failure shows the
    // dashboard somebody had before, not a blank page.
    kdSlotsDefault(out, outdoorId, indoorId);

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

    JsonArrayConst arr = doc["slots"].as<JsonArrayConst>();
    if (arr.isNull()) {
        // Valid JSON with no slots array. Distinct from a corrupt file: it may
        // be a deliberate "show nothing", so it is honoured rather than
        // replaced with the defaults.
        out.clear();
        return true;
    }

    out.clear();
    for (JsonObjectConst o : arr) {
        if (out.count >= KindleSlotList::CAP) {
            Serial.printf("[kindle] %s holds more than %d slots — the rest ignored\n",
                          path, KindleSlotList::CAP);
            break;
        }
        KindleSlot& s = out.slot[out.count];
        s = KindleSlot{};
        copyField(s.sensorId, sizeof(s.sensorId), o["sensor"]);
        copyField(s.metric,   sizeof(s.metric),   o["metric"]);
        copyField(s.label,    sizeof(s.label),    o["label"]);
        s.size     = (uint8_t)(o["size"]     | (int)KSLOT_MEDIUM);
        s.decimals = (uint8_t)(o["decimals"] | (int)KSLOT_DECIMALS_AUTO);
        s.flags    = (uint8_t)(o["flags"]    | (int)KSLOTF_UNIT);
        if (!s.used()) continue;            // skip rather than store a dead slot
        out.count++;
    }

    kdSlotsClamp(out);
    Serial.printf("[kindle] %d slot(s) loaded from %s\n", out.count, path);
    return true;
}

// ---------------------------------------------------------------------------
bool kdSlotsSave(fs::FS& fs, const KindleSlotList& list, const char* path) {
    // Clamped on a COPY before writing. Storing something a renderer would
    // refuse and then reading it back as though it had been checked is how a
    // bad value becomes permanent.
    KindleSlotList tidy = list;
    kdSlotsClamp(tidy);

    JsonDocument doc;
    JsonArray arr = doc["slots"].to<JsonArray>();
    for (int i = 0; i < tidy.count; i++) {
        const KindleSlot& s = tidy.slot[i];
        JsonObject o = arr.add<JsonObject>();
        o["sensor"] = s.sensorId;
        o["metric"] = s.metric;
        if (s.label[0]) o["label"] = s.label;      // omit when it is the default
        o["size"]  = s.size;
        o["flags"] = s.flags;
        if (s.decimals != KSLOT_DECIMALS_AUTO) o["decimals"] = s.decimals;
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
        Serial.println("[kindle] slot file wrote 0 bytes — leaving the old one");
        fs.remove(tmp.c_str());
        return false;
    }

    // Rename over the top: the old file stays intact until the new one is
    // complete on disk, so a power cut costs the change and not the list.
    fs.remove(path);
    if (!fs.rename(tmp.c_str(), path)) {
        Serial.printf("[kindle] could not rename %s into place\n", tmp.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
void kdSlotsBegin(fs::FS& fs, const char* outdoorId, const char* indoorId) {
    kdSlotsLoad(fs, s_slots, outdoorId, indoorId);
}

#endif  // FEATURE_KINDLE_DASHBOARD
