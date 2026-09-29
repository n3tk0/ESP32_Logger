#include "Datalog.h"
#include "../core/Globals.h"
#include "../managers/DataLogger.h"      // flushLogBufferToFS (a stub off legacy builds)
#include "../managers/StorageManager.h"  // getActiveDatalogFile
#include "../pipeline/DataPipeline.h"    // fsMutex
#include "../tasks/TaskManager.h"
#include "../utils/AtomicWrite.h"
#include <LittleFS.h>
#include <atomic>
#include <memory>
#include <new>
#include <string.h>
#include <time.h>
#include "../utils/JsonIO.h"

// Every write in this file runs under fsMutex: datalogAppend() and
// datalogColsSaveIfLearned() under the caller's, datalogColsFromJson()
// through atomicWrite() with the lock.

namespace {

constexpr const char* COLS_PATH = "/datalog_cols.json";

DatalogCol        s_cols[DL_MAX_ENTRIES];
uint8_t           s_n       = 0;
bool              s_auto    = true;
bool              s_learned = false;
uint32_t          s_rev     = 1;
SemaphoreHandle_t s_mux     = nullptr;

std::atomic<bool> s_flushReq{false};

// Line count of the active file, kept so that the retention check does not
// read the whole file on every write. Valid while the file still has the
// size it had when counted.
// One entry per filesystem: with the mirror on, the primary and the mirror
// would otherwise evict each other and every write would recount both.
struct LineCount { const fs::FS* fs; char path[72]; size_t size; int lines; };
LineCount s_lcs[2] = {};

LineCount& lcFor(fs::FS& fs) {
    if (s_lcs[1].fs == &fs) return s_lcs[1];
    if (s_lcs[0].fs == &fs || !s_lcs[0].fs) return s_lcs[0];
    return s_lcs[1];
}

bool lcValid(fs::FS& fs, const char* path, size_t size) {
    const LineCount& c = lcFor(fs);
    return c.fs == &fs && c.size == size && strcmp(c.path, path) == 0;
}

class ColsLock {
public:
    ColsLock()  { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
    ~ColsLock() { if (s_mux) xSemaphoreGive(s_mux); }
};

uint8_t onCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_n; i++) n += s_cols[i].on;
    return n;
}

void setCol(DatalogCol& c, const char* s, const char* m, const char* l) {
    c.on = onCount() < DL_MAX_COLS;     // before c counts: s_n is not bumped yet
    c.agg = DL_AGG_AVG;
    strlcpy(c.sensor, s ? s : "", sizeof(c.sensor));
    strlcpy(c.metric, m ? m : "", sizeof(c.metric));
    if (l && *l) strlcpy(c.label, l, sizeof(c.label));
    else snprintf(c.label, sizeof(c.label), "%s_%s", c.sensor, c.metric);
}

