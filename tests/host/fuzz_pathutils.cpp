// Fuzz target for the security-critical path sanitisers in src/utils/Utils.cpp.
//
// Feeds arbitrary bytes to sanitizePath() / sanitizeFilename() and asserts the
// SAFETY post-conditions the rest of the firmware relies on.  Run under
// libFuzzer+ASan in CI; a -DFUZZ_STANDALONE seeded driver lets it also run
// under plain g++ locally.
#include "src/utils/Utils.cpp"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>

// link stubs (not exercised here)
HostSerial   Serial;
UsbCdcModule usbCdc;
bool   UsbCdcModule::isUsbPinLocked(int) const { return false; }
String UsbCdcModule::getUsbPins() const { return String("18,19"); }

#define FUZZ_CHECK(cond, msg) do { if (!(cond)) {                              \
    fprintf(stderr, "INVARIANT VIOLATED: %s\n", msg); abort(); } } while (0)

static void check_no_bad_bytes(const String& s) {
    const char* c = s.c_str();
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char ch = (unsigned char)c[i];
        FUZZ_CHECK(ch >= 0x20 && ch != 0x7f, "control/DEL byte in sanitised output");
        FUZZ_CHECK(ch != '\\', "backslash in sanitised output");
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    String input(std::string((const char*)data, size));  // keeps embedded NULs

    String p = sanitizePath(input);
    if (!p.isEmpty()) {
        const char* c = p.c_str();
        FUZZ_CHECK(c[0] == '/',                  "sanitizePath result not rooted");
        FUZZ_CHECK(strstr(c, "..") == nullptr,   "sanitizePath leaked '..'");
        FUZZ_CHECK(strstr(c, "//") == nullptr,   "sanitizePath leaked '//'");
        check_no_bad_bytes(p);
        // Normalisation must be idempotent.
        FUZZ_CHECK(sanitizePath(p) == p,         "sanitizePath not idempotent");
    }

    String f = sanitizeFilename(input);
    if (!f.isEmpty()) {
        const char* c = f.c_str();
        FUZZ_CHECK(strchr(c, '/')  == nullptr,   "sanitizeFilename leaked '/'");
        FUZZ_CHECK(f != "." && f != "..",        "sanitizeFilename allowed dot dir");
        check_no_bad_bytes(f);
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
// Deterministic seeded driver so the harness + invariants can run under plain
// g++/ASan locally (no libFuzzer / clang required).
int main() {
    srand(20260529u);
    uint8_t buf[96];
    for (int it = 0; it < 300000; it++) {
        size_t n = (size_t)(rand() % (int)sizeof(buf));
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(rand() & 0xff);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("standalone fuzz: 300000 iterations, all invariants held\n");
    return 0;
}
#endif
