#include "WebServer.h"
#include "../core/Globals.h"
#include "../managers/ConfigManager.h"
#include "../managers/StorageManager.h"
#include "../managers/RtcManager.h"
#include "../managers/WiFiManager.h"
#include "../managers/DataLogger.h"
#include "../utils/Utils.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// ============================================================================
// FAILSAFE HTML
// Показва се когато /www/ файловете липсват от LittleFS.
// Позволява качване на UI файловете без да е нужен serial/OTA достъп.
// ============================================================================
static const char FAILSAFE_HTML[] PROGMEM = R"(<!DOCTYPE html>
<html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Water Logger – Failsafe</title>
<style>
*{box-sizing:border-box}
body{font-family:sans-serif;max-width:540px;margin:24px auto;padding:0 14px;background:#eef2f7}
h2{color:#275673;margin:0 0 4px}
p.warn{color:#c53030;font-weight:600;margin:4px 0 12px}
.card{background:#fff;border-radius:8px;padding:16px;margin:12px 0;box-shadow:0 1px 3px #0002}
.grid{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;font-size:.9em}
.lbl{color:#777}
button{background:#275673;color:#fff;border:none;border-radius:6px;
       padding:8px 18px;cursor:pointer;margin:4px 2px;font-size:.95em}
button.red{background:#c53030}
input[type=file]{display:block;margin:8px 0;width:100%}
select{padding:6px 10px;border-radius:5px;border:1px solid #ccc}
progress{width:100%;height:8px;margin-top:6px}
#msg{font-size:.9em;color:#275673;margin-top:6px;min-height:1.2em}
</style></head>
<body>
<h2>&#128167; Water Logger</h2>
<p class=warn>UI files missing from LittleFS &mdash; upload /www/ to restore full interface.</p>

<div class=card>
  <b>Device status</b>
  <div class=grid id=status><span class=lbl>Loading…</span><span></span></div>
</div>

<div class=card>
  <b>Upload files to LittleFS</b>
  <input type=file id=fInput multiple accept=".html,.css,.js,.json,.ico,.png,.svg">
  <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:4px 0">
    <select id=fDir>
      <option value=/www/>/www/&nbsp;(UI files)</option>
      <option value=/>/&nbsp;(root)</option>
    </select>
    <button onclick=doUpload()>&#8679; Upload</button>
  </div>
  <progress id=fProg value=0 max=100 style=display:none></progress>
  <div id=msg></div>
</div>

<div class=card>
  <b>System actions</b><br>
  <button onclick=doRestart()>&#8635; Restart</button>
  <button class=red onclick=doReset()>&#9888; Factory Reset</button>
</div>

<script>
function q(s){return document.querySelector(s)}
function loadStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    q('#status').innerHTML=
      '<span class=lbl>Version</span><span>'+d.version+'</span>'+
      '<span class=lbl>IP</span><span>'+d.ip+'</span>'+
      '<span class=lbl>Mode</span><span>'+d.mode+'</span>'+
      '<span class=lbl>RTC</span><span>'+d.rtcTime+'</span>'+
      '<span class=lbl>Storage</span><span>'+d.fsUsed+' / '+d.fsTotal+' ('+d.fsPct+'%)</span>'+
      '<span class=lbl>Boot #</span><span>'+d.bootCount+'</span>';
  }).catch(()=>q('#status').innerHTML='<span class=lbl>Error</span><span>Cannot reach device</span>');
}
function doUpload(){
  var files=q('#fInput').files, dir=q('#fDir').value;
  if(!files.length){q('#msg').textContent='Select files first.';return;}
  var prog=q('#fProg'), msg=q('#msg'), i=0;
  prog.style.display='block'; prog.value=0;
  function next(){
    if(i>=files.length){msg.textContent='All done! Refresh to continue.';return;}
    var fd=new FormData(); fd.append('file',files[i]);
    var xhr=new XMLHttpRequest();
    xhr.open('POST','/upload?dir='+encodeURIComponent(dir));
    xhr.upload.onprogress=function(e){if(e.lengthComputable)prog.value=e.loaded/e.total*100;};
    xhr.onload=function(){msg.textContent='OK: '+files[i].name; i++; next();};
    xhr.onerror=function(){msg.textContent='Error: '+files[i].name;};
    xhr.send(fd);
  }
  next();
}
function doRestart(){
  if(!confirm('Restart device?'))return;
  fetch('/restart',{method:'POST'}).then(()=>{
    q('#msg').textContent='Restarting… reconnecting in 4 s';
    setTimeout(()=>location.reload(),4000);
  });
}
function doReset(){
  if(!confirm('Factory reset? All settings will be lost!'))return;
  fetch('/factory_reset',{method:'POST'}).then(()=>{
    q('#msg').textContent='Reset done. Restarting…';
    setTimeout(()=>location.reload(),4000);
  });
}
loadStatus();
setInterval(loadStatus,15000);
</script>
</body></html>)";

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

// Try to serve a file from LittleFS. Returns false if file not found.
static bool tryServeFile(AsyncWebServerRequest* r, const String& path) {
    if (!littleFsAvailable || !LittleFS.exists(path)) return false;
    String ct = "application/octet-stream";
    if      (path.endsWith(".html")) ct = "text/html";
    else if (path.endsWith(".css"))  ct = "text/css";
    else if (path.endsWith(".js"))   ct = "application/javascript";
    else if (path.endsWith(".json")) ct = "application/json";
    else if (path.endsWith(".ico"))  ct = "image/x-icon";
    else if (path.endsWith(".png"))  ct = "image/png";
    else if (path.endsWith(".svg"))  ct = "image/svg+xml";
    r->send(LittleFS, path, ct);
    return true;
}

// Serve /www/<file> from LittleFS, fall back to embedded failsafe.
static void servePage(AsyncWebServerRequest* r, const char* wwwFile) {
    String path = String("/www/") + wwwFile;
    if (!tryServeFile(r, path)) {
        AsyncWebServerResponse* resp =
            r->beginResponse_P(200, "text/html", FAILSAFE_HTML,
                               sizeof(FAILSAFE_HTML) - 1);
        r->send(resp);
    }
}

void sendJsonResponse(AsyncWebServerRequest* r, JsonDocument& doc) {
    String out;
    serializeJson(doc, out);
    r->send(200, "application/json", out);
}

void sendRestartPage(AsyncWebServerRequest* r, const char* message) {
    String html = "<!DOCTYPE html><html><body><p>";
    html += message;
    html += "</p><p>Restarting&#8230;</p>"
            "<script>setTimeout(()=>location.href='/',3500)</script>"
            "</body></html>";
    r->send(200, "text/html", html);
}

String getModeDisplay() {
    if (onlineLoggerMode)      return "Online Logger";
    if (wifiFallbackToAP)      return "AP (fallback)";
    if (wifiConnectedAsClient) return "Client: " + connectedSSID;
    return "Access Point";
}

String getNetworkDisplay() { return currentIPAddress; }

// ============================================================================
// API: GET /api/status
// ============================================================================
static void handleApiStatus(AsyncWebServerRequest* r) {
    JsonDocument doc;
    doc["version"]    = getVersionString();
    doc["ip"]         = currentIPAddress;
    doc["mode"]       = getModeDisplay();
    doc["rtcTime"]    = getRtcDateTimeString();
    doc["rtcValid"]   = rtcValid;
    doc["bootCount"]  = bootCount;
    doc["uptime"]     = millis() / 1000;

    uint64_t used = 0, total = 0; int pct = 0;
    getStorageInfo(used, total, pct);
    doc["fsUsed"]     = formatFileSize(used);
    doc["fsTotal"]    = formatFileSize(total);
    doc["fsPct"]      = pct;
    doc["fsType"]     = currentStorageView;

    noInterrupts(); uint32_t p = pulseCount; interrupts();
    doc["pulses"]     = p;
    doc["totalPulses"]= cycleTotalPulses;
    doc["logState"]   = (int)loggingState;
    doc["cycleBy"]    = cycleStartedBy;
    doc["wakeBy"]     = wakeUpButtonStr;
    doc["apMode"]     = apModeTriggered;
    doc["onlineMode"] = onlineLoggerMode;

    sendJsonResponse(r, doc);
}

// ============================================================================
// API: GET /api/config
// ============================================================================
static void handleApiConfig(AsyncWebServerRequest* r) {
    JsonDocument doc;

    doc["deviceId"]   = config.deviceId;
    doc["deviceName"] = config.deviceName;
    doc["forceWS"]    = config.forceWebServer;

    // Hardware
    JsonObject hw = doc["hw"].to<JsonObject>();
    hw["storageType"]    = (int)config.hardware.storageType;
    hw["wakeupMode"]     = (int)config.hardware.wakeupMode;
    hw["pinWifiTrigger"] = config.hardware.pinWifiTrigger;
    hw["pinFF"]          = config.hardware.pinWakeupFF;
    hw["pinPF"]          = config.hardware.pinWakeupPF;
    hw["pinFlow"]        = config.hardware.pinFlowSensor;
    hw["pinRtcCE"]       = config.hardware.pinRtcCE;
    hw["pinRtcIO"]       = config.hardware.pinRtcIO;
    hw["pinRtcSCLK"]     = config.hardware.pinRtcSCLK;
    hw["pinSdCS"]        = config.hardware.pinSdCS;
    hw["pinSdMOSI"]      = config.hardware.pinSdMOSI;
    hw["pinSdMISO"]      = config.hardware.pinSdMISO;
    hw["pinSdSCK"]       = config.hardware.pinSdSCK;
    hw["cpuFreq"]        = config.hardware.cpuFreqMHz;
    hw["debugMode"]      = config.hardware.debugMode;
    hw["debounceMs"]     = config.hardware.debounceMs;

    // Network
    JsonObject net = doc["net"].to<JsonObject>();
    net["wifiMode"]    = (int)config.network.wifiMode;
    net["apSSID"]      = config.network.apSSID;
    net["apPassword"]  = config.network.apPassword;
    net["clientSSID"]  = config.network.clientSSID;
    net["useStaticIP"] = config.network.useStaticIP;
    char ip[16];
    snprintf(ip,16,"%d.%d.%d.%d",config.network.staticIP[0],config.network.staticIP[1],
             config.network.staticIP[2],config.network.staticIP[3]);
    net["staticIP"]    = ip;
    snprintf(ip,16,"%d.%d.%d.%d",config.network.gateway[0],config.network.gateway[1],
             config.network.gateway[2],config.network.gateway[3]);
    net["gateway"]     = ip;
    snprintf(ip,16,"%d.%d.%d.%d",config.network.subnet[0],config.network.subnet[1],
             config.network.subnet[2],config.network.subnet[3]);
    net["subnet"]      = ip;
    snprintf(ip,16,"%d.%d.%d.%d",config.network.dns[0],config.network.dns[1],
             config.network.dns[2],config.network.dns[3]);
    net["dns"]         = ip;
    snprintf(ip,16,"%d.%d.%d.%d",config.network.apIP[0],config.network.apIP[1],
             config.network.apIP[2],config.network.apIP[3]);
    net["apIP"]        = ip;
    net["ntpServer"]   = config.network.ntpServer;
    net["timezone"]    = config.network.timezone;

    // Datalog
    JsonObject dl = doc["dl"].to<JsonObject>();
    dl["prefix"]       = config.datalog.prefix;
    dl["currentFile"]  = config.datalog.currentFile;
    dl["folder"]       = config.datalog.folder;
    dl["rotation"]     = (int)config.datalog.rotation;
    dl["maxSizeKB"]    = config.datalog.maxSizeKB;
    dl["maxEntries"]   = config.datalog.maxEntries;
    dl["dateFormat"]   = config.datalog.dateFormat;
    dl["timeFormat"]   = config.datalog.timeFormat;
    dl["endFormat"]    = config.datalog.endFormat;
    dl["volumeFormat"] = config.datalog.volumeFormat;
    dl["inclBootCount"]= config.datalog.includeBootCount;
    dl["inclExtra"]    = config.datalog.includeExtraPresses;
    dl["postCorr"]     = config.datalog.postCorrectionEnabled;
    dl["pfToFf"]       = config.datalog.pfToFfThreshold;
    dl["ffToPf"]       = config.datalog.ffToPfThreshold;
    dl["holdThresh"]   = config.datalog.manualPressThresholdMs;

    // Flow meter
    JsonObject fm = doc["fm"].to<JsonObject>();
    fm["pulsesPerL"]   = config.flowMeter.pulsesPerLiter;
    fm["calibMult"]    = config.flowMeter.calibrationMultiplier;
    fm["monitorSecs"]  = config.flowMeter.monitoringWindowSecs;
    fm["testMode"]     = config.flowMeter.testMode;
    fm["blinkMs"]      = config.flowMeter.blinkDuration;

    // Theme
    JsonObject th = doc["th"].to<JsonObject>();
    th["mode"]         = (int)config.theme.mode;
    th["primaryColor"] = config.theme.primaryColor;
    th["accentColor"]  = config.theme.accentColor;
    th["bgColor"]      = config.theme.bgColor;
    th["textColor"]    = config.theme.textColor;
    th["ffColor"]      = config.theme.ffColor;
    th["pfColor"]      = config.theme.pfColor;
    th["chartSource"]  = (int)config.theme.chartSource;
    th["showIcons"]    = config.theme.showIcons;

    sendJsonResponse(r, doc);
}

// ============================================================================
// CONFIG SAVE ENDPOINTS
// ============================================================================

static void handleSaveHardware(AsyncWebServerRequest* r) {
    auto p = [&](const char* n) -> bool { return r->hasParam(n, true); };
    auto v = [&](const char* n) -> String { return r->getParam(n, true)->value(); };

    if (p("storageType"))    config.hardware.storageType    = (StorageType)v("storageType").toInt();
    if (p("wakeupMode"))     config.hardware.wakeupMode     = (WakeupMode)v("wakeupMode").toInt();
    if (p("pinWifiTrigger")) config.hardware.pinWifiTrigger = v("pinWifiTrigger").toInt();
    if (p("pinFF"))          config.hardware.pinWakeupFF    = v("pinFF").toInt();
    if (p("pinPF"))          config.hardware.pinWakeupPF    = v("pinPF").toInt();
    if (p("pinFlow"))        config.hardware.pinFlowSensor  = v("pinFlow").toInt();
    if (p("pinRtcCE"))       config.hardware.pinRtcCE       = v("pinRtcCE").toInt();
    if (p("pinRtcIO"))       config.hardware.pinRtcIO       = v("pinRtcIO").toInt();
    if (p("pinRtcSCLK"))     config.hardware.pinRtcSCLK     = v("pinRtcSCLK").toInt();
    if (p("pinSdCS"))        config.hardware.pinSdCS        = v("pinSdCS").toInt();
    if (p("cpuFreq"))        config.hardware.cpuFreqMHz     = v("cpuFreq").toInt();
    if (p("debounceMs"))     config.hardware.debounceMs     = v("debounceMs").toInt();
    if (p("debugMode"))      config.hardware.debugMode      = v("debugMode") == "1";

    saveConfig();
    sendRestartPage(r, "Hardware settings saved.");
    shouldRestart = true;
    restartTimer  = millis();
}

static void handleSaveNetwork(AsyncWebServerRequest* r) {
    auto p = [&](const char* n) -> bool { return r->hasParam(n, true); };
    auto v = [&](const char* n) -> String { return r->getParam(n, true)->value(); };

    if (p("wifiMode"))      config.network.wifiMode = (WiFiModeType)v("wifiMode").toInt();
    if (p("apSSID"))        strncpy(config.network.apSSID,       v("apSSID").c_str(),       32);
    if (p("apPassword"))    strncpy(config.network.apPassword,   v("apPassword").c_str(),   64);
    if (p("clientSSID"))    strncpy(config.network.clientSSID,   v("clientSSID").c_str(),   32);
    if (p("clientPassword") && v("clientPassword").length() > 0)
                            strncpy(config.network.clientPassword, v("clientPassword").c_str(), 64);
    if (p("ntpServer"))     strncpy(config.network.ntpServer,    v("ntpServer").c_str(),    64);
    if (p("timezone"))      config.network.timezone    = v("timezone").toInt();
    if (p("useStaticIP"))   config.network.useStaticIP = v("useStaticIP") == "1";

    saveConfig();
    sendRestartPage(r, "Network settings saved.");
    shouldRestart = true;
    restartTimer  = millis();
}

static void handleSaveDatalog(AsyncWebServerRequest* r) {
    auto p = [&](const char* n) -> bool { return r->hasParam(n, true); };
    auto v = [&](const char* n) -> String { return r->getParam(n, true)->value(); };

    if (p("prefix"))       strncpy(config.datalog.prefix,  v("prefix").c_str(),  32);
    if (p("folder"))       strncpy(config.datalog.folder,  v("folder").c_str(),  32);
    if (p("rotation"))     config.datalog.rotation     = (DatalogRotation)v("rotation").toInt();
    if (p("maxSizeKB"))    config.datalog.maxSizeKB    = v("maxSizeKB").toInt();
    if (p("maxEntries"))   config.datalog.maxEntries   = v("maxEntries").toInt();
    if (p("dateFormat"))   config.datalog.dateFormat   = v("dateFormat").toInt();
    if (p("timeFormat"))   config.datalog.timeFormat   = v("timeFormat").toInt();
    if (p("endFormat"))    config.datalog.endFormat    = v("endFormat").toInt();
    if (p("volumeFormat")) config.datalog.volumeFormat = v("volumeFormat").toInt();
    if (p("inclBootCount"))config.datalog.includeBootCount    = v("inclBootCount") == "1";
    if (p("inclExtra"))    config.datalog.includeExtraPresses = v("inclExtra") == "1";
    if (p("postCorr"))     config.datalog.postCorrectionEnabled = v("postCorr") == "1";
    if (p("pfToFf"))       config.datalog.pfToFfThreshold    = v("pfToFf").toFloat();
    if (p("ffToPf"))       config.datalog.ffToPfThreshold    = v("ffToPf").toFloat();
    if (p("holdThresh"))   config.datalog.manualPressThresholdMs = v("holdThresh").toInt();

    saveConfig();
    r->send(200, "text/plain", "Datalog settings saved.");
}

static void handleSaveFlowmeter(AsyncWebServerRequest* r) {
    auto p = [&](const char* n) -> bool { return r->hasParam(n, true); };
    auto v = [&](const char* n) -> String { return r->getParam(n, true)->value(); };

    if (p("pulsesPerL"))  config.flowMeter.pulsesPerLiter          = v("pulsesPerL").toFloat();
    if (p("calibMult"))   config.flowMeter.calibrationMultiplier    = v("calibMult").toFloat();
    if (p("monitorSecs")) config.flowMeter.monitoringWindowSecs     = v("monitorSecs").toInt();
    if (p("testMode"))    config.flowMeter.testMode                 = v("testMode") == "1";
    if (p("blinkMs"))     config.flowMeter.blinkDuration            = v("blinkMs").toInt();

    saveConfig();
    r->send(200, "text/plain", "Flow meter settings saved.");
}

static void handleSaveTheme(AsyncWebServerRequest* r) {
    auto p = [&](const char* n) -> bool { return r->hasParam(n, true); };
    auto v = [&](const char* n) -> String { return r->getParam(n, true)->value(); };

    if (p("mode"))         config.theme.mode        = (ThemeMode)v("mode").toInt();
    if (p("primaryColor")) strncpy(config.theme.primaryColor, v("primaryColor").c_str(), 7);
    if (p("accentColor"))  strncpy(config.theme.accentColor,  v("accentColor").c_str(),  7);
    if (p("bgColor"))      strncpy(config.theme.bgColor,      v("bgColor").c_str(),      7);
    if (p("textColor"))    strncpy(config.theme.textColor,    v("textColor").c_str(),    7);
    if (p("ffColor"))      strncpy(config.theme.ffColor,      v("ffColor").c_str(),      7);
    if (p("pfColor"))      strncpy(config.theme.pfColor,      v("pfColor").c_str(),      7);
    if (p("chartSource"))  config.theme.chartSource = (ChartSource)v("chartSource").toInt();
    if (p("showIcons"))    config.theme.showIcons   = v("showIcons") == "1";

    saveConfig();
    r->send(200, "text/plain", "Theme saved.");
}

// ============================================================================
// FILE MANAGER ENDPOINTS
// ============================================================================

static void handleApiFiles(AsyncWebServerRequest* r) {
    String dir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
    dir = sanitizePath(dir);
    fs::FS* fs = getCurrentViewFS();
    if (!fs) { r->send(503, "application/json", "{\"error\":\"No storage\"}"); return; }

    JsonDocument doc;
    doc["dir"] = dir;
    JsonArray files = doc["files"].to<JsonArray>();

    File d = fs->open(dir);
    if (d && d.isDirectory()) {
        while (File f = d.openNextFile()) {
            JsonObject obj = files.add<JsonObject>();
            obj["name"] = String(f.name());
            obj["dir"]  = f.isDirectory();
            obj["size"] = f.isDirectory() ? 0 : (uint32_t)f.size();
            f.close();
        }
        d.close();
    }
    sendJsonResponse(r, doc);
}

static void handleDelete(AsyncWebServerRequest* r) {
    String path = r->hasParam("path") ? r->getParam("path")->value() : "";
    path = sanitizePath(path);
    if (path == "/" || path.isEmpty()) { r->send(400, "text/plain", "Invalid path"); return; }
    fs::FS* fs = getCurrentViewFS();
    if (!fs) { r->send(503, "text/plain", "No storage"); return; }
    deleteRecursive(*fs, path);
    r->send(200, "text/plain", "Deleted: " + path);
}

static void handleDownload(AsyncWebServerRequest* r) {
    String path = r->hasParam("path") ? r->getParam("path")->value() : "";
    path = sanitizePath(path);
    fs::FS* fs = getCurrentViewFS();
    if (!fs || !fs->exists(path)) { r->send(404, "text/plain", "Not found"); return; }
    r->send(*fs, path, "application/octet-stream", true);
}

static void handleMkdir(AsyncWebServerRequest* r) {
    String dir = r->hasParam("dir", true) ? r->getParam("dir", true)->value() : "";
    dir = sanitizePath(dir);
    fs::FS* fs = getCurrentViewFS();
    if (!fs) { r->send(503, "text/plain", "No storage"); return; }
    fs->mkdir(dir);
    r->send(200, "text/plain", "Created: " + dir);
}

// Upload: file handler (called per chunk)
static void handleUploadChunk(AsyncWebServerRequest* r,
                               const String& filename, size_t index,
                               uint8_t* data, size_t len, bool final) {
    static File uploadFile;
    if (index == 0) {
        String dir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        dir = sanitizePath(dir);
        if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
        String name = sanitizeFilename(filename);
        String path = buildPath(dir, name);
        Serial.printf("Upload start: %s\n", path.c_str());
        uploadFile = LittleFS.open(path, FILE_WRITE);
    }
    if (uploadFile && len) uploadFile.write(data, len);
    if (final && uploadFile) {
        uploadFile.close();
        Serial.println("Upload done");
    }
}

static void handleUploadDone(AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "Uploaded");
}

// ============================================================================
// SYSTEM ENDPOINTS
// ============================================================================

static void handleSetTime(AsyncWebServerRequest* r) {
    if (!Rtc) { r->send(503, "text/plain", "No RTC"); return; }
    auto v = [&](const char* n) -> int {
        return r->hasParam(n, true) ? r->getParam(n, true)->value().toInt() : 0;
    };
    int yr = v("year"), mo = v("month"), dy = v("day");
    int hr = v("hour"), mn = v("minute"), sc = v("second");
    if (yr < 2020 || mo < 1 || dy < 1) { r->send(400, "text/plain", "Invalid date"); return; }
    RtcDateTime dt(yr, mo, dy, hr, mn, sc);
    Rtc->SetIsWriteProtected(false);
    Rtc->SetIsRunning(true);
    Rtc->SetDateTime(dt);
    rtcValid = true;
    r->send(200, "text/plain", "RTC set: " + getRtcDateTimeString());
}

static void handleSyncNtp(AsyncWebServerRequest* r) {
    bool ok = syncTimeFromNTP();
    r->send(200, "text/plain",
            ok ? "NTP OK: " + getRtcDateTimeString() : "NTP sync failed");
}

static void handleRestart(AsyncWebServerRequest* r) {
    r->send(200, "text/plain", "Restarting…");
    shouldRestart = true;
    restartTimer  = millis();
}

static void handleFactoryReset(AsyncWebServerRequest* r) {
    loadDefaultConfig();
    saveConfig();
    r->send(200, "text/plain", "Factory reset done. Restarting…");
    shouldRestart = true;
    restartTimer  = millis();
}

static void handleSetStorage(AsyncWebServerRequest* r) {
    String view = r->hasParam("view") ? r->getParam("view")->value() : "";
    if (view == "sdcard" && sdAvailable)    currentStorageView = "sdcard";
    else if (view == "internal")            currentStorageView = "internal";
    r->send(200, "text/plain", "Storage view: " + currentStorageView);
}

// ============================================================================
// SETUP
// ============================================================================
void setupWebServer() {
    // Static assets served directly from LittleFS
    if (littleFsAvailable) {
        server.serveStatic("/www/",       LittleFS, "/www/")
              .setCacheControl("max-age=3600");
        server.serveStatic("/style.css",  LittleFS, "/style.css")
              .setCacheControl("max-age=3600");
        server.serveStatic("/favicon.ico",LittleFS, "/favicon.ico")
              .setCacheControl("max-age=86400");
    }

    // --- HTML page routes ---
    server.on("/",         HTTP_GET, [](AsyncWebServerRequest* r){ servePage(r, "index.html");    });
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* r){ servePage(r, "settings.html"); });
    server.on("/files",    HTTP_GET, [](AsyncWebServerRequest* r){ servePage(r, "files.html");    });
    server.on("/data",     HTTP_GET, [](AsyncWebServerRequest* r){ servePage(r, "data.html");     });

    // --- JSON APIs ---
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/config", HTTP_GET, handleApiConfig);
    server.on("/api/files",  HTTP_GET, handleApiFiles);

    // --- Config save ---
    server.on("/save_hardware",  HTTP_POST, handleSaveHardware);
    server.on("/save_network",   HTTP_POST, handleSaveNetwork);
    server.on("/save_datalog",   HTTP_POST, handleSaveDatalog);
    server.on("/save_flowmeter", HTTP_POST, handleSaveFlowmeter);
    server.on("/save_theme",     HTTP_POST, handleSaveTheme);

    // --- File manager ---
    server.on("/download",  HTTP_GET,  handleDownload);
    server.on("/delete",    HTTP_GET,  handleDelete);
    server.on("/mkdir",     HTTP_POST, handleMkdir);
    server.on("/upload",    HTTP_POST, handleUploadDone,
              [](AsyncWebServerRequest* r, const String& fn, size_t idx,
                 uint8_t* data, size_t len, bool final){
                  handleUploadChunk(r, fn, idx, data, len, final);
              });

    // --- System ---
    server.on("/set_time",       HTTP_POST, handleSetTime);
    server.on("/sync_ntp",       HTTP_POST, handleSyncNtp);
    server.on("/restart",        HTTP_POST, handleRestart);
    server.on("/factory_reset",  HTTP_POST, handleFactoryReset);
    server.on("/set_storage",    HTTP_GET,  handleSetStorage);

    // --- 404: try LittleFS, else 404 text ---
    server.onNotFound([](AsyncWebServerRequest* r) {
        if (tryServeFile(r, r->url())) return;
        r->send(404, "text/plain", "Not found: " + r->url());
    });

    server.begin();
    Serial.println("Web server started");
}
