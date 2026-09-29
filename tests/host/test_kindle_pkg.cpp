// Host unit tests for the Kindle package check — src/web/KindlePkgTar.h,
// docs/KINDLE_UPDATE.md §1.
//
// Archives are built here, byte by byte, as ustar headers — so every refusal
// is exercised against exactly the shape it names, and every accepted package
// is fed in every split from one byte to the whole, which is what the upload
// handler sees segment by segment.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "src/web/KindlePkgTar.h"
#include "check.h"

using namespace kpkg;

// ---------------------------------------------------------------------------
// A tiny tar writer
// ---------------------------------------------------------------------------

struct Entry {
    std::string name;
    std::string data;
    char        type;            // '0', '5', '2', ...
    std::string prefix;
};

static void putOctal(uint8_t* f, size_t n, uint32_t v) {
    // n-1 digits and a NUL, as GNU tar writes them.
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*o", (int)(n - 1), v);
    memcpy(f, buf, n - 1);
    f[n - 1] = 0;
}

static void appendHeader(std::vector<uint8_t>& out, const Entry& e,
                         const char* magic = "ustar", bool badSum = false) {
    uint8_t h[512];
    memset(h, 0, sizeof(h));
    memcpy(h, e.name.data(), e.name.size() < 100 ? e.name.size() : 100);
    putOctal(h + 100, 8, e.type == '5' ? 0755 : 0644);
    putOctal(h + 108, 8, 0);
    putOctal(h + 116, 8, 0);
    putOctal(h + 124, 12, (uint32_t)e.data.size());
    putOctal(h + 136, 12, 0);
    h[156] = (uint8_t)e.type;
    memcpy(h + 257, magic, strlen(magic) + 1 > 6 ? 6 : strlen(magic) + 1);
    h[263] = '0'; h[264] = '0';
    if (!e.prefix.empty()) memcpy(h + 345, e.prefix.data(), e.prefix.size());
    memset(h + 148, ' ', 8);
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    if (badSum) sum++;
    char cs[8];
    snprintf(cs, sizeof(cs), "%06o", sum);
    memcpy(h + 148, cs, 6);
    h[154] = 0; h[155] = ' ';
    out.insert(out.end(), h, h + 512);
}

static void appendEntry(std::vector<uint8_t>& out, const Entry& e) {
    appendHeader(out, e);
    out.insert(out.end(), e.data.begin(), e.data.end());
    const size_t pad = (512 - e.data.size() % 512) % 512;
    out.insert(out.end(), pad, 0);
}

static void appendEnd(std::vector<uint8_t>& out, size_t records = 2) {
    out.insert(out.end(), 512 * records, 0);
}

static std::vector<Entry> goodEntries(const char* ver = "v1.2.3-4-gabcdef0\n") {
    return {
        {"esp32dash/", "", '5', ""},
        {"esp32dash/VERSION", ver, '0', ""},
        {"esp32dash/update_dash.sh", std::string(1300, '#'), '0', ""},
        {"esp32dash/start.sh", "#!/bin/sh\n", '0', ""},
        {"esp32dash/stop.sh", "#!/bin/sh\n", '0', ""},
        {"esp32dash/menu.json", "{}", '0', ""},
        {"esp32dash/layout/", "", '5', ""},
        {"esp32dash/layout/600x800.conf", "X=1\n", '0', ""},
        {"esp32dash/icons/600/fc_-1_34.bmp", std::string(700, 'B'), '0', ""},
    };
}

static std::vector<uint8_t> build(const std::vector<Entry>& es, size_t endRecords = 2) {
    std::vector<uint8_t> v;
    for (const Entry& e : es) appendEntry(v, e);
    appendEnd(v, endRecords);
    return v;
}

static const char* scan(const std::vector<uint8_t>& v, size_t step, char* ver, size_t cap) {
    TarScan s;
    scanBegin(s);
    for (size_t i = 0; i < v.size(); i += step) {
        const size_t n = i + step <= v.size() ? step : v.size() - i;
        scanFeed(s, v.data() + i, n);
    }
    return scanFinish(s, ver, cap);
}