// Which metric is logged in which column, and how it is combined; labels are
// not part of it. The revision moves only when this does, so saving the page
// with the same columns does not throw the window in progress away — and a
// changed mode does, since the window's row goes to a file whose header says
// the new mode (see datalogHeader()).
uint32_t numbering() {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < s_n; i++) {
        if (!s_cols[i].on) continue;
        for (const char* p = s_cols[i].sensor; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        h = (h ^ '|') * 16777619u;
        for (const char* p = s_cols[i].metric; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        h = (h ^ (0x100u | s_cols[i].agg)) * 16777619u;
        h = (h ^ '\n') * 16777619u;
    }
    return h;
}

// Parses {"auto":..,"cols":[..]} into the list. Caller holds the lock.
bool parseCols(JsonVariantConst v) {
    if (!v.is<JsonObjectConst>()) return false;
    const uint32_t before = numbering();
    JsonArrayConst a = v["cols"];
    s_auto = v["auto"] | true;
    s_n = 0;
    for (JsonObjectConst o : a) {
        const char* s = o["s"] | "";
        const char* m = o["m"] | "";
        if (!*s || !*m || s_n >= DL_MAX_ENTRIES) continue;
        DatalogCol& c = s_cols[s_n];
        setCol(c, s, m, o["l"] | "");
        c.on = c.on && !(o["off"] | false);
        c.agg = datalogAggFromName(o["a"] | "");
        s_n++;
    }
    if (numbering() != before) s_rev++;
    return true;
}

void colsJson(JsonObject out) {
    out["auto"] = s_auto;
    JsonArray a = out["cols"].to<JsonArray>();
    for (uint8_t i = 0; i < s_n; i++) {
        JsonObject o = a.add<JsonObject>();
        o["s"] = s_cols[i].sensor;
        o["m"] = s_cols[i].metric;
        o["l"] = s_cols[i].label;
        if (!s_cols[i].on) o["off"] = true;
        // Only when it is not the average, which a column without it means.
        if (s_cols[i].agg != DL_AGG_AVG) o["a"] = datalogAggName(s_cols[i].agg);
    }
}

bool saveCols(fs::FS& fs, SemaphoreHandle_t lock) {
    JsonDocument doc;
    {
        ColsLock g;
        colsJson(doc.to<JsonObject>());
        s_learned = false;
    }
    return atomicWrite(fs, COLS_PATH, [&](File& f) {
        // Through Print&: the serializer the firmware already carries.
        return serializeJson(doc, static_cast<Print&>(f)) > 0;
    }, lock);
}

// ── file helpers ────────────────────────────────────────────────────────────

// Days since 1970-01-01 of a civil date (H. Hinnant's days_from_civil).
long civilDays(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

// Which rotation period a moment falls in, in local time; -1 = no periods.
long periodOf(uint32_t epoch, uint8_t rotation, struct tm* lt) {
    time_t t = (time_t)epoch;
    localtime_r(&t, lt);
    const long days = civilDays(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    switch (rotation) {
        case ROTATION_DAILY:   return days;
        case ROTATION_WEEKLY:  return (days + 3) / 7;   // weeks start on Monday
        case ROTATION_MONTHLY: return (lt->tm_year + 1900L) * 12 + lt->tm_mon;
        default:               return -1;
    }
}

// Moves the active file aside as <stem>_<suffix><ext>, keeping its history
// reachable from the Dashboard's file list. A failed rename leaves the file
// where it is, so the rows still land somewhere.
void archive(fs::FS& fs, const char* path, const char* suffix) {
    const char* slash = strrchr(path, '/');
    const char* dot   = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) dot = path + strlen(path);
    char dst[112];
    for (int k = 1; k <= 9; k++) {
        char tag[8] = "";
        if (k > 1) snprintf(tag, sizeof(tag), "-%d", k);
        snprintf(dst, sizeof(dst), "%.*s_%s%s%s", (int)(dot - path), path, suffix, tag, dot);
        if (!fs.exists(dst)) break;
    }
    if (fs.rename(path, dst)) Serial.printf("[datalog] %s -> %s\n", path, dst);
    else                      Serial.printf("[datalog] could not move %s aside\n", path);
    lcFor(fs).path[0] = '\0';
}

int countLines(fs::FS& fs, const char* path, size_t size) {
    if (lcValid(fs, path, size)) return lcFor(fs).lines;
    File f = fs.open(path, "r");
    if (!f) return 0;
    int count = 0;
    uint8_t buf[256];
    for (;;) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) if (buf[i] == '\n') count++;
    }
    f.close();
    LineCount& c = lcFor(fs);
    c.fs = &fs;
    strlcpy(c.path, path, sizeof(c.path));
    c.size  = size;
    c.lines = count;
    return count;
}

// Drops the oldest rows so that, with `adding` more, the file holds 90 % of
// maxEntries: one rewrite every tenth of the limit instead of one per write.
// The header stays.
bool trim(fs::FS& fs, const char* path, size_t size, int adding) {
    const int maxRows = config.datalog.maxEntries;
    if (maxRows <= 0) return true;
    const int rows = countLines(fs, path, size) - 1;   // the header is not a row
    if (rows + adding <= maxRows) return true;
    int drop = rows + adding - (maxRows - maxRows / 10);
    if (drop > rows) drop = rows;
    if (drop <= 0) return true;

    bool ok = atomicWrite(fs, path, [&](File& dst) -> bool {
        File src = fs.open(path, "r");
        if (!src) return false;
        int line = 0;                   // newlines passed; line 0 is the header
        uint8_t buf[256];
        for (;;) {
            const int n = src.read(buf, sizeof(buf));
            if (n <= 0) break;
            for (int i = 0; i < n; ) {
                const uint8_t* nl = (const uint8_t*)memchr(buf + i, '\n', n - i);
                // Past the dropped rows the rest of the buffer goes in one write.
                const int end = line > drop ? n : (nl ? (int)(nl - buf) + 1 : n);
                if ((line == 0 || line > drop) &&
                    dst.write(buf + i, end - i) != (size_t)(end - i)) {
                    src.close(); return false;
                }
                if (line <= drop && nl) line++;
                i = end;
            }
        }
        src.close();
        return true;
    }, nullptr);   // the caller holds fsMutex
    lcFor(fs).path[0] = '\0';
    if (ok) Serial.printf("[datalog] trimmed %d old rows from %s\n", drop, path);
    return ok;
}

}  // namespace

