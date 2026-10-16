// Host tests for jsonCopy (src/utils/JsonCopy.h): ArduinoJson keeps only a
// pointer to a const char array, so a document built from a struct that is
// then freed or rewritten serializes what that memory holds by then. Through
// jsonCopy() the document has its own copy.
#include <ArduinoJson.h>
#include <string.h>
#include <string>
#include "src/utils/JsonCopy.h"
#include "check.h"

struct Rec { char id[17]; };

static std::string out(const JsonDocument& d) {
    char buf[128];
    serializeJson(d, buf, sizeof(buf));
    return std::string(buf);
}

// What made /api/latest measure one length and write another: the library
// does not copy the array, and nothing about the call says so.
static void test_const_array_is_kept_by_pointer() {
    Rec r{}; strcpy(r.id, "env_indoor");
    JsonDocument d;
    const Rec& cr = r;
    d["id"] = cr.id;
    strcpy(r.id, "rewritten");
    CHECK(out(d) == "{\"id\":\"rewritten\"}");
}

static void test_json_copy_copies() {
    Rec r{}; strcpy(r.id, "env_indoor");
    JsonDocument d;
    const Rec& cr = r;
    d["id"] = jsonCopy(cr.id);
    JsonArray a = d["all"].to<JsonArray>();
    a.add(jsonCopy(cr.id));
    strcpy(r.id, "rewritten");
    CHECK(out(d) == "{\"id\":\"env_indoor\",\"all\":[\"env_indoor\"]}");
}

static void test_freed_struct() {
    Rec* r = new Rec{}; strcpy(r->id, "outside");
    JsonDocument d;
    d["id"] = jsonCopy(static_cast<const Rec*>(r)->id);
    const size_t n = measureJson(d);
    memset(r, 'x', sizeof(*r));
    delete r;
    CHECK_EQ(measureJson(d), n);
    CHECK(out(d) == "{\"id\":\"outside\"}");
}

int main() {
    RUN(test_const_array_is_kept_by_pointer);
    RUN(test_json_copy_copies);
    RUN(test_freed_struct);
    return SUMMARY();
}