static const char* scanWhole(const std::vector<uint8_t>& v) {
    char ver[VER_CAP];
    return scan(v, v.size() ? v.size() : 1, ver, sizeof(ver));
}

// ---------------------------------------------------------------------------

static void test_good_package_every_split() {
    const std::vector<uint8_t> v = build(goodEntries());
    // 1..600 covers every alignment against a header, and a few big ones.
    for (size_t step = 1; step <= 600; step++) {
        char ver[VER_CAP];
        const char* err = scan(v, step, ver, sizeof(ver));
        if (err) { printf("    step %zu: %s\n", step, err); CHECK(err == nullptr); return; }
        if (strcmp(ver, "v1.2.3-4-gabcdef0") != 0) { CHECK_STREQ(ver, "v1.2.3-4-gabcdef0"); return; }
    }
    char ver[VER_CAP];
    CHECK(scan(v, 1460, ver, sizeof(ver)) == nullptr);
    CHECK_STREQ(ver, "v1.2.3-4-gabcdef0");
    CHECK(scan(v, v.size(), ver, sizeof(ver)) == nullptr);
}

static void test_gnu_record_padding_is_fine() {
    // GNU tar pads the archive to a 10 KB record with zeros.
    CHECK(scanWhole(build(goodEntries(), 20)) == nullptr);
}

static void test_version_without_newline_and_crlf() {
    CHECK(scanWhole(build(goodEntries("dev"))) == nullptr);
    char ver[VER_CAP];
    CHECK(scan(build(goodEntries("2026.9.1\r\n")), 7, ver, sizeof(ver)) == nullptr);
    CHECK_STREQ(ver, "2026.9.1");
}

static void test_bad_version() {
    CHECK_STREQ(scanWhole(build(goodEntries("v1 2\n"))), "bad_version");
    CHECK_STREQ(scanWhole(build(goodEntries("\n"))), "bad_version");
    CHECK_STREQ(scanWhole(build(goodEntries(""))), "bad_version");
    CHECK_STREQ(scanWhole(build(goodEntries("$(reboot)\n"))), "bad_version");
    std::string longv(VER_CAP, 'a');
    CHECK_STREQ(scanWhole(build(goodEntries(longv.c_str()))), "bad_version");
}

static void test_missing_required_file() {
    for (int skip = 1; skip <= 5; skip++) {
        std::vector<Entry> es = goodEntries();
        es.erase(es.begin() + skip);
        const char* err = scanWhole(build(es));
        // Dropping VERSION leaves no version to read either; either reason
        // refuses it, and missing_files is the one checked first.
        CHECK_STREQ(err, "missing_files");
    }
}

static void test_links_and_records_refused() {
    const char types[] = {'1', '2', '3', '4', '6', 'L', 'K', 'x', 'g'};
    for (char t : types) {
        std::vector<Entry> es = goodEntries();
        es.push_back({"esp32dash/evil", "", t, ""});
        CHECK_STREQ(scanWhole(build(es)), "bad_entry");
    }
}

static void test_paths_refused() {
    const char* bad[] = {
        "esp32dash/../etc/passwd",
        "esp32dash/./x",
        "esp32dash//x",
        "/esp32dash/x",
        "other/x",
        "esp32dashx/y",
        "esp32dash/a b",
        "esp32dash/a;b",
        "esp32dash/dash.conf",
        "esp32dash/collectors",
        "esp32dash/kual.log",
        "esp32dash/last.txt",
        "esp32dash/x/",           // a regular file named like a directory
    };
    for (const char* p : bad) {
        std::vector<Entry> es = goodEntries();
        es.push_back({p, "x", '0', ""});
        const char* err = scanWhole(build(es));
        if (!err || strcmp(err, "bad_path") != 0) printf("    path %s -> %s\n", p, err ? err : "(ok)");
        CHECK_STREQ(err, "bad_path");
    }
    std::vector<Entry> es = goodEntries();
    es.push_back({"esp32dash/../", "", '5', ""});
    CHECK_STREQ(scanWhole(build(es)), "bad_path");
}

