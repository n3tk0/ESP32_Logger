/**
 * src/web/WebServer.cpp
 * ESP32 Water Logger v5.1.0 – Production audit hardening
 *
 * Architecture:
 *   – Normal mode  : AsyncWebServer serves /www/index.html + /www/js/*.js
 *   – Failsafe mode: If /www/index.html is missing, embedded minimal HTML is
 *                    served that lets the user upload the real UI files,
 *                    create directories, manage files, configure sensors,
 *                    and flash OTA firmware — all from PROGMEM.
 *   – All JSON API endpoints are always available regardless of UI mode.
 *
 * CSP: script-src includes 'unsafe-inline' because the failsafe PROGMEM page
 *      relies on inline <script> blocks and onclick handlers.
 *
 * Compatibility: ESPAsyncWebServer >= 3.11 (const-correct getParam API).
 */

#include "WebServer.h"
#include "../setup.h"                   // WEB_BASIC_AUTH_* macros
#include "../core/Globals.h"
#include "../core/SdCompat.h"           // sdFs() — SD.h only when FEATURE_SD_STORAGE
#include "FailsafeHtml.h"               // gzipped recovery UI (generated)
#include "../core/BoardProfiles.h"      // R11: g_boardProfile + isPinAllowed
#include "../modules/OtaModule.h"       // R20: /do_update respects OtaModule.enabled
#include "../modules/DataLogModule.h"  // /save_datalog delegates to DataLogModule::load()
#include "../managers/ConfigManager.h"
#include "../managers/WiFiManager.h"
#include "../managers/StorageManager.h"
#include "../managers/RtcManager.h"
#include "../managers/DataLogger.h"
#include "../utils/Utils.h"
#include "ApiHandlers.h"
#include "KindleSkin.h"                 // kdSkinClamp() on settings import
#include "FirstRunHandler.h"            // R11 first-run wizard backend
#include "RateLimiter.h"               // Pass 7 rate-limit on mutating routes
#include "CsrfToken.h"                 // Pass 7 CSRF on mutating routes
#include "RequireAuth.h"               // R5: unified mutating-handler auth preamble
#include "../pipeline/DataPipeline.h"   // fsMutex (FS1)
#include "../utils/MutexGuard.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <mbedtls/sha256.h>             // OTA SHA-256 verify (Pass 5 5.6)
#include <WiFi.h>
#include <functional>
#include <memory>
#include <vector>
#include <math.h>
#include <time.h>
#include <sys/time.h>

// Safe strncpy that always null-terminates
#define SAFE_STRNCPY(dst, src, n) do { strncpy(dst, src, (n) - 1); dst[(n) - 1] = '\0'; } while(0)

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

const char RESTART_HEAD[] PROGMEM = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Restarting</title><style>body{font-family:-apple-system,sans-serif;display:flex;justify-content:center;align-items:center;"
    "min-height:100vh;margin:0;background:#f0f2f5}.popup{background:#fff;border-radius:16px;padding:2rem;text-align:center;"
    "box-shadow:0 4px 20px rgba(0,0,0,0.15);max-width:350px}.icon{font-size:4rem;margin-bottom:1rem}"
    ".title{font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem}.msg{color:#666;margin-bottom:1rem}"
    ".progress{background:#e2e8f0;border-radius:8px;height:8px;overflow:hidden;margin-top:1rem}"
    ".bar{height:100%;background:#27ae60;width:0%;transition:width 1s linear}</style></head>"
    "<body><div class='popup'><div class='icon'>&#x1F504;</div><div class='title'>Restarting...</div><div class='msg'>";

const char RESTART_TAIL[] PROGMEM = "</div><div id='counter'>Redirecting in 5 seconds...</div>"
    "<div class='progress'><div class='bar' id='bar'></div></div></div>"
    "<script>var s=5,b=document.getElementById('bar'),c=document.getElementById('counter');"
    "var t=setInterval(function(){s--;b.style.width=(100-s*20)+'%';c.textContent='Redirecting in '+s+' seconds...';"
    "if(s<=0){clearInterval(t);window.location.href='/';}},1000);</script></body></html>";

void sendRestartPage(AsyncWebServerRequest *r, const char* message) {
    String html;
    html.reserve(strlen_P(RESTART_HEAD) + strlen(message) + strlen_P(RESTART_TAIL) + 1);
    html += FPSTR(RESTART_HEAD);
    html += message;
    html += FPSTR(RESTART_TAIL);
    r->send(200, "text/html", html);
}

// ============================================================================
// LIVE SNAPSHOT  (shared by /api/live polling endpoint and SSE /api/events)
// ============================================================================

// SSE channel: clients open `new EventSource('/api/events')`. The handler is
// registered in setupWebServer() so library auth gating still applies.
static AsyncEventSource liveEvents("/api/events");

static void buildLiveSnapshot(JsonDocument& doc) {
    // R28 / AUDIT 6.4: atomic load replaces noInterrupts/interrupts barrier.
    uint32_t safePulses = pulseCount.load(std::memory_order_relaxed);

    doc["time"]      = getRtcDateTimeString();
    doc["chip"]      = ESP.getChipModel();
    doc["version"]   = getVersionString();
    doc["network"]   = getNetworkDisplay();
    doc["ff"]        = digitalRead(config.hardware.pinWakeupFF);
    doc["pf"]        = digitalRead(config.hardware.pinWakeupPF);
    doc["wifi"]      = digitalRead(config.hardware.pinWifiTrigger);
    doc["pulses"]    = safePulses;
    doc["boot"]      = bootCount;
    doc["heap"]      = ESP.getFreeHeap();
    doc["heapTotal"] = ESP.getHeapSize();
    doc["uptime"]    = millis() / 1000;
    doc["trigger"]   = cycleStartedBy;
    doc["cycleTime"] = (millis() - cycleStartTime) / 1000;
    doc["ffCount"]   = highCountFF;
    doc["pfCount"]   = highCountPF;
    doc["totalPulses"] = cycleTotalPulses + safePulses;

    const char* stateNames[] = {"IDLE", "WAIT_FLOW", "MONITORING", "DONE"};
    int stateIdx = (loggingState >= 0 && loggingState <= 3) ? loggingState : 0;
    doc["state"]     = stateNames[stateIdx];
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
    char uBuf[24], tBuf[24];
    snprintf(uBuf, sizeof(uBuf), "%llu", (unsigned long long)used);
    snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)total);
    doc["fsUsed"]  = serialized(String(uBuf));
    doc["fsTotal"] = serialized(String(tBuf));

    doc["ip"] = wifiConnectedAsClient
                ? WiFi.localIP().toString()
                : WiFi.softAPIP().toString();
}

// Called from loop() at ~1 Hz.  Skips work entirely when nobody is subscribed
// so polling-only deployments pay zero cost.
void publishLiveEvent() {
    if (liveEvents.count() == 0) return;
    JsonDocument doc;
    buildLiveSnapshot(doc);
    String buf;
    serializeJson(doc, buf);
    liveEvents.send(buf.c_str(), "live", millis());
}

// ============================================================================
// FAILSAFE HTML  (served when /www/index.html is missing)
// ============================================================================
// ---------------------------------------------------------------------------
// The failsafe recovery page.
//
// It lives in FLASH, not on the filesystem, and that is the entire point: it
// is what answers when LittleFS has no /www — the state in which the device
// is broken and this page is how you reach it to fix it. It cannot be stored
// on the thing it exists to repair.
//
// It is stored GZIPPED (src/web/FailsafeHtml.h, generated from
// src/web/failsafe.html) purely to spend less flash for the same page: 8 KB
// instead of 27 KB, measured. Nothing about where it lives or when it is
// available changes; the browser inflates it. Every browser that can reach
// this device has supported Content-Encoding: gzip for twenty years, and the
// firmware's own /www assets are already served the same way.
//
// Browsers are not the only clients that reach here, though. `curl` sends no
// Accept-Encoding at all unless asked, and a rescue over curl is exactly the
// scenario this page exists for — so a client that has not announced gzip
// gets FAILSAFE_PLAIN below instead of 8 KB of binary on its terminal.

// The no-gzip fallback: ~1 KB, no JavaScript, no compression. It carries the
// two operations that actually recover a device — put a file into /www/, and
// restart — as plain <form> POSTs, so it works in a text browser as well as
// in curl. It is not a second copy of the UI and must not grow into one; the
// flash it costs is taken straight out of what the compression saved.
static const char FAILSAFE_PLAIN[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water Logger - Setup Mode</title></head><body>
<h1>Water Logger &mdash; SETUP MODE</h1>
<p>The normal UI is missing from <code>/www/</code>.</p>
<p>The full recovery page is stored gzip-compressed to save flash. This client
did not send <code>Accept-Encoding: gzip</code>, so this plain version is being
served instead. For the full page:
<code>curl --compressed http://HOST/setup</code></p>
<h2>Upload a file to /www/</h2>
<form method="POST" action="/upload" enctype="multipart/form-data">
<input type="hidden" name="path" value="/www/">
<input type="file" name="file"> <button type="submit">Upload</button>
</form>
<h2>Restart</h2>
<form method="POST" action="/restart"><button type="submit">Restart</button></form>
<h2>From a shell</h2>
<pre>curl --compressed http://HOST/setup
curl -F 'path=/www/' -F 'file=@index.html' http://HOST/upload
curl -X POST http://HOST/restart</pre>
</body></html>
)HTML";

static bool clientAcceptsGzip(AsyncWebServerRequest* r) {
    // const-qualified deliberately: the esphome fork returns AsyncWebHeader*
    // and ESP32Async's returns const AsyncWebHeader*. Binding to the const
    // pointer accepts both, and this use is read-only anyway.
    const AsyncWebHeader* h = r->getHeader("Accept-Encoding");
    // No header means no stated preference. RFC 9110 permits any encoding in
    // that case, but the clients that omit it are the ones least able to cope
    // with a compressed body, so treat silence as "no".
    return h && h->value().indexOf("gzip") >= 0;
}

static void sendFailsafePage(AsyncWebServerRequest* r) {
    if (!clientAcceptsGzip(r)) {
        r->send_P(200, "text/html", FAILSAFE_PLAIN);
        return;
    }
    AsyncWebServerResponse* resp = r->beginResponse_P(
        200, "text/html", FAILSAFE_HTML_GZ, FAILSAFE_HTML_GZ_LEN);
    if (!resp) {
        // Out of heap. Say something rather than nothing: a blank page here
        // is indistinguishable from a dead device, which is the one thing
        // this page exists to rule out.
        r->send(503, "text/plain",
                "Failsafe UI unavailable (out of memory). Retry, or POST /restart.");
        return;
    }
    resp->addHeader("Content-Encoding", "gzip");
    r->send(resp);
}



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
// Hard-cap on entries returned by a single /api/filelist call.  Bounds heap
// use from JsonDocument and prevents a malformed / crafted filesystem from
// driving the AsyncTCP worker OOM.
static const size_t SCANDIR_MAX_ENTRIES = 500;

