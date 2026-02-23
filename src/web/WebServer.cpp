/**
 * src/web/WebServer.cpp
 * ESP32 Water Logger v4.1.4 – Refactored: static files from LittleFS /www/
 *
 * Architecture:
 *   – Normal mode  : AsyncWebServer serves /www/index.html + /www/web.js
 *   – Failsafe mode: If /www/index.html is missing, embedded minimal HTML is
 *                    served that lets the user upload the real UI files.
 *   – All JSON API endpoints are always available regardless of UI mode.
 */

#include "WebServer.h"
#include "../core/Globals.h"
#include "../managers/ConfigManager.h"
#include "../managers/WiFiManager.h"
#include "../managers/StorageManager.h"
#include "../managers/RtcManager.h"
#include "../managers/DataLogger.h"
#include "../utils/Utils.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <functional>
#include <vector>

// ============================================================================
// HELPERS
// ============================================================================

String getModeDisplay() {
    if (onlineLoggerMode) return "Online Logger";
    if (apModeTriggered)  return "Web Server";
    return "Logger";
}

String getNetworkDisplay() {
    if (wifiConnectedAsClient) return connectedSSID;
    return String(strlen(config.network.apSSID) > 0 ? config.network.apSSID : config.deviceName);
}

void sendJsonResponse(AsyncWebServerRequest *r, JsonDocument &doc) {
    String json;
    serializeJson(doc, json);
    r->send(200, "application/json", json);
}

void sendRestartPage(AsyncWebServerRequest *r, const char* message) {
    String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Restarting</title><style>body{font-family:-apple-system,sans-serif;display:flex;justify-content:center;align-items:center;"
        "min-height:100vh;margin:0;background:#f0f2f5}.popup{background:#fff;border-radius:16px;padding:2rem;text-align:center;"
        "box-shadow:0 4px 20px rgba(0,0,0,0.15);max-width:350px}.icon{font-size:4rem;margin-bottom:1rem}"
        ".title{font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem}.msg{color:#666;margin-bottom:1rem}"
        ".progress{background:#e2e8f0;border-radius:8px;height:8px;overflow:hidden;margin-top:1rem}"
        ".bar{height:100%;background:#27ae60;width:0%;transition:width 1s linear}</style></head>"
        "<body><div class='popup'><div class='icon'>&#x1F504;</div><div class='title'>Restarting...</div><div class='msg'>");
    html += message;
    html += F("</div><div id='counter'>Redirecting in 5 seconds...</div>"
        "<div class='progress'><div class='bar' id='bar'></div></div></div>"
        "<script>var s=5,b=document.getElementById('bar'),c=document.getElementById('counter');"
        "var t=setInterval(function(){s--;b.style.width=(100-s*20)+'%';c.textContent='Redirecting in '+s+' seconds...';"
        "if(s<=0){clearInterval(t);window.location.href='/';}},1000);</script></body></html>");
    r->send(200, "text/html", html);
}

