// Host tests for src/web/SettingsJson.cpp — the settings file behind
// GET /export_settings and POST /import_settings.
//
// The file is written by settingsToJson() and read by settingsFromJson(), and
// most sections go through the modules' own save()/load(): the same code the
// Modules page and modules.json use. What a user relies on when restoring a
// backup is that the file brings back what it holds, and leaves alone what it
// deliberately does not carry. Checked here:
//
//   * export → import → export gives back the same file, and importing a
//     device's own file changes nothing in its configuration;
//   * a file restores every setting it carries after they were all changed,
//     including the Kindle language and page shape that older backups lost;
//   * what the file does not restore (WiFi credentials and addresses, pins,
//     the device id, the current log file) survives an import untouched;
//   * a key missing from an older file leaves its setting alone;
//   * values the forms would refuse are refused from a file too.
//
// SettingsJson.cpp, KindleConfigJson.cpp, the four modules it calls and
// Utils.cpp (sanitizePath) are compiled into this TU. The data log's sensor
// columns are faked below: they live in their own file on the device, and all
// the settings file does is pass them through.
#include <Arduino.h>
#include <string>

// The modules range-check enum fields with `(int)e < 0`, which -Wextra calls
// always false for an unsigned enum; true here, and harmless on the device,
// where the check is kept for the day the enum's type changes. Silenced for
// the firmware sources only, so that a warning in this file still shows.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
// ThemeModule.cpp and DataLogModule.cpp each have a file-local copyStr(), and
// this is one translation unit, so each is given its own name here.
#define copyStr themeCopyStr
#include "src/modules/ThemeModule.cpp"
#undef copyStr
#define copyStr datalogCopyStr
#include "src/modules/DataLogModule.cpp"
#undef copyStr
#include "src/modules/WiFiModule.cpp"
#include "src/modules/TimeModule.cpp"
#include "src/web/KindleConfigJson.cpp"
#include "src/web/SettingsJson.cpp"
#include "src/utils/Utils.cpp"
#pragma GCC diagnostic pop
#include "check.h"

// ---- the firmware around it ------------------------------------------------
DeviceConfig config;
HostSerial   Serial;
UsbCdcModule usbCdc;
bool   UsbCdcModule::isUsbPinLocked(int) const { return false; }
String UsbCdcModule::getUsbPins() const { return String("18,19"); }

volatile uint8_t g_pendingNtpSync   = 0;
volatile int8_t  g_lastNtpSyncResult = 0;
static int g_tzApplied = 0;
void applyTimeZone() { g_tzApplied++; }

// The sensor columns: a document of their own, passed through whole.
static JsonDocument g_cols;
void datalogColsToJson(JsonObject out) { out.set(g_cols.as<JsonObjectConst>()); }
bool datalogColsFromJson(JsonVariantConst v) { g_cols.set(v); return true; }
DatalogLayout datalogLayout(bool) {
    DatalogLayout l{};
    l.volume = true; l.ff = true; l.pf = false;
    return l;
}

// ---- helpers ----------------------------------------------------------------
static std::string exportFile(bool reveal = false) {
    JsonDocument d;
    settingsToJson(d.to<JsonObject>(), reveal);
    std::string s;
    serializeJson(d, s);
    return s;
}

static bool importFile(const std::string& s) {
    JsonDocument d;
    if (deserializeJson(d, s.c_str(), s.size())) return false;
    settingsFromJson(d.as<JsonObjectConst>());
    return true;
}

static void setIp(uint8_t ip[4], uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    ip[0] = a; ip[1] = b; ip[2] = c; ip[3] = d;
}

template <size_t N>
static void put(char (&dst)[N], const char* s) {
    strncpy(dst, s, N - 1);
    dst[N - 1] = '\0';
}

// Two configurations are the same when every field is. Not memcmp() alone:
// a string field holds whatever an earlier, longer value left after its NUL,
// which is not part of the setting, so those bytes are cleared first.
template <size_t N>
static void tidy(char (&s)[N]) {
    const size_t n = strnlen(s, N);
    memset(s + n, 0, N - n);
}