// Returns true if the scan was truncated because SCANDIR_MAX_ENTRIES was hit.
static bool scanDir(fs::FS& fs, const String& dir, JsonArray& arr,
                    const String& filter, bool recursive) {
    std::vector<String> stack;
    stack.push_back(dir);

    while (!stack.empty()) {
        String currentDir = stack.back();
        stack.pop_back();

        while (currentDir.length() > 1 && currentDir.endsWith("/")) currentDir.remove(currentDir.length() - 1);

        File d = fs.open(currentDir);
        if (!d || !d.isDirectory()) {
            if (d) d.close();
            continue;
        }

        while (File entry = d.openNextFile()) {
            if (arr.size() >= SCANDIR_MAX_ENTRIES) {
                entry.close();
                d.close();
                return true;   // truncated
            }
            String name = String(entry.name());
            if (name.startsWith("/")) {
                int slash = name.lastIndexOf('/');
                name = (slash >= 0) ? name.substring(slash + 1) : name;
            }
            String fullPath = (currentDir == "/") ? "/" + name : currentDir + "/" + name;

            if (entry.isDirectory()) {
                if (recursive) {
                    stack.push_back(fullPath);
                } else {
                    JsonObject o = arr.add<JsonObject>();
                    o["name"]  = name;
                    o["path"]  = fullPath;
                    o["isDir"] = true;
                    o["size"]  = 0;
                }
            } else {
                bool include = filter.isEmpty() ||
                    (filter == "log" && (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")));
                if (include) {
                    JsonObject o = arr.add<JsonObject>();
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
    return false;
}

// ============================================================================
// IP ARRAY FORMATTER  (uint8_t[4] → "A.B.C.D")
// ============================================================================
static void fmtIP(const uint8_t* ip, char* buf16) {
    snprintf(buf16, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// ============================================================================
// ROUTE HANDLERS
// ============================================================================
// These are the bodies that used to be inline lambdas inside
// setupWebServer().  They are named functions for one measured reason:
// AsyncWebServer::on() takes std::function, every lambda is its own type, and
// GCC therefore instantiated a separate _Function_handler for each one -- 47
// copies of identical type-erasure code, 11,398 bytes.  Passing a plain
// void(*)(AsyncWebServerRequest*) makes every registration construct
// std::function from the same type, so one instantiation serves all of them.
//
// Nothing else changed: the bodies are verbatim, and each server.on() call
// stayed exactly where it was.  Registration ORDER decides which handler wins
// (first canHandle() to match), so it must not be rearranged.
// ============================================================================

#ifdef UI_CDN_BASE
// The CDN bootstrap page and its boot script.  These live at file scope
// because the handlers that serve them do: they used to be statics inside
// setupWebServer(), which compiled only while the handlers were lambdas
// nested in the same function.  Hoisting the handlers out without moving
// these broke every -DUI_CDN_BASE build and nothing noticed, because no
// CI job set the flag.  One now does.
static const char CDN_BOOTSTRAP_HTML[] PROGMEM =
    "<!DOCTYPE html><html lang=\"en\" id=\"htmlRoot\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
    "<title>Water Logger</title>"
    "<link rel=\"stylesheet\" href=\"" UI_CDN_BASE "/style.css\">"
    "<script src=\"" UI_CDN_BASE "/js/theme-boot.js\"></script>"
    "</head><body>"
    "<div id=\"cdnBoot\" style=\"font-family:sans-serif;padding:2rem;text-align:center\">"
    "Loading UI from CDN…</div>"
    "<script src=\"/cdn-boot.js\"></script>"
    "</body></html>";

// Boot script served from the device — `script-src 'self'` covers it
// without needing 'unsafe-inline'.  Fetches the SPA HTML from the CDN
// and replaces the bootstrap document; on failure, shows a link back
// to the on-device UI.
static const char CDN_BOOT_JS[] PROGMEM =
    "fetch('" UI_CDN_BASE "/index.html').then(function(r){return r.text();})"
    ".then(function(t){document.open();document.write(t);document.close();})"
    ".catch(function(e){"
      "document.getElementById('cdnBoot').innerHTML="
        "'CDN unreachable. <a href=\"/?_local=1\">Use local UI</a>';"
    "});";
#endif

#ifdef UI_CDN_BASE
static void h_get_cdn_boot_js(AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp =
        r->beginResponse_P(200, "application/javascript", CDN_BOOT_JS);
    resp->addHeader("Cache-Control", "public, max-age=300");
    r->send(resp);
}
#endif

static void h_get_root(AsyncWebServerRequest* r) {
#ifdef UI_CDN_BASE
    // Build-time CDN opt-in: 1 KB bootstrap from PROGMEM loads the SPA
    // from the hosted URL.  All API calls still target this device —
    // only the static bundle moves off-flash.  ?_local=1 lets devs
    // force the on-device copy when the CDN is unreachable.
    if (!r->hasParam("_local")) {
        r->send_P(200, "text/html", CDN_BOOTSTRAP_HTML);
        return;
    }
#endif
    if (littleFsAvailable && LittleFS.exists("/www/index.html.gz")) {
        AsyncWebServerResponse* resp =
            r->beginResponse(LittleFS, "/www/index.html.gz", "text/html");
        if (resp) {
            resp->addHeader("Content-Encoding", "gzip");
            r->send(resp);
            return;
        }
        // beginResponse can return null on low heap or a race with the
        // file being removed between exists() and beginResponse() —
        // fall through to the plain index.html / failsafe instead of
        // dereferencing the null pointer (observed crash MEPC=0x42036bcc
        // on first GET / after AP join).
    }
    if (littleFsAvailable && LittleFS.exists("/www/index.html")) {
        r->send(LittleFS, "/www/index.html", "text/html");
        return;
    }
    sendFailsafePage(r);
}

static void h_get_setup(AsyncWebServerRequest* r) {
    sendFailsafePage(r);
}

static void h_get_api_csrf_token(AsyncWebServerRequest* r) {
    String body = "{\"token\":\"";
    body += CsrfToken::get();
    body += "\"}";
    AsyncWebServerResponse* resp = r->beginResponse(200, "application/json", body);
    resp->addHeader("Cache-Control", "no-store");
    r->send(resp);
}

static void h_get_api_live(AsyncWebServerRequest* r) {
    JsonDocument doc;
    buildLiveSnapshot(doc);
    sendJsonResponse(r, doc);
}

static void h_get_api_recent_logs(AsyncWebServerRequest* r) {
    JsonDocument doc;
    JsonArray logs = doc["logs"].to<JsonArray>();

    uint32_t sinceBoot = 0;
    bool hasSince = false;
    if (r->hasParam("since")) {
        sinceBoot = (uint32_t)r->getParam("since")->value().toInt();
        hasSince  = true;
    }
    doc["bootCount"] = bootCount;  // client advances cursor from this

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

    // Efficient tail-read: seek to last ~1KB of file instead of reading every line.
    // Buffers moved to the heap — the previous on-stack `lastLines[5][160]` +
    // `lineBuf[160]` (~960 B) ate most of the AsyncTCP worker's budget.
    constexpr int    LR_LINES  = 5;
    constexpr size_t LR_LINELN = 160;
    const size_t   TAIL_BYTES = 1024;
    const size_t   fSize      = f.size();
    const bool     seeked     = fSize > TAIL_BYTES;
    const size_t   toRead     = seeked ? TAIL_BYTES : fSize;

    auto lastLines = std::unique_ptr<char[]>(new (std::nothrow) char[LR_LINES * LR_LINELN]);
    // ONE read of the tail, not one IPC round trip per character. f.read() on a
    // char at a time went through the VFS layer for every byte, which is what
    // made this handler block the Async worker for hundreds of milliseconds on
    // a full log.
    auto blockBuf  = std::unique_ptr<char[]>(new (std::nothrow) char[toRead + 1]);
    if (!lastLines || !blockBuf) {
        f.close();
        // Reported, not swallowed. A silent failure here returns a valid,
        // empty log list — indistinguishable from a device that simply has
        // nothing logged, which is the wrong thing to conclude when memory is
        // the problem.
        doc["error"] = "out of memory";
        sendJsonResponse(r, doc);
        return;
    }
    auto slot = [&](int k) { return lastLines.get() + (k * LR_LINELN); };

    int lCount = 0;
    if (seeked) f.seek(fSize - TAIL_BYTES);

    const size_t bytesRead = f.read((uint8_t*)blockBuf.get(), toRead);
    f.close();
    blockBuf[bytesRead] = '\0';

    char* ptr       = blockBuf.get();
    char* const end = blockBuf.get() + bytesRead;

    // A seek lands mid-line, so the first fragment is not a line. If there is
    // no newline in the whole window the file's last line is longer than the
    // window and NONE of what was read is a complete line — emitting the
    // fragment would present a truncated record as a whole one.
    if (seeked) {
        char* nl = (char*)memchr(ptr, '\n', (size_t)(end - ptr));
        ptr = nl ? nl + 1 : end;
    }

    while (ptr < end) {
        // memchr bounded by `end`, not strchr: strchr for the '\r' scanned the
        // entire remaining buffer on every line, which turned a 1 KB window of
        // short lines into a quadratic walk for no reason.
        char* nl = (char*)memchr(ptr, '\n', (size_t)(end - ptr));
        if (!nl) nl = end;

        char* cr = (char*)memchr(ptr, '\r', (size_t)(nl - ptr));
        size_t len = (size_t)((cr ? cr : nl) - ptr);

        if (len > 0) {
            if (len >= LR_LINELN) len = LR_LINELN - 1;
            memcpy(slot(lCount % LR_LINES), ptr, len);
            slot(lCount % LR_LINES)[len] = '\0';
            lCount++;
        }
        ptr = nl + 1;
    }

    int count = lCount < LR_LINES ? lCount : LR_LINES;
    for (int i = 0; i < count; i++) {
        int idx = (lCount - 1 - i) % LR_LINES;
        char* lineStr = slot(idx);
        
        char* saveptr;
        char* tokens[10];
        int tCount = 0;
        char* tok = strtok_r(lineStr, "|", &saveptr);
        while (tok && tCount < 10) {
            tokens[tCount++] = tok;
            tok = strtok_r(NULL, "|", &saveptr);
        }

        if (tCount >= 7) {
            // Opportunistic `?since=<bootcount>` filter — scan tokens for
            // a `#:<n>` entry and skip if n <= sinceBoot.  If no such token
            // exists (user disabled includeBootCount) the filter is a
            // no-op and the entry is returned as before.
            if (hasSince) {
                bool skip = false;
                for (int t = 0; t < tCount; t++) {
                    if (tokens[t][0] == '#' && tokens[t][1] == ':') {
                        uint32_t bc = (uint32_t)atoi(tokens[t] + 2);
                        if (bc <= sinceBoot) skip = true;
                        break;
                    }
                }
                if (skip) continue;
            }

            JsonObject entry = logs.add<JsonObject>();
            int tail = tCount - 1;

            if (tCount >= 8) {
                char timeBuf[80];
                snprintf(timeBuf, sizeof(timeBuf), "%s %s-%s", tokens[0], tokens[1], tokens[2]);
                entry["time"] = timeBuf;
            } else {
                char timeBuf[80];
                snprintf(timeBuf, sizeof(timeBuf), "%s|%s", tokens[0], tokens[1]);
                entry["time"] = timeBuf;
            }

            entry["trigger"] = tokens[tail - 3];
            
            char* vs = tokens[tail - 2];
            if (strncmp(vs, "L:", 2) == 0) vs += 2;
            for (char* p = vs; *p; p++) if (*p == ',') *p = '.';
            char volBuf[32];
            snprintf(volBuf, sizeof(volBuf), "%s L", vs);
            entry["volume"] = volBuf;
            
            char* ffs = tokens[tail - 1];
            if (strncmp(ffs, "FF", 2) == 0) ffs += 2;
            entry["ff"] = atoi(ffs);
            
            char* pfs = tokens[tail];
            if (strncmp(pfs, "PF", 2) == 0) pfs += 2;
            entry["pf"] = atoi(pfs);
        }
    }
    sendJsonResponse(r, doc);
}

static void h_get_api_filelist(AsyncWebServerRequest* r) {
    JsonDocument doc;
    JsonArray files = doc["files"].to<JsonArray>();

    String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
    // sanitizePath() rejects "..", backslash, control chars, NUL (returns
    // "") — mirrors /download, /delete, /move_file. Without it a caller
    // could enumerate arbitrary directories (e.g. /config) by traversal.
    String dir     = sanitizePath(r->hasParam("dir") ? r->getParam("dir")->value() : "/");
    // sanitizePath() returns "" for a rejected/traversal path; reject
    // explicitly with 400 like /download, /delete, /mkdir, /move_file
    // rather than letting scanDir() open an empty path.
    if (dir.isEmpty()) {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid dir\"}");
        return;
    }
    String filter  = r->hasParam("filter")  ? r->getParam("filter")->value()  : "";
    bool recursive = r->hasParam("recursive");

    fs::FS* targetFS = nullptr;
    if (storage == "sdcard" && sdAvailable)              targetFS = sdFs();
    else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
    else if (littleFsAvailable)                          targetFS = &LittleFS;

    if (targetFS) {
        bool truncated = scanDir(*targetFS, dir, files, filter, recursive);
        if (truncated) doc["truncated"] = true;
        uint64_t used = 0, total = 0; int pct = 0;
        getStorageInfo(used, total, pct, storage);
        char uBuf[24], tBuf[24];
        snprintf(uBuf, sizeof(uBuf), "%llu", (unsigned long long)used);
        snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)total);
        doc["used"]    = serialized(String(uBuf));
        doc["total"]   = serialized(String(tBuf));
        doc["percent"] = pct;
    } else {
        doc["error"] = "Storage not available";
    }

    doc["currentFile"] = getActiveDatalogFile();
    sendJsonResponse(r, doc);
}

static void h_get_api_changelog(AsyncWebServerRequest* r) {
    if (LittleFS.exists("/www/changelog.txt"))
        r->send(LittleFS, "/www/changelog.txt", "text/plain");
    else if (LittleFS.exists("/changelog.txt"))
        r->send(LittleFS, "/changelog.txt", "text/plain");
    else
        r->send(404, "text/plain", "Changelog not found. Upload /www/changelog.txt");
}

static void h_get_export_settings(AsyncWebServerRequest* r) {
    char ipBuf[16];

    // Guarantee the payload is complete regardless of how config landed
    // in memory — fillConfigDefaults() is idempotent, so callers still
    // see their last-saved non-default values.  Audit Pass 4 F:
    // "Remove duplicated fallback logic in /export_settings by running
    // applyDefaults() before serialization."
    fillConfigDefaults();

    JsonDocument doc;

    // ── Identity ──────────────────────────────────────────────────────────
    doc["deviceName"]     = strlen(config.deviceName) ? config.deviceName : "Water Logger";
    doc["deviceId"]       = config.deviceId;
    doc["forceWebServer"] = config.forceWebServer;

    // ── Theme ─────────────────────────────────────────────────────────────
    JsonObject th = doc["theme"].to<JsonObject>();
    th["mode"]              = (int)config.theme.mode;
    th["primaryColor"]      = config.theme.primaryColor;
    th["secondaryColor"]    = config.theme.secondaryColor;
    th["lightBgColor"]      = config.theme.lightBgColor;
    th["lightTextColor"]    = config.theme.lightTextColor;
    th["darkBgColor"]       = config.theme.darkBgColor;
    th["darkTextColor"]     = config.theme.darkTextColor;
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
    th["chartLocalPath"]    = strlen(config.theme.chartLocalPath) ? config.theme.chartLocalPath : "/uPlot.iife.min.js";
    th["chartLabelFormat"]  = (int)config.theme.chartLabelFormat;
    th["showIcons"]         = config.theme.showIcons;

    // ── Flow Meter ────────────────────────────────────────────────────────
    JsonObject fm = doc["flowMeter"].to<JsonObject>();
    fm["pulsesPerLiter"]                = config.flowMeter.pulsesPerLiter > 0    ? config.flowMeter.pulsesPerLiter    : 450.0f;
    fm["calibrationMultiplier"]         = config.flowMeter.calibrationMultiplier ? config.flowMeter.calibrationMultiplier : 1.0f;
    fm["testMode"]                      = config.flowMeter.testMode;
    fm["blinkDuration"]                 = config.flowMeter.blinkDuration > 0 ? config.flowMeter.blinkDuration : 250;

    // ── Datalog ───────────────────────────────────────────────────────────
    JsonObject dl = doc["datalog"].to<JsonObject>();
    dl["rotation"]               = (int)config.datalog.rotation;
    dl["maxSizeKB"]              = config.datalog.maxSizeKB > 0 ? config.datalog.maxSizeKB : 1024;
    dl["maxEntries"]             = config.datalog.maxEntries > 0 ? config.datalog.maxEntries : 10000;
    dl["folder"]                 = config.datalog.folder;
    dl["timestampFilename"]      = config.datalog.timestampFilename;
    dl["includeDeviceId"]        = config.datalog.includeDeviceId;
    dl["prefix"]                 = strlen(config.datalog.prefix) ? config.datalog.prefix : "datalog";
    dl["dateFormat"]             = (int)config.datalog.dateFormat;
    dl["timeFormat"]             = (int)config.datalog.timeFormat;
    dl["endFormat"]              = (int)config.datalog.endFormat;
    dl["volumeFormat"]           = (int)config.datalog.volumeFormat;
    dl["includeBootCount"]       = config.datalog.includeBootCount;
    dl["includeExtraPresses"]    = config.datalog.includeExtraPresses;
    dl["postCorrectionEnabled"]  = config.datalog.postCorrectionEnabled;
    dl["pfToFfThreshold"]        = config.datalog.pfToFfThreshold > 0 ? config.datalog.pfToFfThreshold : 4.5f;
    dl["ffToPfThreshold"]        = config.datalog.ffToPfThreshold > 0 ? config.datalog.ffToPfThreshold : 3.7f;
    dl["manualPressThresholdMs"] = config.datalog.manualPressThresholdMs;

    // ── Logger (wide-CSV pipeline) ────────────────────────────────────────
    JsonObject lg = doc["logger"].to<JsonObject>();
    lg["csvLoggingEnabled"]         = config.logger.csvLoggingEnabled;
    lg["aggregationIntervalSec"]    = config.logger.aggregationIntervalSec ? config.logger.aggregationIntervalSec : 60;

    // ── Kindle dashboard appearance ───────────────────────────────────────
    // Exported on every build, including one without FEATURE_KINDLE_DASHBOARD:
    // a settings file is a record of the device's configuration, and dropping
    // a section because this particular firmware cannot draw it would mean a
    // backup taken on one build quietly resetting the appearance on another.
    JsonObject kd = doc["kindle"].to<JsonObject>();
    kd["face"]          = config.kindle.face;
    kd["faceCustom"]    = config.kindle.faceCustom;
    kd["boldZones"]     = config.kindle.boldZones;
    kd["showFlags"]     = config.kindle.showFlags;
    kd["clockStyle"]    = config.kindle.clockStyle;
    kd["timeFormat"]    = config.kindle.timeFormat;
    kd["dateFormat"]    = config.kindle.dateFormat;
    kd["pressureUnit"]  = config.kindle.pressureUnit;
    kd["tempDecimals"]  = config.kindle.tempDecimals;

    // ── Network ───────────────────────────────────────────────────────────
    JsonObject net = doc["network"].to<JsonObject>();
    net["wifiMode"]       = (int)config.network.wifiMode;
    net["apSSID"]         = strlen(config.network.apSSID)         ? config.network.apSSID         : DEFAULT_AP_SSID;
    const char* apPw = "***";
    const char* clPw = "***";
#if WEB_BASIC_AUTH_ENABLED
    if (r->hasParam("reveal_secrets") &&
        r->getParam("reveal_secrets")->value() == "1") {
        apPw = config.network.apPassword;
        clPw = config.network.clientPassword;
    }
#endif
    net["apPassword"]     = apPw;
    net["clientSSID"]     = config.network.clientSSID;
    net["clientPassword"] = clPw;
    net["ntpServer"]      = strlen(config.network.ntpServer)      ? config.network.ntpServer      : DEFAULT_NTP_SERVER;
    net["timezone"]       = config.network.timezone;
    net["useStaticIP"]    = config.network.useStaticIP;

    // AP network — uint8_t[4] arrays → "A.B.C.D" strings
    fmtIP(config.network.apIP,      ipBuf); net["apIP"]      = ipBuf;
    fmtIP(config.network.apGateway, ipBuf); net["apGateway"] = ipBuf;
    fmtIP(config.network.apSubnet,  ipBuf); net["apSubnet"]  = ipBuf;

    // Client static IP — uint8_t[4] arrays → "A.B.C.D" strings
    fmtIP(config.network.staticIP, ipBuf); net["staticIP"] = ipBuf;
    fmtIP(config.network.gateway,  ipBuf); net["gateway"]  = ipBuf;
    fmtIP(config.network.subnet,   ipBuf); net["subnet"]   = ipBuf;
    fmtIP(config.network.dns,      ipBuf); net["dns"]      = ipBuf;

    // ── Hardware ──────────────────────────────────────────────────────────
    JsonObject hw = doc["hardware"].to<JsonObject>();
    hw["storageType"]        = (int)config.hardware.storageType;
    hw["wakeupMode"]         = (int)config.hardware.wakeupMode;
    hw["cpuFreqMHz"]         = config.hardware.cpuFreqMHz > 0 ? config.hardware.cpuFreqMHz : 80;
    hw["defaultStorageView"] = config.hardware.defaultStorageView;
    hw["debounceMs"]         = config.hardware.debounceMs > 0  ? config.hardware.debounceMs : 100;
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

    AsyncResponseStream *resp = r->beginResponseStream("application/json");
    String fn = String(strlen(config.deviceName) ? config.deviceName : "device") + "_settings.json";
    resp->addHeader("Content-Disposition", "attachment; filename=\"" + fn + "\"");
    serializeJson(doc, *resp);
    r->send(resp);
}

static void h_post_save_device(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (r->hasParam("deviceName", true))
        SAFE_STRNCPY(config.deviceName, r->getParam("deviceName", true)->value().c_str(), sizeof(config.deviceName));
    if (r->hasParam("deviceId", true)) {
        String newId = r->getParam("deviceId", true)->value();
        if (newId.length() > 0 && newId.length() <= 12)
            SAFE_STRNCPY(config.deviceId, newId.c_str(), sizeof(config.deviceId));
    }
    config.forceWebServer = r->hasParam("forceWebServer", true);
    if (r->hasParam("defaultStorageView", true))
        config.hardware.defaultStorageView = r->getParam("defaultStorageView", true)->value().toInt();
    // PR #105 follow-up: Reset Boot Count migrated from /save_flowmeter
    // (page retired) to the System Info card on settings_device.
    if (r->hasParam("resetBootCount", true)) { bootCount = 0; backupBootCount(); }
    saveConfig();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_save_hardware(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    // R11: validate every pin parameter against the active board profile
    // before assigning. PIN_UNSET / -1 is accepted (means "unconfigured").
    // First violation aborts the save with a 400; partial assignment is
    // never persisted (saveConfig runs only at the end).
    auto setPin = [&](const char* name, uint8_t& dest) -> bool {
        if (!r->hasParam(name, true)) return true;
        int v = r->getParam(name, true)->value().toInt();
        if (v == -1) { dest = PIN_UNSET; return true; }
        if (v < 0 || v > 255) {
            r->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"pin out of range\"}");
            return false;
        }
        if (!isPinAllowed(g_boardProfile, (uint8_t)v, PIN_PURPOSE_GENERIC)) {
            char body[180];
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"%s = GPIO%d rejected: %s\"}",
                     name, v, pinRejectReason(g_boardProfile, (uint8_t)v));
            r->send(400, "application/json", body);
            return false;
        }
        dest = (uint8_t)v;
        return true;
    };
    if (r->hasParam("storageType", true))    config.hardware.storageType    = (StorageType)r->getParam("storageType", true)->value().toInt();
    if (r->hasParam("wakeupMode", true))     config.hardware.wakeupMode     = (WakeupMode)r->getParam("wakeupMode", true)->value().toInt();
    if (!setPin("pinWifiTrigger", config.hardware.pinWifiTrigger)) return;
    if (!setPin("pinWakeupFF",    config.hardware.pinWakeupFF))    return;
    if (!setPin("pinWakeupPF",    config.hardware.pinWakeupPF))    return;
    if (!setPin("pinFlowSensor",  config.hardware.pinFlowSensor))  return;
    if (!setPin("pinRtcCE",       config.hardware.pinRtcCE))       return;
    if (!setPin("pinRtcIO",       config.hardware.pinRtcIO))       return;
    if (!setPin("pinRtcSCLK",     config.hardware.pinRtcSCLK))     return;
    if (!setPin("pinSdCS",        config.hardware.pinSdCS))        return;
    if (!setPin("pinSdMOSI",      config.hardware.pinSdMOSI))      return;
    if (!setPin("pinSdMISO",      config.hardware.pinSdMISO))      return;
    if (!setPin("pinSdSCK",       config.hardware.pinSdSCK))       return;
    // Duplicate-pin check (Gemini medium on PR #87). After every
    // setPin above succeeded, scan the final config for any two
    // assigned pins on the same GPIO and refuse the whole save.
    {
        struct PinRef { const char* name; uint8_t value; };
        PinRef refs[] = {
            {"pinWifiTrigger", config.hardware.pinWifiTrigger},
            {"pinWakeupFF",    config.hardware.pinWakeupFF},
            {"pinWakeupPF",    config.hardware.pinWakeupPF},
            {"pinFlowSensor",  config.hardware.pinFlowSensor},
            {"pinRtcCE",       config.hardware.pinRtcCE},
            {"pinRtcIO",       config.hardware.pinRtcIO},
            {"pinRtcSCLK",     config.hardware.pinRtcSCLK},
            {"pinSdCS",        config.hardware.pinSdCS},
            {"pinSdMOSI",      config.hardware.pinSdMOSI},
            {"pinSdMISO",      config.hardware.pinSdMISO},
            {"pinSdSCK",       config.hardware.pinSdSCK},
        };
        constexpr int N = sizeof(refs) / sizeof(refs[0]);
        for (int i = 0; i < N; i++) {
            if (refs[i].value == PIN_UNSET) continue;
            for (int j = i + 1; j < N; j++) {
                if (refs[j].value == refs[i].value) {
                    char body[180];
                    snprintf(body, sizeof(body),
                             "{\"ok\":false,\"error\":\"duplicate pin: %s and %s both = GPIO%u\"}",
                             refs[i].name, refs[j].name, refs[i].value);
                    r->send(400, "application/json", body);
                    return;
                }
            }
        }
    }
    if (r->hasParam("cpuFreqMHz", true))     config.hardware.cpuFreqMHz     = r->getParam("cpuFreqMHz", true)->value().toInt();
    if (r->hasParam("debounceMs", true))     config.hardware.debounceMs     = constrain(r->getParam("debounceMs", true)->value().toInt(), 20, 500);
    if (r->hasParam("debugMode", true))      config.hardware.debugMode      = r->getParam("debugMode", true)->value() == "1";
    
    // Chunk G: move testMode / blinkDuration to hardware page
    if (r->hasParam("testMode", true)) {
        config.flowMeter.testMode = r->getParam("testMode", true)->value() == "on" || r->getParam("testMode", true)->value() == "1";
    } else {
        config.flowMeter.testMode = false;
    }
    if (r->hasParam("blinkDuration", true)) {
        // PR #105 review (Gemini medium): restore the lower-bound clamp
        // the original /save_flowmeter handler had — values < 50 ms make
        // the LED timing-loop in ESP_Logger.ino spin too tight.
        config.flowMeter.blinkDuration = max(50L, (long)r->getParam("blinkDuration", true)->value().toInt());
    }
    saveConfig();
    sendRestartPage(r, "Device is restarting with new hardware settings.");
    shouldRestart = true;
    restartTimer  = millis();
}

static void h_post_save_theme(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (r->hasParam("themeMode", true))        config.theme.mode           = (ThemeMode)r->getParam("themeMode", true)->value().toInt();
    config.theme.showIcons = r->hasParam("showIcons", true);
    if (r->hasParam("primaryColor", true))     SAFE_STRNCPY(config.theme.primaryColor,      r->getParam("primaryColor", true)->value().c_str(), sizeof(config.theme.primaryColor));
    if (r->hasParam("secondaryColor", true))   SAFE_STRNCPY(config.theme.secondaryColor,    r->getParam("secondaryColor", true)->value().c_str(), sizeof(config.theme.secondaryColor));
    if (r->hasParam("lightBgColor", true))     SAFE_STRNCPY(config.theme.lightBgColor,      r->getParam("lightBgColor", true)->value().c_str(), sizeof(config.theme.lightBgColor));
    if (r->hasParam("lightTextColor", true))   SAFE_STRNCPY(config.theme.lightTextColor,    r->getParam("lightTextColor", true)->value().c_str(), sizeof(config.theme.lightTextColor));
    if (r->hasParam("darkBgColor", true))      SAFE_STRNCPY(config.theme.darkBgColor,       r->getParam("darkBgColor", true)->value().c_str(), sizeof(config.theme.darkBgColor));
    if (r->hasParam("darkTextColor", true))    SAFE_STRNCPY(config.theme.darkTextColor,     r->getParam("darkTextColor", true)->value().c_str(), sizeof(config.theme.darkTextColor));
    if (r->hasParam("ffColor", true))          SAFE_STRNCPY(config.theme.ffColor,           r->getParam("ffColor", true)->value().c_str(), sizeof(config.theme.ffColor));
    if (r->hasParam("pfColor", true))          SAFE_STRNCPY(config.theme.pfColor,           r->getParam("pfColor", true)->value().c_str(), sizeof(config.theme.pfColor));
    if (r->hasParam("otherColor", true))       SAFE_STRNCPY(config.theme.otherColor,        r->getParam("otherColor", true)->value().c_str(), sizeof(config.theme.otherColor));
    if (r->hasParam("storageBarColor", true))  SAFE_STRNCPY(config.theme.storageBarColor,   r->getParam("storageBarColor", true)->value().c_str(), sizeof(config.theme.storageBarColor));
    if (r->hasParam("storageBar70Color", true))SAFE_STRNCPY(config.theme.storageBar70Color, r->getParam("storageBar70Color", true)->value().c_str(), sizeof(config.theme.storageBar70Color));
    if (r->hasParam("storageBar90Color", true))SAFE_STRNCPY(config.theme.storageBar90Color, r->getParam("storageBar90Color", true)->value().c_str(), sizeof(config.theme.storageBar90Color));
    if (r->hasParam("storageBarBorder", true)) SAFE_STRNCPY(config.theme.storageBarBorder,  r->getParam("storageBarBorder", true)->value().c_str(), sizeof(config.theme.storageBarBorder));
    if (r->hasParam("logoSource", true))       SAFE_STRNCPY(config.theme.logoSource,        r->getParam("logoSource", true)->value().c_str(), sizeof(config.theme.logoSource));
    if (r->hasParam("faviconPath", true))      SAFE_STRNCPY(config.theme.faviconPath,       r->getParam("faviconPath", true)->value().c_str(), sizeof(config.theme.faviconPath));
    if (r->hasParam("boardDiagramPath", true)) SAFE_STRNCPY(config.theme.boardDiagramPath,  r->getParam("boardDiagramPath", true)->value().c_str(), sizeof(config.theme.boardDiagramPath));
    if (r->hasParam("chartSource", true))      config.theme.chartSource      = (ChartSource)r->getParam("chartSource", true)->value().toInt();
    if (r->hasParam("chartLocalPath", true))   SAFE_STRNCPY(config.theme.chartLocalPath,    r->getParam("chartLocalPath", true)->value().c_str(), sizeof(config.theme.chartLocalPath));
    if (r->hasParam("chartLabelFormat", true)) config.theme.chartLabelFormat = (ChartLabelFormat)r->getParam("chartLabelFormat", true)->value().toInt();
    saveConfig();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_save_datalog(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;

    // ----- Path validation at the HTTP boundary --------------------------
    // DataLogModule::load() trusts that prefix/folder/currentFile have
    // already been sanitised; the gates below keep that contract honest
    // and return HTTP 400 with field-specific messages if they fail.
    auto isSafePrefix = [](const String& s) -> bool {
        if (s.length() == 0 || s.length() > 32) return false;
        for (size_t i = 0; i < s.length(); i++) {
            char c = s[i];
            if (c == '/' || c == '\\' || c == '\0' || (unsigned char)c < 0x20 || c == 0x7f) return false;
        }
        if (s == "." || s == "..") return false;
        return true;
    };
    String safeCurrentFile;
    String safePrefix;
    String safeFolder;
    bool   haveCurrentFile = false, havePrefix = false, haveFolder = false;
    if (r->hasParam("currentFile", true)) {
        safeCurrentFile = sanitizePath(r->getParam("currentFile", true)->value());
        if (safeCurrentFile.length() == 0) {
            r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid currentFile path\"}");
            return;
        }
        if (!fsAvailable || !activeFS || !activeFS->exists(safeCurrentFile)) {
            r->send(400, "application/json", "{\"ok\":false,\"error\":\"currentFile does not exist\"}");
            return;
        }
        haveCurrentFile = true;
    }
    if (r->hasParam("prefix", true)) {
        safePrefix = r->getParam("prefix", true)->value();
        if (!isSafePrefix(safePrefix)) {
            r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid prefix (no slashes, control chars, or ..)\"}");
            return;
        }
        havePrefix = true;
    }
    if (r->hasParam("folder", true)) {
        String fld = r->getParam("folder", true)->value();
        if (fld.length() > 0) {
            safeFolder = sanitizePath(fld);
            if (safeFolder.length() == 0) {
                r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid folder path\"}");
                return;
            }
        } // else: empty folder = root, fine
        haveFolder = true;
    }

    // ----- Project form params into a JsonDocument & delegate -----------
    // DataLogModule::load() is the single source of truth for clamps,
    // enum bounds, and assignment to config.datalog. Both /save_datalog
    // (here) and /import_settings call it, so format-field drift across
    // entry points is impossible by construction.
    JsonDocument doc;
    JsonObject cfg = doc.to<JsonObject>();
    if (haveCurrentFile)                       cfg["currentFile"]            = safeCurrentFile;
    if (havePrefix)                            cfg["prefix"]                 = safePrefix;
    if (haveFolder)                            cfg["folder"]                 = safeFolder;
    if (r->hasParam("rotation", true))         cfg["rotation"]               = r->getParam("rotation", true)->value().toInt();
    if (r->hasParam("maxSizeKB", true))        cfg["maxSizeKB"]              = r->getParam("maxSizeKB", true)->value().toInt();
    if (r->hasParam("maxEntries", true))       cfg["maxEntries"]             = r->getParam("maxEntries", true)->value().toInt();
    cfg["timestampFilename"]                   = r->hasParam("timestampFilename", true);
    cfg["includeDeviceId"]                     = r->hasParam("includeDeviceId", true);
    cfg["includeBootCount"]                    = r->hasParam("includeBootCount", true);
    cfg["includeExtraPresses"]                 = r->hasParam("includeExtraPresses", true);
    if (r->hasParam("dateFormat", true))       cfg["dateFormat"]             = r->getParam("dateFormat", true)->value().toInt();
    if (r->hasParam("timeFormat", true))       cfg["timeFormat"]             = r->getParam("timeFormat", true)->value().toInt();
    if (r->hasParam("endFormat", true))        cfg["endFormat"]              = r->getParam("endFormat", true)->value().toInt();
    if (r->hasParam("volumeFormat", true))     cfg["volumeFormat"]           = r->getParam("volumeFormat", true)->value().toInt();
    cfg["postCorrectionEnabled"]               = r->hasParam("postCorrectionEnabled", true);
    if (r->hasParam("pfToFfThreshold", true))         cfg["pfToFfThreshold"]        = r->getParam("pfToFfThreshold", true)->value().toFloat();
    if (r->hasParam("ffToPfThreshold", true))         cfg["ffToPfThreshold"]        = r->getParam("ffToPfThreshold", true)->value().toFloat();
    if (r->hasParam("manualPressThresholdMs", true))  cfg["manualPressThresholdMs"] = r->getParam("manualPressThresholdMs", true)->value().toInt();

    // ArduinoJson v7 doesn't expose .as<>() on JsonObject — but
    // JsonObject is implicitly convertible to JsonObjectConst, which is
    // what DataLogModule::load() expects.
    DataLogModule::instance().load(cfg);

    // Wide-CSV pipeline knobs (config.logger.*) live on the sensors page
    // now (POST /save_sensorlog). The "create" / "switch" file actions
    // moved to /api/datalog/{create,switch}.

    saveConfig();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_save_sensorlog(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    config.logger.csvLoggingEnabled = r->hasParam("csvLoggingEnabled", true);
    if (r->hasParam("aggregationIntervalSec", true))
        config.logger.aggregationIntervalSec = constrain(
            r->getParam("aggregationIntervalSec", true)->value().toInt(), 5, 3600);
    saveConfig();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_api_datalog_create(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (!fsAvailable || !activeFS) {
        r->send(503, "application/json", "{\"ok\":false,\"error\":\"no fs\"}");
        return;
    }
    if (!r->hasParam("prefix", true)) {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"prefix required\"}");
        return;
    }
    String prefix = r->getParam("prefix", true)->value();
    // Same prefix rules as /save_datalog (kept in sync — see the inline
    // isSafePrefix lambda in that handler).
    if (prefix.length() == 0 || prefix.length() > 32 || prefix == "." || prefix == "..") {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid prefix\"}");
        return;
    }
    for (size_t i = 0; i < prefix.length(); i++) {
        char c = prefix[i];
        if (c == '/' || c == '\\' || c == '\0' || (unsigned char)c < 0x20 || c == 0x7f) {
            r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid prefix\"}");
            return;
        }
    }
    String folder = r->hasParam("folder", true) ? r->getParam("folder", true)->value() : "";
    if (folder.length() > 0) {
        folder = sanitizePath(folder);
        if (folder.length() == 0) {
            r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid folder\"}");
            return;
        }
        if (!folder.endsWith("/")) folder += "/";
    } else {
        folder = "/";
    }
    if (folder != "/" && !activeFS->exists(folder)) activeFS->mkdir(folder);

    bool incDeviceId = r->hasParam("includeDeviceId", true);
    bool timestampFn = r->hasParam("timestampFilename", true);

    String newFile = folder + prefix;
    if (incDeviceId && strlen(config.deviceId) > 0)
        newFile += "_" + String(config.deviceId);
    if (timestampFn) {
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
    if (!f) {
        r->send(500, "application/json", "{\"ok\":false,\"error\":\"Failed to create file\"}");
        return;
    }
    f.close();

    // Make this the active file unless caller explicitly opts out via
    // ?switch=0; default behaviour mirrors the pre-split UX where the
    // newly-created file became current.
    bool switchToNew = !r->hasParam("switch", true) ||
                        r->getParam("switch", true)->value() != "0";
    if (switchToNew) {
        SAFE_STRNCPY(config.datalog.currentFile, newFile.c_str(), sizeof(config.datalog.currentFile));
        saveConfig();
    }

    String body = String("{\"ok\":true,\"file\":\"") + newFile + "\"}";
    r->send(200, "application/json", body);
}

static void h_post_api_datalog_switch(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (!fsAvailable || !activeFS) {
        r->send(503, "application/json", "{\"ok\":false,\"error\":\"no fs\"}");
        return;
    }
    if (!r->hasParam("path", true)) {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"path required\"}");
        return;
    }
    String path = sanitizePath(r->getParam("path", true)->value());
    if (path.length() == 0) {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}");
        return;
    }
    if (!activeFS->exists(path)) {
        r->send(404, "application/json", "{\"ok\":false,\"error\":\"file not found\"}");
        return;
    }
    SAFE_STRNCPY(config.datalog.currentFile, path.c_str(), sizeof(config.datalog.currentFile));
    saveConfig();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_save_network(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (r->hasParam("wifiMode", true))       config.network.wifiMode = (WiFiModeType)r->getParam("wifiMode", true)->value().toInt();
    if (r->hasParam("apSSID", true))         SAFE_STRNCPY(config.network.apSSID,         r->getParam("apSSID", true)->value().c_str(), sizeof(config.network.apSSID));
    // R13 follow-up (Codex P1 on PR #89): /export_settings masks
    // passwords as "***". The SPA round-trips that value back here
    // on any unrelated save; without this guard the real password
    // would be overwritten with "***". Treat "***" as the
    // keep-existing sentinel.
    if (r->hasParam("apPassword", true)     && r->getParam("apPassword", true)->value()     != "***") SAFE_STRNCPY(config.network.apPassword,     r->getParam("apPassword", true)->value().c_str(), sizeof(config.network.apPassword));
    if (r->hasParam("clientSSID", true))     SAFE_STRNCPY(config.network.clientSSID,     r->getParam("clientSSID", true)->value().c_str(), sizeof(config.network.clientSSID));
    if (r->hasParam("clientPassword", true) && r->getParam("clientPassword", true)->value() != "***") SAFE_STRNCPY(config.network.clientPassword, r->getParam("clientPassword", true)->value().c_str(), sizeof(config.network.clientPassword));
    config.network.useStaticIP = r->hasParam("useStaticIP", true);

    auto parseIP = [&](const char* param, uint8_t* dst) {
        if (r->hasParam(param, true)) {
            uint8_t tmp[4];
            if (sscanf(r->getParam(param, true)->value().c_str(), "%hhu.%hhu.%hhu.%hhu", &tmp[0], &tmp[1], &tmp[2], &tmp[3]) == 4) {
                memcpy(dst, tmp, 4);
            }
        }
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
    shouldRestart = true;
    restartTimer  = millis();
}

static void h_post_save_time(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (r->hasParam("ntpServer", true)) SAFE_STRNCPY(config.network.ntpServer, r->getParam("ntpServer", true)->value().c_str(), sizeof(config.network.ntpServer));
    if (r->hasParam("timezone", true))  config.network.timezone = r->getParam("timezone", true)->value().toInt();
    saveConfig();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_set_time(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;   // was rate-limit only — add CSRF
    if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
        r->send(409, "application/json", "{\"ok\":false,\"error\":\"Busy\"}");
        return;
    }
    if (r->hasParam("date", true) && r->hasParam("time", true)) {
        String ds = r->getParam("date", true)->value();
        String ts = r->getParam("time", true)->value();
        int yr = ds.substring(0,4).toInt(), mo = ds.substring(5,7).toInt(), dy = ds.substring(8,10).toInt();
        int hr = ts.substring(0,2).toInt(), mi = ts.substring(3,5).toInt();

        // Always set the POSIX system clock so time(nullptr) works even
        // without hardware RTC.  Input is treated as UTC.  ESP32's newlib
        // does not expose timegm(); we get UTC-mktime by saving TZ,
        // forcing UTC0 around mktime(), then restoring.
        struct tm ti = {};
        ti.tm_year = yr - 1900; ti.tm_mon = mo - 1; ti.tm_mday = dy;
        ti.tm_hour = hr; ti.tm_min = mi; ti.tm_sec = 0;
        const char* prevTz = getenv("TZ");
        setenv("TZ", "UTC0", 1); tzset();
        time_t epoch = mktime(&ti);
        if (prevTz) setenv("TZ", prevTz, 1); else unsetenv("TZ");
        tzset();
        struct timeval tv = { epoch, 0 };
        settimeofday(&tv, nullptr);
        rtcValid = true;

        // Defer hardware RTC writes to loop() — three delay() calls sum to
        // ~120 ms and block the AsyncTCP worker for every concurrent request.
        // (AUDIT 3.17)
        if (Rtc) {
            g_pendingRtcTime = { (uint16_t)yr, (uint8_t)mo, (uint8_t)dy,
                                 (uint8_t)hr, (uint8_t)mi };
            g_pendingRtcSet.store(true, std::memory_order_release);
        }
        r->send(200, "application/json", "{\"ok\":true}");
    } else {
        r->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing date or time\"}");
    }
}