// ============================================================================
// FAILSAFE HTML  (served when /www/index.html is missing)
// ============================================================================
static const char FAILSAFE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water Logger – Setup Mode</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#f0f4f8;color:#2d3748;min-height:100vh}
header{background:#275673;color:#fff;padding:16px 20px;display:flex;align-items:center;gap:12px}
header h1{font-size:1.2rem}
.badge{background:#e74c3c;color:#fff;border-radius:12px;padding:2px 10px;font-size:.75rem;font-weight:700}
.container{max-width:700px;margin:24px auto;padding:0 16px}
.card{background:#fff;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.08);margin-bottom:16px;overflow:hidden}
.card-header{padding:14px 18px;border-bottom:1px solid #e2e8f0;font-weight:600;background:#f7fafc}
.card-body{padding:18px}
.drop{border:2px dashed #cbd5e0;border-radius:8px;padding:28px;text-align:center;cursor:pointer;transition:.2s;margin-bottom:12px}
.drop:hover,.drop.over{border-color:#275673;background:#ebf4ff}
.drop input{display:none}
.drop p{color:#718096;font-size:.9rem;margin-top:6px}
.btn{display:inline-flex;align-items:center;gap:6px;padding:9px 18px;border:none;border-radius:8px;font-size:.9rem;font-weight:500;cursor:pointer;transition:.15s}
.btn-primary{background:#275673;color:#fff}.btn-primary:hover{background:#1d4259}
.btn-danger{background:#e74c3c;color:#fff}.btn-danger:hover{background:#c0392b}
.btn-sm{padding:5px 12px;font-size:.8rem}
progress{width:100%;height:8px;border-radius:4px;margin-top:8px;display:none}
.msg{margin-top:8px;font-size:.9rem;min-height:1.2em;color:#275673}
.file-list{font-size:.88rem}
.file-row{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-bottom:1px solid #e2e8f0}
.file-row:last-child{border:none}
.fname{word-break:break-all}
.fsize{color:#718096;white-space:nowrap;margin:0 10px}
.actions{display:flex;gap:6px;flex-shrink:0}
.alert{padding:12px 16px;border-radius:8px;margin-bottom:12px;font-size:.9rem}
.alert-warn{background:#fef3c7;color:#92400e;border:1px solid #fcd34d}
.alert-success{background:#d1fae5;color:#065f46;border:1px solid #6ee7b7}
input[type=text]{width:100%;padding:8px 12px;border:1px solid #e2e8f0;border-radius:6px;font-size:.9rem}
</style>
</head>
<body>
<header>
  <div>
    <h1>&#x1F4A7; Water Logger <span class="badge">SETUP MODE</span></h1>
    <div style="font-size:.8rem;opacity:.8;margin-top:2px">Upload the UI files to restore normal operation</div>
  </div>
</header>
<div class="container">
  <div class="alert alert-warn">
    &#x26A0;&#xFE0F; <strong>UI files not found.</strong>
    Upload <code>index.html</code> and <code>web.js</code> to <code>/www/</code> to restore the full interface.
  </div>

  <!-- Upload -->
  <div class="card">
    <div class="card-header">&#x1F4E4; Upload Files to /www/</div>
    <div class="card-body">
      <div class="drop" id="dropZone" onclick="document.getElementById('fileInput').click()">
        <input type="file" id="fileInput" multiple>
        &#x2B06; <strong>Click or drag files here</strong>
        <p>Supports: index.html, web.js, style.css, images, etc. All files go into /www/</p>
      </div>
      <progress id="prog" value="0" max="100"></progress>
      <div class="msg" id="uploadMsg"></div>
    </div>
  </div>

  <!-- File List -->
  <div class="card">
    <div class="card-header" style="display:flex;justify-content:space-between;align-items:center">
      <span>&#x1F4C1; LittleFS Files</span>
      <button class="btn btn-sm btn-primary" onclick="loadFiles()">&#x21BA; Refresh</button>
    </div>
    <div class="card-body" style="padding:0 18px">
      <div class="file-list" id="fileList">Loading...</div>
    </div>
  </div>

  <!-- Rename -->
  <div class="card">
    <div class="card-header">&#x270F;&#xFE0F; Rename File</div>
    <div class="card-body">
      <div style="display:flex;gap:8px;flex-wrap:wrap">
        <input type="text" id="renSrc" placeholder="Source path e.g. /www/old.js" style="flex:1;min-width:150px">
        <input type="text" id="renDst" placeholder="Destination path e.g. /www/web.js" style="flex:1;min-width:150px">
        <button class="btn btn-primary" onclick="doRename()">Rename</button>
      </div>
      <div class="msg" id="renMsg"></div>
    </div>
  </div>

  <!-- Restart -->
  <div class="card">
    <div class="card-header">&#x1F504; Device Control</div>
    <div class="card-body">
      <button class="btn btn-primary" onclick="if(confirm('Restart now?'))fetch('/restart').then(function(){location.reload();})">
        &#x1F504; Restart Device
      </button>
    </div>
  </div>
</div>

<script>
var dropZone = document.getElementById('dropZone');
var fileInput = document.getElementById('fileInput');

dropZone.addEventListener('dragover', function(e){ e.preventDefault(); dropZone.classList.add('over'); });
dropZone.addEventListener('dragleave', function(){ dropZone.classList.remove('over'); });
dropZone.addEventListener('drop', function(e){ e.preventDefault(); dropZone.classList.remove('over'); uploadFiles(e.dataTransfer.files); });
fileInput.addEventListener('change', function(){ uploadFiles(this.files); });

function uploadFiles(files) {
  if (!files || !files.length) return;
  var prog = document.getElementById('prog');
  var msg = document.getElementById('uploadMsg');
  var i = 0;
  prog.style.display = 'block';

  function next() {
    if (i >= files.length) {
      msg.textContent = 'Done! ' + files.length + ' file(s) uploaded.';
      prog.style.display = 'none';
      fileInput.value = '';
      loadFiles();
      return;
    }
    var fd = new FormData();
    fd.append('file', files[i]);
    fd.append('path', '/www/');
    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function(ev) {
      if (ev.lengthComputable) prog.value = Math.round(ev.loaded / ev.total * 100);
    };
    xhr.onload = function() {
      msg.textContent = 'Uploaded: ' + files[i].name + ' (' + (i+1) + '/' + files.length + ')';
      i++; next();
    };
    xhr.onerror = function() {
      msg.textContent = 'Error uploading: ' + files[i].name;
      prog.style.display = 'none';
    };
    xhr.open('POST', '/upload');
    xhr.send(fd);
  }
  next();
}

function loadFiles() {
  fetch('/api/filelist?storage=internal&dir=/www/')
    .then(function(r){ return r.json(); })
    .then(function(d) {
      var files = d.files || [];
      if (!files.length) {
        document.getElementById('fileList').innerHTML = '<div style="padding:12px 0;color:#718096">No files in /www/</div>';
        return;
      }
      var html = '';
      files.forEach(function(f) {
        html += '<div class="file-row">' +
          '<span class="fname">&#x1F4C4; ' + f.path + '</span>' +
          '<span class="fsize">' + fmtBytes(f.size) + '</span>' +
          '<span class="actions">' +
          '<a href="/download?file=' + encodeURIComponent(f.path) + '&storage=internal" class="btn btn-sm btn-primary">&#x1F4E5;</a>' +
          '<button class="btn btn-sm btn-danger" onclick="delFile(\'' + f.path.replace(/'/g,"\\'") + '\')">&#x1F5D1;</button>' +
          '</span></div>';
      });
      document.getElementById('fileList').innerHTML = html;
    })
    .catch(function() {
      document.getElementById('fileList').innerHTML = '<div style="color:#e74c3c;padding:12px 0">Error loading file list</div>';
    });
}

function fmtBytes(b) {
  if (b >= 1048576) return (b/1048576).toFixed(1)+' MB';
  if (b >= 1024)    return (b/1024).toFixed(1)+' KB';
  return b+' B';
}

function delFile(path) {
  if (!confirm('Delete ' + path + '?')) return;
  fetch('/delete?path=' + encodeURIComponent(path) + '&storage=internal')
    .then(function(){ loadFiles(); })
    .catch(function(e){ alert('Error: ' + e); });
}

function doRename() {
  var src = document.getElementById('renSrc').value.trim();
  var dst = document.getElementById('renDst').value.trim();
  var msg = document.getElementById('renMsg');
  if (!src || !dst) { msg.textContent = 'Both paths required.'; return; }
  // Extract newName and destDir from dst
  var parts = dst.lastIndexOf('/');
  var newName = dst.substring(parts + 1);
  var destDir = parts <= 0 ? '/' : dst.substring(0, parts);
  fetch('/move_file?src=' + encodeURIComponent(src) + '&newName=' + encodeURIComponent(newName) + '&destDir=' + encodeURIComponent(destDir) + '&storage=internal')
    .then(function() { msg.textContent = 'Renamed to ' + dst; loadFiles(); })
    .catch(function(e) { msg.textContent = 'Error: ' + e; });
}

loadFiles();
</script>
</body>
</html>
)HTML";

// ============================================================================
// MIME TYPE HELPER
// ============================================================================
static String getMime(const String& path) {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".gif"))  return "image/gif";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".txt") || path.endsWith(".log") || path.endsWith(".csv")) return "text/plain";
    if (path.endsWith(".bin"))  return "application/octet-stream";
    return "application/octet-stream";
}

// ============================================================================
// FILE LIST HELPER  (used in /api/filelist)
// ============================================================================
static void scanDir(fs::FS& fs, const String& dir, JsonArray& arr,
                    const String& filter, bool recursive) {
    File d = fs.open(dir);
    if (!d || !d.isDirectory()) return;
    while (File entry = d.openNextFile()) {
        String name = String(entry.name());
        String fullPath = (dir == "/") ? "/" + name : dir + "/" + name;
        bool isDir = entry.isDirectory();
        if (isDir) {
            if (recursive) scanDir(fs, fullPath, arr, filter, true);
            else {
                JsonObject o = arr.createNestedObject();
                o["name"]  = name;
                o["path"]  = fullPath;
                o["isDir"] = true;
                o["size"]  = 0;
            }
        } else {
            bool include = filter.isEmpty() ||
                (filter == "log" && (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")));
            if (include) {
                JsonObject o = arr.createNestedObject();
                o["name"]  = name;
                o["path"]  = fullPath;
                o["isDir"] = false;
                o["size"]  = (uint32_t)entry.size();
            }
        }
        entry.close();
    }
    d.close();
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================
void setupWebServer() {
    Serial.println("Setting up web server...");

    // ── Static file serving from LittleFS /www/ ──────────────────────────────
    bool uiReady = LittleFS.exists("/www/index.html");

    if (uiReady) {
        server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
        Serial.println("Web UI: serving from /www/");
    } else {
        // Failsafe: serve embedded minimal page
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
            r->send_P(200, "text/html", FAILSAFE_HTML);
        });
        Serial.println("Web UI: FAILSAFE mode (upload /www/index.html to restore)");
    }

    // ── Redirect /dashboard /files /live /settings to SPA ────────────────────
    auto spaRedirect = [](AsyncWebServerRequest *r) { r->redirect("/"); };
    server.on("/dashboard",          HTTP_GET, spaRedirect);
    server.on("/files",              HTTP_GET, spaRedirect);
    server.on("/live",               HTTP_GET, spaRedirect);
    server.on("/settings",           HTTP_GET, spaRedirect);
    server.on("/settings_device",    HTTP_GET, spaRedirect);
    server.on("/settings_flowmeter", HTTP_GET, spaRedirect);
    server.on("/settings_hardware",  HTTP_GET, spaRedirect);
    server.on("/settings_theme",     HTTP_GET, spaRedirect);
    server.on("/settings_time",      HTTP_GET, spaRedirect);
    server.on("/settings_network",   HTTP_GET, spaRedirect);
    server.on("/settings_datalog",   HTTP_GET, spaRedirect);

    // =========================================================================
    // API: STATUS  (used by SPA bootstrap)
    // =========================================================================
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<1024> doc;
        doc["device"]  = config.deviceName;
        doc["deviceId"] = config.deviceId;
        doc["version"] = getVersionString();
        doc["time"]    = getRtcDateTimeString();
        doc["network"] = getNetworkDisplay();
        doc["ip"]      = currentIPAddress;
        doc["boot"]    = bootCount;
        doc["heap"]    = ESP.getFreeHeap();
        doc["heapTotal"] = ESP.getHeapSize();
        doc["chip"]    = ESP.getChipModel();
        doc["cpu"]     = getCpuFrequencyMhz();
        doc["mode"]    = getModeDisplay();
        doc["wifi"]    = wifiConnectedAsClient ? "client" : "ap";
        doc["freeSketch"] = ESP.getFreeSketchSpace();

        // Storage
        uint64_t used = 0, total = 0; int pct = 0;
        getStorageInfo(used, total, pct);
        doc["fsUsed"]  = (uint32_t)used;
        doc["fsTotal"] = (uint32_t)total;
        doc["fsPct"]   = pct;
        doc["defaultStorageView"] = config.hardware.defaultStorageView;

        // RTC state
        if (Rtc) {
            doc["rtcProtected"] = Rtc->GetIsWriteProtected();
            doc["rtcRunning"]   = Rtc->GetIsRunning();
        }

        // Theme (nested object for SPA)
        JsonObject th = doc.createNestedObject("theme");
        th["mode"]              = (int)config.theme.mode;
        th["primaryColor"]      = config.theme.primaryColor;
        th["secondaryColor"]    = config.theme.secondaryColor;
        th["bgColor"]           = config.theme.bgColor;
        th["textColor"]         = config.theme.textColor;
        th["ffColor"]           = config.theme.ffColor;
        th["pfColor"]           = config.theme.pfColor;
        th["otherColor"]        = config.theme.otherColor;
        th["storageBarColor"]   = config.theme.storageBarColor;
        th["storageBar70Color"] = config.theme.storageBar70Color;
        th["storageBar90Color"] = config.theme.storageBar90Color;
        th["storageBarBorder"]  = config.theme.storageBarBorder;
        th["logoSource"]        = config.theme.logoSource;
        th["faviconPath"]       = config.theme.faviconPath;
        th["boardDiagramPath"]  = config.theme.boardDiagramPath;
        th["chartSource"]       = (int)config.theme.chartSource;
        th["chartLocalPath"]    = config.theme.chartLocalPath;
        th["chartLabelFormat"]  = (int)config.theme.chartLabelFormat;
        th["showIcons"]         = config.theme.showIcons;

        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: LIVE  (polled every 500 ms by SPA live page)
    // =========================================================================
    server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<1024> doc;

        noInterrupts();
        uint32_t safePulses = pulseCount;
        interrupts();

        doc["time"]    = getRtcDateTimeString();
        doc["ff"]      = digitalRead(config.hardware.pinWakeupFF);
        doc["pf"]      = digitalRead(config.hardware.pinWakeupPF);
        doc["wifi"]    = digitalRead(config.hardware.pinWifiTrigger);
        doc["pulses"]  = safePulses;
        doc["boot"]    = bootCount;
        doc["heap"]    = ESP.getFreeHeap();
        doc["heapTotal"] = ESP.getHeapSize();
        doc["uptime"]  = millis() / 1000;
        doc["trigger"] = cycleStartedBy;
        doc["cycleTime"] = (millis() - cycleStartTime) / 1000;
        doc["ffCount"] = highCountFF;
        doc["pfCount"] = highCountPF;
        doc["totalPulses"] = cycleTotalPulses + safePulses;

        const char* stateNames[] = {"IDLE", "WAIT_FLOW", "MONITORING", "DONE"};
        doc["state"]     = stateNames[loggingState];
        doc["stateTime"] = (millis() - stateStartTime) / 1000;

        if (loggingState == STATE_WAIT_FLOW) {
            long rem = (BUTTON_WAIT_FLOW_MS - (millis() - stateStartTime)) / 1000;
            doc["stateRemaining"] = rem > 0 ? rem : 0;
        } else if (loggingState == STATE_MONITORING && lastFlowPulseTime > 0) {
            long rem = (FLOW_IDLE_TIMEOUT_MS - (millis() - lastFlowPulseTime)) / 1000;
            doc["stateRemaining"] = rem > 0 ? rem : 0;
        } else {
            doc["stateRemaining"] = -1;
        }

        float liters = 0;
        if (config.flowMeter.pulsesPerLiter > 0)
            liters = (float)safePulses / config.flowMeter.pulsesPerLiter * config.flowMeter.calibrationMultiplier;
        doc["liters"] = liters;
        doc["mode"]   = onlineLoggerMode ? "online" : (apModeTriggered ? "webonly" : "logging");

        uint64_t used = 0, total = 0; int pct = 0;
        getStorageInfo(used, total, pct);
        doc["fsUsed"]  = (uint32_t)used;
        doc["fsTotal"] = (uint32_t)total;

        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: RECENT LOGS
    // =========================================================================
    server.on("/api/recent_logs", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<2048> doc;
        JsonArray logs = doc.createNestedArray("logs");

        if (!fsAvailable || !activeFS) {
            doc["error"] = "Storage not available";
            sendJsonResponse(r, doc);
            return;
        }

        String logFile = getActiveDatalogFile();
        if (!activeFS->exists(logFile)) {
            doc["error"] = "Log file not found";
            sendJsonResponse(r, doc);
            return;
        }

        File f = activeFS->open(logFile, "r");
        if (!f) {
            doc["error"] = "Cannot open file";
            sendJsonResponse(r, doc);
            return;
        }

        std::vector<String> lines;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) lines.push_back(line);
        }
        f.close();

        int totalLines = (int)lines.size();
        int startIdx   = totalLines > 5 ? totalLines - 5 : 0;
        for (int i = totalLines - 1; i >= startIdx; i--) {
            String line = lines[i];
            int p1 = line.indexOf('|');
            int p2 = line.indexOf('|', p1 + 1);
            int p3 = line.indexOf('|', p2 + 1);
            int p4 = line.indexOf('|', p3 + 1);
            int p5 = line.indexOf('|', p4 + 1);
            int p6 = line.indexOf('|', p5 + 1);
            int p7 = line.indexOf('|', p6 + 1);

            if (p1 > 0 && p2 > p1 && p3 > p2 && p4 > p3) {
                JsonObject entry = logs.createNestedObject();
                bool isNewFmt = (p7 > p6);
                if (isNewFmt) {
                    entry["time"]    = line.substring(0, p1) + " " + line.substring(p1+1, p2) + "-" + line.substring(p2+1, p3);
                    entry["trigger"] = line.substring(p4+1, p5);
                    String vs = line.substring(p5+1, p6); vs.replace("L:", ""); vs.replace(",", "."); entry["volume"] = vs + " L";
                    String ffs = line.substring(p6+1, p7); ffs.replace("FF", ""); entry["ff"] = ffs.toInt();
                    String pfs = line.substring(p7+1);     pfs.replace("PF", ""); entry["pf"] = pfs.toInt();
                } else {
                    entry["time"]    = line.substring(0, p2);
                    entry["trigger"] = line.substring(p3+1, p4);
                    String vs = line.substring(p4+1, p5); vs.replace("L:", ""); vs.replace(",", "."); entry["volume"] = vs + " L";
                    String ffs = line.substring(p5+1, p6); ffs.replace("FF", ""); entry["ff"] = ffs.toInt();
                    String pfs = line.substring(p6+1);     pfs.replace("PF", ""); entry["pf"] = pfs.toInt();
                }
            }
        }
        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: FILE LIST  (unified, replaces /api/stats file list)
    // =========================================================================
    server.on("/api/filelist", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<4096> doc;
        JsonArray files = doc.createNestedArray("files");

        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        String dir     = r->hasParam("dir")     ? r->getParam("dir")->value()     : "/";
        String filter  = r->hasParam("filter")  ? r->getParam("filter")->value()  : "";
        bool recursive = r->hasParam("recursive");

        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)           targetFS = &SD;
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        else if (littleFsAvailable)                        targetFS = &LittleFS;

        if (targetFS) {
            scanDir(*targetFS, dir, files, filter, recursive);

            // Storage stats
            uint64_t used = 0, total = 0; int pct = 0;
            getStorageInfo(used, total, pct, storage);
            doc["used"]    = (uint32_t)used;
            doc["total"]   = (uint32_t)total;
            doc["percent"] = pct;
        } else {
            doc["error"] = "Storage not available";
        }

        doc["currentFile"] = getActiveDatalogFile();
        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: CHANGELOG  (always LittleFS)
    // =========================================================================
    server.on("/api/changelog", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (LittleFS.exists("/changelog.txt"))
            r->send(LittleFS, "/changelog.txt", "text/plain");
        else
            r->send(404, "text/plain", "Changelog not found. Upload /changelog.txt");
    });

    // =========================================================================
    // API: REGEN DEVICE ID
    // =========================================================================
    server.on("/api/regen-id", HTTP_POST, [](AsyncWebServerRequest *r) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        String newId = mac.substring(mac.length() - 8);
        newId.toUpperCase();
        r->send(200, "text/plain", newId);
    });

    // =========================================================================
    // SAVE ENDPOINTS
    // =========================================================================

    // /save_device
    server.on("/save_device", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("deviceName", true))
            strncpy(config.deviceName, r->getParam("deviceName", true)->value().c_str(), 32);
        if (r->hasParam("deviceId", true)) {
            String newId = r->getParam("deviceId", true)->value();
            if (newId.length() > 0 && newId.length() <= 12)
                strncpy(config.deviceId, newId.c_str(), sizeof(config.deviceId) - 1);
        }
        config.forceWebServer = r->hasParam("forceWebServer", true);
        if (r->hasParam("defaultStorageView", true))
            config.hardware.defaultStorageView = r->getParam("defaultStorageView", true)->value().toInt();
        saveConfig();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // /save_flowmeter
    server.on("/save_flowmeter", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("pulsesPerLiter", true))
            config.flowMeter.pulsesPerLiter = r->getParam("pulsesPerLiter", true)->value().toFloat();
        if (r->hasParam("calibrationMultiplier", true))
            config.flowMeter.calibrationMultiplier = r->getParam("calibrationMultiplier", true)->value().toFloat();
        if (r->hasParam("monitoringWindowSecs", true))
            config.flowMeter.monitoringWindowSecs = r->getParam("monitoringWindowSecs", true)->value().toInt();
        if (r->hasParam("firstLoopWindowSecs", true))
            config.flowMeter.firstLoopMonitoringWindowSecs = r->getParam("firstLoopWindowSecs", true)->value().toInt();
        config.flowMeter.testMode = r->hasParam("testMode", true);
        if (r->hasParam("blinkDuration", true))
            config.flowMeter.blinkDuration = r->getParam("blinkDuration", true)->value().toInt();
        if (r->hasParam("resetBootCount", true)) { bootCount = 0; backupBootCount(); }
        saveConfig();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // /save_hardware  → restart required
    server.on("/save_hardware", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("storageType", true))    config.hardware.storageType   = (StorageType)r->getParam("storageType", true)->value().toInt();
        if (r->hasParam("wakeupMode", true))     config.hardware.wakeupMode    = (WakeupMode)r->getParam("wakeupMode", true)->value().toInt();
        if (r->hasParam("pinWifiTrigger", true)) config.hardware.pinWifiTrigger = r->getParam("pinWifiTrigger", true)->value().toInt();
        if (r->hasParam("pinWakeupFF", true))    config.hardware.pinWakeupFF   = r->getParam("pinWakeupFF", true)->value().toInt();
        if (r->hasParam("pinWakeupPF", true))    config.hardware.pinWakeupPF   = r->getParam("pinWakeupPF", true)->value().toInt();
        if (r->hasParam("pinFlowSensor", true))  config.hardware.pinFlowSensor = r->getParam("pinFlowSensor", true)->value().toInt();
        if (r->hasParam("pinRtcCE", true))       config.hardware.pinRtcCE      = r->getParam("pinRtcCE", true)->value().toInt();
        if (r->hasParam("pinRtcIO", true))       config.hardware.pinRtcIO      = r->getParam("pinRtcIO", true)->value().toInt();
        if (r->hasParam("pinRtcSCLK", true))     config.hardware.pinRtcSCLK    = r->getParam("pinRtcSCLK", true)->value().toInt();
        if (r->hasParam("pinSdCS", true))        config.hardware.pinSdCS       = r->getParam("pinSdCS", true)->value().toInt();
        if (r->hasParam("pinSdMOSI", true))      config.hardware.pinSdMOSI     = r->getParam("pinSdMOSI", true)->value().toInt();
        if (r->hasParam("pinSdMISO", true))      config.hardware.pinSdMISO     = r->getParam("pinSdMISO", true)->value().toInt();
        if (r->hasParam("pinSdSCK", true))       config.hardware.pinSdSCK      = r->getParam("pinSdSCK", true)->value().toInt();
        if (r->hasParam("cpuFreqMHz", true))     config.hardware.cpuFreqMHz    = r->getParam("cpuFreqMHz", true)->value().toInt();
        if (r->hasParam("debounceMs", true))     config.hardware.debounceMs    = constrain(r->getParam("debounceMs", true)->value().toInt(), 20, 500);
        saveConfig();
        sendRestartPage(r, "Device is restarting with new hardware settings.");
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    });

    // /save_theme
    server.on("/save_theme", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("themeMode", true))        config.theme.mode           = (ThemeMode)r->getParam("themeMode", true)->value().toInt();
        config.theme.showIcons = r->hasParam("showIcons", true);
        if (r->hasParam("primaryColor", true))     strncpy(config.theme.primaryColor,      r->getParam("primaryColor", true)->value().c_str(), 7);
        if (r->hasParam("secondaryColor", true))   strncpy(config.theme.secondaryColor,    r->getParam("secondaryColor", true)->value().c_str(), 7);
        if (r->hasParam("bgColor", true))          strncpy(config.theme.bgColor,           r->getParam("bgColor", true)->value().c_str(), 7);
        if (r->hasParam("textColor", true))        strncpy(config.theme.textColor,         r->getParam("textColor", true)->value().c_str(), 7);
        if (r->hasParam("ffColor", true))          strncpy(config.theme.ffColor,           r->getParam("ffColor", true)->value().c_str(), 7);
        if (r->hasParam("pfColor", true))          strncpy(config.theme.pfColor,           r->getParam("pfColor", true)->value().c_str(), 7);
        if (r->hasParam("otherColor", true))       strncpy(config.theme.otherColor,        r->getParam("otherColor", true)->value().c_str(), 7);
        if (r->hasParam("storageBarColor", true))  strncpy(config.theme.storageBarColor,   r->getParam("storageBarColor", true)->value().c_str(), 7);
        if (r->hasParam("storageBar70Color", true))strncpy(config.theme.storageBar70Color, r->getParam("storageBar70Color", true)->value().c_str(), 7);
        if (r->hasParam("storageBar90Color", true))strncpy(config.theme.storageBar90Color, r->getParam("storageBar90Color", true)->value().c_str(), 7);
        if (r->hasParam("storageBarBorder", true)) strncpy(config.theme.storageBarBorder,  r->getParam("storageBarBorder", true)->value().c_str(), 7);
        if (r->hasParam("logoSource", true))       strncpy(config.theme.logoSource,        r->getParam("logoSource", true)->value().c_str(), 128);
        if (r->hasParam("faviconPath", true))      strncpy(config.theme.faviconPath,       r->getParam("faviconPath", true)->value().c_str(), 32);
        if (r->hasParam("boardDiagramPath", true)) strncpy(config.theme.boardDiagramPath,  r->getParam("boardDiagramPath", true)->value().c_str(), 64);
        if (r->hasParam("chartSource", true))      config.theme.chartSource      = (ChartSource)r->getParam("chartSource", true)->value().toInt();
        if (r->hasParam("chartLocalPath", true))   strncpy(config.theme.chartLocalPath,    r->getParam("chartLocalPath", true)->value().c_str(), 64);
        if (r->hasParam("chartLabelFormat", true)) config.theme.chartLabelFormat = (ChartLabelFormat)r->getParam("chartLabelFormat", true)->value().toInt();
        saveConfig();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // /save_datalog
    server.on("/save_datalog", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("currentFile", true))  strncpy(config.datalog.currentFile, r->getParam("currentFile", true)->value().c_str(), 64);
        if (r->hasParam("prefix", true))       strncpy(config.datalog.prefix,      r->getParam("prefix", true)->value().c_str(), 32);
        if (r->hasParam("folder", true))       strncpy(config.datalog.folder,      r->getParam("folder", true)->value().c_str(), 32);
        if (r->hasParam("rotation", true))     config.datalog.rotation   = (DatalogRotation)r->getParam("rotation", true)->value().toInt();
        if (r->hasParam("maxSizeKB", true))    config.datalog.maxSizeKB  = r->getParam("maxSizeKB", true)->value().toInt();
        config.datalog.timestampFilename   = r->hasParam("timestampFilename", true);
        config.datalog.includeDeviceId     = r->hasParam("includeDeviceId", true);
        if (r->hasParam("dateFormat", true))   config.datalog.dateFormat  = r->getParam("dateFormat", true)->value().toInt();
        if (r->hasParam("timeFormat", true))   config.datalog.timeFormat  = r->getParam("timeFormat", true)->value().toInt();
        if (r->hasParam("endFormat", true))    config.datalog.endFormat   = r->getParam("endFormat", true)->value().toInt();
        if (r->hasParam("volumeFormat", true)) config.datalog.volumeFormat= r->getParam("volumeFormat", true)->value().toInt();
        config.datalog.includeBootCount    = r->hasParam("includeBootCount", true) && r->getParam("includeBootCount", true)->value() == "1";
        config.datalog.includeExtraPresses = r->hasParam("includeExtraPresses", true) && r->getParam("includeExtraPresses", true)->value() == "1";
        config.datalog.postCorrectionEnabled = r->hasParam("postCorrectionEnabled", true);
        if (r->hasParam("pfToFfThreshold", true))       config.datalog.pfToFfThreshold       = r->getParam("pfToFfThreshold", true)->value().toFloat();
        if (r->hasParam("ffToPfThreshold", true))       config.datalog.ffToPfThreshold       = r->getParam("ffToPfThreshold", true)->value().toFloat();
        if (r->hasParam("manualPressThresholdMs", true))config.datalog.manualPressThresholdMs= r->getParam("manualPressThresholdMs", true)->value().toInt();

        saveConfig();

        // Create new log file if requested
        String action = r->hasParam("action", true) ? r->getParam("action", true)->value() : "";
        if (action == "create" && fsAvailable && activeFS) {
            String folder = String(config.datalog.folder);
            if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
            if (folder.length() > 0 && !folder.endsWith("/"))   folder += "/";
            if (folder.length() == 0) folder = "/";
            if (folder != "/" && !activeFS->exists(folder)) activeFS->mkdir(folder);

            String newFile = folder + String(config.datalog.prefix);
            if (config.datalog.includeDeviceId && strlen(config.deviceId) > 0)
                newFile += "_" + String(config.deviceId);
            if (config.datalog.timestampFilename) {
                if (Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    char buf[20];
                    snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d",
                        now.Year(), now.Month(), now.Day(), now.Hour(), now.Minute(), now.Second());
                    newFile += buf;
                } else {
                    newFile += "_" + String(millis());
                }
            }
            newFile += ".txt";
            File f = activeFS->open(newFile, "w");
            if (f) {
                f.close();
                strncpy(config.datalog.currentFile, newFile.c_str(), 64);
                saveConfig();
                r->send(200, "application/json", "{\"ok\":true,\"file\":\"" + newFile + "\"}");
                return;
            }
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // /save_network  → restart required
    server.on("/save_network", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("wifiMode", true))       config.network.wifiMode = (WiFiModeType)r->getParam("wifiMode", true)->value().toInt();
        if (r->hasParam("apSSID", true))         strncpy(config.network.apSSID,         r->getParam("apSSID", true)->value().c_str(), 32);
        if (r->hasParam("apPassword", true))     strncpy(config.network.apPassword,     r->getParam("apPassword", true)->value().c_str(), 64);
        if (r->hasParam("clientSSID", true))     strncpy(config.network.clientSSID,     r->getParam("clientSSID", true)->value().c_str(), 32);
        if (r->hasParam("clientPassword", true)) strncpy(config.network.clientPassword, r->getParam("clientPassword", true)->value().c_str(), 64);
        config.network.useStaticIP = r->hasParam("useStaticIP", true);

        auto parseIP = [&](const char* param, uint8_t* dst) {
            if (r->hasParam(param, true)) sscanf(r->getParam(param, true)->value().c_str(), "%hhu.%hhu.%hhu.%hhu", dst, dst+1, dst+2, dst+3);
        };
        parseIP("staticIP",  config.network.staticIP);
        parseIP("gateway",   config.network.gateway);
        parseIP("subnet",    config.network.subnet);
        parseIP("dns",       config.network.dns);
        parseIP("apIP",      config.network.apIP);
        parseIP("apGateway", config.network.apGateway);
        parseIP("apSubnet",  config.network.apSubnet);

        saveConfig();
        sendRestartPage(r, "Device is restarting with new network settings.");
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    });

    // /save_time
    server.on("/save_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("ntpServer", true)) strncpy(config.network.ntpServer, r->getParam("ntpServer", true)->value().c_str(), 64);
        if (r->hasParam("timezone", true))  config.network.timezone = r->getParam("timezone", true)->value().toInt();
        saveConfig();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // =========================================================================
    // TIME MANAGEMENT
    // =========================================================================
    server.on("/set_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            r->send(409, "application/json", "{\"ok\":false,\"error\":\"Busy\"}");
            return;
        }
        if (r->hasParam("date", true) && r->hasParam("time", true) && Rtc) {
            String ds = r->getParam("date", true)->value();
            String ts = r->getParam("time", true)->value();
            int yr = ds.substring(0,4).toInt(), mo = ds.substring(5,7).toInt(), dy = ds.substring(8,10).toInt();
            int hr = ts.substring(0,2).toInt(), mi = ts.substring(3,5).toInt();
            RtcDateTime dt(yr, mo, dy, hr, mi, 0);
            bool ok = false;
            for (int attempt = 0; attempt < 3 && !ok; attempt++) {
                Rtc->SetIsWriteProtected(false); delay(10);
                Rtc->SetIsRunning(true); delay(10);
                Rtc->SetDateTime(dt); delay(100);
                RtcDateTime v = Rtc->GetDateTime();
                if (v.Year() == yr && v.Month() == mo && v.Day() == dy) { ok = true; rtcValid = true; }
            }
            r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"RTC write failed\"}");
        } else {
            r->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing params or no RTC\"}");
        }
    });

    server.on("/sync_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        bool ok = syncTimeFromNTP();
        if (ok) rtcValid = true;
        r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"NTP sync failed\"}");
    });

    server.on("/rtc_protect", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (Rtc) {
            bool protect = r->hasParam("protect", true);
            Rtc->SetIsWriteProtected(protect);
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/flush_logs", HTTP_POST, [](AsyncWebServerRequest *r) {
        flushLogBufferToFS();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/backup_bootcount", HTTP_POST, [](AsyncWebServerRequest *r) {
        backupBootCount();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/restore_bootcount", HTTP_POST, [](AsyncWebServerRequest *r) {
        uint32_t old = bootCount;
        restoreBootCount();
        String j = "{\"ok\":true,\"old\":" + String(old) + ",\"new\":" + String(bootCount) + "}";
        r->send(200, "application/json", j);
    });

    // =========================================================================
    // RESTART
    // =========================================================================
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "application/json", "{\"ok\":true}");
        shouldRestart = true;
        restartTimer  = millis();
    });
    server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *r) {
        r->send(200, "application/json", "{\"ok\":true}");
        shouldRestart = true;
        restartTimer  = millis();
    });

    // =========================================================================
    // FILE OPERATIONS
    // =========================================================================

    // /download
    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("file")) { r->send(400, "text/plain", "No file"); return; }
        String path = sanitizeFilename(r->getParam("file")->value());
        if (!path.startsWith("/")) path = "/" + path;
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        fs::FS* targetFS = (storage == "sdcard" && sdAvailable) ? (fs::FS*)&SD :
                           (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
        if (targetFS && targetFS->exists(path)) {
            String filename = path.substring(path.lastIndexOf('/') + 1);
            AsyncWebServerResponse *resp = r->beginResponse(*targetFS, path, "application/octet-stream");
            resp->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            r->send(resp);
        } else {
            r->send(404, "text/plain", "Not found");
        }
    });

    // /delete
    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("path")) { r->redirect("/"); return; }
        String path    = sanitizeFilename(r->getParam("path")->value());
        if (!path.startsWith("/")) path = "/" + path;
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)           targetFS = &SD;
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        else if (activeFS) targetFS = activeFS;

        bool deleted = false;
        if (targetFS) {
            File f = targetFS->open(path);
            if (f) {
                bool isDir = f.isDirectory(); f.close();
                if (isDir) deleteRecursive(*targetFS, path);
                else       targetFS->remove(path);
                deleted = true;
            }
        }
        r->send(200, "application/json", deleted ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    // /mkdir
    server.on("/mkdir", HTTP_GET, [](AsyncWebServerRequest *r) {
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("name") || !targetFS) { r->send(400, "text/plain", "Missing name"); return; }
        String dir  = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        if (storage == "sdcard" && sdAvailable) targetFS = &SD;
        else targetFS = &LittleFS;
        String name = sanitizeFilename(r->getParam("name")->value());
        String fp   = buildPath(dir, name);
        bool ok = targetFS->mkdir(fp);
        r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    // /move_file
    server.on("/move_file", HTTP_GET, [](AsyncWebServerRequest *r) {
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        String src     = r->hasParam("src")     ? sanitizePath(r->getParam("src")->value())     : "";
        String newName = r->hasParam("newName") ? r->getParam("newName")->value() : "";
        String destDir = r->hasParam("destDir") ? r->getParam("destDir")->value() : "";
        if (src.isEmpty() || newName.isEmpty()) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing params\"}"); return; }
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)           targetFS = &SD;
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        if (!targetFS) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"No storage\"}"); return; }
        String dstDir = destDir.isEmpty() ? src.substring(0, src.lastIndexOf('/')) : destDir;
        if (dstDir.isEmpty()) dstDir = "/";
        String dstPath = buildPath(dstDir, newName);
        bool ok = targetFS->rename(src, dstPath);
        r->send(200, "application/json", ok ? "{\"ok\":true,\"dst\":\"" + dstPath + "\"}" : "{\"ok\":false}");
    });

    // /upload
    static String _uploadDir     = "/";
    static String _uploadStorage = "internal";
    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            r->send(200, "application/json", "{\"ok\":true}");
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            static File _uploadFile;
            if (!index) {
                _uploadDir     = request->hasArg("path") ? request->arg("path") : "/www/";
                if (!_uploadDir.startsWith("/")) _uploadDir = "/" + _uploadDir;
                _uploadStorage = request->hasArg("storage") ? request->arg("storage") : currentStorageView;
            }
            fs::FS* targetFS = (_uploadStorage == "sdcard" && sdAvailable) ? (fs::FS*)&SD :
                               (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
            String fullPath = buildPath(_uploadDir, filename);
            if (!index && targetFS) {
                // Ensure parent dir exists
                targetFS->mkdir(_uploadDir);
                Serial.printf("Upload [%s]: %s\n", _uploadStorage.c_str(), fullPath.c_str());
                _uploadFile = targetFS->open(fullPath, "w");
            }
            if (_uploadFile && len) _uploadFile.write(data, len);
            if (final && _uploadFile) { Serial.printf("Upload done: %u bytes\n", index + len); _uploadFile.close(); }
        }
    );

    // =========================================================================
    // EXPORT / IMPORT SETTINGS
    // =========================================================================
    server.on("/export_settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        doc["deviceName"]    = config.deviceName;
        doc["deviceId"]      = config.deviceId;
        doc["forceWebServer"]= config.forceWebServer;

        JsonObject th = doc["theme"].to<JsonObject>();
        th["mode"]              = (int)config.theme.mode;
        th["primaryColor"]      = config.theme.primaryColor;
        th["secondaryColor"]    = config.theme.secondaryColor;
        th["bgColor"]           = config.theme.bgColor;
        th["textColor"]         = config.theme.textColor;
        th["ffColor"]           = config.theme.ffColor;
        th["pfColor"]           = config.theme.pfColor;
        th["otherColor"]        = config.theme.otherColor;
        th["logoSource"]        = config.theme.logoSource;
        th["faviconPath"]       = config.theme.faviconPath;
        th["boardDiagramPath"]  = config.theme.boardDiagramPath;
        th["chartSource"]       = (int)config.theme.chartSource;
        th["chartLocalPath"]    = config.theme.chartLocalPath;
        th["chartLabelFormat"]  = (int)config.theme.chartLabelFormat;
        th["showIcons"]         = config.theme.showIcons;

        JsonObject fm = doc["flowMeter"].to<JsonObject>();
        fm["pulsesPerLiter"]                = config.flowMeter.pulsesPerLiter;
        fm["calibrationMultiplier"]         = config.flowMeter.calibrationMultiplier;
        fm["monitoringWindowSecs"]          = config.flowMeter.monitoringWindowSecs;
        fm["firstLoopMonitoringWindowSecs"] = config.flowMeter.firstLoopMonitoringWindowSecs;
        fm["blinkDuration"]                 = config.flowMeter.blinkDuration;

        JsonObject dl = doc["datalog"].to<JsonObject>();
        dl["rotation"]              = (int)config.datalog.rotation;
        dl["maxSizeKB"]             = config.datalog.maxSizeKB;
        dl["folder"]                = config.datalog.folder;
        dl["prefix"]                = config.datalog.prefix;
        dl["dateFormat"]            = config.datalog.dateFormat;
        dl["timeFormat"]            = config.datalog.timeFormat;
        dl["endFormat"]             = config.datalog.endFormat;
        dl["volumeFormat"]          = config.datalog.volumeFormat;
        dl["includeBootCount"]      = config.datalog.includeBootCount;
        dl["includeExtraPresses"]   = config.datalog.includeExtraPresses;
        dl["postCorrectionEnabled"] = config.datalog.postCorrectionEnabled;
        dl["pfToFfThreshold"]       = config.datalog.pfToFfThreshold;
        dl["ffToPfThreshold"]       = config.datalog.ffToPfThreshold;
        dl["manualPressThresholdMs"]= config.datalog.manualPressThresholdMs;

        JsonObject net = doc["network"].to<JsonObject>();
        net["wifiMode"]    = (int)config.network.wifiMode;
        net["apSSID"]      = config.network.apSSID;
        net["clientSSID"]  = config.network.clientSSID;
        net["ntpServer"]   = config.network.ntpServer;
        net["timezone"]    = config.network.timezone;
        net["useStaticIP"] = config.network.useStaticIP;

        JsonObject hw = doc["hardware"].to<JsonObject>();
        hw["storageType"]        = (int)config.hardware.storageType;
        hw["wakeupMode"]         = (int)config.hardware.wakeupMode;
        hw["cpuFreqMHz"]         = config.hardware.cpuFreqMHz;
        hw["defaultStorageView"] = config.hardware.defaultStorageView;
        hw["debounceMs"]         = config.hardware.debounceMs;
        hw["pinWifiTrigger"]     = config.hardware.pinWifiTrigger;
        hw["pinWakeupFF"]        = config.hardware.pinWakeupFF;
        hw["pinWakeupPF"]        = config.hardware.pinWakeupPF;
        hw["pinFlowSensor"]      = config.hardware.pinFlowSensor;
        hw["pinRtcCE"]           = config.hardware.pinRtcCE;
        hw["pinRtcIO"]           = config.hardware.pinRtcIO;
        hw["pinRtcSCLK"]         = config.hardware.pinRtcSCLK;
        hw["pinSdCS"]            = config.hardware.pinSdCS;
        hw["pinSdMOSI"]          = config.hardware.pinSdMOSI;
        hw["pinSdMISO"]          = config.hardware.pinSdMISO;
        hw["pinSdSCK"]           = config.hardware.pinSdSCK;

        String json;
        serializeJsonPretty(doc, json);
        AsyncWebServerResponse *resp = r->beginResponse(200, "application/json", json);
        String fn = String(config.deviceName) + "_settings.json";
        resp->addHeader("Content-Disposition", "attachment; filename=\"" + fn + "\"");
        r->send(resp);
    });

    static String _importBuf;
    server.on("/import_settings", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            if (_importBuf.isEmpty()) { r->send(400, "text/plain", "No data"); return; }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, _importBuf);
            _importBuf = "";
            if (err) { r->send(400, "text/plain", String("JSON error: ") + err.c_str()); return; }

            if (doc["deviceName"].is<const char*>()) strncpy(config.deviceName, doc["deviceName"], 32);
            if (doc["forceWebServer"].is<bool>()) config.forceWebServer = doc["forceWebServer"];

            if (doc["theme"].is<JsonObject>()) {
                JsonObject t = doc["theme"];
                if (t["mode"].is<int>()) config.theme.mode = (ThemeMode)(int)t["mode"];
                auto cp7 = [&](const char* k, char* dst){ if(t[k].is<const char*>()) strncpy(dst, t[k], 7); };
                cp7("primaryColor",      config.theme.primaryColor);
                cp7("secondaryColor",    config.theme.secondaryColor);
                cp7("bgColor",           config.theme.bgColor);
                cp7("textColor",         config.theme.textColor);
                cp7("ffColor",           config.theme.ffColor);
                cp7("pfColor",           config.theme.pfColor);
                cp7("otherColor",        config.theme.otherColor);
                if (t["showIcons"].is<bool>()) config.theme.showIcons = t["showIcons"];
                if (t["chartSource"].is<int>()) config.theme.chartSource = (ChartSource)(int)t["chartSource"];
                if (t["chartLabelFormat"].is<int>()) config.theme.chartLabelFormat = (ChartLabelFormat)(int)t["chartLabelFormat"];
            }
            if (doc["flowMeter"].is<JsonObject>()) {
                JsonObject fm = doc["flowMeter"];
                if (fm["pulsesPerLiter"].is<float>()) config.flowMeter.pulsesPerLiter = fm["pulsesPerLiter"];
                if (fm["calibrationMultiplier"].is<float>()) config.flowMeter.calibrationMultiplier = fm["calibrationMultiplier"];
                if (fm["monitoringWindowSecs"].is<int>()) config.flowMeter.monitoringWindowSecs = fm["monitoringWindowSecs"];
                if (fm["firstLoopMonitoringWindowSecs"].is<int>()) config.flowMeter.firstLoopMonitoringWindowSecs = fm["firstLoopMonitoringWindowSecs"];
            }
            if (doc["datalog"].is<JsonObject>()) {
                JsonObject dl = doc["datalog"];
                if (dl["rotation"].is<int>()) config.datalog.rotation = (DatalogRotation)(int)dl["rotation"];
                if (dl["maxSizeKB"].is<int>()) config.datalog.maxSizeKB = dl["maxSizeKB"];
                if (dl["dateFormat"].is<int>()) config.datalog.dateFormat = dl["dateFormat"];
                if (dl["timeFormat"].is<int>()) config.datalog.timeFormat = dl["timeFormat"];
                if (dl["endFormat"].is<int>()) config.datalog.endFormat = dl["endFormat"];
                if (dl["volumeFormat"].is<int>()) config.datalog.volumeFormat = dl["volumeFormat"];
                if (dl["includeBootCount"].is<bool>()) config.datalog.includeBootCount = dl["includeBootCount"];
                if (dl["includeExtraPresses"].is<bool>()) config.datalog.includeExtraPresses = dl["includeExtraPresses"];
                if (dl["postCorrectionEnabled"].is<bool>()) config.datalog.postCorrectionEnabled = dl["postCorrectionEnabled"];
                if (dl["pfToFfThreshold"].is<float>()) config.datalog.pfToFfThreshold = dl["pfToFfThreshold"];
                if (dl["ffToPfThreshold"].is<float>()) config.datalog.ffToPfThreshold = dl["ffToPfThreshold"];
                if (dl["manualPressThresholdMs"].is<int>()) config.datalog.manualPressThresholdMs = dl["manualPressThresholdMs"];
            }
            if (doc["network"].is<JsonObject>()) {
                JsonObject net = doc["network"];
                if (net["wifiMode"].is<int>()) config.network.wifiMode = (WiFiModeType)(int)net["wifiMode"];
                if (net["ntpServer"].is<const char*>()) strncpy(config.network.ntpServer, net["ntpServer"], 64);
                if (net["timezone"].is<int>()) config.network.timezone = net["timezone"];
                if (net["useStaticIP"].is<bool>()) config.network.useStaticIP = net["useStaticIP"];
            }
            if (doc["hardware"].is<JsonObject>()) {
                JsonObject hw = doc["hardware"];
                if (hw["storageType"].is<int>()) config.hardware.storageType = (StorageType)(int)hw["storageType"];
                if (hw["wakeupMode"].is<int>()) config.hardware.wakeupMode = (WakeupMode)(int)hw["wakeupMode"];
                if (hw["cpuFreqMHz"].is<int>()) config.hardware.cpuFreqMHz = hw["cpuFreqMHz"];
                if (hw["defaultStorageView"].is<int>()) config.hardware.defaultStorageView = hw["defaultStorageView"];
                if (hw["debounceMs"].is<int>()) config.hardware.debounceMs = hw["debounceMs"];
            }
            saveConfig();
            r->send(200, "text/plain", "OK");
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) _importBuf = "";
            for (size_t i = 0; i < len; i++) _importBuf += (char)data[i];
        }
    );

    // =========================================================================
    // WIFI SCAN
    // =========================================================================
    server.on("/wifi_scan_start", HTTP_GET, [](AsyncWebServerRequest *r) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        r->send(200, "text/plain", "OK");
    });

    server.on("/wifi_scan_result", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<2048> doc;
        JsonArray nets = doc.createNestedArray("networks");
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            doc["scanning"] = true;
        } else if (n == WIFI_SCAN_FAILED) {
            doc["error"] = "Scan failed";
        } else if (n >= 0) {
            for (int i = 0; i < n && i < 20; i++) {
                JsonObject net = nets.createNestedObject();
                net["ssid"]   = WiFi.SSID(i);
                net["rssi"]   = WiFi.RSSI(i);
                net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            WiFi.scanDelete();
        }
        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // OTA FIRMWARE UPDATE
    // =========================================================================
    server.on("/do_update", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse *resp = r->beginResponse(200, "application/json",
                ok ? "{\"success\":true,\"message\":\"Update complete, restarting...\"}"
                   : "{\"success\":false,\"message\":\"Update failed\"}");
            resp->addHeader("Connection", "close");
            r->send(resp);
            if (ok) { safeWiFiShutdown(); delay(200); ESP.restart(); }
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("OTA start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
            }
            if (Update.write(data, len) != len) Update.printError(Serial);
            if (final) {
                if (Update.end(true)) Serial.printf("OTA done: %u bytes\n", index + len);
                else Update.printError(Serial);
            }
        }
    );

    // =========================================================================
    // STATIC FILE FALLBACK (not found handler)
    // =========================================================================
    server.onNotFound([](AsyncWebServerRequest *r) {
        String path = r->url();

        // Try LittleFS first (system assets: logo, favicon, board diagram, etc.)
        if (littleFsAvailable && LittleFS.exists(path)) {
            r->send(LittleFS, path, getMime(path));
            return;
        }
        // Then try activeFS (SD for log files, etc.)
        if (fsAvailable && activeFS && activeFS->exists(path)) {
            r->send(*activeFS, path, getMime(path));
            return;
        }
        // SPA fallback: for any GET without extension, serve index.html
        if (r->method() == HTTP_GET && path.indexOf('.') < 0) {
            if (uiReady && LittleFS.exists("/www/index.html")) {
                r->send(LittleFS, "/www/index.html", "text/html");
                return;
            }
            r->send_P(200, "text/html", FAILSAFE_HTML);
            return;
        }
        r->send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.printf("Web server started. Free heap: %d\n", ESP.getFreeHeap());
}