static void tidy(DeviceConfig& c) {
    tidy(c.deviceId); tidy(c.deviceName);
    ThemeConfig& t = c.theme;
    tidy(t.primaryColor); tidy(t.secondaryColor); tidy(t.accentColor);
    tidy(t.lightBgColor); tidy(t.lightTextColor); tidy(t.darkBgColor);
    tidy(t.darkTextColor); tidy(t.ffColor); tidy(t.pfColor); tidy(t.otherColor);
    tidy(t.storageBarColor); tidy(t.storageBar70Color); tidy(t.storageBar90Color);
    tidy(t.storageBarBorder); tidy(t.logoSource); tidy(t.faviconPath);
    tidy(t.boardDiagramPath); tidy(t.chartLocalPath);
    tidy(c.datalog.prefix); tidy(c.datalog.currentFile); tidy(c.datalog.folder);
    NetworkConfig& n = c.network;
    tidy(n.apSSID); tidy(n.apPassword); tidy(n.clientSSID);
    tidy(n.clientPassword); tidy(n.ntpServer);
    tidy(c.kindle.faceCustom); tidy(c.kindle.outdoorSensor); tidy(c.kindle.indoorSensor);
}

static bool sameConfig(DeviceConfig a, DeviceConfig b) {
    tidy(a);
    tidy(b);
    return memcmp(&a, &b, sizeof(a)) == 0;
}

// A device someone has set up: every exported field away from zero, every
// value one the forms would accept. Floats are exact in binary, so nothing
// can differ by a rounding step on the way through the text.
static void populate() {
    memset(&config, 0, sizeof(config));
    put(config.deviceId,   "A1B2C3");
    put(config.deviceName, "Garden logger");
    config.forceWebServer = true;

    ThemeConfig& t = config.theme;
    t.mode = THEME_DARK;
    put(t.primaryColor, "#123456");   put(t.secondaryColor, "#abcdef");
    put(t.lightBgColor, "#fafafa");   put(t.lightTextColor, "#111");
    put(t.darkBgColor,  "#202020");   put(t.darkTextColor,  "#eee");
    put(t.ffColor,      "#ff0000");   put(t.pfColor,        "#00ff00");
    put(t.otherColor,   "#0000ff");
    put(t.storageBarColor, "#010101"); put(t.storageBar70Color, "#020202");
    put(t.storageBar90Color, "#030303"); put(t.storageBarBorder, "#040404");
    put(t.logoSource,   "https://example.com/logo.png");
    put(t.faviconPath,  "/www/fav.ico");
    put(t.boardDiagramPath, "/www/board.svg");
    t.chartSource = CHART_CDN;
    put(t.chartLocalPath, "/www/uplot.js");
    t.showIcons = true;
    t.chartLabelFormat = LABEL_BOTH;

    DatalogConfig& d = config.datalog;
    put(d.prefix, "garden");
    put(d.currentFile, "/logs/garden_1.txt");
    put(d.folder, "/logs");
    d.rotation = ROTATION_WEEKLY;
    d.maxSizeKB = 2048;
    d.maxEntries = 5000;
    d.includeDeviceId = true;
    d.timestampFilename = true;
    d.dateFormat = 3; d.timeFormat = 2; d.endFormat = 1; d.volumeFormat = 2;
    d.includeBootCount = true;
    d.includeExtraPresses = true;
    d.postCorrectionEnabled = true;
    d.pfToFfThreshold = 5.5f;
    d.ffToPfThreshold = 2.25f;
    d.manualPressThresholdMs = 750;

    config.flowMeter.pulsesPerLiter = 330.5f;
    config.flowMeter.calibrationMultiplier = 1.25f;
    config.flowMeter.testMode = true;
    config.flowMeter.blinkDuration = 300;

    HardwareConfig& h = config.hardware;
    h.storageType = STORAGE_SD_CARD;
    h.wakeupMode = WAKEUP_GPIO_ACTIVE_LOW;
    h.pinWifiTrigger = 2; h.pinWakeupFF = 3; h.pinWakeupPF = 4; h.pinFlowSensor = 5;
    h.pinRtcCE = 6; h.pinRtcIO = 7; h.pinRtcSCLK = 8;
    h.pinSdCS = 9; h.pinSdMOSI = 10; h.pinSdMISO = 20; h.pinSdSCK = 21;
    h.cpuFreqMHz = 160;
    h.activeCpuMHz = 80;
    h.defaultStorageView = 1;
    h.debounceMs = 150;

    NetworkConfig& n = config.network;
    n.wifiMode = WIFIMODE_CLIENT;
    put(n.apSSID, "Logger-AP");
    put(n.apPassword, "ap-secret");
    put(n.clientSSID, "HomeNet");
    put(n.clientPassword, "client-secret");
    n.useStaticIP = true;
    setIp(n.staticIP, 192, 168, 1, 50);
    setIp(n.gateway,  192, 168, 1, 1);
    setIp(n.subnet,   255, 255, 255, 0);
    setIp(n.dns,      1, 1, 1, 1);
    put(n.ntpServer, "time.example.org");
    n.timezone = -5;
    n.dstOffsetHours = 1;
    setIp(n.apIP,      10, 0, 0, 1);
    setIp(n.apGateway, 10, 0, 0, 1);
    setIp(n.apSubnet,  255, 255, 255, 0);
    n.dstRule = 1;

    config.logger.csvLoggingEnabled = true;
    config.logger.aggregationIntervalSec = 300;

    KindleConfig& k = config.kindle;
    k.face = KFACE_CUSTOM;
    put(k.faceCustom, "Georgia, serif");
    k.boldZones = 0x0105;
    k.showFlags = 0x00A5;
    k.clockStyle = KCLOCK_RULED;
    k.timeFormat = KTIME_12;
    k.dateFormat = KDATE_ISO;
    k.pressureUnit = KPRESS_MMHG;
    k.tempDecimals = 1;
    k.refreshSec = 900;
    k.followData = 1;
    k.clockPinRefresh = 0;
    k.fbinkResW = 1072;
    put(k.outdoorSensor, "bme_out");
    put(k.indoorSensor, "sht_in");
    k.lang = KLANG_BG;
    k.layoutMode = KLAYOUT_STANDALONE;
    k.rotation = KROT_90;
    k.clockOff = 1;
    k.clockSync = 7;
    k.pageRot = KROT_180 + 1;
    k.weekStyle = KWEEK_OUTLINE | KWEEK_FORECAST;
    k.rules = kdRulesPack(2, 1, 2);
    k.metricSize = kdSizePack(3, 5);
    kdSkinClamp(k);   // a stored config has been through it

    g_cols.clear();
    g_cols["temp_out"] = true;
    g_cols["hum_out"]  = false;
}