static void h_post_sync_time(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (g_pendingNtpSync != 0) {
        r->send(202, "application/json", "{\"ok\":true,\"pending\":true,\"running\":true}");
        return;
    }
    g_lastNtpSyncResult = 0;
    g_pendingNtpSync    = 1;
    r->send(202, "application/json", "{\"ok\":true,\"pending\":true}");
}

static void h_get_api_time_sync_status(AsyncWebServerRequest* r) {
    String j = "{\"pending\":";
    j += (g_pendingNtpSync != 0 ? "true" : "false");
    j += ",\"result\":";
    j += String((int)g_lastNtpSyncResult);  // 0=unknown, 1=ok, -1=fail
    j += "}";
    r->send(200, "application/json", j);
}

static void h_post_rtc_protect(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    if (Rtc) {
        bool protect = r->hasParam("protect", true);
        Rtc->SetIsWriteProtected(protect);
    }
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_flush_logs(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    flushLogBufferToFS();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_backup_bootcount(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    backupBootCount();
    r->send(200, "application/json", "{\"ok\":true}");
}

static void h_post_restore_bootcount(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    uint32_t old = bootCount;
    restoreBootCount();
    String j = "{\"ok\":true,\"old\":" + String(old) + ",\"new\":" + String(bootCount) + "}";
    r->send(200, "application/json", j);
}

static void h_post_factory_reset(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    r->send(200, "application/json", "{\"ok\":true}");
    DBGLN("[FACTORY RESET] Formatting LittleFS…");
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
        if (LittleFS.format()) {
            DBGLN("[FACTORY RESET] LittleFS formatted OK – restarting");
        } else {
            DBGLN("[FACTORY RESET] LittleFS format FAILED – restarting anyway");
        }
    }
    // Invalidate safe-mode magic so the next boot zeroes the counter
    // regardless of the ESP_RST_SW reset reason. /factory_reset is a
    // user-initiated wipe, NOT a crash.
    g_resetMagic = 0;
    g_pendingWiFiShutdown = true;
    shouldRestart = true;
    restartTimer  = millis();
}

static void h_post_restart(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    r->send(200, "application/json", "{\"ok\":true}");
    shouldRestart = true;
    restartTimer  = millis();
}

static void h_post_api_format_filesystem(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    Serial.println("[Format] /api/format_filesystem — wiping LittleFS");
    // R12 Gemini HIGH: acquire fsMutex around the long destructive op.
    // Without it, a concurrent StorageTask write or web-handler read
    // can race the format and corrupt the partition mid-erase.
    // 30 s timeout — format takes ~5-15 s on 4 MB LittleFS.
    bool ok = false;
    {
        MutexGuard g(fsMutex, pdMS_TO_TICKS(30000));
        if (fsMutex && !g.isLocked()) {
            r->send(503, "application/json",
                    "{\"ok\":false,\"error\":\"fs busy\"}");
            return;
        }
        ok = LittleFS.format();
    }
    if (!ok) {
        Serial.println("[Format] FAILED");
        r->send(500, "application/json",
                "{\"ok\":false,\"error\":\"format failed\"}");
        return;
    }
    Serial.println("[Format] OK — rebooting");
    r->send(200, "application/json",
            "{\"ok\":true,\"message\":\"formatted, rebooting\"}");
    shouldRestart = true;
    restartTimer  = millis();
}

static void h_get_download(AsyncWebServerRequest* r) {
    if (!r->hasParam("file")) { r->send(400, "text/plain", "No file"); return; }
    String path = sanitizePath(r->getParam("file")->value());
    if (path.isEmpty() || path == "/") { r->send(400, "text/plain", "Invalid path"); return; }
    if (isPathProtected(path) && !isPathDownloadAllowed(path)) {
        r->send(403, "application/json",
                "{\"ok\":false,\"error\":\"protected path\"}");
        return;
    }
    String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
    fs::FS* targetFS = (storage == "sdcard" && sdAvailable) ? sdFs() :
                       (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
    if (targetFS && targetFS->exists(path)) {
        String filename = path.substring(path.lastIndexOf('/') + 1);
        
        // Handle 0-byte files explicitly because ESPAsyncWebServer's beginResponse(FS)
        // returns nullptr for them, resulting in a spurious 404.
        File f = targetFS->open(path, "r");
        if (f && !f.isDirectory() && f.size() == 0) {
            f.close();
            AsyncWebServerResponse *resp = r->beginResponse(200, "application/octet-stream", "");
            if (!resp) {
                r->send(500, "application/json", "{\"ok\":false,\"error\":\"out_of_memory\"}");
                return;
            }
            resp->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            r->send(resp);
            return;
        }
        if (f) f.close();

        // Pass download=true so AsyncFileResponse emits its own single
        // "Content-Disposition: attachment; filename=..." header.  Previously
        // we let it default to download=false (which emits an "inline"
        // disposition) and then added our own "attachment" header on top —
        // producing TWO Content-Disposition headers, which Chromium/Edge
        // reject outright with ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_DISPOSITION
        // (every non-empty file failed to download).
        // Null-check resp: exists() → beginResponse() has a TOCTOU window;
        // the file may be deleted between the two calls.  (AUDIT 3.18)
        AsyncWebServerResponse *resp =
            r->beginResponse(*targetFS, path, "application/octet-stream", true);
        if (!resp) { r->send(404, "text/plain", "Not found"); return; }
        r->send(resp);
    } else {
        r->send(404, "text/plain", "Not found");
    }
}

static void h_post_mkdir(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    fs::FS* targetFS = getCurrentViewFS();
    if (!r->hasParam("name") || !targetFS) { r->send(400, "text/plain", "Missing name"); return; }
    String dirRaw  = r->hasParam("dir")     ? r->getParam("dir")->value()     : "/";
    String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
    if (storage == "sdcard" && sdAvailable) targetFS = sdFs();
    else targetFS = &LittleFS;
    String dir  = sanitizePath(dirRaw);
    String name = sanitizeFilename(r->getParam("name")->value());
    if (dir.isEmpty() || name.isEmpty()) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid name or dir\"}"); return; }
    String fp   = buildPath(dir, name);
    // 500 ms cap: report busy rather than freeze the AsyncTCP worker.
    if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        r->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
        return;
    }
    bool ok = targetFS->mkdir(fp);
    if (fsMutex) xSemaphoreGive(fsMutex);
    r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"mkdir failed\"}");
}

