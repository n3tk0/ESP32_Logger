// Host unit tests for the security-critical path / URL helpers in
// src/utils/Utils.cpp: sanitizePath, sanitizeFilename, isPathProtected,
// buildPath, urlEncode.
//
// Utils.cpp is compiled straight into this TU; the few firmware globals it
// references but that these tests don't exercise (Serial, usbCdc) are stubbed
// below so the binary links.
#include "src/utils/Utils.cpp"
#include "check.h"

// ---- link stubs (not under test) -------------------------------------------
HostSerial   Serial;
UsbCdcModule usbCdc;
bool   UsbCdcModule::isUsbPinLocked(int) const { return false; }
String UsbCdcModule::getUsbPins() const { return String("18,19"); }

// ---- buildPath -------------------------------------------------------------
static void test_buildPath() {
    CHECK(buildPath("/", "f.txt")   == "/f.txt");
    CHECK(buildPath("",  "f.txt")   == "/f.txt");
    CHECK(buildPath("/a", "b")      == "/a/b");
    CHECK(buildPath("/a/b", "c.txt")== "/a/b/c.txt");
}

// ---- sanitizePath ----------------------------------------------------------
static void test_sanitizePath_normalises() {
    CHECK(sanitizePath("/a/b")     == "/a/b");
    CHECK(sanitizePath("a/b")      == "/a/b");   // adds leading slash
    CHECK(sanitizePath("/a/./b")   == "/a/b");   // strips "."
    CHECK(sanitizePath("//a///b/") == "/a/b");   // collapses + trims trailing
    CHECK(sanitizePath("/")        == "/");
}

static void test_sanitizePath_rejects() {
    CHECK(sanitizePath("")          == "");      // empty
    CHECK(sanitizePath("/a/../b")   == "");      // traversal
    CHECK(sanitizePath("..")        == "");      // bare traversal
    CHECK(sanitizePath("/a/b\\c")   == "");      // backslash
    CHECK(sanitizePath("/a\001b")   == "");      // control char (octal escape)
    String tooLong = "/";
    for (int i = 0; i < 300; i++) tooLong += "a";
    CHECK(sanitizePath(tooLong)     == "");      // > 256 cap
}

// ---- sanitizeFilename ------------------------------------------------------
static void test_sanitizeFilename() {
    CHECK(sanitizeFilename("data.csv") == "data.csv");
    CHECK(sanitizeFilename("")         == "");
    CHECK(sanitizeFilename(".")        == "");
    CHECK(sanitizeFilename("..")       == "");
    CHECK(sanitizeFilename("a/b")      == "");   // no slashes allowed
    CHECK(sanitizeFilename("a\\b")     == "");   // no backslash
    CHECK(sanitizeFilename("a\007b")   == "");   // control char (BEL, octal escape)
}

// ---- isPathProtected -------------------------------------------------------
static void test_isPathProtected() {
    CHECK(isPathProtected("/config.bin"));
    CHECK(isPathProtected("/platform_config.json"));
    CHECK(isPathProtected("/_setup"));
    CHECK(isPathProtected("/_setup/wizard.json"));
    CHECK(!isPathProtected("/www/index.html"));
    CHECK(!isPathProtected("/logs/2026-05-29.csv"));
    CHECK(!isPathProtected(""));
}

// ---- urlEncode -------------------------------------------------------------
static void test_urlEncode() {
    CHECK(urlEncode("abcXYZ123") == "abcXYZ123");
    CHECK(urlEncode("-_.~")      == "-_.~");      // unreserved, untouched
    CHECK(urlEncode("a b")       == "a%20b");
    CHECK(urlEncode("a&b=c")     == "a%26b%3Dc");
    CHECK(urlEncode("/")         == "%2F");
}

int main() {
    RUN(test_buildPath);
    RUN(test_sanitizePath_normalises);
    RUN(test_sanitizePath_rejects);
    RUN(test_sanitizeFilename);
    RUN(test_isPathProtected);
    RUN(test_urlEncode);
    return SUMMARY();
}
