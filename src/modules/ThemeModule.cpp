#include "ThemeModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"
#include "../utils/Utils.h"

namespace {

void copyStr(char* dst, size_t n, const char* src) {
    if (src) strlcpy(dst, src, n);
}

static bool _isHexColor(const char* s) {
    if (!s || s[0] != '#') return false;
    size_t n = strlen(s);
    if (n != 4 && n != 7) return false;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// PROGMEM schema — drives Form.bind() in the new Settings UI (phase 4).
const char THEME_SCHEMA[] PROGMEM =
    "{\"fields\":["
      "{\"id\":\"mode\",\"type\":\"enum\",\"label\":\"Mode\","
        "\"options\":[{\"v\":0,\"l\":\"Light\"},{\"v\":1,\"l\":\"Dark\"},{\"v\":2,\"l\":\"Auto\"}]},"
      "{\"id\":\"showIcons\",\"type\":\"bool\",\"label\":\"Show icons\"},"
      "{\"id\":\"primaryColor\",\"type\":\"color\",\"label\":\"Primary\"},"
      "{\"id\":\"secondaryColor\",\"type\":\"color\",\"label\":\"Secondary\"},"
      "{\"id\":\"lightBgColor\",\"type\":\"color\",\"label\":\"Light BG\"},"
      "{\"id\":\"lightTextColor\",\"type\":\"color\",\"label\":\"Light text\"},"
      "{\"id\":\"darkBgColor\",\"type\":\"color\",\"label\":\"Dark BG\"},"
      "{\"id\":\"darkTextColor\",\"type\":\"color\",\"label\":\"Dark text\"},"
      "{\"id\":\"chartSource\",\"type\":\"enum\",\"label\":\"Chart source\","
        "\"options\":[{\"v\":0,\"l\":\"Local\"},{\"v\":1,\"l\":\"CDN\"}]},"
      "{\"id\":\"chartLocalPath\",\"type\":\"string\",\"max\":64,\"label\":\"Chart JS path\","
        "\"showIf\":{\"chartSource\":0}},"
      "{\"id\":\"chartLabelFormat\",\"type\":\"enum\",\"label\":\"Chart labels\","
        "\"options\":[{\"v\":0,\"l\":\"Date/time\"},{\"v\":1,\"l\":\"Boot #\"},{\"v\":2,\"l\":\"Both\"}]},"
      "{\"id\":\"logoSource\",\"type\":\"string\",\"max\":128,\"label\":\"Logo\"},"
      "{\"id\":\"faviconPath\",\"type\":\"string\",\"max\":32,\"label\":\"Favicon\"}"
    "]}";

} // namespace

// ---------------------------------------------------------------------------
bool ThemeModule::load(JsonObjectConst cfg) {
    ThemeConfig& t = config.theme;
    t.mode             = (ThemeMode)(cfg["mode"] | (int)t.mode);
    t.showIcons        = cfg["showIcons"] | t.showIcons;
    t.chartSource      = (ChartSource)(cfg["chartSource"] | (int)t.chartSource);
    t.chartLabelFormat = (ChartLabelFormat)(cfg["chartLabelFormat"] | (int)t.chartLabelFormat);

    bool warnedColor = false;
    auto loadColor = [&](char* dst, size_t n, const char* key) {
        const char* v = cfg[key] | (const char*)nullptr;
        if (!v) return;
        if (_isHexColor(v)) { copyStr(dst, n, v); }
        else if (!warnedColor) {
            Serial.printf("[ThemeModule] rejected invalid color for %s\n", key);
            warnedColor = true;
        }
    };
    loadColor(t.primaryColor,   sizeof(t.primaryColor),   "primaryColor");
    loadColor(t.secondaryColor, sizeof(t.secondaryColor), "secondaryColor");
    loadColor(t.lightBgColor,   sizeof(t.lightBgColor),   "lightBgColor");
    loadColor(t.lightTextColor, sizeof(t.lightTextColor), "lightTextColor");
    loadColor(t.darkBgColor,    sizeof(t.darkBgColor),    "darkBgColor");
    loadColor(t.darkTextColor,  sizeof(t.darkTextColor),  "darkTextColor");

    auto loadPath = [&](char* dst, size_t n, const char* key) {
        const char* v = cfg[key] | (const char*)nullptr;
        if (!v) return;           // key absent — keep existing
        String val(v);
        if (val.length() == 0) { copyStr(dst, n, ""); return; }  // explicit clear
        if (val.startsWith("http://") || val.startsWith("https://")) {
            // External URL: reject control chars and quotes, accept as-is
            for (size_t i = 0; i < val.length(); i++) {
                if ((unsigned char)val[i] < 0x20 || val[i] == '"' || val[i] == '\'') return;
            }
            copyStr(dst, n, v);
        } else {
            String clean = sanitizePath(val);
            if (clean.length() == 0 || isPathProtected(clean)) return;
            copyStr(dst, n, clean.c_str());
        }
    };
    loadPath(t.chartLocalPath, sizeof(t.chartLocalPath), "chartLocalPath");
    loadPath(t.logoSource,     sizeof(t.logoSource),     "logoSource");
    loadPath(t.faviconPath,    sizeof(t.faviconPath),    "faviconPath");
    return true;
}

// ---------------------------------------------------------------------------
void ThemeModule::save(JsonObject cfg) const {
    const ThemeConfig& t = config.theme;
    cfg["mode"]             = (int)t.mode;
    cfg["showIcons"]        = t.showIcons;
    cfg["chartSource"]      = (int)t.chartSource;
    cfg["chartLabelFormat"] = (int)t.chartLabelFormat;
    cfg["primaryColor"]     = t.primaryColor;
    cfg["secondaryColor"]   = t.secondaryColor;
    cfg["lightBgColor"]     = t.lightBgColor;
    cfg["lightTextColor"]   = t.lightTextColor;
    cfg["darkBgColor"]      = t.darkBgColor;
    cfg["darkTextColor"]    = t.darkTextColor;
    cfg["chartLocalPath"]   = t.chartLocalPath;
    cfg["logoSource"]       = t.logoSource;
    cfg["faviconPath"]      = t.faviconPath;
}

// ---------------------------------------------------------------------------
const char* ThemeModule::schema() const {
    return THEME_SCHEMA;
}