// ============================================================================
// Columns
// ============================================================================
void datalogColsBegin(fs::FS& fs) {
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    File f = fs.open(COLS_PATH, "r");
    if (!f) return;                     // no file: auto, every metric
    JsonDocument doc;
    DeserializationError e = deserializeJsonFile(doc, f);
    f.close();
    ColsLock g;
    if (e || !parseCols(doc.as<JsonVariantConst>()))
        Serial.println("[datalog] /datalog_cols.json unreadable - logging every metric");
}

int datalogColsCopy(DatalogCol* out, int max, uint32_t* rev) {
    ColsLock g;
    int n = 0;
    for (uint8_t i = 0; i < s_n && n < max; i++) {
        if (!s_cols[i].on) continue;
        if (out) out[n] = s_cols[i];
        n++;
    }
    if (rev) *rev = s_rev;
    return n;
}

uint32_t datalogColsRev() {
    ColsLock g;
    return s_rev;
}

int datalogColsAggs(uint8_t* out, int max, uint32_t* rev) {
    ColsLock g;
    int n = 0;
    for (uint8_t i = 0; i < s_n && n < max; i++)
        if (s_cols[i].on) out[n++] = s_cols[i].agg;
    if (rev) *rev = s_rev;
    return n;
}

int datalogColsLearn(const char* sensor, const char* metric) {
    if (!sensor || !metric) return -1;
    ColsLock g;
    int col = 0;                        // position among the logged columns
    for (uint8_t i = 0; i < s_n; i++) {
        const DatalogCol& c = s_cols[i];
        if (strncmp(c.sensor, sensor, sizeof(c.sensor) - 1) == 0 &&
            strncmp(c.metric, metric, sizeof(c.metric) - 1) == 0)
            return c.on ? col : -1;
        col += c.on;
    }
    if (!s_auto || s_n >= DL_MAX_ENTRIES) return -1;
    // Appending keeps every existing column where it was, so the revision
    // (which says "renumbered") does not change.
    DatalogCol& c = s_cols[s_n];
    setCol(c, sensor, metric, nullptr);
    s_n++;
    s_learned = true;
    return c.on ? col : -1;
}

void datalogColsSaveIfLearned(fs::FS& fs) {
    bool learned;
    { ColsLock g; learned = s_learned; }
    if (learned && !saveCols(fs, nullptr))
        Serial.println("[datalog] could not save /datalog_cols.json");
}

void datalogColsToJson(JsonObject out) {
    ColsLock g;
    colsJson(out);
}

bool datalogColsFromJson(JsonVariantConst v) {
    {
        ColsLock g;
        if (!parseCols(v)) return false;
    }
    return littleFsAvailable ? saveCols(LittleFS, fsMutex) : true;
}

// ============================================================================
// Layout and header
// ============================================================================
DatalogLayout datalogLayout(bool possible) {
    const DatalogConfig& d = config.datalog;
    DatalogLayout l = {};
    l.dateFormat   = d.dateFormat;
    l.timeFormat   = d.timeFormat;
    l.endFormat    = d.endFormat;
    l.volumeFormat = d.volumeFormat;
    l.boot         = d.includeBootCount;
#if PLATFORM_LEGACY_BUILD
    const bool legacy = g_platformMode == PLATFORM_LEGACY;
#else
    const bool legacy = false;
#endif
#if defined(SENSOR_WATERFLOW_ENABLED)
    const bool runs = legacy || g_platformMode == PLATFORM_HYBRID;
#else
    const bool runs = legacy;
#endif
    const bool extra = possible || d.includeExtraPresses;
    l.volume = runs && (possible || d.volumeFormat != VOL_OFF);
    l.ff = legacy && extra && config.hardware.pinWakeupFF != PIN_UNSET;
    l.pf = legacy && extra && config.hardware.pinWakeupPF != PIN_UNSET;
    return l;
}

bool datalogSensorRows() {
    return config.logger.csvLoggingEnabled && g_platformMode != PLATFORM_LEGACY;
}

int datalogHeader(char* buf, size_t cap, uint32_t* rev) {
    const DatalogLayout l = datalogLayout();
    ColsLock g;                         // the labels are read in place
    if (rev) *rev = s_rev;
    const char* labels[DL_MAX_COLS];
    uint8_t     aggs[DL_MAX_COLS];
    int n = 0;
    for (uint8_t i = 0; datalogSensorRows() && i < s_n && n < DL_MAX_COLS; i++)
        if (s_cols[i].on) { aggs[n] = s_cols[i].agg; labels[n++] = s_cols[i].label; }
    return dlFormatHeader(buf, cap, l, labels, n, aggs);
}

