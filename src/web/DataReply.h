#pragma once
// src/web/DataReply.h
//
// /api/data's reply, written a piece at a time (beginChunkedResponse): the
// points, compact, and where the writing has got to. Points carry an index
// into a small metric/unit table. The page always asks for one sensor and
// metric; a request over more than KINDS pairs (no filter, many sensors) is
// refused as busy rather than grown.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <new>

#include "../core/SensorTypes.h"   // SensorReading

struct DataReply {
    struct Pt { uint32_t ts; float v; uint8_t k; };
    static constexpr size_t KINDS = 32;    // a sensor has at most 12 metrics (#217)
    char   metric[KINDS][sizeof(SensorReading::metric)];
    char   unit[KINDS][sizeof(SensorReading::unit)];
    size_t kinds = 0;
    Pt*    pts = nullptr;
    size_t count = 0;
    char   head[256];
    size_t headLen = 0;
    size_t next = 0;          // next point to write
    size_t headDone = 0;      // bytes of head already written
    char   line[96];          // a point that did not fit the last piece
    size_t lineLen = 0, lineDone = 0;
    size_t tailDone = 0;      // bytes of "]}" already written

    ~DataReply() { delete[] pts; }

    static DataReply* make(const SensorReading* agg, size_t n) {
        DataReply* r = new (std::nothrow) DataReply();
        if (!r) return nullptr;
        r->pts = n ? new (std::nothrow) Pt[n] : nullptr;
        if (n && !r->pts) { delete r; return nullptr; }
        for (size_t i = 0; i < n; i++) {
            size_t k = 0;
            while (k < r->kinds && (strcmp(r->metric[k], agg[i].metric) ||
                                    strcmp(r->unit[k], agg[i].unit))) k++;
            if (k == r->kinds) {
                if (k == KINDS) { delete r; return nullptr; }
                snprintf(r->metric[k], sizeof(r->metric[k]), "%s", agg[i].metric);
                snprintf(r->unit[k],   sizeof(r->unit[k]),   "%s", agg[i].unit);
                r->kinds++;
            }
            r->pts[i] = { agg[i].timestamp, agg[i].value, (uint8_t)k };
        }
        r->count = n;
        return r;
    }

    // Copies what is left of [src, src+len) from *done on; true once all of it is out.
    static bool put(uint8_t*& out, size_t& room, const char* src, size_t len, size_t& done) {
        const size_t k = len - done < room ? len - done : room;
        memcpy(out, src + done, k);
        out += k; room -= k; done += k;
        return done == len;
    }

    size_t fill(uint8_t* buf, size_t maxLen) {
        uint8_t* out = buf;
        size_t room = maxLen;
        if (!put(out, room, head, headLen, headDone)) return maxLen;
        for (;;) {
            if (lineDone < lineLen && !put(out, room, line, lineLen, lineDone))
                return maxLen;
            if (next >= count) break;
            const Pt& p = pts[next];
            char valBuf[16];
            snprintf(valBuf, sizeof(valBuf), "%.4g", p.v);
            // Include metric and unit so clients can display axes correctly (#12)
            int k = snprintf(line, sizeof(line),
                             "%s{\"ts\":%u,\"v\":%s,\"metric\":\"%s\",\"unit\":\"%s\"}",
                             next ? "," : "", (unsigned)p.ts, valBuf,
                             metric[p.k], unit[p.k]);
            lineLen  = k < 0 ? 0 : ((size_t)k < sizeof(line) ? (size_t)k : sizeof(line) - 1);
            lineDone = 0;
            next++;
        }
        put(out, room, "]}", 2, tailDone);
        return (size_t)(out - buf);    // 0 once everything is out: the end
    }
};
