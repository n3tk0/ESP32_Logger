#include "ThemeModule.h"
#include "../core/Globals.h"
#include "../core/Config.h"

namespace {

void copyStr(char* dst, size_t n, const char* src) {
    if (src) strlcpy(dst, src, n);
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

    copyStr(t.primaryColor,      sizeof(t.primaryColor),      cfg["primaryColor"]      | (const char*)nullptr);
    copyStr(t.secondaryColor,    sizeof(t.secondaryColor),    cfg["secondaryColor"]    | (const char*)nullptr);
    copyStr(t.lightBgColor,      sizeof(t.lightBgColor),      cfg["lightBgColor"]      | (const char*)nullptr);
    copyStr(t.lightTextColor,    sizeof(t.lightTextColor),    cfg["lightTextColor"]    | (const char*)nullptr);
    copyStr(t.darkBgColor,       sizeof(t.darkBgColor),       cfg["darkBgColor"]       | (const char*)nullptr);
    copyStr(t.darkTextColor,     sizeof(t.darkTextColor),     cfg["darkTextColor"]     | (const char*)nullptr);
    copyStr(t.chartLocalPath,    sizeof(t.chartLocalPath),    cfg["chartLocalPath"]    | (const char*)nullptr);
    copyStr(t.logoSource,        sizeof(t.logoSource),        cfg["logoSource"]        | (const char*)nullptr);
    copyStr(t.faviconPath,       sizeof(t.faviconPath),       cfg["faviconPath"]       | (const char*)nullptr);
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