bool datalogSamePeriod(uint32_t a, uint32_t b) {
    struct tm lt;
    const uint8_t rot = config.datalog.rotation;
    if (a < 1000000000UL || b < 1000000000UL) return true;
    return periodOf(a, rot, &lt) == periodOf(b, rot, &lt);
}

// ============================================================================
// Writer
// ============================================================================
int datalogAppend(fs::FS& fs, const char* header, const char* lines,
                  int nLines, uint32_t epoch) {
    if (!header || !lines || nLines <= 0) return 0;
    char path[72];
    strlcpy(path, getActiveDatalogFile().c_str(), sizeof(path));

    // The folder, when the name has one.
    if (const char* slash = strrchr(path, '/')) {
        if (slash > path) {
            String dir(path, slash - path);
            if (!fs.exists(dir)) fs.mkdir(dir);
        }
    }

    // Rotation and the header, on the file as it stands.
    size_t size = 0;
    if (fs.exists(path)) {
        File f = fs.open(path, "r");
        if (f) {
            size = f.size();
            const time_t lastWrite = f.getLastWrite();
            // Longer than any header (DL_HEADER_MAX). On the heap: the legacy
            // flush also runs on the web server's task.
            constexpr size_t FIRST = DL_HEADER_MAX;
            std::unique_ptr<char[]> first(new (std::nothrow) char[FIRST]);
            if (!first) { f.close(); return -1; }
            size_t k = f.readBytesUntil('\n', first.get(), FIRST - 1);
            first[k] = '\0';
            if (k && first[k - 1] == '\r') first[k - 1] = '\0';
            f.close();

            const uint8_t rot = config.datalog.rotation;
            struct tm lt;
            char suffix[24] = "";
            // Both times must be real clock times: a row stamped before the
            // clock was set would otherwise start a new file every write.
            const long period = epoch > 1000000000UL ? periodOf(epoch, rot, &lt) : -1;
            if (period >= 0 && lastWrite > 1000000000L &&
                periodOf((uint32_t)lastWrite, rot, &lt) != period) {
                if (rot == ROTATION_MONTHLY) strftime(suffix, sizeof(suffix), "%Y-%m", &lt);
                else                         strftime(suffix, sizeof(suffix), "%Y-%m-%d", &lt);
            } else if (size == 0) {
                // An empty file just takes the header.
            } else if ((rot == ROTATION_SIZE && size > config.datalog.maxSizeKB * 1024UL) ||
                       strcmp(first.get(), header) != 0) {
                // Too big, or a different layout: a file keeps one header.
                time_t t = (time_t)epoch;
                localtime_r(&t, &lt);
                strftime(suffix, sizeof(suffix), "%Y%m%d-%H%M%S", &lt);
            }
            if (suffix[0]) { archive(fs, path, suffix); size = 0; }
        }
    }

    if (size > 0 && !trim(fs, path, size, nLines)) {
        // Not appending, rather than deleting the file: the usual reason the
        // trim fails is a full filesystem, and the rows already on it are
        // the history. The file just must not grow past its limit.
        Serial.println("[datalog] trim failed - rows not written, log left intact");
        return 0;
    }

    File f = fs.open(path, FILE_APPEND);
    if (!f) { Serial.printf("[datalog] cannot open %s\n", path); return -1; }
    const size_t before = f.size();
    if (before == 0) { f.print(header); f.print("\r\n"); }

    int written = 0;
    const char* p = lines;
    for (int i = 0; i < nLines && *p; i++) {
        const char* nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (f.write((const uint8_t*)p, len) != len) break;
        written++;
        p += len;
    }
    const size_t after = f.size();
    f.close();

    // Keep the line count in step with the file instead of recounting it.
    const int hdr = before == 0 ? 1 : 0;
    if (before == 0 || lcValid(fs, path, before)) {
        LineCount& c = lcFor(fs);
        c.lines = (before == 0 ? 0 : c.lines) + hdr + written;
        c.fs = &fs;
        strlcpy(c.path, path, sizeof(c.path));
        c.size  = after;
    }
    return written;
}

// ============================================================================
// Flush on demand
// ============================================================================
void datalogRequestFlush()    { s_flushReq.store(true); }
bool datalogFlushRequested()  { return s_flushReq.load(); }
void datalogFlushDone()       { s_flushReq.store(false); }

void datalogFlushAndWait(uint32_t timeoutMs) {
    flushLogBufferToFS();
    if (!TaskManager::running.load()) return;
    datalogRequestFlush();
    const uint32_t t0 = millis();
    while (s_flushReq.load() && millis() - t0 < timeoutMs) delay(20);
}