static void h_post_move_file(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
    String src     = r->hasParam("src")     ? sanitizePath(r->getParam("src")->value())     : "";
    String newName = r->hasParam("newName") ? sanitizeFilename(r->getParam("newName")->value()) : "";
    String destRaw = r->hasParam("destDir") ? r->getParam("destDir")->value() : "";
    if (src.isEmpty() || newName.isEmpty() || src == "/") { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid src or newName\"}"); return; }
    if (isPathProtected(src)) { r->send(403, "application/json", "{\"ok\":false,\"error\":\"Protected path\"}"); return; }
    fs::FS* targetFS = nullptr;
    if (storage == "sdcard" && sdAvailable)              targetFS = sdFs();
    else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
    if (!targetFS) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"No storage\"}"); return; }
    String dstDir;
    if (destRaw.isEmpty()) {
        dstDir = src.substring(0, src.lastIndexOf('/'));
        if (dstDir.isEmpty()) dstDir = "/";
    } else {
        dstDir = sanitizePath(destRaw);
        if (dstDir.isEmpty()) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid destDir\"}"); return; }
    }
    String dstPath = buildPath(dstDir, newName);
    if (isPathProtected(dstPath)) { r->send(403, "application/json", "{\"ok\":false,\"error\":\"Protected path\"}"); return; }
    // 500 ms cap: report busy rather than freeze the AsyncTCP worker.
    if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        r->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
        return;
    }
    bool ok = targetFS->rename(src, dstPath);
    if (fsMutex) xSemaphoreGive(fsMutex);
    r->send(200, "application/json", ok ? "{\"ok\":true,\"dst\":\"" + dstPath + "\"}" : "{\"ok\":false,\"error\":\"Rename failed\"}");
}

