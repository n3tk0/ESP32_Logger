#include "DatalogFormat.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

// Appends to buf at *n; false (and *n unchanged) when it does not fit.
bool put(char* buf, size_t cap, int* n, const char* s) {
    size_t len = strlen(s);
    if ((size_t)*n + len >= cap) return false;
    memcpy(buf + *n, s, len);
    *n += (int)len;
    buf[*n] = '\0';
    return true;
}

// A header label must not break the line into more fields or lines.
bool putLabel(char* buf, size_t cap, int* n, const char* s) {
    if (!put(buf, cap, n, s ? s : "")) return false;
    for (char* p = buf + *n - strlen(s ? s : ""); *p; p++)
        if (*p == '|' || (unsigned char)*p < 0x20) *p = '_';
    return true;
}

void fmtClock(char* out, size_t cap, uint8_t timeFormat, const struct tm& t) {
    switch (timeFormat) {
        case 1:  // TIME_HHMM
            snprintf(out, cap, "%02d:%02d", t.tm_hour, t.tm_min); break;
        case 2: { // TIME_12H
            int h = t.tm_hour % 12; if (!h) h = 12;
            snprintf(out, cap, "%d:%02d:%02d%s", h, t.tm_min, t.tm_sec,
                     t.tm_hour < 12 ? "AM" : "PM");
            break;
        }
        default: // TIME_HHMMSS
            snprintf(out, cap, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
DatalogFieldIdx dlFieldIndex(const DatalogLayout& l) {
    DatalogFieldIdx ix;
    int8_t i = 0;
    ix.date    = l.dateFormat != 0 ? i++ : -1;
    ix.start   = i++;
    ix.end     = l.endFormat != 2 ? i++ : -1;
    ix.boot    = l.boot ? i++ : -1;
    ix.trigger = i++;
    ix.volume  = l.volume ? i++ : -1;
    ix.ff      = l.ff ? i++ : -1;
    ix.pf      = l.pf ? i++ : -1;
    ix.nBase   = (uint8_t)i;
    return ix;
}

// ---------------------------------------------------------------------------
int dlFormatHeader(char* buf, size_t cap, const DatalogLayout& l,
                   const char* const* labels, int nLabels) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    int n = 0;
    bool ok = true;
    if (l.dateFormat != 0) ok = ok && put(buf, cap, &n, "Date|");
    ok = ok && put(buf, cap, &n, "Start");
    if (l.endFormat != 2) ok = ok && put(buf, cap, &n, l.endFormat == 1 ? "|Duration" : "|End");
    if (l.boot)   ok = ok && put(buf, cap, &n, "|Boot");
    ok = ok && put(buf, cap, &n, "|Trigger");
    if (l.volume) ok = ok && put(buf, cap, &n, "|Volume");
    if (l.ff)     ok = ok && put(buf, cap, &n, "|FF");
    if (l.pf)     ok = ok && put(buf, cap, &n, "|PF");
    for (int i = 0; ok && i < nLabels; i++)
        ok = put(buf, cap, &n, "|") && putLabel(buf, cap, &n, labels[i]);
    return ok ? n : -1;
}

// ---------------------------------------------------------------------------
int dlFormatValue(char* buf, size_t cap, float v) {
    if (!buf || cap == 0) return -1;
    if (!isfinite(v)) { buf[0] = '\0'; return 0; }
    int n = snprintf(buf, cap, "%.2f", (double)v);
    if (n < 0 || (size_t)n >= cap) { buf[0] = '\0'; return -1; }
    if (strchr(buf, '.')) {
        while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
        if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
    }
    if (strcmp(buf, "-0") == 0) { buf[0] = '0'; buf[1] = '\0'; n = 1; }
    return n;
}

// ---------------------------------------------------------------------------
int dlFormatRow(char* buf, size_t cap, const DatalogLayout& l,
                const DatalogRow& r, const float* vals, int nVals) {
    if (!buf || cap == 0) return -1;
    buf[0] = '\0';
    // UTC epochs shown as local time; 0 stays the zeroed struct the original
    // format printed for "no end time".
    struct tm st = {}, et = {};
    { time_t t = (time_t)r.start; if (r.start) localtime_r(&t, &st); }
    { time_t t = (time_t)r.end;   if (r.end)   localtime_r(&t, &et); }

    int n = 0;
    bool ok = true;
    char f[40];   // room for any int the compiler cannot rule out

    if (l.dateFormat != 0) {
        const int d = st.tm_mday, m = st.tm_mon + 1, y = st.tm_year + 1900;
        switch (l.dateFormat) {
            case 2:  snprintf(f, sizeof(f), "%02d/%02d/%04d", m, d, y); break;
            case 3:  snprintf(f, sizeof(f), "%04d-%02d-%02d", y, m, d); break;
            case 4:  snprintf(f, sizeof(f), "%02d.%02d.%04d", d, m, y); break;
            default: snprintf(f, sizeof(f), "%02d/%02d/%04d", d, m, y);
        }
        ok = ok && put(buf, cap, &n, f) && put(buf, cap, &n, "|");
    }
    fmtClock(f, sizeof(f), l.timeFormat, st);
    ok = ok && put(buf, cap, &n, f);

    if (l.endFormat != 2) {
        if (l.endFormat == 1) {
            uint32_t dur = (r.end > r.start) ? r.end - r.start : 0;
            snprintf(f, sizeof(f), "%lus", (unsigned long)dur);
        } else {
            fmtClock(f, sizeof(f), l.timeFormat, et);
        }
        ok = ok && put(buf, cap, &n, "|") && put(buf, cap, &n, f);
    }
    if (l.boot) {
        snprintf(f, sizeof(f), "|#:%u", (unsigned)r.boot);
        ok = ok && put(buf, cap, &n, f);
    }
    ok = ok && put(buf, cap, &n, "|") && put(buf, cap, &n, r.trigger);

    // From here on a field may be empty; `lastFull` remembers where the row
    // held its last non-empty field so the empty tail can be cut off.
    int lastFull = n;
    if (l.volume) {
        ok = ok && put(buf, cap, &n, "|");
        if (ok && isfinite(r.volume)) {
            char v[20];
            snprintf(v, sizeof(v), "%.2f", (double)r.volume);
            if (l.volumeFormat == 0) { for (char* p = v; *p; p++) if (*p == '.') *p = ','; }
            if (l.volumeFormat <= 1) ok = put(buf, cap, &n, "L:");
            ok = ok && put(buf, cap, &n, v);
            lastFull = n;
        }
    }
    if (l.ff) {
        ok = ok && put(buf, cap, &n, "|");
        if (ok && r.ff >= 0) {
            snprintf(f, sizeof(f), "FF%d", r.ff);
            ok = put(buf, cap, &n, f);
            lastFull = n;
        }
    }
    if (l.pf) {
        ok = ok && put(buf, cap, &n, "|");
        if (ok && r.pf >= 0) {
            snprintf(f, sizeof(f), "PF%d", r.pf);
            ok = put(buf, cap, &n, f);
            lastFull = n;
        }
    }
    for (int i = 0; ok && vals && i < nVals; i++) {
        ok = put(buf, cap, &n, "|");
        if (ok && dlFormatValue(f, sizeof(f), vals[i]) > 0) {
            ok = put(buf, cap, &n, f);
            lastFull = n;
        }
    }
    if (!ok) { buf[0] = '\0'; return -1; }
    n = lastFull;
    buf[n] = '\0';
    return n;
}

// ---------------------------------------------------------------------------
bool dlIsHeaderLine(const char* line) {
    if (!line || !*line) return false;
    for (const char* p = line; *p && *p != '|' && *p != '\r' && *p != '\n'; p++)
        if (*p >= '0' && *p <= '9') return false;
    return true;
}