static void test_prefix_field_joins() {
    std::vector<Entry> es = goodEntries();
    es.push_back({"deep.conf", "x", '0', "esp32dash/layout"});
    CHECK(scanWhole(build(es)) == nullptr);
    es.push_back({"x", "x", '0', "esp32dash/.."});
    CHECK_STREQ(scanWhole(build(es)), "bad_path");
}

static void test_not_a_tar() {
    std::vector<uint8_t> zip = {'P', 'K', 3, 4};
    zip.resize(4096, 0x55);
    CHECK_STREQ(scanWhole(zip), "not_package");

    // The gzip the MRPI payload carries.
    std::vector<uint8_t> gz = {0x1f, 0x8b, 8, 0};
    gz.resize(2048, 0);
    CHECK_STREQ(scanWhole(gz), "not_package");

    // A tar with a wrong checksum.
    std::vector<uint8_t> v;
    appendHeader(v, {"esp32dash/VERSION", "", '0', ""}, "ustar", true);
    appendEnd(v);
    CHECK_STREQ(scanWhole(v), "not_package");

    // Old v7 tar: no magic.
    std::vector<uint8_t> w;
    appendHeader(w, {"esp32dash/VERSION", "", '0', ""}, "");
    appendEnd(w);
    CHECK_STREQ(scanWhole(w), "not_package");

    CHECK_STREQ(scanWhole({}), "not_package");
}

static void test_truncated() {
    const std::vector<uint8_t> v = build(goodEntries());
    for (size_t cut : {size_t(100), size_t(512), size_t(2000), v.size() - 512 - 1, v.size() - 1024}) {
        std::vector<uint8_t> t(v.begin(), v.begin() + cut);
        const char* err = scanWhole(t);
        if (!err || strcmp(err, "truncated") != 0) printf("    cut %zu -> %s\n", cut, err ? err : "(ok)");
        CHECK_STREQ(err, "truncated");
    }
}

static void test_garbage_after_end() {
    std::vector<uint8_t> v = build(goodEntries());
    v.push_back(1);
    CHECK_STREQ(scanWhole(v), "not_package");
}

static void test_one_zero_block_then_more() {
    std::vector<uint8_t> v;
    const std::vector<Entry> es = goodEntries();
    appendEntry(v, es[0]);
    v.insert(v.end(), 512, 0);
    for (size_t i = 1; i < es.size(); i++) appendEntry(v, es[i]);
    appendEnd(v);
    CHECK_STREQ(scanWhole(v), "not_package");
}

static void test_too_big() {
    std::vector<Entry> es = goodEntries();
    es.push_back({"esp32dash/big", std::string(MAX_SIZE, 'x'), '0', ""});
    CHECK_STREQ(scanWhole(build(es)), "too_big");
}

static void test_ver_ok() {
    CHECK(verOk("v1.2.3-4-gabcdef0-dirty"));
    CHECK(verOk("2026.10.1+local"));
    CHECK(!verOk(""));
    CHECK(!verOk(nullptr));
    CHECK(!verOk("a/b"));
    CHECK(!verOk("a&b=c"));
}

int main() {
    RUN(test_good_package_every_split);
    RUN(test_gnu_record_padding_is_fine);
    RUN(test_version_without_newline_and_crlf);
    RUN(test_bad_version);
    RUN(test_missing_required_file);
    RUN(test_links_and_records_refused);
    RUN(test_paths_refused);
    RUN(test_prefix_field_joins);
    RUN(test_not_a_tar);
    RUN(test_truncated);
    RUN(test_garbage_after_end);
    RUN(test_one_zero_block_then_more);
    RUN(test_too_big);
    RUN(test_ver_ok);
    return SUMMARY();
}