static void h_get_wifi_scan_start(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;
    // Only widen AP → AP_STA so the AP stays up; leave a client-only
    // device in STA (it can scan without broadcasting an AP).
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    r->send(200, "text/plain", "OK");
}

static void h_get_wifi_scan_result(AsyncWebServerRequest* r) {
    JsonDocument doc;
    JsonArray nets = doc["networks"].to<JsonArray>();
    // Read-only on purpose: this endpoint is polled with a plain GET and
    // is NOT behind requireMutatingAuth, so it must never start radio work.
    // The (CSRF-guarded) /wifi_scan_start owns starting/re-kicking scans;
    // the client re-triggers it if this reports an error.
    int n = WiFi.scanComplete();
    if      (n == WIFI_SCAN_RUNNING) { doc["scanning"] = true; }
    else if (n == WIFI_SCAN_FAILED)  { doc["error"] = "Scan failed"; }
    else if (n >= 0) {
        for (int i = 0; i < n && i < 20; i++) {
            JsonObject net = nets.add<JsonObject>();
            net["ssid"]   = WiFi.SSID(i);
            net["rssi"]   = WiFi.RSSI(i);
            net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
    }
    sendJsonResponse(r, doc);
}

static void h_get_api_platform_config(AsyncWebServerRequest* r) {
    if (!fsAvailable || !activeFS || !activeFS->exists("/platform_config.json")) {
        r->send(404, "application/json", "{\"ok\":false,\"error\":\"platform_config.json not found\"}");
        return;
    }
    r->send(*activeFS, "/platform_config.json", "application/json");
}

static void h_post_api_platform_reload(AsyncWebServerRequest* r) {
    if (!requireMutatingAuth(r)) return;   // was unprotected — reboots device
    // Signal to main loop / TaskManager to reload configs
    // Full reload requires restart; signal shouldRestart
    shouldRestart = true;
    restartTimer  = millis();
    r->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

// AsyncWebHandler::canHandle() is `const` in ESP32Async/ESPAsyncWebServer and
// non-const in esphome/ESPAsyncWebServer-esphome. One signature cannot satisfy
// both, and `override` turns the mismatch into a hard error rather than a
// silently-never-called method — which is the failure mode worth avoiding.
// ASYNCWEBSERVER_VERSION_MAJOR exists only in the ESP32Async line (it comes
// from its AsyncWebServerVersion.h), so it is the discriminator.
#ifdef ASYNCWEBSERVER_VERSION_MAJOR
#  define LOGGER_CANHANDLE_CV const
#else
#  define LOGGER_CANHANDLE_CV
#endif

#if WEB_BASIC_AUTH_ENABLED
// R22 / AUDIT 5.3: refuse to compile when Basic Auth is enabled with the
// shipped placeholder admin/admin credentials. Operators MUST override
// both WEB_BASIC_AUTH_USER and WEB_BASIC_AUTH_PASS at build time (via
// platformio.ini build_flags or a custom setup.h) before flashing an
// internet-exposed build.
//
// Uses string_view for a compile-time comparison (project is on gnu++17 —
// see R11 designated-initializer rewrite for the toolchain note). Lives
// inside the WEB_BASIC_AUTH_ENABLED block so other configurations that
// don't compile auth in still build cleanly with the placeholder defaults.
#include <string_view>
static_assert(
    std::string_view{WEB_BASIC_AUTH_USER} != "admin" ||
    std::string_view{WEB_BASIC_AUTH_PASS} != "admin",
    "Override the default Basic Auth credentials before enabling "
    "WEB_BASIC_AUTH_ENABLED. Set WEB_BASIC_AUTH_USER and "
    "WEB_BASIC_AUTH_PASS in setup.h or via -D build flags. "
    "Shipping admin/admin in production is a known critical (AUDIT 5.3).");

// Front-door handler: when Basic Auth is compiled in, this handler is
// registered FIRST, so it's matched before anything else. It only claims
// the request when the client is NOT authenticated, which makes us return
// 401 + WWW-Authenticate and discard the body. Authenticated requests fall
// through to the real handler chain.
class AsyncAuthGateHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* r) LOGGER_CANHANDLE_CV override {
        return !r->authenticate(WEB_BASIC_AUTH_USER, WEB_BASIC_AUTH_PASS);
    }
    void handleRequest(AsyncWebServerRequest* r) override {
        r->requestAuthentication();
    }
};
static AsyncAuthGateHandler s_authGate;
#endif

// R11 first-run gate. When g_setupRequired is true (no board profile
// selected), this handler is registered BEFORE the auth gate and claims
// every request whose URL is not in the wizard whitelist. It redirects
// to /firstrun so the user can pick a board + assign pins before the
// rest of the firmware is exposed.
//
// Whitelisted (allowed through while setup is required):
//   - /firstrun, /firstrun.html         (wizard page)
//   - /api/firstrun                     (POST handler that saves profile)
//   - /api/board-profiles               (GET profile list for the wizard)
//   - static asset extensions           (CSS, JS, fonts, favicon)
class FirstRunGateHandler : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest* r) LOGGER_CANHANDLE_CV override {
        if (!g_setupRequired) return false;
        const String& url = r->url();
        if (url == "/firstrun" || url == "/firstrun.html")        return false;
        if (url.startsWith("/api/firstrun"))                       return false;
        if (url.startsWith("/api/board-profiles"))                 return false;
        if (url.endsWith(".css")  || url.endsWith(".js"))          return false;
        if (url.endsWith(".woff2")|| url.endsWith(".ico"))         return false;
        if (url.endsWith(".png")  || url.endsWith(".svg"))         return false;
        return true;
    }
    void handleRequest(AsyncWebServerRequest* r) override {
        r->redirect("/firstrun");
    }
};
static FirstRunGateHandler s_firstRunGate;

void setupWebServer() {
    DBGLN("Setting up web server...");

    // R11: first-run gate runs before auth gate. The wizard must be
    // reachable on a fresh device even when Basic Auth is compiled in —
    // setting credentials is part of the wizard's job (a later phase).
    server.addHandler(&s_firstRunGate);
    registerFirstRunRoutes();
    if (g_setupRequired) DBGLN("Web server: first-run wizard required");

#if WEB_BASIC_AUTH_ENABLED
    // Must be registered FIRST — handlers are matched in insertion order and
    // the first canHandle()==true wins. Gating every request keeps secrets in
    // GET responses (export_settings, platform_config) behind the same wall
    // as the mutating endpoints.
    server.addHandler(&s_authGate);
    DBGLN("Web server: Basic Auth ENABLED");
#endif

    // C2: track web activity for idle power restore
    auto touchActivity = []() { g_lastWebActivity = millis(); };

    // Defense-in-depth headers applied to every response.  Pass 4 A4 removed
    // every inline on* handler and the inline theme-boot <script>, so
    // script-src no longer needs 'unsafe-inline' — any injected <script>
    // (stored XSS, rogue file upload) is now blocked by the browser.
    // style-src still keeps 'unsafe-inline' because many layout style="…"
    // attributes remain; tightening that is a separate pass.
    //
    // When the firmware is built with -DUI_CDN_BASE the CSP must permit the
    // CDN host in script-src / style-src / connect-src / img-src so the
    // bootstrap can pull assets and the SPA can call back to the device.
    // Path-restricted source (e.g. `https://example.com/v4.2.0/`) is honoured
    // by every modern browser and is tighter than a bare origin (codex P1
    // review on PR #54).
#ifdef UI_CDN_BASE
    DefaultHeaders::Instance().addHeader(
        "Content-Security-Policy",
        "default-src 'self'; "
        "script-src 'self' " UI_CDN_BASE "/; "
        "style-src 'self' 'unsafe-inline' " UI_CDN_BASE "/; "
        "img-src 'self' data: " UI_CDN_BASE "/; "
        "font-src 'self' " UI_CDN_BASE "/; "
        "connect-src 'self' " UI_CDN_BASE "/; "
        "frame-ancestors 'none'; "
        "base-uri 'self'"
    );
#else
    // cdn.jsdelivr.net is allowed in script-src / style-src so the uPlot CDN
    // fallback works when the library file is not present on LittleFS.
    // 'unsafe-inline' is already present for the failsafe PROGMEM page; adding
    // a CDN host does not weaken the existing posture further.
    DefaultHeaders::Instance().addHeader(
        "Content-Security-Policy",
        "default-src 'self'; "
        "script-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; "
        "style-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; "
        "img-src 'self' data:; "
        "connect-src 'self'; "
        "frame-ancestors 'none'; "
        "base-uri 'self'"
    );
#endif
    DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
    DefaultHeaders::Instance().addHeader("X-Frame-Options", "DENY");
    DefaultHeaders::Instance().addHeader("Referrer-Policy", "no-referrer");

    // Root handler decides per-request whether a real SPA shell is present;
    // this means uploading /www/index.html after boot starts serving the SPA
    // immediately, without the reboot that the old uiReady-gated registration
    // required (audit Pass 7 "serveStatic only registered in the uiReady
    // branch").  Registered BEFORE serveStatic so it wins route matching for
    // the exact `/` path.  Also honours a pre-gzipped index.html.gz sibling
    // (audit Pass 4 C1) — flash savings are worth a single extension probe.
    //
    // ── Pass 4 C4 — Optional CDN UI ─────────────────────────────────────────
    // When the firmware is built with -DUI_CDN_BASE="https://example.com/v4",
    // the root handler emits a tiny bootstrap that loads /style.css and
    // /js/*.js from the CDN instead of LittleFS.  Frees ~200 KB of LittleFS
    // for logs.  No build flag → behaviour unchanged.  Local-served pages
    // and the failsafe HTML are still always available as fallback.
#ifdef UI_CDN_BASE
    // Bootstrap HTML hoisted out of the request handler (gemini review
    // PR #54).  CSP-compatible (codex P1 on PR #54): no <base href> (would
    // violate base-uri 'self') and no inline <script> (would need
    // 'unsafe-inline' even with the CDN whitelisted in script-src).  The
    // boot logic lives in /cdn-boot.js, served from the device itself so
    // script-src 'self' covers it.  Stylesheet / theme-boot loaded by
    // absolute CDN URL — the relaxed CSP whitelists UI_CDN_BASE.


    server.on("/cdn-boot.js", HTTP_GET, h_get_cdn_boot_js);
#endif

    server.on("/", HTTP_GET, h_get_root);

    // Always register the static tree so asset fetches (js/css/images) work
    // the moment `/www/` is populated.  5-min cache: reuses JS/CSS across
    // page navigation but still picks up firmware-bundled UI changes within
    // a few minutes of a release.  AsyncStaticWebHandler already probes a
    // `.gz` sibling automatically and emits `Content-Encoding: gzip` — no
    // extra wiring needed for the regular asset tree.
    server.serveStatic("/", LittleFS, "/www/")
          .setDefaultFile("index.html")
          .setCacheControl("public, max-age=300, must-revalidate");

    if (LittleFS.exists("/www/index.html") || LittleFS.exists("/www/index.html.gz")) {
        DBGLN("Web UI: serving from /www/");
    } else {
        DBGLN("Web UI: FAILSAFE mode (upload /www/index.html to restore)");
    }

    server.on("/setup", HTTP_GET, h_get_setup);

    auto spaRedirect = [](AsyncWebServerRequest *r) { r->redirect("/"); };
    server.on("/dashboard",          HTTP_GET, spaRedirect);
    server.on("/files",              HTTP_GET, spaRedirect);
    server.on("/live",               HTTP_GET, spaRedirect);
    server.on("/settings",           HTTP_GET, spaRedirect);
    server.on("/settings_device",    HTTP_GET, spaRedirect);
    server.on("/settings_hardware",  HTTP_GET, spaRedirect);
    server.on("/settings_theme",     HTTP_GET, spaRedirect);
    server.on("/settings_time",      HTTP_GET, spaRedirect);
    server.on("/settings_network",   HTTP_GET, spaRedirect);
    server.on("/settings_datalog",   HTTP_GET, spaRedirect);

    // ── Captive-portal probe endpoints (Pass 5 5.5 phase 2) ────────────────
    // Phones, laptops, and game consoles all probe a hardcoded URL on join
    // to detect captive portals.  The DNS responder in WiFiManager already
    // points every hostname at us; redirecting these paths to "/" makes the
    // OS auto-pop its captive-portal banner so the user lands on the SPA
    // without typing any IP.  Reachable from any host header thanks to the
    // wildcard DNS, so we don't filter by Host.
    auto captiveRedirect = [](AsyncWebServerRequest *r) {
        // 302 to the SPA root with an ABSOLUTE URL pointing at the AP IP
        // (gemini review PR #48).  Some older Android NCSI implementations
        // and Windows captive-portal probes refuse to follow relative
        // redirects when the Host header doesn't match the expected probe
        // domain — an absolute URL sidesteps that entirely.
        //
        // Prefer `r->client()->localIP()` — it's the IP of the interface
        // that actually received the request, which (a) avoids the
        // transient 0.0.0.0 that softAPIP() can return right after softAP()
        // startup, and (b) handles WIFI_AP_STA correctly when the test
        // endpoint puts us in dual mode.  Fall back to softAPIP/localIP
        // when the underlying AsyncClient has already torn down — observed
        // crash with MEPC=0x42036bcc on a captive-portal probe arriving
        // milliseconds before the client struct was published.
        IPAddress ip;
        if (auto* c = r->client()) ip = c->localIP();
        if (ip == IPAddress((uint32_t)0)) ip = WiFi.softAPIP();
        if (ip == IPAddress((uint32_t)0)) ip = WiFi.localIP();
        String url = "http://" + ip.toString() + "/";
        r->redirect(url);
    };
    // Apple iOS / macOS
    server.on("/hotspot-detect.html",  HTTP_GET, captiveRedirect);
    server.on("/library/test/success.html", HTTP_GET, captiveRedirect);
    // Android — expects 204 NoContent normally; we return 302 so the OS
    // recognises a portal and prompts the user.
    server.on("/generate_204",         HTTP_GET, captiveRedirect);
    server.on("/gen_204",              HTTP_GET, captiveRedirect);
    // Windows
    server.on("/connecttest.txt",      HTTP_GET, captiveRedirect);
    server.on("/redirect",             HTTP_GET, captiveRedirect);
    server.on("/ncsi.txt",             HTTP_GET, captiveRedirect);
    // Mozilla / Firefox
    server.on("/canonical.html",       HTTP_GET, captiveRedirect);
    server.on("/success.txt",          HTTP_GET, captiveRedirect);

    // =========================================================================
    // API: STATUS
    // Keys consumed by web.js:
    //   applyStatus: device, deviceId, version, time, network, ip, boot, heap,
    //                heapTotal, chip, cpu, mode, theme{...}, freeSketch
    //   sdInit:      device/deviceName, deviceId, defaultStorageView,
    //                forceWebServer, version, boot, mode, heap, cpu, chip
    //   timeInit:    time, boot, rtcRunning, rtcProtected, wifi, ip
    //   netInit:     wifi, network, ip
    // =========================================================================
    // ── /api/status payload builders (shared by /api/identity, /api/runtime,
    //    /api/theme).  Each fills the supplied JsonObject in place so the
    //    same code path produces both the focused and combined responses.
    auto fillIdentity = [](JsonObject o) {
        o["device"]         = strlen(config.deviceName) ? config.deviceName : "Water Logger";
        o["deviceName"]     = o["device"];   // alias – sdInit uses both
        o["deviceId"]       = config.deviceId;
        o["version"]        = getVersionString();
        o["forceWebServer"] = config.forceWebServer;
        o["network"]        = getNetworkDisplay();
        o["ip"]             = wifiConnectedAsClient
                              ? WiFi.localIP().toString()
                              : WiFi.softAPIP().toString();
        o["gateway"]        = wifiConnectedAsClient ? WiFi.gatewayIP().toString() : "";
        o["subnet"]         = wifiConnectedAsClient ? WiFi.subnetMask().toString() : "";
        o["dns"]            = wifiConnectedAsClient ? WiFi.dnsIP().toString()      : "";
    };

    auto fillRuntime = [](JsonObject o) {
        o["time"]       = getRtcDateTimeString();
        o["rssi"]       = wifiConnectedAsClient ? WiFi.RSSI() : -100;
        o["boot"]       = bootCount;
        o["heap"]       = ESP.getFreeHeap();
        o["heapTotal"]  = ESP.getHeapSize();
        o["heapPct"]    = (int)(ESP.getFreeHeap() * 100UL / ESP.getHeapSize());
        o["chip"]       = ESP.getChipModel();
        o["cpu"]        = getCpuFrequencyMhz();
        o["mode"]       = getModeDisplay();
        o["wifi"]       = wifiConnectedAsClient ? "client" : "ap";
        o["freeSketch"] = ESP.getFreeSketchSpace();

        uint64_t used = 0, total = 0; int pct = 0;
        getStorageInfo(used, total, pct);
        char uBuf[24], tBuf[24];
        snprintf(uBuf, sizeof(uBuf), "%llu", (unsigned long long)used);
        snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)total);
        o["fsUsed"]             = serialized(String(uBuf));
        o["fsTotal"]            = serialized(String(tBuf));
        o["fsPct"]              = pct;
        o["defaultStorageView"] = config.hardware.defaultStorageView;
        o["currentFile"]        = getActiveDatalogFile();

        o["rtcPresent"] = (Rtc != nullptr);
        if (Rtc) {
            o["rtcProtected"] = Rtc->GetIsWriteProtected();
            o["rtcRunning"]   = Rtc->GetIsRunning();
            RtcDateTime rtNow = Rtc->GetDateTime();
            o["timeSource"]   = (rtNow.Year() >= 2020) ? "rtc" : "unknown";
        } else {
            o["rtcProtected"] = false;
            o["rtcRunning"]   = false;
            time_t sysT = time(nullptr);
            o["timeSource"]   = (sysT > 1000000000L) ? "ntp" : "unknown";
        }

        // Build-time capability flags — UI uses these to hide pages/cards
        // for features the firmware was built without.  Flowmeter is the
        // only optional sensor with its own settings page; the others
        // (BME280, SDS011, …) live in the unified sensor list.
        JsonObject caps = o["caps"].to<JsonObject>();