// ---- export → import → export ----------------------------------------------
static void test_own_file_changes_nothing() {
    populate();
    const std::string a = exportFile();
    const DeviceConfig before = config;
    CHECK(importFile(a));
    CHECK(sameConfig(config, before));
    const std::string b = exportFile();
    CHECK(a == b);
    if (a != b) std::printf("        first:  %s\n        second: %s\n", a.c_str(), b.c_str());
}

// Every setting the file carries, changed after the backup was taken, comes
// back from it — this list IS the contract of what an import restores.
static void test_restores_what_it_carries() {
    populate();
    const std::string a = exportFile();
    const DeviceConfig want = config;
    const std::string wantCols = [] { std::string s; serializeJson(g_cols, s); return s; }();

    put(config.deviceName, "Something else");
    config.forceWebServer = false;

    ThemeConfig& t = config.theme;
    t.mode = THEME_LIGHT;
    put(t.primaryColor, "#000000"); put(t.secondaryColor, "#000000");
    put(t.lightBgColor, "#000000"); put(t.lightTextColor, "#000000");
    put(t.darkBgColor,  "#000000"); put(t.darkTextColor,  "#000000");
    put(t.ffColor, "#000000"); put(t.pfColor, "#000000"); put(t.otherColor, "#000000");
    put(t.logoSource, "/www/other.png");
    put(t.faviconPath, "/www/other.ico");
    put(t.chartLocalPath, "/www/other.js");
    t.chartSource = CHART_LOCAL;
    t.showIcons = false;
    t.chartLabelFormat = LABEL_DATETIME;

    DatalogConfig& d = config.datalog;
    put(d.prefix, "other");
    put(d.folder, "/other");
    d.rotation = ROTATION_NONE;
    d.maxSizeKB = 100; d.maxEntries = 100;
    d.includeDeviceId = false; d.timestampFilename = false;
    d.dateFormat = 0; d.timeFormat = 0; d.endFormat = 0; d.volumeFormat = 0;
    d.includeBootCount = false; d.includeExtraPresses = false;
    d.postCorrectionEnabled = false;
    d.pfToFfThreshold = 1.0f; d.ffToPfThreshold = 1.0f;
    d.manualPressThresholdMs = 10;
    g_cols.clear();

    config.flowMeter.pulsesPerLiter = 1.0f;
    config.flowMeter.calibrationMultiplier = 2.0f;

    config.hardware.storageType = STORAGE_LITTLEFS;
    config.hardware.wakeupMode = WAKEUP_GPIO_ACTIVE_HIGH;
    config.hardware.cpuFreqMHz = 80;
    config.hardware.activeCpuMHz = 0;
    config.hardware.defaultStorageView = 0;
    config.hardware.debounceMs = 50;

    NetworkConfig& n = config.network;
    n.wifiMode = WIFIMODE_AP;
    n.useStaticIP = false;
    put(n.ntpServer, "pool.ntp.org");
    n.timezone = 3; n.dstOffsetHours = 2; n.dstRule = 2;

    config.logger.csvLoggingEnabled = false;
    config.logger.aggregationIntervalSec = 60;

    memset(&config.kindle, 0, sizeof(config.kindle));

    g_tzApplied = 0;
    CHECK(importFile(a));
    CHECK(sameConfig(config, want));
    CHECK(exportFile() == a);
    std::string cols; serializeJson(g_cols, cols);
    CHECK(cols == wantCols);
    CHECK(g_tzApplied >= 1);   // the time zone is applied by the module
}

