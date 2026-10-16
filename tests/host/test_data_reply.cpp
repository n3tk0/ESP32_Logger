// Host tests for DataReply (src/web/DataReply.h): /api/data's chunked reply
// must come out byte for byte as the buffered one did, whatever size of
// piece the web server asks for.
#include <string>
#include "src/web/DataReply.h"
#include "check.h"

static SensorReading* readings(size_t n) {
    SensorReading* a = new SensorReading[n ? n : 1];
    for (size_t i = 0; i < n; i++) {
        a[i].timestamp = 1700000000u + (uint32_t)i;
        a[i].value     = 21.5f + (float)i * 0.01f;
        strcpy(a[i].metric, i % 2 ? "humidity" : "temperature");
        strcpy(a[i].unit,   i % 2 ? "%" : "C");
    }
    return a;
}

// What the handler used to print into one buffer.
static std::string expected(const DataReply& r, const SensorReading* a, size_t n) {
    std::string s(r.head, r.headLen);
    char line[200];
    for (size_t i = 0; i < n; i++) {
        char v[16];
        snprintf(v, sizeof(v), "%.4g", a[i].value);
        snprintf(line, sizeof(line), "%s{\"ts\":%u,\"v\":%s,\"metric\":\"%s\",\"unit\":\"%s\"}",
                 i ? "," : "", (unsigned)a[i].timestamp, v, a[i].metric, a[i].unit);
        s += line;
    }
    return s + "]}";
}

static void test_any_piece_size() {
    const size_t counts[] = { 0, 1, 5, 40, 300 };
    const size_t pieces[] = { 1, 2, 7, 64, 1436 };
    for (size_t n : counts) for (size_t piece : pieces) {
        SensorReading* a = readings(n);
        DataReply* r = DataReply::make(a, n);
        CHECK(r != nullptr);
        r->headLen = (size_t)snprintf(r->head, sizeof(r->head), "{\"count\":%u,\"data\":[", (unsigned)n);
        std::string got;
        uint8_t buf[2048];
        for (size_t k; (k = r->fill(buf, piece)) != 0; ) {
            CHECK(k <= piece);
            got.append((const char*)buf, k);
        }
        CHECK(got == expected(*r, a, n));
        delete r;
        delete[] a;
    }
}

static void test_too_many_metrics_is_refused() {
    const size_t n = DataReply::KINDS + 1;
    SensorReading* a = readings(n);
    for (size_t i = 0; i < n; i++) snprintf(a[i].metric, sizeof(a[i].metric), "m%u", (unsigned)i);
    CHECK(DataReply::make(a, n) == nullptr);
    DataReply* r = DataReply::make(a, n - 1);
    CHECK(r != nullptr);
    delete r;
    delete[] a;
}

int main() {
    RUN(test_any_piece_size);
    RUN(test_too_many_metrics_is_refused);
    return SUMMARY();
}