#if defined(SENSOR_WATERFLOW_ENABLED)
        caps["flowmeter"] = true;
#else
        caps["flowmeter"] = false;
#endif
        caps["platformMode"] = (int)g_platformMode;
    };

    auto fillTheme = [](JsonObject o) {
        o["mode"]              = (int)config.theme.mode;
        o["primaryColor"]      = config.theme.primaryColor;
        o["secondaryColor"]    = config.theme.secondaryColor;
        o["lightBgColor"]      = config.theme.lightBgColor;
        o["lightTextColor"]    = config.theme.lightTextColor;
        o["darkBgColor"]       = config.theme.darkBgColor;
        o["darkTextColor"]     = config.theme.darkTextColor;
        o["ffColor"]           = config.theme.ffColor;
        o["pfColor"]           = config.theme.pfColor;
        o["otherColor"]        = config.theme.otherColor;
        o["storageBarColor"]   = config.theme.storageBarColor;
        o["storageBar70Color"] = config.theme.storageBar70Color;
        o["storageBar90Color"] = config.theme.storageBar90Color;
        o["storageBarBorder"]  = config.theme.storageBarBorder;
        o["logoSource"]        = config.theme.logoSource;
        o["faviconPath"]       = config.theme.faviconPath;
        o["boardDiagramPath"]  = config.theme.boardDiagramPath;
        o["chartSource"]       = (int)config.theme.chartSource;
        o["chartLocalPath"]    = strlen(config.theme.chartLocalPath) ? config.theme.chartLocalPath : "/uPlot.iife.min.js";
        o["chartLabelFormat"]  = (int)config.theme.chartLabelFormat;
        o["showIcons"]         = config.theme.showIcons;
    };

    server.on("/api/status", HTTP_GET, [fillIdentity, fillRuntime, fillTheme](AsyncWebServerRequest *r) {
        g_lastWebActivity = millis();   // C2: any poll = active user
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        fillIdentity(root);
        fillRuntime(root);
        fillTheme(doc["theme"].to<JsonObject>());
        sendJsonResponse(r, doc);
    });

    server.on("/api/identity", HTTP_GET, [fillIdentity](AsyncWebServerRequest *r) {
        JsonDocument doc;
        fillIdentity(doc.to<JsonObject>());
        sendJsonResponse(r, doc);
    });

    server.on("/api/runtime", HTTP_GET, [fillRuntime](AsyncWebServerRequest *r) {
        g_lastWebActivity = millis();   // runtime polls count as activity too
        JsonDocument doc;
        fillRuntime(doc.to<JsonObject>());
        sendJsonResponse(r, doc);
    });

    // Pass 7 — per-boot CSRF token for the SPA to inject into mutating
    // calls.  Generated on first call from esp_random(); same token
    // returned for the lifetime of the firmware run.
    server.on("/api/csrf-token", HTTP_GET, h_get_api_csrf_token);

    server.on("/api/theme", HTTP_GET, [fillTheme](AsyncWebServerRequest *r) {
        JsonDocument doc;
        fillTheme(doc.to<JsonObject>());
        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: LIVE  (legacy poll endpoint + SSE channel /api/events)
    // -------------------------------------------------------------------------
    // Modern clients open `new EventSource('/api/events')` and receive 'live'
    // events at ~1 Hz, driven from loop() via publishLiveEvent(). Older
    // clients (or fallback when EventSource fails) keep polling /api/live.
    // Both paths build the same JSON snapshot via buildLiveSnapshot().
    // =========================================================================
    server.on("/api/live", HTTP_GET, h_get_api_live);

    // SSE channel — same payload, pushed at 1 Hz by publishLiveEvent().
    server.addHandler(&liveEvents);

    // =========================================================================
    // API: RECENT LOGS
    // =========================================================================
    // Supports `?since=<bootcount>` (audit Pass 4 D2).  When present, entries
    // whose "#:<n>" token is <= the supplied value are suppressed.  The
    // response always echoes the current bootCount so the client can advance
    // its cursor; if the logfile has no bootCount tokens (user disabled the
    // column, or legacy file) the filter degrades to a no-op.
    server.on("/api/recent_logs", HTTP_GET, h_get_api_recent_logs);

    // =========================================================================
    // API: FILE LIST
    // =========================================================================
    server.on("/api/filelist", HTTP_GET, h_get_api_filelist);

    // =========================================================================
    // API: CHANGELOG
    // =========================================================================
    server.on("/api/changelog", HTTP_GET, h_get_api_changelog);

    // =========================================================================
    // API: PREVIEW NEXT DEVICE ID
    // Returns a fresh MAC-derived ID.  This endpoint NEVER persists; the user
    // must click Save in the Device settings page to commit, so the shown ID
    // always matches the stored one (audit Pass 7 clarification).  The legacy
    // /api/regen-id alias is kept for one release for backwards compat.
    // =========================================================================
    auto nextIdHandler = [](AsyncWebServerRequest *r) {
        if (!requireMutatingAuth(r)) return;
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        String newId = mac.substring(mac.length() - 8);
        newId.toUpperCase();
        r->send(200, "text/plain", newId);
    };
    server.on("/api/next-id",  HTTP_POST, nextIdHandler);
    server.on("/api/regen-id", HTTP_POST, nextIdHandler);  // legacy alias

    // =========================================================================
    // EXPORT SETTINGS
    // Keys consumed by web.js:
    //   hwInit:  hardware{storageType, pinSdCS, pinSdMOSI, pinSdMISO, pinSdSCK,
    //                      wakeupMode, debounceMs, pinWifiTrigger, pinWakeupFF,
    //                      pinWakeupPF, pinFlowSensor, pinRtcCE, pinRtcIO, pinRtcSCLK, cpuFreqMHz}
    //            (also flowMeter.testMode / blinkDuration after PR #105 follow-up)
    //   thInit:  theme{mode, showIcons, primaryColor, secondaryColor, bgColor, textColor,
    //                   ffColor, pfColor, otherColor, storageBarColor, storageBar70Color,
    //                   storageBar90Color, storageBarBorder, logoSource, faviconPath,
    //                   boardDiagramPath, chartSource, chartLocalPath, chartLabelFormat}
    //   netInit: network{wifiMode, apSSID, apPassword, apIP, apGateway, apSubnet,
    //                     clientSSID, clientPassword, useStaticIP, staticIP,
    //                     gateway, subnet, dns}
    //   timeInit: network{ntpServer, timezone}
    //   dlInit:  datalog{prefix, folder, rotation, maxSizeKB, timestampFilename,
    //                     includeDeviceId, dateFormat, timeFormat, endFormat,
    //                     includeBootCount, volumeFormat, includeExtraPresses,
    //                     postCorrectionEnabled, pfToFfThreshold, ffToPfThreshold,
    //                     manualPressThresholdMs}
    // =========================================================================
    server.on("/export_settings", HTTP_GET, h_get_export_settings);

    // =========================================================================
    // SAVE ENDPOINTS
    // =========================================================================

    server.on("/save_device", HTTP_POST, h_post_save_device);

    // PR #105 follow-up: /save_flowmeter endpoint retired together with the
    // standalone settings_flowmeter page. pulsesPerLiter / calibrationMultiplier
    // now live on the YF-S201 sensor card (POST /api/config/platform);
    // testMode + blinkDuration moved to /save_hardware; resetBootCount moved
    // to /save_device (System Info card).

server.on("/save_hardware", HTTP_POST, h_post_save_hardware);

    server.on("/save_theme", HTTP_POST, h_post_save_theme);

    server.on("/save_datalog", HTTP_POST, h_post_save_datalog);

    // POST /save_sensorlog — wide-CSV pipeline knobs (config.logger.*).
    // Backs the "Sensor CSV logging" card on the Sensors page; split out of
    // /save_datalog so the flow-meter event log and the sensor-CSV pipeline
    // each have their own form + endpoint.
    server.on("/save_sensorlog", HTTP_POST, h_post_save_sensorlog);

    // POST /api/datalog/create  body: prefix + folder + flags + (optional) action=switch
    // Creates a new log file from prefix/folder/timestamp+deviceId flags
    // without saving any other datalog settings. If action=switch, also sets
    // it as the active file. Decoupled from /save_datalog so users editing
    // format/rotation fields can't accidentally create a file on submit.
    server.on("/api/datalog/create", HTTP_POST, h_post_api_datalog_create);

    // POST /api/datalog/switch  body: path
    // Sets config.datalog.currentFile to an EXISTING log file. Pure
    // active-file pointer change; doesn't touch any other datalog field.
    server.on("/api/datalog/switch", HTTP_POST, h_post_api_datalog_switch);

    server.on("/save_network", HTTP_POST, h_post_save_network);

    server.on("/save_time", HTTP_POST, h_post_save_time);

    // =========================================================================
    // TIME MANAGEMENT
    // =========================================================================
    server.on("/set_time", HTTP_POST, h_post_set_time);

    // NTP sync can block up to ~10 seconds inside syncTimeFromNTP() (20 × 500 ms
    // retry loop). Doing that on the AsyncTCP worker stalls every other HTTP
    // connection. Instead we set g_pendingNtpSync and the main loop picks it
    // up on its next iteration. Clients poll /api/time_sync_status.
    server.on("/sync_time", HTTP_POST, h_post_sync_time);

    server.on("/api/time_sync_status", HTTP_GET, h_get_api_time_sync_status);

    server.on("/rtc_protect", HTTP_POST, h_post_rtc_protect);

    server.on("/flush_logs", HTTP_POST, h_post_flush_logs);

    server.on("/backup_bootcount", HTTP_POST, h_post_backup_bootcount);

    server.on("/restore_bootcount", HTTP_POST, h_post_restore_bootcount);

    // =========================================================================
    // FACTORY RESET  – formats LittleFS, erases config, restarts
    // =========================================================================
    server.on("/factory_reset", HTTP_POST, h_post_factory_reset);

    // =========================================================================
    // RESTART
    // =========================================================================
    server.on("/restart", HTTP_POST, h_post_restart);

    // R12 / AUDIT 1.7: the only path that ever formats LittleFS.
    // Replaces the implicit formatOnFail=true behaviour with an explicit,
    // authenticated, single-purpose endpoint. Used by the failsafe UI's
    // "Format Filesystem" button when the user has decided the partition
    // is unrecoverable. Schedules a reboot after the format so the freshly
    // mounted FS is picked up by setup() on the next boot.
    server.on("/api/format_filesystem", HTTP_POST, h_post_api_format_filesystem);

    // =========================================================================
    // FILE OPERATIONS
    // =========================================================================

    server.on("/download", HTTP_GET, h_get_download);

    // Accept both GET (legacy web.js compat) and POST (preferred)
    auto deleteHandler = [](AsyncWebServerRequest *r) {
        if (!requireMutatingAuth(r)) return;
        if (!r->hasParam("path") && !r->hasParam("path", true)) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing path\"}"); return; }
        // Prefer POST param; fall back to query — sanitizePath rejects "..",
        // control chars, backslash, and NUL, returning "" on any violation.
        String raw = r->hasParam("path", true)
                     ? r->getParam("path", true)->value()
                     : r->getParam("path")->value();
        String path = sanitizePath(raw);
        if (path.isEmpty() || path == "/") { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid path\"}"); return; }
        if (isPathProtected(path))          { r->send(403, "application/json", "{\"ok\":false,\"error\":\"Protected path\"}"); return; }
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)              targetFS = sdFs();
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        else if (activeFS) targetFS = activeFS;
        bool deleted = false;
        if (targetFS && targetFS->exists(path)) {
            File f = targetFS->open(path, FILE_READ);
            bool isDir = f && f.isDirectory();
            if (f) f.close();
            // 500 ms cap: if a storage task is holding fsMutex we'd rather
            // return busy to the client than freeze the AsyncTCP worker.
            if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
                r->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
                return;
            }
            deleted = isDir ? deleteRecursive(*targetFS, path) : targetFS->remove(path);
            if (fsMutex) xSemaphoreGive(fsMutex);
        }
        r->send(200, "application/json", deleted ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Delete failed\"}");
    };
    server.on("/delete", HTTP_POST, deleteHandler);

    server.on("/mkdir", HTTP_POST, h_post_mkdir);

    server.on("/move_file", HTTP_POST, h_post_move_file);

    // Upload handler: per-request state in _tempObject tracks file handle
    // AND mutex-held flag so that client abort (onDisconnect) can release both.
    struct UploadCtx { File file; bool mutexHeld; bool failed; bool authFailed; };
    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            // Auth was checked in onUpload at index==0 (onUpload fires before
            // onRequest in ESPAsyncWebServer). Only clean up + send response here.
            UploadCtx* ctx = (UploadCtx*)r->_tempObject;
            if (ctx) {
                if (ctx->file) ctx->file.close();
                bool failed    = ctx->failed;
                bool authFailed = ctx->authFailed;
                delete ctx;
                r->_tempObject = nullptr;
                if (authFailed) return;  // 403 already sent in onUpload
                if (failed) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Upload failed\"}"); return; }
            } else {
                // No context means onUpload either could not allocate UploadCtx
                // (OOM) or never ran (no multipart file part).  Report the
                // failure instead of falsely returning 200 OK.
                r->send(500, "application/json",
                        "{\"ok\":false,\"error\":\"Upload context unavailable\"}");
                return;
            }
            r->send(200, "application/json", "{\"ok\":true}");
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                // Clean up leftover state from a previous aborted upload
                UploadCtx* old = (UploadCtx*)request->_tempObject;
                if (old) {
                    if (old->file) old->file.close();
                    delete old;
                    request->_tempObject = nullptr;
                }

                // ML-1: allocate the per-request context ONCE and register the
                // disconnect cleaner IMMEDIATELY — before any validation can
                // early-return.  Previously each validation-failure path
                // allocated its own UploadCtx and returned without registering
                // onDisconnect, so a client abort during validation leaked the
                // ctx (the framework reclaims _tempObject via free(), which
                // skips ~UploadCtx and never closes the File).
                auto* ctx = new (std::nothrow) UploadCtx{File(), false, false, false};
                request->_tempObject = ctx;
                if (!ctx) { DBGLN("Upload: ctx alloc failed"); return; }
                request->onDisconnect([request]() {
                    UploadCtx* c = (UploadCtx*)request->_tempObject;
                    if (!c) return;
                    if (c->file) c->file.close();
                    delete c;
                    request->_tempObject = nullptr;
                });

                // Auth check here — onUpload fires before onRequest in
                // ESPAsyncWebServer, so checking in onRequest is too late to
                // prevent unauthorized file writes.
                if (!requireMutatingAuth(request)) {
                    ctx->authFailed = true;
                    return;
                }

                String upDirRaw = request->hasParam("path")
                                  ? request->getParam("path")->value()
                                  : String("/www/");
                String upDir = sanitizePath(upDirRaw);
                if (upDir.isEmpty()) {
                    DBGF("Upload: invalid path '%s'\n", upDirRaw.c_str());
                    ctx->failed = true;
                    return;
                }

                String safeName = sanitizeFilename(filename);
                if (safeName.isEmpty()) {
                    DBGF("Upload: invalid filename '%s'\n", filename.c_str());
                    ctx->failed = true;
                    return;
                }

                String upStorage = request->hasParam("storage")
                                   ? request->getParam("storage")->value()
                                   : String("internal");

                bool wantSD = (upStorage == "sdcard");
                fs::FS* targetFS = (wantSD && sdAvailable)
                                   ? sdFs()
                                   : (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
                if (!targetFS) {
                    DBGLN("Upload: no filesystem available");
                    ctx->failed = true;
                    return;
                }

                String upPath = buildPath(upDir, safeName);
                if (!wantSD && isPathProtected(upPath)) {
                    DBGF("Upload: refusing protected path %s\n", upPath.c_str());
                    ctx->failed = true;
                    return;
                }

                // Disk-full guard for internal storage. Use content-length when
                // supplied; otherwise require at least 32 KB headroom.
                if (targetFS == (fs::FS*)&LittleFS) {
                    size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
                    size_t need = request->contentLength() ? request->contentLength() : 32768;
                    if (free < need) {
                        DBGF("Upload: disk full (free=%u need=%u)\n",
                                      (unsigned)free, (unsigned)need);
                        ctx->failed = true;
                        return;
                    }
                }

                DBGF("Upload start [%s]: %s\n", upStorage.c_str(), upPath.c_str());

                {
                    MutexGuard g(fsMutex, pdMS_TO_TICKS(5000));
                    if (upDir != "/") targetFS->mkdir(upDir);
                    ctx->file = targetFS->open(upPath, FILE_WRITE);
                }
                if (!ctx->file) {
                    DBGF("Upload: cannot open %s for write\n", upPath.c_str());
                    ctx->failed = true;
                }
            }

            UploadCtx* ctx = (UploadCtx*)request->_tempObject;
            if (ctx && ctx->file && !ctx->failed && len) {
                MutexGuard g(fsMutex, pdMS_TO_TICKS(2000));
                if (ctx->file.write(data, len) != len) {
                    DBGLN("Upload: short write (disk full?)");
                    ctx->failed = true;
                }
            }

            if (final && ctx) {
                if (ctx->file) {
                    DBGF("Upload done: %s (%u bytes)\n",
                                  filename.c_str(), (unsigned)(index + len));
                    ctx->file.close();
                }
            }
        }
    );

    // =========================================================================
    // IMPORT SETTINGS
    // =========================================================================
    server.on("/import_settings", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            if (!requireMutatingAuth(r)) return;
            // (void*)1 is the OOM sentinel set by the body callback when the
            // accumulation buffer could not be allocated.  Report the 500 here
            // (exactly once); the body callback deliberately does NOT send, so
            // we avoid a double response that would corrupt the AsyncTCP
            // connection.
            if (r->_tempObject == reinterpret_cast<void*>(1)) {
                r->_tempObject = nullptr;
                r->send(500, "application/json", "{\"ok\":false,\"error\":\"out_of_memory\"}");
                return;
            }
            String* buf = static_cast<String*>(r->_tempObject);
            if (!buf || buf->isEmpty()) { r->send(400, "text/plain", "No data"); return; }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *buf);
            delete buf;
            r->_tempObject = nullptr;
            if (err) { r->send(400, "text/plain", String("JSON error: ") + err.c_str()); return; }

            if (doc["deviceName"].is<const char*>()) SAFE_STRNCPY(config.deviceName, doc["deviceName"], sizeof(config.deviceName));
            if (doc["forceWebServer"].is<bool>()) config.forceWebServer = doc["forceWebServer"];

            if (doc["theme"].is<JsonObject>()) {
                JsonObject t = doc["theme"];
                if (t["mode"].is<int>()) config.theme.mode = (ThemeMode)(int)t["mode"];
                auto cpColor = [&](const char* k, char* dst, size_t sz){ if(t[k].is<const char*>()) SAFE_STRNCPY(dst, t[k], sz); };
                cpColor("primaryColor",   config.theme.primaryColor,   sizeof(config.theme.primaryColor));
                cpColor("secondaryColor", config.theme.secondaryColor, sizeof(config.theme.secondaryColor));
                cpColor("lightBgColor",   config.theme.lightBgColor,   sizeof(config.theme.lightBgColor));
                cpColor("lightTextColor", config.theme.lightTextColor, sizeof(config.theme.lightTextColor));
                cpColor("darkBgColor",    config.theme.darkBgColor,    sizeof(config.theme.darkBgColor));
                cpColor("darkTextColor",  config.theme.darkTextColor,  sizeof(config.theme.darkTextColor));
                cpColor("ffColor",        config.theme.ffColor,        sizeof(config.theme.ffColor));
                cpColor("pfColor",        config.theme.pfColor,        sizeof(config.theme.pfColor));
                cpColor("otherColor",     config.theme.otherColor,     sizeof(config.theme.otherColor));
                if (t["showIcons"].is<bool>())        config.theme.showIcons        = t["showIcons"];
                if (t["chartSource"].is<int>())       config.theme.chartSource       = (ChartSource)(int)t["chartSource"];
                if (t["chartLabelFormat"].is<int>())  config.theme.chartLabelFormat  = (ChartLabelFormat)(int)t["chartLabelFormat"];
            }
            if (doc["flowMeter"].is<JsonObject>()) {
                JsonObject fm = doc["flowMeter"];
                if (fm["pulsesPerLiter"].is<float>())               config.flowMeter.pulsesPerLiter               = fm["pulsesPerLiter"];
                if (fm["calibrationMultiplier"].is<float>())        config.flowMeter.calibrationMultiplier        = fm["calibrationMultiplier"];
            }
            if (doc["datalog"].is<JsonObject>()) {
                JsonObject dl = doc["datalog"];
                if (dl["rotation"].is<int>())               config.datalog.rotation               = (DatalogRotation)(int)dl["rotation"];
                if (dl["maxSizeKB"].is<int>())              config.datalog.maxSizeKB              = constrain(dl["maxSizeKB"].as<int>(), 10, 10000);
                if (dl["maxEntries"].is<int>())             config.datalog.maxEntries             = constrain(dl["maxEntries"].as<int>(), 10, 65535);
                if (dl["dateFormat"].is<int>())             config.datalog.dateFormat             = dl["dateFormat"];
                if (dl["timeFormat"].is<int>())             config.datalog.timeFormat             = dl["timeFormat"];
                if (dl["endFormat"].is<int>())              config.datalog.endFormat              = dl["endFormat"];
                if (dl["volumeFormat"].is<int>())           config.datalog.volumeFormat           = dl["volumeFormat"];
                if (dl["includeBootCount"].is<bool>())      config.datalog.includeBootCount       = dl["includeBootCount"];
                if (dl["includeExtraPresses"].is<bool>())   config.datalog.includeExtraPresses    = dl["includeExtraPresses"];
                if (dl["postCorrectionEnabled"].is<bool>()) config.datalog.postCorrectionEnabled  = dl["postCorrectionEnabled"];
                if (dl["pfToFfThreshold"].is<float>())      config.datalog.pfToFfThreshold        = max(0.1f, dl["pfToFfThreshold"].as<float>());
                if (dl["ffToPfThreshold"].is<float>())      config.datalog.ffToPfThreshold        = max(0.1f, dl["ffToPfThreshold"].as<float>());
                if (dl["manualPressThresholdMs"].is<int>()) config.datalog.manualPressThresholdMs = dl["manualPressThresholdMs"];
            }
            if (doc["network"].is<JsonObject>()) {
                JsonObject net = doc["network"];
                if (net["wifiMode"].is<int>())         config.network.wifiMode   = (WiFiModeType)(int)net["wifiMode"];
                if (net["ntpServer"].is<const char*>()) SAFE_STRNCPY(config.network.ntpServer, net["ntpServer"], sizeof(config.network.ntpServer));
                if (net["timezone"].is<int>())         config.network.timezone   = net["timezone"];
                if (net["useStaticIP"].is<bool>())     config.network.useStaticIP= net["useStaticIP"];
            }
            if (doc["hardware"].is<JsonObject>()) {
                JsonObject hw = doc["hardware"];
                if (hw["storageType"].is<int>())        config.hardware.storageType        = (StorageType)(int)hw["storageType"];
                if (hw["wakeupMode"].is<int>())         config.hardware.wakeupMode         = (WakeupMode)(int)hw["wakeupMode"];
                if (hw["cpuFreqMHz"].is<int>())         config.hardware.cpuFreqMHz         = hw["cpuFreqMHz"];
                if (hw["defaultStorageView"].is<int>()) config.hardware.defaultStorageView = hw["defaultStorageView"];
                if (hw["debounceMs"].is<int>())         config.hardware.debounceMs         = hw["debounceMs"];
                if (hw["debugMode"].is<bool>())         config.hardware.debugMode          = hw["debugMode"].as<bool>();
            }
            if (doc["logger"].is<JsonObject>()) {
                JsonObject lg = doc["logger"];
                if (lg["csvLoggingEnabled"].is<bool>())         config.logger.csvLoggingEnabled         = lg["csvLoggingEnabled"];
                if (lg["aggregationIntervalSec"].is<int>())     config.logger.aggregationIntervalSec    = constrain(lg["aggregationIntervalSec"].as<int>(), 5, 3600);
            }
            if (doc["kindle"].is<JsonObject>()) {
                JsonObject kd = doc["kindle"];
                if (kd["face"].is<int>())          config.kindle.face         = (uint8_t)kd["face"].as<int>();
                if (kd["faceCustom"].is<const char*>())
                    SAFE_STRNCPY(config.kindle.faceCustom, kd["faceCustom"], sizeof(config.kindle.faceCustom));
                if (kd["boldZones"].is<int>())     config.kindle.boldZones    = (uint16_t)kd["boldZones"].as<int>();
                if (kd["showFlags"].is<int>())     config.kindle.showFlags    = (uint16_t)kd["showFlags"].as<int>();
                if (kd["clockStyle"].is<int>())    config.kindle.clockStyle   = (uint8_t)kd["clockStyle"].as<int>();
                if (kd["timeFormat"].is<int>())    config.kindle.timeFormat   = (uint8_t)kd["timeFormat"].as<int>();
                if (kd["dateFormat"].is<int>())    config.kindle.dateFormat   = (uint8_t)kd["dateFormat"].as<int>();
                if (kd["pressureUnit"].is<int>())  config.kindle.pressureUnit = (uint8_t)kd["pressureUnit"].as<int>();
                if (kd["tempDecimals"].is<int>())  config.kindle.tempDecimals = (uint8_t)kd["tempDecimals"].as<int>();
                // An imported file is not a form: it can carry anything,
                // including values written by a firmware that had one more
                // clock style than this one. Clamped here so the renderer
                // never has to consider that it might not have been.
                kdSkinClamp(config.kindle);
            }
            saveConfig();
            r->send(200, "text/plain", "OK");
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            // Hard cap matches the accept-limit below so a malicious
            // Content-Length cannot trigger a huge heap allocation.
            constexpr size_t kImportMax = 8192;
            if (!index) {
                String* buf = new (std::nothrow) String();
                if (!buf) {
                    // Flag OOM with a sentinel instead of sending here: the
                    // request-completion handler emits the single 500.  Sending
                    // from both callbacks would double-respond and corrupt the
                    // AsyncTCP connection.
                    req->_tempObject = reinterpret_cast<void*>(1);
                    return;
                }
                // ML-2: publish the pointer and register the disconnect cleaner
                // IMMEDIATELY after allocation — before reserve() — so there is
                // no window in which an abort could leak the buffer.
                // R13 follow-up (Codex P2 on PR #89): client disconnect before
                // the request callback fires would otherwise leak the heap
                // buffer. onDisconnect runs even on aborts; freeing here makes
                // the success path's delete a no-op (delete on nullptr is
                // well-defined).
                req->_tempObject = buf;
                req->onDisconnect([req]() {
                    // Never delete the OOM sentinel (it is not a real pointer).
                    if (req->_tempObject != reinterpret_cast<void*>(1))
                        delete static_cast<String*>(req->_tempObject);
                    req->_tempObject = nullptr;
                });
                size_t hint = req->contentLength() > 0 ? req->contentLength() : 4096;
                if (hint > kImportMax) hint = kImportMax;
                buf->reserve(hint);
            }
            if (req->_tempObject == reinterpret_cast<void*>(1)) return;  // OOM already flagged
            String* buf = static_cast<String*>(req->_tempObject);
            if (!buf) return;
            if (buf->length() + len > kImportMax) return; // Hard cap
            buf->concat((const char*)data, len);
        }
    );

    // =========================================================================
    // WIFI SCAN
    // =========================================================================
    server.on("/wifi_scan_start", HTTP_GET, h_get_wifi_scan_start);

    server.on("/wifi_scan_result", HTTP_GET, h_get_wifi_scan_result);

    // =========================================================================
    // OTA FIRMWARE UPDATE
    //
    // POST /do_update[?sha256=<64-hex>]
    //   Streams the firmware image into the OTA partition.  When the
    //   `sha256` query param is supplied, the server hashes every chunk
    //   with mbedTLS and compares the digest before committing — any
    //   mismatch aborts the update and returns 400.  When the param is
    //   absent, behaviour is unchanged (Pass 5 5.6 first slice).
    //
    //   Rejection paths:
    //     • magic byte != 0xE9  → invalid image
    //     • Update.begin failed → flash partition unavailable
    //     • SHA-256 mismatch    → tampered/corrupted image
    // =========================================================================
    {
        // OTA upload state — per-request via _tempObject so concurrent
        // requests can't corrupt each other's hash context (gemini review
        // PR #49).  The destructor releases the mbedTLS hardware-SHA lock,
        // so any early exit (magic-byte fail, Update.begin fail, mismatch)
        // OR a client disconnect reclaims the engine cleanly via
        // request->onDisconnect.
        struct OtaCtx {
            bool rejected     = false;
            bool shaMismatch  = false;
            bool shaActive    = false;
            bool authFailed   = false;
            String expectedSha;
            mbedtls_sha256_context sha;
            ~OtaCtx() {
                if (shaActive) {
                    mbedtls_sha256_free(&sha);
                    shaActive = false;
                }
            }
        };

        server.on("/do_update", HTTP_POST,
            [](AsyncWebServerRequest *r) {
                // Auth was checked in onUpload at index==0 (onUpload fires before
                // onRequest in ESPAsyncWebServer). Only send the result here.
                OtaCtx* ctx = static_cast<OtaCtx*>(r->_tempObject);
                if (ctx && ctx->authFailed) {
                    delete ctx;
                    r->_tempObject = nullptr;
                    return;  // 403 already sent in onUpload
                }
                bool rejected    = ctx ? ctx->rejected    : true;
                bool shaMismatch = ctx ? ctx->shaMismatch : false;

                bool ok = !rejected && !Update.hasError();
                const char* msg;
                if (shaMismatch) {
                    msg = "{\"success\":false,\"message\":\"SHA-256 mismatch — image rejected\"}";
                } else if (rejected) {
                    msg = "{\"success\":false,\"message\":\"Invalid firmware image\"}";
                } else if (ok) {
                    msg = "{\"success\":true,\"message\":\"Update complete, restarting...\"}";
                } else {
                    msg = "{\"success\":false,\"message\":\"Update failed\"}";
                }
                AsyncWebServerResponse *resp = r->beginResponse(ok ? 200 : 400,
                    "application/json", msg);
                resp->addHeader("Connection", "close");
                r->send(resp);

                // Free per-request state.  Context destructor releases the
                // mbedTLS SHA engine if the upload aborted before final.
                if (ctx) {
                    delete ctx;
                    r->_tempObject = nullptr;
                }
                if (ok) {
                    shouldRestart = true;
                    restartTimer = millis();
                }
            },
            [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
                if (!index) {
                    auto* ctx = new (std::nothrow) OtaCtx();
                    if (!ctx) {
                        DBGLN("OTA: ctx alloc failed");
                        return;
                    }
                    req->_tempObject = ctx;
                    // Reclaim ctx + SHA engine if the client drops the
                    // connection before onRequest fires.
                    req->onDisconnect([req]() {
                        OtaCtx* leftover = static_cast<OtaCtx*>(req->_tempObject);
                        if (leftover) {
                            delete leftover;
                            req->_tempObject = nullptr;
                        }
                    });

                    DBGF("OTA start: %s\n", filename.c_str());

                    // Auth check here — onUpload fires before onRequest in
                    // ESPAsyncWebServer, so checking in onRequest is too late
                    // to prevent unauthorized flash writes.
                    if (!requireMutatingAuth(req)) {
                        ctx->authFailed = true;
                        return;
                    }

                    // R20 / AUDIT 13.5: OtaModule's enabled flag now actually
                    // gates the upload path. Toggling /api/modules/ota/enable
                    // off refuses subsequent /do_update requests with 503.
                    // Closes the "enable switch with no semantic effect" bug.
                    //
                    // Response shape matches the existing /do_update contract
                    // (success + message — see line 2012 and the doOta()
                    // frontend at line 198). Using {ok, error} would render
                    // as "undefined" in the upload UI (Codex P2 on PR #97).
                    if (!OtaModule::instance().isEnabled()) {
                        ctx->authFailed = true;   // reuses the rejection path
                        req->send(503, "application/json",
                                  "{\"success\":false,\"message\":"
                                  "\"OTA disabled in /api/modules/ota\"}");
                        return;
                    }

                    // Expected hash arrives as a query param (header-free
                    // for client simplicity).  Empty → verification skipped.
                    if (req->hasParam("sha256")) {
                        ctx->expectedSha = req->getParam("sha256")->value();
                        ctx->expectedSha.toLowerCase();
                        if (ctx->expectedSha.length() != 64) {
                            DBGF("OTA: bad sha256 length %u (expected 64), ignoring\n",
                                 (unsigned)ctx->expectedSha.length());
                            ctx->expectedSha = "";
                        }
                    }

                    // First byte of an ESP32 firmware image must be
                    // ESP_IMAGE_HEADER_MAGIC (0xE9). Reject anything else
                    // before touching flash.
                    if (len < 1 || data[0] != 0xE9) {
                        DBGLN("OTA: bad magic byte, rejecting");
                        ctx->rejected = true;
                        return;
                    }
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                        Update.printError(Serial);
                        ctx->rejected = true;
                        return;
                    }
                    // Initialise the hasher exactly once per upload, regardless
                    // of whether verification was requested — the cost is tiny
                    // and lets us log the actual digest for debugging.
                    mbedtls_sha256_init(&ctx->sha);
                    mbedtls_sha256_starts(&ctx->sha, 0);  // 0 = SHA-256, not -224
                    ctx->shaActive = true;
                }

                OtaCtx* ctx = static_cast<OtaCtx*>(req->_tempObject);
                if (!ctx || ctx->authFailed || ctx->rejected) return;

                if (Update.write(data, len) != len) Update.printError(Serial);
                if (ctx->shaActive) {
                    mbedtls_sha256_update(&ctx->sha, data, len);
                }
                if (final) {
                    if (ctx->shaActive) {
                        uint8_t digest[32];
                        mbedtls_sha256_finish(&ctx->sha, digest);
                        mbedtls_sha256_free(&ctx->sha);
                        ctx->shaActive = false;   // destructor now a no-op

                        char hex[65];
                        for (int i = 0; i < 32; i++) {
                            snprintf(hex + i*2, 3, "%02x", digest[i]);
                        }
                        hex[64] = '\0';
                        DBGF("OTA: SHA-256 = %s\n", hex);

                        if (ctx->expectedSha.length() == 64 &&
                            !ctx->expectedSha.equalsIgnoreCase(hex)) {
                            DBGF("OTA: SHA-256 mismatch — expected %s\n",
                                 ctx->expectedSha.c_str());
                            ctx->rejected    = true;
                            ctx->shaMismatch = true;
                            Update.abort();
                            return;
                        }
                    }
                    if (Update.end(true)) DBGF("OTA done: %u bytes\n", index + len);
                    else Update.printError(Serial);
                }
            }
        );
    }

    // =========================================================================
    // STATIC FILE FALLBACK (not found handler)
    // =========================================================================
    server.onNotFound([touchActivity](AsyncWebServerRequest *r) {
        touchActivity();   // C2: track web activity for idle power management
        String path = r->url();

        // ── Pass 4 F — /api/v1/* alias layer ────────────────────────────────
        // Forward versioned API requests to the unversioned route via 307
        // (preserves method + body, unlike 302/303 which can downgrade POST
        // to GET in some clients).  Lets the deployed UI keep working for
        // one release while clients migrate to /api/v1/.  Query string is
        // rebuilt from parsed GET params; POST bodies are resent verbatim
        // by the client when it follows the 307.
        if (path.startsWith("/api/v1/")) {
            String rewritten = "/api/" + path.substring(strlen("/api/v1/"));
            // Rebuild the query string from parsed params.  This fork of
            // ESPAsyncWebServer doesn't keep the raw query around (gemini
            // review PR #50 suggested r->queryString() but that accessor
            // doesn't exist here), so we re-encode each value defensively
            // via urlEncode() in utils — gemini review PR #54 asked for
            // it to be centralised so future call sites don't reinvent it.
            String query;
            for (size_t i = 0; i < r->params(); i++) {
                const AsyncWebParameter* p = r->getParam(i);
                if (!p || p->isFile() || p->isPost()) continue;
                if (query.length()) query += "&";
                query += urlEncode(p->name());
                query += "=";
                query += urlEncode(p->value());
            }
            if (query.length()) { rewritten += "?"; rewritten += query; }
            AsyncWebServerResponse* resp = r->beginResponse(307);
            resp->addHeader("Location", rewritten);
            r->send(resp);
            return;
        }

        if (path.startsWith("/www/")) {
            if (littleFsAvailable && LittleFS.exists(path)) {
                r->send(LittleFS, path, getMime(path));
                return;
            }
            // Probe .gz-only files (flash-saving mode: only .gz on disk)
            String gzPath = path + ".gz";
            if (littleFsAvailable && LittleFS.exists(gzPath)) {
                AsyncWebServerResponse* resp =
                    r->beginResponse(LittleFS, gzPath, getMime(path));
                if (resp) {
                    resp->addHeader("Content-Encoding", "gzip");
                    r->send(resp);
                    return;
                }
                r->send(500, "text/plain", "Out of memory");
                return;
            }
            r->send(404, "text/plain", "Not found: " + path);
            return;
        }

        // Also check if the requested file exists in /www/ as a .gz (e.g. /chart.min.js -> /www/chart.min.js.gz)
        String wwwGzPath = "/www" + path + ".gz";
        if (littleFsAvailable && LittleFS.exists(wwwGzPath)) {
            AsyncWebServerResponse* resp =
                r->beginResponse(LittleFS, wwwGzPath, getMime(path));
            if (resp) {
                resp->addHeader("Content-Encoding", "gzip");
                r->send(resp);
                return;
            }
            // Fall through to plain-file lookup below.
        }

        // And check if it exists in /www/ uncompressed
        String wwwPath = "/www" + path;
        if (littleFsAvailable && LittleFS.exists(wwwPath)) {
            r->send(LittleFS, wwwPath, getMime(path));
            return;
        }

        if (path == "/web.js"     || path == "/style.css" ||
            path == "/index.html" || path == "/index.htm") {
            r->send(404, "text/plain", "Moved to /www/");
            return;
        }

        if (littleFsAvailable && LittleFS.exists(path)) {
            r->send(LittleFS, path, getMime(path));
            return;
        }
        if (fsAvailable && activeFS && activeFS->exists(path)) {
            r->send(*activeFS, path, getMime(path));
            return;
        }
        if (r->method() == HTTP_GET && path.indexOf('.') < 0) {
            if (littleFsAvailable && LittleFS.exists("/www/index.html")) {
                r->send(LittleFS, "/www/index.html", "text/html");
                return;
            }
            if (littleFsAvailable && LittleFS.exists("/www/index.html.gz")) {
                AsyncWebServerResponse* resp =
                    r->beginResponse(LittleFS, "/www/index.html.gz", "text/html");
                if (resp) {
                    resp->addHeader("Content-Encoding", "gzip");
                    r->send(resp);
                    return;
                }
                // Fall through to the failsafe page below if the gz response
                // couldn't be allocated.
            }
            sendFailsafePage(r);
            return;
        }
        r->send(404, "text/plain", "Not found");
    });

    // =========================================================================
    // API: PLATFORM CONFIG  (GET = read, POST = write)
    // Used by Core Logic settings page to manage /platform_config.json
    // =========================================================================
    server.on("/api/platform_config", HTTP_GET, h_get_api_platform_config);

    // POST /save_platform — receives JSON body, writes to /platform_config.json
    // Crash-safe: streams into /platform_config.tmp, then renames on success.
    // If the client aborts or the device resets mid-write, the real file stays
    // intact and the tmp is removed on next save attempt.
    // Uses static file handle (only one request at a time on embedded device).
    {
        static File s_pcfgFile;
        static bool s_pcfgMutexHeld = false;
        static bool s_pcfgComplete  = false;
        static bool s_pcfgAuthFail  = false;   // CSRF failed in onBody (index==0)
        static constexpr const char* PCFG_PATH = "/platform_config.json";
        static constexpr const char* PCFG_TMP  = "/platform_config.tmp";

        auto pcfgCleanup = []() {
            if (s_pcfgFile) s_pcfgFile.close();
            // If the upload didn't finish cleanly, discard the partial tmp so
            // the real platform_config.json remains the last good copy.
            if (!s_pcfgComplete && activeFS && activeFS->exists(PCFG_TMP)) {
                activeFS->remove(PCFG_TMP);
            }
            if (s_pcfgMutexHeld && fsMutex) {
                xSemaphoreGive(fsMutex);
                s_pcfgMutexHeld = false;
            }
        };

        server.on("/save_platform", HTTP_POST,
            [pcfgCleanup](AsyncWebServerRequest *r) {
                // CSRF is enforced in the onBody callback below (index==0),
                // BEFORE the file is written — the onBody runs incrementally
                // as chunks arrive, whereas THIS handler runs only after the
                // whole body is received, i.e. after the config would already
                // be overwritten.  Here we just emit the single response for
                // the outcome onBody recorded.
                if (s_pcfgAuthFail) {
                    s_pcfgAuthFail = false;
                    pcfgCleanup();
                    r->send(403, "application/json", "{\"ok\":false,\"error\":\"csrf\"}");
                    return;
                }
                if (rateLimit429(r)) { pcfgCleanup(); return; }
                if (!fsAvailable || !activeFS) {
                    pcfgCleanup();
                    r->send(503, "application/json", "{\"ok\":false,\"error\":\"no fs\"}");
                    return;
                }
                r->send(200, "application/json", "{\"ok\":true}");
            },
            nullptr,
            [pcfgCleanup](AsyncWebServerRequest *r, uint8_t *data, size_t len,
               size_t index, size_t total) {
                if (!fsAvailable || !activeFS) return;
                if (index == 0) {
                    s_pcfgComplete = false;
                    s_pcfgAuthFail = false;
                    // CSRF FIRST — before opening the tmp file / taking the FS
                    // mutex — so a cross-site POST can't mutate config at all.
                    // Non-sending check: the request handler emits the 403 so
                    // there's exactly one response. Swallow the rest of the
                    // body without touching the filesystem.
                    if (!CsrfToken::valid(r)) { s_pcfgAuthFail = true; return; }
                    if (fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                        s_pcfgMutexHeld = true;
                    }
                    // Clean up any leftover tmp from a previous aborted save.
                    if (activeFS->exists(PCFG_TMP)) activeFS->remove(PCFG_TMP);
                    s_pcfgFile = activeFS->open(PCFG_TMP, FILE_WRITE);
                    if (!s_pcfgFile && s_pcfgMutexHeld && fsMutex) {
                        xSemaphoreGive(fsMutex);
                        s_pcfgMutexHeld = false;
                    }
                    // Release FS mutex + discard partial tmp if the client aborts.
                    r->onDisconnect(pcfgCleanup);
                }
                // CSRF failed at index 0 — ignore every remaining chunk so a
                // multi-chunk unauthorized body never reaches the write/rename
                // below (no file was opened; the rename must not run either).
                if (s_pcfgAuthFail) return;
                if (s_pcfgFile) {
                    s_pcfgFile.write(data, len);
                }
                if (index + len >= total) {
                    // Close the tmp file, then atomically replace the real one.
                    if (s_pcfgFile) { s_pcfgFile.close(); s_pcfgFile = File(); }
                    // LittleFS rename overwrites; SD/FAT does not — fall back to
                    // remove+rename if the first attempt fails. If we crash between
                    // remove and rename the tmp is still on disk, but no recovery
                    // path picks it up, so the small window is acceptable.
                    bool ok = activeFS->rename(PCFG_TMP, PCFG_PATH);
                    if (!ok) {
                        if (activeFS->exists(PCFG_PATH)) activeFS->remove(PCFG_PATH);
                        ok = activeFS->rename(PCFG_TMP, PCFG_PATH);
                    }
                    if (!ok) {
                        // Rename still failed — clean up the tmp so we don't leak it.
                        if (activeFS->exists(PCFG_TMP)) activeFS->remove(PCFG_TMP);
                    } else {
                        s_pcfgComplete = true;  // success → cleanup() won't delete anything
                    }
                    pcfgCleanup();
                }
            }
        );
    }

    // =========================================================================
    // API: PLATFORM RELOAD — trigger live sensor/exporter reload after save
    // =========================================================================
    server.on("/api/platform_reload", HTTP_POST, h_post_api_platform_reload);

    // server.begin() is intentionally NOT called here.  ESP_Logger.ino
    // registers additional API routes via registerApiRoutes() AFTER this
    // function returns; calling server.on(...) post-begin() corrupts the
    // handler list while the AsyncTCP service task is iterating it,
    // producing null-deref crashes deep inside _server access on the first
    // request (observed MEPC=0x42036c5e in AsyncWebServer::_rewriteRequest).
    // The single startWebServer() call below is invoked once after all
    // route registrations are complete.
}

void startWebServer() {
    server.begin();
    DBGF("Web server started. Free heap: %d\n", ESP.getFreeHeap());
}