// The Kindle language and page shape, which a backup used to drop: restoring
// one quietly put the panel back to the built-in language and "auto".
static void test_kindle_lang_and_layout_survive() {
    populate();
    const std::string a = exportFile();
    CHECK(a.find("\"lang\":2") != std::string::npos);
    CHECK(a.find("\"layoutMode\":2") != std::string::npos);
    config.kindle.lang = KLANG_AUTO;
    config.kindle.layoutMode = KLAYOUT_AUTO;
    CHECK(importFile(a));
    CHECK_EQ(config.kindle.lang, KLANG_BG);
    CHECK_EQ(config.kindle.layoutMode, KLAYOUT_STANDALONE);
}

// What the file does not bring back: the WiFi credentials (the passwords are
// "***" in it), the client SSID and the addresses, which without the passwords
// would point the device at a network it cannot join; the pins, which belong
// to this board; the device id; the log file being written; and the fields no
// import ever took (the storage bar colours, the board picture, the flow
// meter's test switches).
static void test_leaves_alone_what_it_does_not_carry() {
    populate();
    const std::string a = exportFile();
    CHECK(a.find("client-secret") == std::string::npos);
    CHECK(a.find("ap-secret") == std::string::npos);
    CHECK(a.find("\"clientPassword\":\"***\"") != std::string::npos);

    put(config.deviceId, "ZZZZ");
    NetworkConfig& n = config.network;
    put(n.apSSID, "Other-AP"); put(n.apPassword, "p1");
    put(n.clientSSID, "OtherNet"); put(n.clientPassword, "p2");
    setIp(n.staticIP, 10, 1, 1, 9);
    setIp(n.apIP, 172, 16, 0, 1);
    config.hardware.pinFlowSensor = 1;
    config.hardware.pinSdCS = 0;
    put(config.datalog.currentFile, "/logs/now.txt");
    put(config.theme.storageBarColor, "#999999");
    put(config.theme.boardDiagramPath, "/www/b2.svg");
    config.flowMeter.testMode = false;
    config.flowMeter.blinkDuration = 99;
    const DeviceConfig want = config;

    CHECK(importFile(a));
    CHECK(sameConfig(config, want));
    CHECK_STREQ(config.network.clientPassword, "p2");
    CHECK_STREQ(config.datalog.currentFile, "/logs/now.txt");
}

static void test_reveal_puts_the_passwords_in() {
    populate();
    const std::string a = exportFile(true);
    CHECK(a.find("\"clientPassword\":\"client-secret\"") != std::string::npos);
    CHECK(a.find("\"apPassword\":\"ap-secret\"") != std::string::npos);
}

// A file from an older firmware lacks the keys that came later; each one it
// lacks leaves its setting as it is rather than resetting it.
static void test_missing_keys_leave_settings_alone() {
    populate();
    const DeviceConfig want = config;
    CHECK(importFile("{}"));
    CHECK(sameConfig(config, want));

    CHECK(importFile("{\"kindle\":{\"face\":1},\"datalog\":{\"maxEntries\":777},"
                     "\"network\":{\"timezone\":4}}"));
    CHECK_EQ(config.kindle.face, 1);
    CHECK_EQ(config.kindle.lang, KLANG_BG);
    CHECK_EQ(config.kindle.layoutMode, KLAYOUT_STANDALONE);
    CHECK_EQ(config.kindle.refreshSec, 900);
    CHECK_EQ(config.datalog.maxEntries, 777);
    CHECK_STREQ(config.datalog.prefix, "garden");
    CHECK_EQ(config.network.timezone, 4);
    CHECK_STREQ(config.network.ntpServer, "time.example.org");
    CHECK_STREQ(config.theme.primaryColor, "#123456");
}

// A file is not a form, but it gets the same checks: what the page could not
// have sent does not get in by being typed into a backup.
static void test_file_values_are_checked() {
    populate();
    CHECK(importFile(
        "{\"theme\":{\"primaryColor\":\"red;}\",\"logoSource\":\"/../secret\"},"
        "\"datalog\":{\"prefix\":\"../up\",\"folder\":\"/a/../../b\",\"maxSizeKB\":1,"
                      "\"currentFile\":\"/elsewhere.txt\"},"
        "\"logger\":{\"aggregationIntervalSec\":1},"
        "\"kindle\":{\"lang\":9,\"faceCustom\":\"x}body{display:none\",\"fbinkResW\":60000}}"));
    CHECK_STREQ(config.theme.primaryColor, "#123456");
    CHECK_STREQ(config.theme.logoSource, "https://example.com/logo.png");
    CHECK_STREQ(config.datalog.prefix, "garden");
    CHECK_STREQ(config.datalog.folder, "/logs");
    CHECK_STREQ(config.datalog.currentFile, "/logs/garden_1.txt");
    CHECK_EQ(config.datalog.maxSizeKB, 10);
    CHECK_EQ(config.logger.aggregationIntervalSec, 5);
    CHECK_EQ(config.kindle.lang, KLANG_AUTO);
    CHECK(strchr(config.kindle.faceCustom, '}') == nullptr);
    CHECK_EQ(config.kindle.fbinkResW, 4096);

    // A folder that only needs tidying is tidied, as the form does.
    CHECK(importFile("{\"datalog\":{\"folder\":\"logs2//\"}}"));
    CHECK_STREQ(config.datalog.folder, "/logs2");

    // Names too long for the form are refused, not cut to fit.
    CHECK(importFile("{\"datalog\":{\"folder\":\"/logs/station_north_greenhouse_2025\","
                     "\"prefix\":\"a_prefix_that_is_longer_than_32_chars\"}}"));
    CHECK_STREQ(config.datalog.folder, "/logs2");
    CHECK_STREQ(config.datalog.prefix, "garden");

    // Enums outside their values keep what was stored; debounce is clamped
    // as the form clamps it; an hour off the clock falls back to UTC+2, the
    // firmware default, and one that would wrap in an int8_t is not taken.
    const DeviceConfig before = config;
    CHECK(importFile("{\"network\":{\"wifiMode\":7,\"timezone\":20},"
                     "\"hardware\":{\"storageType\":99,\"wakeupMode\":42,\"debounceMs\":5000}}"));
    CHECK_EQ((int)config.network.wifiMode, (int)before.network.wifiMode);
    CHECK_EQ((int)config.hardware.storageType, (int)before.hardware.storageType);
    CHECK_EQ((int)config.hardware.wakeupMode, (int)before.hardware.wakeupMode);
    CHECK_EQ(config.hardware.debounceMs, 500);
    CHECK_EQ(config.network.timezone, 2);
    CHECK(importFile("{\"network\":{\"timezone\":250},\"hardware\":{\"debounceMs\":1}}"));
    CHECK_EQ(config.network.timezone, 2);
    CHECK_EQ(config.hardware.debounceMs, 20);
}

int main() {
    RUN(test_own_file_changes_nothing);
    RUN(test_restores_what_it_carries);
    RUN(test_kindle_lang_and_layout_survive);
    RUN(test_leaves_alone_what_it_does_not_carry);
    RUN(test_reveal_puts_the_passwords_in);
    RUN(test_missing_keys_leave_settings_alone);
    RUN(test_file_values_are_checked);
    return SUMMARY();
}
