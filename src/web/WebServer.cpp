/**
 * src/web/WebServer.cpp
 * ESP32 Water Logger v4.1.4
 * Author: Petko Georgiev / Villeroy & Boch Bulgaria
 *
 * EXTRACTED FROM: Gemini3_v4_1_4.ino
 * Contains ALL original web handlers - identical HTML/JS to original.
 *
 * ПРОМЯНА: safeWiFiShutdown() добавен преди ESP.restart() в:
 *   /save_hardware, /save_network
 * Всички останали handler-и са идентични с оригинала.
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
// HELPER FUNCTIONS (were global in monolithic .ino)
// ============================================================================

String getModeDisplay() {
    if (onlineLoggerMode) return "🟢 Online Logger";
    if (apModeTriggered)  return "🔵 Web Server";
    return "🔴 Logger";
}

String icon(const char* emoji) {
    return config.theme.showIcons ? String(emoji) + " " : "";
}

String getThemeClass() {
    switch (config.theme.mode) {
        case THEME_DARK: return "theme-dark";
        case THEME_AUTO: return "theme-auto";
        default:         return "theme-light";
    }
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
    String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Restarting</title>"
        "<style>body{font-family:-apple-system,sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;background:#f0f2f5}"
        ".popup{background:#fff;border-radius:16px;padding:2rem;text-align:center;box-shadow:0 4px 20px rgba(0,0,0,0.15);max-width:350px}"
        ".icon{font-size:4rem;margin-bottom:1rem}.title{font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem}.msg{color:#666;margin-bottom:1rem}"
        ".progress{background:#e2e8f0;border-radius:8px;height:8px;overflow:hidden;margin-top:1rem}"
        ".bar{height:100%;background:#27ae60;width:0%;transition:width 1s linear}</style></head>"
        "<body><div class='popup'><div class='icon'>🔄</div><div class='title'>Restarting...</div><div class='msg'>");
    html += message;
    html += F("</div><div id='counter'>Redirecting in 5 seconds...</div>"
        "<div class='progress'><div class='bar' id='bar'></div></div></div>"
        "<script>var s=5,b=document.getElementById('bar'),c=document.getElementById('counter');"
        "var t=setInterval(function(){s--;b.style.width=(100-s*20)+'%';c.textContent='Redirecting in '+s+' seconds...';"
        "if(s<=0){clearInterval(t);window.location.href='/';}},1000);</script></body></html>");
    r->send(200, "text/html", html);
}

// Get current page for navigation highlighting
String getCurrentPage(const char* title) {
    String t = String(title);
    if (t == "Dashboard") return "dashboard";
    if (t == "File Manager") return "files";
    if (t == "Settings" || t.startsWith("Device") || t.startsWith("Flow") || 
        t.startsWith("Hardware") || t.startsWith("Theme") || t.startsWith("Network") ||
        t.startsWith("Time") || t.startsWith("Data Log") || t.startsWith("Firmware")) return "settings";
    if (t == "Live Monitor") return "live";
    return "";
}

// Write sidebar navigation (desktop)
void writeSidebar(Print& out, const char* currentPage) {
}

// Write bottom navigation (mobile)
void writeBottomNav(Print& out, const char* currentPage) {
    out.print(F("<nav class='bottom-nav'>"));
    
    out.printf("<a href='/dashboard' class='%s'>", strcmp(currentPage, "dashboard") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("📊"));
    out.printf("</span>%s</a>", "Home");  // Home
    
    out.printf("<a href='/files' class='%s'>", strcmp(currentPage, "files") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("📁"));
    out.printf("</span>%s</a>", "Files");  // Files
    
    out.printf("<a href='/live' class='%s'>", strcmp(currentPage, "live") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("📡"));
    out.print(F("</span>Live</a>"));  // Keep short for mobile
    
    out.printf("<a href='/settings' class='%s'>", strcmp(currentPage, "settings") == 0 ? "active" : "");
    out.print(F("<span class='icon'>"));
    out.print(icon("⚙️"));
    out.printf("</span>%s</a>", "Settings");  // Settings
    
    out.print(F("</nav>"));
}

// ============================================================================
// CHUNKED HTML RESPONSE HELPER
// ============================================================================
void sendChunkedHtml(AsyncWebServerRequest *r, const char* title, std::function<void(Print&)> bodyWriter) {
    AsyncResponseStream *response = r->beginResponseStream("text/html");
    String currentPage = getCurrentPage(title);
    
    // HTML Header
    response->print(F("<!DOCTYPE html><html class='"));
    response->print(getThemeClass());
    response->print(F("'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'><title>"));
    response->print(title);
    response->print(F(" - "));
    response->print(config.deviceName);
    response->print(F("</title>"));
    // Favicon
    if (strlen(config.theme.faviconPath) > 0) {
        response->printf("<link rel='icon' href='%s'>", config.theme.faviconPath);
    }
    // CSS from external file
    response->print(F("<link rel='stylesheet' href='/style.css'>"));
    response->print(F("<style>"));
    
    // Custom colors from config
    response->printf(":root{--primary:%s;--secondary:%s}",
        config.theme.primaryColor, config.theme.secondaryColor);
    
    response->print(F("</style></head><body>"));
    
    // Sidebar (desktop)
    writeSidebar(*response, currentPage.c_str());
    
    // App Header (mobile)
    response->print(F("<header class='app-header hide-desktop'>"));
    response->print(F("<div class='logo'>"));
    if (strlen(config.theme.logoSource) > 0) {
        response->printf("<img src='%s' alt='' onerror=\"this.style.display='none'\">", config.theme.logoSource);
    }
    response->print(config.deviceName);
    response->print(F("</div>"));
    response->print(F("<div class='status'>"));
    response->print(F("<div class='time'>"));
    response->print(getRtcTimeString());
    response->print(F("</div>"));
    response->print(getNetworkDisplay());
    response->print(F("</div></header>"));
    
    // Main content area
    response->print(F("<main class='main-content'>"));
    
    // Body content via callback
    bodyWriter(*response);
    
    // Footer
    response->print(F("<footer class='app-footer'>"));
    response->print(F("<div class='footer-grid'>"));
    response->print("<span>");
    response->print(icon("🔄"));
    response->printf("Boot: %d</span>", bootCount);
    response->print("<span>");
    response->print(icon("⚡"));
    response->printf("%dMHz</span>", getCpuFrequencyMhz());
    response->print("<span>");
    response->print(icon("💾"));
    response->printf("%s / %s</span>", 
        formatFileSize(ESP.getFreeHeap()).c_str(),
        formatFileSize(ESP.getHeapSize()).c_str());
    response->print(F("</div>"));
    response->print(F("<div class='footer-row'>"));
    response->print(F("<span>"));
    response->print(getNetworkDisplay());
    response->print(F("</span>"));
    response->print(F("<span style='text-align:right'>IP: "));
    response->print(currentIPAddress);
    response->print(F("</span>"));
    response->print(F("</div>"));
    response->print(F("<div class='footer-version'>Board: "));
    response->print(ESP.getChipModel());
    response->print(F(" - Firmware: "));
    response->print(getVersionString());
    response->print(F("</div></footer>"));
    
    response->print(F("</main>"));
    
    // Bottom Navigation (mobile)
    writeBottomNav(*response, currentPage.c_str());
    
    response->print(F("</body></html>"));
    r->send(response);
}

// ============================================================================
// FILE LIST GENERATOR
// ============================================================================

void writeFileList(Print& out, const String& dir, bool editMode) {
    fs::FS* viewFS = getCurrentViewFS();
    if (!viewFS) {
        out.print(F("<p>Storage not available</p>"));
        return;
    }
    
    File d = viewFS->open(dir);
    if (!d || !d.isDirectory()) {
        out.print(F("<p>Cannot open directory</p>"));
        return;
    }
    
    bool hasFiles = false;
    while (File entry = d.openNextFile()) {
        hasFiles = true;
        String name = String(entry.name());
        String fullPath = buildPath(dir, name);
        bool isDir = entry.isDirectory();
        size_t size = entry.size();
        entry.close();
        
        out.print(F("<div class='list-item'><span>"));
        out.print(isDir ? "📁 " : "📄 ");
        out.print(name);
        if (!isDir) {
            out.print(F(" <small class='text-muted'>("));
            out.print(formatFileSize(size));
            out.print(F(")</small>"));
        }
        out.print(F("</span><span class='btn-group'>"));
        
        // Basic actions (always visible)
        if (isDir) {
            out.printf("<a href='/files?storage=%s&dir=%s%s' class='btn btn-sm btn-primary'>Open</a>", 
                currentStorageView.c_str(), fullPath.c_str(), editMode ? "&edit=1" : "");
        } else {
            out.printf("<a href='/download?storage=%s&file=%s' class='btn btn-sm btn-secondary'>📥</a>", 
                currentStorageView.c_str(), fullPath.c_str());
        }
        
        // Edit actions (only if edit mode)
        if (editMode) {
            // Move/Rename button - opens popup
            out.printf("<button class='btn btn-sm btn-secondary' onclick=\"showMove('%s','%s')\">✂️</button>", 
                fullPath.c_str(), name.c_str());
            
            // Delete button
            out.printf("<a href='/delete?storage=%s&path=%s&dir=%s' class='btn btn-sm btn-danger' onclick='return confirm(\"Delete %s?\")'>🗑️</a>", 
                currentStorageView.c_str(), fullPath.c_str(), dir.c_str(), name.c_str());
        }
        out.print(F("</span></div>"));
    }
    d.close();
    
    if (!hasFiles) out.print(F("<div class='list-item text-muted'>Empty directory</div>"));
}


// ============================================================================
// DATALOG FILE HELPERS
// ============================================================================



// ============================================================================
// WEB SERVER SETUP
// ============================================================================
void setupWebServer() {
    Serial.println("Setting up web server...");
    
    // Root redirect to Statistics (main dashboard)
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/dashboard"); });
    
    // ========== DASHBOARD (Statistics - Main Page) ==========
    server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Dashboard", [](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("📊"));
            out.printf("%s</h1></div>", "Dashboard");
            
            // File selector card
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;gap:1rem;flex-wrap:wrap;align-items:center'>"));
            out.print(F("<select id='fileSelect' class='form-input form-select' style='flex:1;min-width:200px' onchange='loadData()'>"));
            out.print(generateDatalogFileOptions());
            out.print(F("</select>"));
            out.print(F("<button class='btn btn-primary' onclick='loadData()'>🔄</button>"));
            out.print(F("</div></div></div>"));
            
            // Filters card
            out.printf("<div class='card'><div class='card-header'>🔍 %s</div><div class='card-body'>", "Filters");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Start");
            out.print(F("<input type='date' id='startDate' class='form-input' onchange='applyFilters()'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "End");
            out.print(F("<input type='date' id='endDate' class='form-input' onchange='applyFilters()'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Trigger");
            out.print(F("<select id='eventFilter' class='form-input form-select' onchange='applyFilters()'>"));
            out.printf("<option value='ALL'>%s</option>", "All");
            out.printf("<option value='BTN'>%s</option>", "Buttons Only");
            out.printf("<option value='FF'>%s</option>", "Full Flush Btn");
            out.printf("<option value='PF'>%s</option>", "Part Flush Btn");
            out.print(F("</select></div>"));
            out.print(F("<div class='form-group'><label class='form-label'>Extra presses</label>"));
            out.print(F("<select id='pressFilter' class='form-input form-select' onchange='applyFilters()'>"));
            out.printf("<option value='ALL'>%s</option>", "All");
            out.printf("<option value='EXTRA'>%s</option>", "With Extra presses");
            out.printf("<option value='NONE'>%s</option>", "No Extra presses");
            out.print(F("</select></div>"));
            out.print(F("</div>"));
            // Exclude zero checkbox
            out.print(F("<div style='margin-top:8px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'>"));
            out.print(F("<label style='display:flex;align-items:center;gap:6px;cursor:pointer'>"));
            out.print(F("<input type='checkbox' id='excludeZero' onchange='applyFilters()' checked>"));
            out.print(F("<span>Exclude 0.00L entries</span></label>"));
            out.print(F("<button onclick='exportCSV()' class='btn btn-sm btn-secondary'>📥 Export CSV</button>"));
            out.print(F("</div>"));
            out.print(F("</div></div>"));
            
            out.print(F("<div id='errorMsg' class='alert alert-error' style='display:none'></div>"));
            
            // Stats cards
            out.print(F("<div class='stats-grid mb-2'>"));
            out.printf("<div class='stat-card'><div class='value' id='totalVol'>0.00</div><div class='label'>%s</div></div>", "Liters");
            out.printf("<div class='stat-card'><div class='value' id='eventCount'>0</div><div class='label'>%s</div></div>", "Events");
            out.printf("<div class='stat-card'><div class='value' id='totalFF' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.ffColor, "Extra FF");
            out.printf("<div class='stat-card'><div class='value' id='totalPF' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.pfColor, "Extra PF");
            out.print(F("</div>"));
            
            // Chart card
            out.printf("<div class='card'><div class='card-header'>📈 %s</div>", "Volume");
            out.print(F("<div class='card-body' style='height:350px'><canvas id='chart'></canvas></div>"));
            out.print(F("<div class='card-body' style='padding-top:0'>"));
            out.print(F("<div style='display:flex;gap:1rem;justify-content:center;flex-wrap:wrap;font-size:.85rem'>"));
            out.printf("<span><span style='display:inline-block;width:12px;height:12px;border-radius:50%%;background:%s;margin-right:4px'></span>%s</span>", config.theme.ffColor, "Full Flush Btn");
            out.printf("<span><span style='display:inline-block;width:12px;height:12px;border-radius:50%%;background:%s;margin-right:4px'></span>%s</span>", config.theme.pfColor, "Part Flush Btn");
            out.printf("<span><span style='display:inline-block;width:12px;height:12px;border-radius:50%%;background:%s;margin-right:4px'></span>%s</span>", config.theme.otherColor, "Other");
            out.print(F("</div></div></div>"));
            
            // Chart.js loading
            if (config.theme.chartSource == CHART_LOCAL) {
                out.printf("<script src='%s'></script>", config.theme.chartLocalPath);
            } else {
                out.print(F("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"));
            }
            
            // JavaScript
            out.print(F("<script>var chart=null,rawData='',filteredData=[];"));
            out.printf("var deviceId='%s';", config.deviceId);
            out.printf("var colors={ff:'%s',pf:'%s',other:'%s'};", config.theme.ffColor, config.theme.pfColor, config.theme.otherColor);
            // JS translation strings
            out.printf("var lang={trigger:'%s',liters:'%s',extraFF:'%s',extraPF:'%s'};", 
                "Trigger", "Liters", "Extra FF", "Extra PF");
            
            out.print(F("function loadData(){var file=document.getElementById('fileSelect').value;"));
            out.print(F("document.getElementById('errorMsg').style.display='none';"));
            out.print(F("fetch('/download?file='+encodeURIComponent(file)).then(r=>{if(!r.ok)throw new Error('Failed');return r.text();})"));
            out.print(F(".then(data=>{rawData=data;applyFilters();})"));
            out.print(F(".catch(e=>{document.getElementById('errorMsg').textContent='Error loading: '+e.message;document.getElementById('errorMsg').style.display='block';});}"));
            
            out.print(F("function applyFilters(){if(!rawData){loadData();return;}processData(rawData);}"));
            
            // Flexible processData - auto-detects format from each line
            out.print(F("function processData(data){"));
            out.print(F("var lines=data.trim().split('\\n'),filtered=[],startVal=document.getElementById('startDate').value,"));
            out.print(F("endVal=document.getElementById('endDate').value,filterType=document.getElementById('eventFilter').value,"));
            out.print(F("pressType=document.getElementById('pressFilter').value,"));
            out.print(F("excludeZero=document.getElementById('excludeZero').checked,"));
            out.print(F("tVol=0,tFF=0,tPF=0;"));
            out.print(F("lines.forEach(function(line){"));
            out.print(F("var p=line.split('|');if(p.length<2)return;"));
            // Auto-detect date format
            out.print(F("var dateStr='',timeStr='',endStr='',boot='',reason='',vol=0,ff=0,pf=0,i=0;"));
            // Check first field for date (contains / or - or . with 4-digit year)
            out.print(F("if(p[0].match(/\\d{2}[\\/\\.\\-]\\d{2}[\\/\\.\\-]\\d{4}/)||p[0].match(/\\d{4}\\-\\d{2}\\-\\d{2}/)){dateStr=p[0];i=1;}"));
            // Next is time (contains : )
            out.print(F("if(p[i]&&p[i].indexOf(':')>=0){timeStr=p[i];i++;}"));
            // Check for end time/duration (has : or ends with s)
            out.print(F("if(p[i]&&(p[i].indexOf(':')>=0||p[i].match(/^\\d+s$/))){endStr=p[i];i++;}"));
            // Check for boot count (#:)
            out.print(F("if(p[i]&&p[i].indexOf('#:')===0){boot=p[i].substring(2);i++;}"));
            // Next should be trigger (FF_BTN, PF_BTN, IDLE, etc)
            out.print(F("if(p[i]&&(p[i].indexOf('FF')>=0||p[i].indexOf('PF')>=0||p[i]==='IDLE')){reason=p[i];i++;}"));
            // Volume (L: prefix or just number)
            out.print(F("if(p[i]){var vStr=p[i].replace('L:','').replace(',','.');vol=parseFloat(vStr)||0;i++;}"));
            // Extra FF
            out.print(F("if(p[i]&&p[i].indexOf('FF')===0){ff=parseInt(p[i].replace('FF',''))||0;i++;}"));
            // Extra PF
            out.print(F("if(p[i]&&p[i].indexOf('PF')===0){pf=parseInt(p[i].replace('PF',''))||0;}"));
            // Parse date for filtering
            out.print(F("var entryDate='';"));
            out.print(F("if(dateStr){var m;"));
            out.print(F("if(m=dateStr.match(/(\\d{2})[\\/\\.](\\d{2})[\\/\\.](\\d{4})/))entryDate=m[3]+'-'+m[2]+'-'+m[1];"));
            out.print(F("else if(m=dateStr.match(/(\\d{4})\\-(\\d{2})\\-(\\d{2})/))entryDate=m[1]+'-'+m[2]+'-'+m[3];}"));
            // Apply filters
            out.print(F("if(startVal&&entryDate&&entryDate<startVal)return;if(endVal&&entryDate&&entryDate>endVal)return;"));
            out.print(F("if(filterType==='BTN'&&reason.indexOf('FF')===-1&&reason.indexOf('PF')===-1)return;"));
            out.print(F("if(filterType==='FF'&&reason.indexOf('FF')===-1)return;"));
            out.print(F("if(filterType==='PF'&&reason.indexOf('PF')===-1)return;"));
            out.print(F("if(pressType==='EXTRA'&&ff===0&&pf===0)return;"));
            out.print(F("if(pressType==='NONE'&&(ff>0||pf>0))return;"));
            out.print(F("if(excludeZero&&vol===0)return;"));
            out.print(F("tFF+=ff;tPF+=pf;tVol+=vol;"));
            // time = start only, fullTime = start-end for CSV
            out.print(F("var fullTime=timeStr+(endStr?'-'+endStr:'');"));
            out.print(F("filtered.push({date:dateStr||'N/A',time:timeStr,fullTime:fullTime,boot:boot,vol:vol,reason:reason,ff:ff,pf:pf});});"));
            out.print(F("filteredData=filtered;"));
            out.print(F("document.getElementById('totalVol').textContent=tVol.toFixed(2)+' L';"));
            out.print(F("document.getElementById('eventCount').textContent=filtered.length;"));
            out.print(F("document.getElementById('totalFF').textContent=tFF;"));
            out.print(F("document.getElementById('totalPF').textContent=tPF;"));
            out.print(F("renderChart(filtered);}"));
            
            // Export CSV function - uses fullTime and boot
            out.print(F("function exportCSV(){if(!filteredData.length){alert('No data to export');return;}"));
            out.print(F("var csv='Date,Time,Boot,Volume (L),Trigger,Extra FF,Extra PF\\n';"));
            out.print(F("filteredData.forEach(function(d){csv+=d.date+','+d.fullTime+','+(d.boot||'')+','+d.vol.toFixed(2)+','+d.reason+','+d.ff+','+d.pf+'\\n';});"));
            // Build filename: deviceId_filters_date.csv
            out.print(F("var f=deviceId;"));
            out.print(F("var ft=document.getElementById('eventFilter').value;if(ft!=='ALL')f+='_'+ft;"));
            out.print(F("var pt=document.getElementById('pressFilter').value;if(pt!=='ALL')f+='_'+pt;"));
            out.print(F("if(document.getElementById('excludeZero').checked)f+='_noZero';"));
            out.print(F("var sd=document.getElementById('startDate').value;if(sd)f+='_from'+sd;"));
            out.print(F("var ed=document.getElementById('endDate').value;if(ed)f+='_to'+ed;"));
            out.print(F("var today=new Date().toISOString().slice(0,10);f+='_'+today+'.csv';"));
            out.print(F("var blob=new Blob([csv],{type:'text/csv'});var url=URL.createObjectURL(blob);"));
            out.print(F("var a=document.createElement('a');a.href=url;a.download=f;a.click();URL.revokeObjectURL(url);}")); 
            
            // renderChart with configurable label format
            out.print(F("function renderChart(data){"));
            out.print(F("var ctx=document.getElementById('chart').getContext('2d');"));
            out.print(F("if(chart)chart.destroy();"));
            out.print(F("var clr=data.map(d=>{if(d.reason.indexOf('FF')>=0)return colors.ff;if(d.reason.indexOf('PF')>=0)return colors.pf;return colors.other;});"));
            // Label format: 0=DateTime, 1=BootCount, 2=Both
            out.printf("var lblFmt=%d;", config.theme.chartLabelFormat);
            out.print(F("var lbls=data.map(d=>{"));
            out.print(F("if(lblFmt===1)return d.boot?'#'+d.boot:'#?';"));
            out.print(F("if(lblFmt===2)return(d.date+' '+d.time+(d.boot?' #'+d.boot:''));"));
            out.print(F("return d.date+' '+d.time;});"));
            out.print(F("chart=new Chart(ctx,{type:'bar',data:{labels:lbls,datasets:[{"));
            out.print(F("label:lang.liters+' (L)',data:data.map(d=>d.vol),backgroundColor:clr,borderWidth:0}]},"));
            out.print(F("options:{responsive:true,maintainAspectRatio:false,plugins:{tooltip:{callbacks:{"));
            out.print(F("afterLabel:ctx=>{var d=data[ctx.dataIndex];return[lang.trigger+': '+d.reason,'Boot: '+(d.boot||'N/A'),lang.extraFF+': '+d.ff,lang.extraPF+': '+d.pf];}"));
            out.print(F("}}},scales:{y:{beginAtZero:true,title:{display:true,text:lang.liters}}}}});}"));
            
            out.print(F("window.onload=loadData;"));
            out.print(F("</script>"));
        });
    });
    
    // ========== FILE MANAGER ==========
    server.on("/files", HTTP_GET, [](AsyncWebServerRequest *r) {
        currentDir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        currentDir = sanitizeFilename(currentDir);
        if (!currentDir.startsWith("/")) currentDir = "/" + currentDir;
        bool editMode = r->hasParam("edit") && r->getParam("edit")->value() == "1";
        
        // Storage selection
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        } else {
            // If no parameter, use the setting from Device Settings
            if (config.hardware.defaultStorageView == 1 && sdAvailable) {
                currentStorageView = "sdcard";
            } else {
                currentStorageView = "internal";
            }
        }
        // Validate storage view
        if (currentStorageView == "sdcard" && !sdAvailable) {
            currentStorageView = "internal";
        }
        if (currentStorageView == "internal" && !littleFsAvailable) {
            currentStorageView = sdAvailable ? "sdcard" : "internal";
        }
        
        sendChunkedHtml(r, "Files", [editMode](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("📁"));
            out.printf("%s</h1></div>", "File Manager");
            
            // Status message
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            // Storage tabs (only show if both available)
            if (littleFsAvailable || sdAvailable) {
                out.print(F("<div class='tabs' style='display:flex;gap:4px;margin-bottom:1rem'>"));
                
                if (littleFsAvailable) {
                    out.print(F("<a href='/files?storage=internal&dir=/' class='btn "));
                    out.print(currentStorageView == "internal" ? "btn-primary" : "btn-secondary");
                    out.printf("'>💾 %s</a>", "Internal");
                }
                
                if (sdAvailable) {
                    out.print(F("<a href='/files?storage=sdcard&dir=/' class='btn "));
                    out.print(currentStorageView == "sdcard" ? "btn-primary" : "btn-secondary");
                    out.printf("'>💳 %s</a>", "SD Card");
                }
                
                out.print(F("</div>"));
            }
            
            // Storage info card
            uint64_t used, total; int percent;
            getStorageInfo(used, total, percent, currentStorageView);
            
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px'>"));
            out.print(F("<span>"));
            out.print(formatFileSize(used));
            out.print(F(" / "));
            out.print(formatFileSize(total));
            out.print(F("</span>"));
            out.printf("<span class='text-muted'>%d%%</span>", percent);
            out.print(F("</div>"));
            out.print(F("<div class='progress'><div class='progress-bar"));
            if (percent >= 90) out.print(F(" progress-bar-danger"));
            else if (percent >= 70) out.print(F(" progress-bar-warning"));
            else out.print(F(" progress-bar-success"));
            out.printf("' style='width:%d%%'></div></div>", percent);
            out.print(F("</div></div>"));
            
            // Directory card
            out.print(F("<div class='card'>"));
            out.print(F("<div class='card-header' style='display:flex;justify-content:space-between;align-items:center'>"));
            out.print(F("<span>📂 "));
            out.print(currentStorageView == "sdcard" ? "[SD] " : "[Int] ");
            out.print(currentDir == "/" ? "Root" : currentDir.c_str());
            out.print(F("</span>"));
            out.print(F("<div class='btn-group'>"));
            if (currentDir != "/") {
                int lastSlash = currentDir.lastIndexOf('/');
                String parent = lastSlash <= 0 ? "/" : currentDir.substring(0, lastSlash);
                if (editMode) {
                    out.printf("<a href='/files?storage=%s&dir=%s&edit=1' class='btn btn-sm btn-secondary'>⬆️ Up</a>", currentStorageView.c_str(), parent.c_str());
                } else {
                    out.printf("<a href='/files?storage=%s&dir=%s' class='btn btn-sm btn-secondary'>⬆️ Up</a>", currentStorageView.c_str(), parent.c_str());
                }
            }
            if (editMode) {
                out.printf("<a href='/files?storage=%s&dir=%s' class='btn btn-sm btn-secondary'>✖️ Done</a>", currentStorageView.c_str(), currentDir.c_str());
            } else {
                out.printf("<a href='/files?storage=%s&dir=%s&edit=1' class='btn btn-sm btn-primary'>✏️ Edit</a>", currentStorageView.c_str(), currentDir.c_str());
            }
            out.print(F("</div></div>"));
            
            out.print(F("<div class='card-body' style='padding:0'>"));
            writeFileList(out, currentDir, editMode);
            out.print(F("</div>"));
            
            // Upload/Create forms (edit mode only)
            if (editMode) {
                out.print(F("<div class='card-body' style='border-top:1px solid var(--border);background:var(--bg)'>"));
                out.print(F("<div style='display:flex;gap:1rem;flex-wrap:wrap;align-items:center'>"));
                out.print(F("<form id='fileUploadForm' action='/upload' method='POST' enctype='multipart/form-data' style='display:flex;gap:8px;align-items:center'>"));
                out.printf("<input type='hidden' name='path' value='%s'>", currentDir.c_str());
                out.print(F("<input type='file' name='file' id='fileInput' required class='form-input' style='width:auto'>"));
                out.print(F("<button type='submit' class='btn btn-sm btn-success'>📤 Upload</button></form>"));
                out.print(F("<form action='/mkdir' method='GET' style='display:flex;gap:8px;align-items:center'>"));
                out.printf("<input type='hidden' name='dir' value='%s'>", currentDir.c_str());
                out.print(F("<input type='text' name='name' placeholder='New folder' required class='form-input' style='width:120px'>"));
                out.print(F("<button type='submit' class='btn btn-sm btn-primary'>📁 Create</button></form>"));
                out.print(F("</div>"));
                // Upload progress bar
                out.print(F("<div id='uploadProg' style='display:none;margin-top:8px'>"));
                out.print(F("<div style='display:flex;align-items:center;gap:8px'>"));
                out.print(F("<div class='progress' style='flex:1;height:10px'><div id='uploadBar' class='progress-bar progress-bar-success' style='width:0%'></div></div>"));
                out.print(F("<span id='uploadPct' style='font-size:0.8rem;min-width:45px'>0%</span></div></div>"));
                out.print(F("</div>"));
                
                // ===== MOVE/RENAME POPUP =====
                out.print(F("<div id='movePopup' class='popup-overlay'>"));
                out.print(F("<div class='popup-content' style='text-align:left;max-width:400px'>"));
                out.print(F("<div style='font-size:2.5rem;text-align:center;margin-bottom:0.5rem'>✂️</div>"));
                out.print(F("<div style='font-size:1.2rem;font-weight:bold;text-align:center;margin-bottom:1rem'>Move / Rename</div>"));
                out.print(F("<form action='/move_file' method='GET'>"));
                out.printf("<input type='hidden' name='storage' value='%s'>", currentStorageView.c_str());
                out.printf("<input type='hidden' name='dir' value='%s'>", currentDir.c_str());
                out.print(F("<input type='hidden' name='src' id='mvSrc'>"));
                out.print(F("<div class='form-group'><label class='form-label'>New Name</label>"));
                out.print(F("<input type='text' name='newName' id='mvName' class='form-input' required></div>"));
                out.print(F("<div class='form-group'><label class='form-label'>Move to Folder</label>"));
                out.print(F("<select name='destDir' id='mvDest' class='form-input form-select'>"));
                out.print(F("<option value=''>(same location)</option>"));
                out.print(F("<option value='/'>/ (root)</option>"));
                // List folders from current storage
                fs::FS* viewFS = getCurrentViewFS();
                if (viewFS) {
                    std::function<void(const String&, int)> listDirs = [&](const String& path, int depth) {
                        if (depth > 3) return;
                        File d = viewFS->open(path);
                        if (d && d.isDirectory()) {
                            while (File f = d.openNextFile()) {
                                if (f.isDirectory()) {
                                    String fp = buildPath(path, String(f.name()));
                                    out.print(F("<option value='")); out.print(fp); out.print(F("'>"));
                                    for(int i=0;i<depth;i++) out.print(F("&nbsp;&nbsp;"));
                                    out.print(f.name()); out.print(F("</option>"));
                                    listDirs(fp, depth + 1);
                                }
                                f.close();
                            }
                            d.close();
                        }
                    };
                    listDirs("/", 1);
                }
                out.print(F("</select></div>"));
                out.print(F("<div style='display:flex;gap:0.5rem;justify-content:flex-end;margin-top:1rem'>"));
                out.print(F("<button type='button' class='btn btn-secondary' onclick=\"hidePopup('movePopup')\">Cancel</button>"));
                out.print(F("<button type='submit' class='btn btn-primary'>✂️ Apply</button>"));
                out.print(F("</div></form></div></div>"));
                
                // Upload progress script + popup functions
                out.print(F("<script>"));
                out.print(F("document.getElementById('fileUploadForm').onsubmit=function(e){"));
                out.print(F("e.preventDefault();var f=document.getElementById('fileInput').files[0];if(!f)return;"));
                out.print(F("var xhr=new XMLHttpRequest(),fd=new FormData(this);"));
                out.print(F("document.getElementById('uploadProg').style.display='block';"));
                out.print(F("xhr.upload.onprogress=function(ev){if(ev.lengthComputable){"));
                out.print(F("var p=Math.round(ev.loaded/ev.total*100);"));
                out.print(F("document.getElementById('uploadBar').style.width=p+'%';"));
                out.print(F("document.getElementById('uploadPct').textContent=p+'%';}};"));
                out.print(F("xhr.onload=function(){setTimeout(function(){location.reload();},500);};"));
                out.print(F("xhr.onerror=function(){alert('Upload failed');location.reload();};"));
                out.print(F("xhr.open('POST','/upload');xhr.send(fd);};"));
                // Popup helper functions
                out.print(F("function showPopup(id){document.getElementById(id).style.display='flex';}"));
                out.print(F("function hidePopup(id){document.getElementById(id).style.display='none';}"));
                out.print(F("function showMove(path,name){document.getElementById('mvSrc').value=path;"));
                out.print(F("document.getElementById('mvName').value=name;showPopup('movePopup');}"));
                out.print(F("</script>"));
            }
            out.print(F("</div>"));
        });
    });
    
    // ========== SETTINGS MENU ==========
    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Settings", [](Print& out) {
            // Restart confirmation popup
            out.print(F("<div id='restartPopup' class='popup-overlay'>"));
            out.print(F("<div class='popup-content'>"));
            out.print(F("<div id='popupIcon' style='font-size:4rem;margin-bottom:1rem'>🔄</div>"));
            out.print(F("<div id='popupTitle' style='font-size:1.5rem;font-weight:bold;margin-bottom:0.5rem'>Restart Device?</div>"));
            out.print(F("<div id='popupMsg' style='margin-bottom:1rem'>The device will restart. Any unsaved changes will be lost.</div>"));
            out.print(F("<div id='popupProgress' style='display:none'>"));
            out.print(F("<div style='background:#e0e0e0;border-radius:4px;height:12px;margin-bottom:0.5rem;overflow:hidden'><div id='popupBar' style='width:0%;height:100%;background:#f39c12;transition:width 0.3s'></div></div>"));
            out.print(F("</div>"));
            out.print(F("<div id='popupButtons' style='display:flex;gap:1rem;justify-content:center'>"));
            out.print(F("<button class='btn btn-danger' onclick='confirmRestart()'>✓ Confirm</button>"));
            out.print(F("<button class='btn btn-secondary' onclick='cancelRestart()'>✗ Cancel</button>"));
            out.print(F("</div></div></div>"));
            
            // Page header with restart button
            out.print(F("<div class='page-header' style='display:flex;justify-content:space-between;align-items:flex-start'>"));
            out.print(F("<div><h1>"));
            out.print(icon("⚙️"));
            out.print("Settings");
            out.print(F("</h1></div>"));
            out.printf("<button onclick='showRestartPopup()' class='btn btn-warning'>🔄 %s</button>", "Restart");
            out.print(F("</div>"));
            
            // Settings grid
            out.print(F("<div class='settings-grid'>"));
            
            // Device
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_device'\"><div class='icon'>"));
            out.print(icon("📱"));
            out.printf("</div><div class='title'>%s</div></div>", "Device");
            
            // Flow Meter
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_flowmeter'\"><div class='icon'>"));
            out.print(icon("💧"));
            out.printf("</div><div class='title'>%s</div></div>", "Flow Meter");
            
            // Hardware
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_hardware'\"><div class='icon'>"));
            out.print(icon("🔧"));
            out.printf("</div><div class='title'>%s</div></div>", "Hardware");
            
            // Data Log
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_datalog'\"><div class='icon'>"));
            out.print(icon("📝"));
            out.printf("</div><div class='title'>%s</div></div>", "Data Log");
            
            // Theme
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_theme'\"><div class='icon'>"));
            out.print(icon("🎨"));
            out.printf("</div><div class='title'>%s</div></div>", "Theme");
            
            // Network
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_network'\"><div class='icon'>"));
            out.print(icon("🌐"));
            out.printf("</div><div class='title'>%s</div></div>", "Network");
            
            // Time Sync
            out.print(F("<div class='setting-card' onclick=\"location.href='/settings_time'\"><div class='icon'>"));
            out.print(icon("🕐"));
            out.printf("</div><div class='title'>%s</div></div>", "Time Sync");
            
            
            out.print(F("</div>"));
            
            // Backup & Restore card
            out.print(F("<div class='card mt-2'><div class='card-header'>💾 Backup & Restore</div>"));
            out.print(F("<div class='card-body'>"));
            out.print(F("<div class='btn-group' style='margin-bottom:1rem'>"));
            out.printf("<a href='/export_settings' download='%s_settings.json' class='btn btn-primary'>📥 Export Settings</a>", config.deviceName);
            out.print(F("</div>"));
            out.print(F("<form id='importForm' action='/import_settings' method='POST' enctype='multipart/form-data'>"));
            out.print(F("<div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>"));
            out.print(F("<input type='file' name='settings' id='importFile' accept='.json' class='form-input' style='flex:1;min-width:150px'>"));
            out.print(F("<button type='submit' class='btn btn-secondary'>📤 Import Settings</button></div>"));
            out.print(F("</form>"));
            // Import progress bar
            out.print(F("<div id='importProg' style='display:none;margin-top:8px'>"));
            out.print(F("<div style='display:flex;align-items:center;gap:8px'>"));
            out.print(F("<div class='progress' style='flex:1;height:10px'><div id='importBar' class='progress-bar progress-bar-success' style='width:0%'></div></div>"));
            out.print(F("<span id='importPct' style='font-size:0.8rem;min-width:45px'>0%</span></div></div>"));
            out.print(F("</div></div>"));
            
            // Import progress script
            out.print(F("<script>"));
            out.print(F("document.getElementById('importForm').onsubmit=function(e){"));
            out.print(F("e.preventDefault();var f=document.getElementById('importFile').files[0];if(!f)return;"));
            out.print(F("var xhr=new XMLHttpRequest(),fd=new FormData(this);"));
            out.print(F("document.getElementById('importProg').style.display='block';"));
            out.print(F("xhr.upload.onprogress=function(ev){if(ev.lengthComputable){"));
            out.print(F("var p=Math.round(ev.loaded/ev.total*100);"));
            out.print(F("document.getElementById('importBar').style.width=p+'%';"));
            out.print(F("document.getElementById('importPct').textContent=p+'%';}};"));
            out.print(F("xhr.onload=function(){"));
            out.print(F("if(xhr.status===200){alert('Settings imported successfully!');location.reload();}"));
            out.print(F("else{alert('Import failed: '+xhr.responseText);document.getElementById('importProg').style.display='none';}};"));
            out.print(F("xhr.onerror=function(){alert('Import failed');document.getElementById('importProg').style.display='none';};"));
            out.print(F("xhr.open('POST','/import_settings');xhr.send(fd);};"));
            out.print(F("</script>"));
            
            // JavaScript for restart popup
            out.print(F("<script>"));
            out.print(F("function showRestartPopup(){"));
            out.print(F("document.getElementById('popupIcon').textContent='🔄';"));
            out.print(F("document.getElementById('popupTitle').textContent='Restart Device?';"));
            out.print(F("document.getElementById('popupMsg').textContent='The device will restart. Any unsaved changes will be lost.';"));
            out.print(F("document.getElementById('popupProgress').style.display='none';"));
            out.print(F("document.getElementById('popupButtons').style.display='flex';"));
            out.print(F("document.getElementById('restartPopup').style.display='flex';"));
            out.print(F("}"));
            out.print(F("function cancelRestart(){"));
            out.print(F("document.getElementById('restartPopup').style.display='none';"));
            out.print(F("}"));
            out.print(F("function confirmRestart(){"));
            out.print(F("document.getElementById('popupButtons').style.display='none';"));
            out.print(F("document.getElementById('popupProgress').style.display='block';"));
            out.print(F("document.getElementById('popupIcon').textContent='⏳';"));
            out.print(F("document.getElementById('popupTitle').textContent='Restarting...';"));
            out.print(F("var seconds=5;"));
            out.print(F("var tick=function(){"));
            out.print(F("document.getElementById('popupMsg').innerHTML='Redirecting in <strong>'+seconds+'</strong> seconds...';"));
            out.print(F("document.getElementById('popupBar').style.width=((5-seconds)*20)+'%';"));
            out.print(F("if(seconds<=0){fetch('/restart').then(()=>{window.location.href='/dashboard';}).catch(()=>{window.location.href='/dashboard';});}"));
            out.print(F("else{seconds--;setTimeout(tick,1000);}"));
            out.print(F("};tick();"));
            out.print(F("}"));
            out.print(F("</script>"));
        });
    });
    
    // Restart device (supports both AJAX and direct access)
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Check if it's an AJAX request or direct browser access
        bool isAjax = r->hasHeader("X-Requested-With") || 
                      (r->hasHeader("Accept") && r->header("Accept").indexOf("application/json") >= 0);
        
        if (isAjax) {
            // AJAX request - return JSON and restart
            r->send(200, "application/json", "{\"success\":true,\"message\":\"Restarting...\"}");
        } else {
            // Direct browser access - show page with redirect
            r->send(200, "text/html", 
                "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Restarting</title><style>body{font-family:system-ui,sans-serif;text-align:center;padding:50px;background:#f0f2f5}"
                ".box{background:#fff;padding:2rem;border-radius:12px;max-width:400px;margin:0 auto;box-shadow:0 2px 8px rgba(0,0,0,.1)}"
                "</style></head><body><div class='box'><div style='font-size:4rem'>🔄</div>"
                "<h2>Restarting...</h2><p>Device will restart in a moment.</p>"
                "<p id='counter'>Redirecting in 5 seconds...</p></div>"
                "<script>var s=5;setInterval(function(){s--;if(s<=0)location.href='/dashboard';"
                "else document.getElementById('counter').textContent='Redirecting in '+s+' seconds...';},1000);</script>"
                "</body></html>");
        }
        
        shouldRestart = true;
        restartTimer = millis();
    });
    
    // ========== DEVICE SETTINGS ==========
    server.on("/settings_device", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Device", [](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("📱"));
            out.printf("%s</h1>", "Device");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Device");
            out.print(F("</div>"));
            
            out.print(F("<form action='/save_device' method='POST'>"));
            
            // Identity card
            out.printf("<div class='card'><div class='card-header'>📛 %s</div><div class='card-body'>", "Identity");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='deviceName' value='%s' maxlength='32' class='form-input'></div>", "Device Name", config.deviceName);
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Device ID");
            out.print(F("<div style='display:flex;gap:8px;flex-wrap:wrap'>"));
            out.printf("<input type='text' id='devIdInput' name='deviceId' value='%s' maxlength='12' class='form-input' style='flex:1;min-width:120px' disabled>", config.deviceId);
            out.printf("<button type='button' onclick='regenId()' class='btn btn-secondary btn-sm'>🔄 %s</button>", "New ID");
            out.print(F("<button type='button' onclick='toggleManualId()' class='btn btn-secondary btn-sm'>✏️ Manual</button>"));
            out.print(F("</div></div>"));
            out.print(F("</div></div>"));

            // Storage View Preference
            out.printf("<div class='card'><div class='card-header'>💾 %s</div><div class='card-body'>", "Storage");
            out.printf("<div class='form-group'><label class='form-label'>%s</label>", "Default Storage");
            out.print(F("<select name='defaultStorageView' class='form-input form-select'"));
            if (!sdAvailable) out.print(F(" disabled style='background-color:#eee;color:#999'"));
            out.print(F(">"));
            out.printf("<option value='0' %s>💾 %s (LittleFS)</option>", 
                config.hardware.defaultStorageView == 0 ? "selected" : "", "Internal");
            out.printf("<option value='1' %s>💳 %s</option>", 
                config.hardware.defaultStorageView == 1 ? "selected" : "", "SD Card");
            out.print(F("</select></div></div></div>"));
            
            // Web Server card
            out.printf("<div class='card'><div class='card-header'>🌐 %s</div><div class='card-body'>", "Web Server");
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='forceWebServer' value='1' %s> %s</label></div>", config.forceWebServer ? "checked" : "", "Force Web Server On");
            out.print(F("</div></div>"));
            
            // System Info card
            out.printf("<div class='card'><div class='card-header'>📊 %s</div><div class='card-body'>", "System Info");
            out.print(F("<div class='form-row'>"));
            out.printf("<div><strong>%s</strong><div class='text-primary'>%s</div></div>", "Firmware", getVersionString().c_str());
            out.printf("<div><strong>%s</strong><div>%d</div></div>", "Boot Count", bootCount);
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Mode", getModeDisplay().c_str());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Free Heap", formatFileSize(ESP.getFreeHeap()).c_str());
            out.print(F("</div>"));
            out.print(F("<div class='form-row mt-1'>"));
            out.printf("<div><strong>CPU</strong><div>%d MHz</div></div>", getCpuFrequencyMhz());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Chip", ESP.getChipModel());
            out.printf("<div><strong>%s</strong><div>%s</div></div>", "Flash", formatFileSize(ESP.getFlashChipSize()).c_str());
            out.printf("<div><strong>SDK</strong><div>%s</div></div>", ESP.getSdkVersion());
            out.print(F("</div></div></div>"));
            
            // Changelog card - loads from /changelog.txt (LittleFS)
            out.print(F("<div class='card'><div class='card-header' onclick=\"var el=document.getElementById('changelog');el.classList.toggle('hidden');if(!el.dataset.loaded)loadChangelog();\" style='cursor:pointer'>"));
            out.printf("📜 %s <small class='text-muted'>(tap)</small></div>", "Change Log");
            out.print(F("<div class='card-body hidden' id='changelog' data-loaded=''>"));
            out.printf("<div class='text-muted'>%s...</div>", "Loading");
            out.print(F("</div></div>"));
            
            // About card
            out.print(F("<div class='card'><div class='card-header'>ℹ️ About</div><div class='card-body'>"));
            out.print(F("<div style='text-align:center'>"));
            out.print(F("<div style='font-size:1.2rem;font-weight:bold;color:var(--primary);margin-bottom:0.5rem'>Water Logger</div>"));
            out.print(F("<div style='color:var(--text-muted);font-size:0.9rem'>Created by</div>"));
            out.print(F("<div style='font-weight:600;margin:0.5rem 0'>Petko Georgiev</div>"));
            out.print(F("<div style='color:var(--text-muted);font-size:0.85rem'>Villeroy &amp; Boch Bulgaria</div>"));
            out.print(F("<div style='color:var(--text-muted);font-size:0.85rem;margin-top:0.25rem'>📍 Sevlievo, Bulgaria</div>"));
            out.print(F("</div></div></div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>💾 Save</button></form>"));
            out.print(F("<script>"));
            out.print(F("function loadChangelog(){"));
            out.print(F("var el=document.getElementById('changelog');"));
            out.print(F("fetch('/api/changelog').then(r=>r.ok?r.text():Promise.reject('File not found')).then(txt=>{"));
            out.print(F("var html='';var lines=txt.trim().split('\\n');var inVersion=false;"));
            out.print(F("lines.forEach(function(line){"));
            out.print(F("line=line.trim();if(!line)return;"));
            out.print(F("if(line.startsWith('##')){"));
            out.print(F("if(inVersion)html+='</ul></div>';"));
            out.print(F("var ver=line.substring(2).trim();"));
            out.print(F("var isCurrent=ver.indexOf('Current')>=0||lines.indexOf(line)<3;"));
            out.print(F("html+='<div style=\"margin-top:0.5rem;padding:0.5rem;'+(isCurrent?'background:var(--primary);color:#fff':'background:var(--bg)')+';border-radius:4px\">';"));
            out.print(F("html+='<strong>'+ver+'</strong><ul style=\"margin:0.5rem 0 0 1rem;padding:0;font-size:0.9rem\">';"));
            out.print(F("inVersion=true;"));
            out.print(F("}else if(line.startsWith('-')&&inVersion){"));
            out.print(F("html+='<li>'+line.substring(1).trim()+'</li>';"));
            out.print(F("}});"));
            out.print(F("if(inVersion)html+='</ul></div>';"));
            out.print(F("el.innerHTML=html;el.dataset.loaded='1';"));
            out.print(F("}).catch(e=>{el.innerHTML='<div class=\"alert alert-warning\">Changelog not found. Upload /changelog.txt</div>';el.dataset.loaded='1';});"));
            out.print(F("}"));
            out.print(R"raw(
            function regenId() {
                if(confirm('Generate new ID based on MAC address?')) {
                    fetch('/api/regen-id', {method: 'POST'})
                    .then(response => response.text())
                    .then(newId => {
                        document.getElementById('devIdInput').value = newId;
                        document.getElementById('devIdInput').disabled = false;
                        alert('New ID generated: ' + newId + '. Click Save to apply.');
                    })
                    .catch(err => alert('Error generating ID: ' + err));
                }
            }
            function toggleManualId() {
                var inp = document.getElementById('devIdInput');
                if(inp.disabled) {
                    inp.disabled = false;
                    inp.focus();
                    inp.select();
                } else {
                    inp.disabled = true;
                }
            }
            )raw");
            out.print(F("</script>"));
        });
    });
    
    server.on("/save_device", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("deviceName", true))
            strncpy(config.deviceName, r->getParam("deviceName", true)->value().c_str(), 32);
        if (r->hasParam("deviceId", true)) {
            String newId = r->getParam("deviceId", true)->value();
            if (newId.length() > 0 && newId.length() <= 12) {
                strncpy(config.deviceId, newId.c_str(), sizeof(config.deviceId) - 1);
            }
        }
        config.forceWebServer = r->hasParam("forceWebServer", true);
        if (r->hasParam("defaultStorageView", true)) {
            config.hardware.defaultStorageView = r->getParam("defaultStorageView", true)->value().toInt();
        }
        saveConfig();
        r->redirect("/settings_device");
    });
    
    // API endpoint for generating new Device ID from MAC address
    server.on("/api/regen-id", HTTP_POST, [](AsyncWebServerRequest *request) {
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        // Use last 8 characters (4 bytes) of MAC
        String newId = mac.substring(mac.length() - 8);
        newId.toUpperCase();
        request->send(200, "text/plain", newId);
    });
    
    // ========== FLOW METER SETTINGS ==========
    server.on("/settings_flowmeter", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Flow Meter", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("💧"));
            out.printf("%s</h1>", "Flow Meter");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Flow Meter");
            
            out.print(F("<form action='/save_flowmeter' method='POST'>"));
            
            // Calibration card
            out.printf("<div class='card'><div class='card-header'>📏 %s</div><div class='card-body'>", "Calibration");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' step='0.1' name='pulsesPerLiter' value='%.1f' class='form-input'></div>", "Pulses/Liter", config.flowMeter.pulsesPerLiter);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' step='0.01' name='calibrationMultiplier' value='%.2f' class='form-input'></div>", "Multiplier", config.flowMeter.calibrationMultiplier);
            out.print(F("</div></div></div>"));
            
            // Timing card
            out.printf("<div class='card'><div class='card-header'>⏱️ %s</div><div class='card-body'>", "Timing");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='monitoringWindowSecs' value='%d' class='form-input'>", "Window (sec)", config.flowMeter.monitoringWindowSecs);
            out.print(F("<p class='form-hint'>Time window of idle before logging.</p></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='firstLoopWindowSecs' value='%d' class='form-input'>", "First Loop (sec)", config.flowMeter.firstLoopMonitoringWindowSecs);
            out.print(F("<p class='form-hint'>First loop delay before logging (for delayed fill).</p></div>"));
            out.print(F("</div></div></div>"));
            
            // Test Mode card
            out.printf("<div class='card'><div class='card-header'>🧪 %s</div><div class='card-body'>", "Test Mode");
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='testMode' value='1' %s> %s</label>", config.flowMeter.testMode ? "checked" : "", "Enable");
            out.print(F("<p class='form-hint'>Activate WiFi Pin as LED output for visual check.<br>Solid light = IDLE / Blink = Water running.</p></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s (ms)</label><input type='number' name='blinkDuration' value='%d' class='form-input'></div>", "Blink Duration", config.flowMeter.blinkDuration);
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-top:1rem'>"));
            out.printf("<span>%s: <strong>%d</strong></span>", "Boot Count", bootCount);
            out.print(F("<button type='submit' name='resetBootCount' value='1' class='btn btn-sm btn-danger'>🔄 Reset</button>"));
            out.print(F("</div></div></div>"));
            
            out.printf("<button type='submit' class='btn btn-primary btn-block'>💾 %s</button></form>", "Save");
        });
    });
    
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
        if (r->hasParam("resetBootCount", true)) {
            bootCount = 0;
            backupBootCount();
        }
        saveConfig();
        r->redirect("/settings_flowmeter");
    });
    
    // ========== HARDWARE SETTINGS ==========
    server.on("/settings_hardware", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Hardware", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("🔧"));
            out.printf("%s</h1>", "Hardware");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Hardware");
            
            out.printf("<div class='alert alert-warning'>⚠️ %s</div>", "Changes require device restart to take effect");
            
            // Board diagram card
            if (strlen(config.theme.boardDiagramPath) > 0) {
                out.print(F("<div class='card'><div class='card-body text-center'>"));
                out.printf("<img src='%s' style='max-width:100%%;height:auto' onerror=\"this.parentElement.parentElement.style.display='none'\">", config.theme.boardDiagramPath);
                out.print(F("</div></div>"));
            }
            
            out.print(F("<form action='/save_hardware' method='POST'>"));
            
            // Storage card
            out.printf("<div class='card'><div class='card-header'>💾 %s</div><div class='card-body'>", "Storage");
            out.print(F("<div class='form-group'><select name='storageType' id='storageType' class='form-input form-select' onchange='toggleSD()'>"));
            out.printf("<option value='0' %s>LittleFS (%s)</option>", config.hardware.storageType == STORAGE_LITTLEFS ? "selected" : "", "Internal");
            out.printf("<option value='1' %s>%s (SPI)</option>", config.hardware.storageType == STORAGE_SD_CARD ? "selected" : "", "SD Card");
            out.print(F("</select></div>"));
            
            String sdDisplay = config.hardware.storageType == STORAGE_SD_CARD ? "block" : "none";
            out.printf("<div id='sdPins' style='display:%s;margin-top:1rem'>", sdDisplay.c_str());
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>CS</label><input type='number' name='pinSdCS' value='%d' class='form-input'></div>", config.hardware.pinSdCS);
            out.printf("<div class='form-group'><label class='form-label'>MOSI</label><input type='number' name='pinSdMOSI' value='%d' class='form-input'></div>", config.hardware.pinSdMOSI);
            out.printf("<div class='form-group'><label class='form-label'>MISO</label><input type='number' name='pinSdMISO' value='%d' class='form-input'></div>", config.hardware.pinSdMISO);
            out.printf("<div class='form-group'><label class='form-label'>SCK</label><input type='number' name='pinSdSCK' value='%d' class='form-input'></div>", config.hardware.pinSdSCK);
            out.print(F("</div></div></div></div>"));
            
            // Wake-up card
            out.printf("<div class='card'><div class='card-header'>😴 %s</div><div class='card-body'>", "Wakeup Mode");
            out.print(F("<div class='form-group'><label class='form-label'>Button Active Level</label><select name='wakeupMode' class='form-input form-select'>"));
            out.printf("<option value='0' %s>Active HIGH (VCC)</option>", config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_HIGH ? "selected" : "");
            out.printf("<option value='1' %s>Active LOW (GND)</option>", config.hardware.wakeupMode == WAKEUP_GPIO_ACTIVE_LOW ? "selected" : "");
            out.print(F("</select><p class='form-hint'>HIGH = button connects to VCC, LOW = button connects to GND</p></div>"));
            out.printf("<div class='form-group'><label class='form-label'>Debounce (ms)</label><input type='number' name='debounceMs' value='%d' min='20' max='500' class='form-input'>", config.hardware.debounceMs);
            out.print(F("<p class='form-hint'>Higher values reduce false triggers but slow response</p></div>"));
            out.print(F("</div></div>"));
            
            // Button & Sensor Pins card
            out.printf("<div class='card'><div class='card-header'>🔘 %s</div><div class='card-body'>", "Pin Configuration");
            out.print(F("<p class='text-muted' style='font-size:.8rem;margin-bottom:1rem'>GPIO pin numbers for ESP32-C3</p>"));
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='pinWifiTrigger' value='%d' class='form-input'></div>", "WiFi Trigger", config.hardware.pinWifiTrigger);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='pinWakeupFF' value='%d' class='form-input'></div>", "Full Flush Btn", config.hardware.pinWakeupFF);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='number' name='pinWakeupPF' value='%d' class='form-input'></div>", "Part Flush Btn", config.hardware.pinWakeupPF);
            out.printf("<div class='form-group'><label class='form-label'>Flow</label><input type='number' name='pinFlowSensor' value='%d' class='form-input'></div>", config.hardware.pinFlowSensor);
            out.print(F("</div></div></div>"));
            
            // RTC Pins card
            out.print(F("<div class='card'><div class='card-header'>🕐 RTC DS1302</div><div class='card-body'>"));
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>CE</label><input type='number' name='pinRtcCE' value='%d' class='form-input'></div>", config.hardware.pinRtcCE);
            out.printf("<div class='form-group'><label class='form-label'>IO</label><input type='number' name='pinRtcIO' value='%d' class='form-input'></div>", config.hardware.pinRtcIO);
            out.printf("<div class='form-group'><label class='form-label'>CLK</label><input type='number' name='pinRtcSCLK' value='%d' class='form-input'></div>", config.hardware.pinRtcSCLK);
            out.print(F("</div></div></div>"));
            
            // CPU card
            out.printf("<div class='card'><div class='card-header'>⚡ %s</div><div class='card-body'>", "CPU Frequency");
            out.print(F("<div class='form-group'><select name='cpuFreqMHz' class='form-input form-select'>"));
            out.printf("<option value='80' %s>80 MHz</option>", config.hardware.cpuFreqMHz == 80 ? "selected" : "");
            out.printf("<option value='160' %s>160 MHz</option>", config.hardware.cpuFreqMHz == 160 ? "selected" : "");
            out.print(F("</select></div></div></div>"));
            
            out.printf("<button type='submit' class='btn btn-primary btn-block' onclick='return confirm(\"Settings will be saved and device will restart. Continue?\")'>💾 %s</button></form>", "Save & Restart");
            
            out.print(F("<script>function toggleSD(){var e=document.getElementById('storageType'),d=document.getElementById('sdPins');d.style.display=e.value=='1'?'block':'none';}</script>"));
        });
    });
    
    server.on("/save_hardware", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Save all hardware settings
        if (r->hasParam("storageType", true))
            config.hardware.storageType = (StorageType)r->getParam("storageType", true)->value().toInt();
        if (r->hasParam("wakeupMode", true))
            config.hardware.wakeupMode = (WakeupMode)r->getParam("wakeupMode", true)->value().toInt();
        if (r->hasParam("pinWifiTrigger", true))
            config.hardware.pinWifiTrigger = r->getParam("pinWifiTrigger", true)->value().toInt();
        if (r->hasParam("pinWakeupFF", true))
            config.hardware.pinWakeupFF = r->getParam("pinWakeupFF", true)->value().toInt();
        if (r->hasParam("pinWakeupPF", true))
            config.hardware.pinWakeupPF = r->getParam("pinWakeupPF", true)->value().toInt();
        if (r->hasParam("pinFlowSensor", true))
            config.hardware.pinFlowSensor = r->getParam("pinFlowSensor", true)->value().toInt();
        if (r->hasParam("pinRtcCE", true))
            config.hardware.pinRtcCE = r->getParam("pinRtcCE", true)->value().toInt();
        if (r->hasParam("pinRtcIO", true))
            config.hardware.pinRtcIO = r->getParam("pinRtcIO", true)->value().toInt();
        if (r->hasParam("pinRtcSCLK", true))
            config.hardware.pinRtcSCLK = r->getParam("pinRtcSCLK", true)->value().toInt();
        if (r->hasParam("pinSdCS", true))
            config.hardware.pinSdCS = r->getParam("pinSdCS", true)->value().toInt();
        if (r->hasParam("pinSdMOSI", true))
            config.hardware.pinSdMOSI = r->getParam("pinSdMOSI", true)->value().toInt();
        if (r->hasParam("pinSdMISO", true))
            config.hardware.pinSdMISO = r->getParam("pinSdMISO", true)->value().toInt();
        if (r->hasParam("pinSdSCK", true))
            config.hardware.pinSdSCK = r->getParam("pinSdSCK", true)->value().toInt();
        if (r->hasParam("cpuFreqMHz", true))
            config.hardware.cpuFreqMHz = r->getParam("cpuFreqMHz", true)->value().toInt();
        if (r->hasParam("debounceMs", true))
            config.hardware.debounceMs = constrain(r->getParam("debounceMs", true)->value().toInt(), 20, 500);
        saveConfig();
        
        sendRestartPage(r, "Device is restarting with new hardware settings.");
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    });

    // ========== THEME SETTINGS ==========
    server.on("/settings_theme", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Theme", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("🎨"));
            out.printf("%s</h1>", "Theme");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Theme");
            
            out.print(F("<form action='/save_theme' method='POST'>"));
            
            // Mode card
            out.printf("<div class='card'><div class='card-header'>🌓 %s</div><div class='card-body'>", "Mode");
            out.print(F("<div class='form-group'><select name='themeMode' class='form-input form-select'>"));
            out.printf("<option value='0' %s>☀️ %s</option>", config.theme.mode == THEME_LIGHT ? "selected" : "", "Light");
            out.printf("<option value='1' %s>🌙 %s</option>", config.theme.mode == THEME_DARK ? "selected" : "", "Dark");
            out.printf("<option value='2' %s>🔄 %s</option>", config.theme.mode == THEME_AUTO ? "selected" : "", "Auto");
            out.print(F("</select></div>"));
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='showIcons' value='1' %s> %s</label></div>", config.theme.showIcons ? "checked" : "", "Show icons");
            out.print(F("</div></div>"));
            
            // Colors card
            out.printf("<div class='card'><div class='card-header'>🎨 %s</div><div class='card-body'>", "Colors");
            out.print(F("<div class='color-grid'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='primaryColor' value='%s' class='form-input'></div>", "Primary", config.theme.primaryColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='secondaryColor' value='%s' class='form-input'></div>", "Secondary", config.theme.secondaryColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='bgColor' value='%s' class='form-input'></div>", "Background", config.theme.bgColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='textColor' value='%s' class='form-input'></div>", "Text", config.theme.textColor);
            out.print(F("</div>"));
            
            out.print(F("<h4 class='mt-2'>Chart</h4>"));
            out.print(F("<div class='color-grid' style='grid-template-columns:repeat(3,1fr)'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='ffColor' value='%s' class='form-input'></div>", "Full Flush Btn", config.theme.ffColor);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='color' name='pfColor' value='%s' class='form-input'></div>", "Part Flush Btn", config.theme.pfColor);
            out.printf("<div class='form-group'><label class='form-label'>Other</label><input type='color' name='otherColor' value='%s' class='form-input'></div>", config.theme.otherColor);
            out.print(F("</div>"));
            
            out.printf("<h4 class='mt-2'>%s</h4>", "Storage");
            out.print(F("<div class='color-grid'>"));
            out.printf("<div class='form-group'><label class='form-label'>Normal</label><input type='color' name='storageBarColor' value='%s' class='form-input'></div>", config.theme.storageBarColor);
            out.printf("<div class='form-group'><label class='form-label'>70%%+</label><input type='color' name='storageBar70Color' value='%s' class='form-input'></div>", config.theme.storageBar70Color);
            out.printf("<div class='form-group'><label class='form-label'>90%%+</label><input type='color' name='storageBar90Color' value='%s' class='form-input'></div>", config.theme.storageBar90Color);
            out.printf("<div class='form-group'><label class='form-label'>Border</label><input type='color' name='storageBarBorder' value='%s' class='form-input'></div>", config.theme.storageBarBorder);
            out.print(F("</div></div></div>"));
            
            // Images card
            out.printf("<div class='card'><div class='card-header'>🖼️ %s</div><div class='card-body'>", "Images");
            out.print(F("<p class='text-muted' style='font-size:.8rem;margin-bottom:1rem'>Supported: PNG, JPG, GIF, SVG, ICO. Path: /file.png or /folder/file.png</p>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='logoSource' value='%s' placeholder='/logo.png' class='form-input'></div>", "Logo Path", config.theme.logoSource);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='faviconPath' value='%s' placeholder='/favicon.ico' class='form-input'></div>", "Favicon", config.theme.faviconPath);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='boardDiagramPath' value='%s' placeholder='/board.png' class='form-input'></div>", "Board Diagram", config.theme.boardDiagramPath);
            out.print(F("</div></div>"));
            
            // Chart.js card
            out.printf("<div class='card'><div class='card-header'>📈 %s</div><div class='card-body'>", "Chart Source");
            out.print(F("<div class='form-group'><label class='form-label'>Chart.js Library</label><select name='chartSource' id='chartSource' class='form-input form-select' onchange='toggleChartPath()'>"));
            out.printf("<option value='0' %s>📁 %s</option>", config.theme.chartSource == CHART_LOCAL ? "selected" : "", "Local File");
            out.printf("<option value='1' %s>🌐 %s</option>", config.theme.chartSource == CHART_CDN ? "selected" : "", "CDN (Online)");
            out.print(F("</select><p class='form-hint'>Local: requires /chart.min.js file, CDN: needs internet</p></div>"));
            String localDisplay = config.theme.chartSource == CHART_LOCAL ? "block" : "none";
            out.printf("<div id='chartLocalPath' style='display:%s'>", localDisplay.c_str());
            out.printf("<div class='form-group'><label class='form-label'>Local Path</label><input type='text' name='chartLocalPath' value='%s' placeholder='/chart.min.js' class='form-input'></div>", config.theme.chartLocalPath);
            out.print(F("</div>"));
            out.print(F("<script>function toggleChartPath(){document.getElementById('chartLocalPath').style.display=document.getElementById('chartSource').value=='0'?'block':'none';}</script>"));
            
            // Dashboard column label format
            out.print(F("<div class='form-group mt-1'><label class='form-label'>Dashboard Column Labels</label>"));
            out.print(F("<select name='chartLabelFormat' class='form-input form-select'>"));
            out.printf("<option value='0' %s>📅 Date + Time</option>", config.theme.chartLabelFormat == LABEL_DATETIME ? "selected" : "");
            out.printf("<option value='1' %s>🔢 Boot Count (#1234)</option>", config.theme.chartLabelFormat == LABEL_BOOTCOUNT ? "selected" : "");
            out.printf("<option value='2' %s>📅🔢 Both (Date Time #Boot)</option>", config.theme.chartLabelFormat == LABEL_BOTH ? "selected" : "");
            out.print(F("</select><p class='form-hint'>How to label bars in dashboard chart</p></div>"));
            out.print(F("</div></div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>💾 Save</button></form>"));
        });
    });
    
    server.on("/save_theme", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("themeMode", true))
            config.theme.mode = (ThemeMode)r->getParam("themeMode", true)->value().toInt();
        config.theme.showIcons = r->hasParam("showIcons", true);
        if (r->hasParam("primaryColor", true))
            strncpy(config.theme.primaryColor, r->getParam("primaryColor", true)->value().c_str(), 7);
        if (r->hasParam("secondaryColor", true))
            strncpy(config.theme.secondaryColor, r->getParam("secondaryColor", true)->value().c_str(), 7);
        if (r->hasParam("bgColor", true))
            strncpy(config.theme.bgColor, r->getParam("bgColor", true)->value().c_str(), 7);
        if (r->hasParam("textColor", true))
            strncpy(config.theme.textColor, r->getParam("textColor", true)->value().c_str(), 7);
        if (r->hasParam("ffColor", true))
            strncpy(config.theme.ffColor, r->getParam("ffColor", true)->value().c_str(), 7);
        if (r->hasParam("pfColor", true))
            strncpy(config.theme.pfColor, r->getParam("pfColor", true)->value().c_str(), 7);
        if (r->hasParam("otherColor", true))
            strncpy(config.theme.otherColor, r->getParam("otherColor", true)->value().c_str(), 7);
        if (r->hasParam("storageBarColor", true))
            strncpy(config.theme.storageBarColor, r->getParam("storageBarColor", true)->value().c_str(), 7);
        if (r->hasParam("storageBar70Color", true))
            strncpy(config.theme.storageBar70Color, r->getParam("storageBar70Color", true)->value().c_str(), 7);
        if (r->hasParam("storageBar90Color", true))
            strncpy(config.theme.storageBar90Color, r->getParam("storageBar90Color", true)->value().c_str(), 7);
        if (r->hasParam("storageBarBorder", true))
            strncpy(config.theme.storageBarBorder, r->getParam("storageBarBorder", true)->value().c_str(), 7);
        if (r->hasParam("logoSource", true))
            strncpy(config.theme.logoSource, r->getParam("logoSource", true)->value().c_str(), 128);
        if (r->hasParam("faviconPath", true))
            strncpy(config.theme.faviconPath, r->getParam("faviconPath", true)->value().c_str(), 32);
        if (r->hasParam("boardDiagramPath", true))
            strncpy(config.theme.boardDiagramPath, r->getParam("boardDiagramPath", true)->value().c_str(), 64);
        if (r->hasParam("chartSource", true))
            config.theme.chartSource = (ChartSource)r->getParam("chartSource", true)->value().toInt();
        if (r->hasParam("chartLocalPath", true))
            strncpy(config.theme.chartLocalPath, r->getParam("chartLocalPath", true)->value().c_str(), 64);
        if (r->hasParam("chartLabelFormat", true))
            config.theme.chartLabelFormat = (ChartLabelFormat)r->getParam("chartLabelFormat", true)->value().toInt();
        
        saveConfig();
        r->redirect("/settings_theme");
    });
    
    // ========== TIME SYNC SETTINGS ==========
    server.on("/settings_time", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Time", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("🕐"));
            out.printf("%s</h1>", "Time Sync");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Time Sync");
            
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            // RTC Status card
            out.printf("<div class='card'><div class='card-header'>📟 %s</div><div class='card-body'>", "RTC Status");
            RtcDateTime now;
            bool timeValid = false;
            if (Rtc) {
                now = Rtc->GetDateTime();
                timeValid = (now.Year() >= 2020 && now.Year() <= 2100 && now.Month() >= 1 && now.Day() >= 1);
            }
            
            if (!Rtc) {
                out.printf("<div class='alert alert-error'>❌ RTC %s</div>", "Error");
            } else if (!timeValid) {
                out.printf("<div class='alert alert-warning'>⚠️ %s</div>", "Warning");
            } else {
                out.printf("<div class='alert alert-success'>✅ %s</div>", "Success");
            }
            
            out.print(F("<div class='text-center' style='font-size:2.5rem;font-family:monospace;font-weight:bold;margin:1rem 0'>"));
            out.print(getRtcDateTimeString());
            out.print(F("</div>"));
            
            if (Rtc) {
                out.print(F("<p class='text-muted text-center' style='font-size:0.8rem'>"));
                out.printf("Protected: %s | Running: %s",
                    Rtc->GetIsWriteProtected() ? "Yes" : "No",
                    Rtc->GetIsRunning() ? "Yes" : "No");
                out.print(F("</p>"));
            }
            out.print(F("</div></div>"));
            
            // Manual Set card
            out.printf("<div class='card card-accent' style='border-top-color:#27ae60'><div class='card-header'>⏰ %s</div><div class='card-body'>", "Set Time");
            out.print(F("<form action='/set_time' method='POST'>"));
            out.print(F("<div class='form-row'>"));
            out.print(F("<div class='form-group'><label class='form-label'>Date</label><input type='date' name='date' required class='form-input'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='time' name='time' required class='form-input'></div>", "Time");
            out.printf("</div><button type='submit' class='btn btn-success btn-block'>⏰ %s</button></form>", "Set Time");
            out.print(F("</div></div>"));
            
            // NTP Sync card
            out.printf("<div class='card'><div class='card-header'>🌐 %s</div><div class='card-body'>", "NTP Sync");
            
            if (wifiConnectedAsClient) {
                out.printf("<div class='alert alert-success'>✅ %s</div>", "Connected");
                out.print(F("<form action='/sync_time' method='POST' style='margin-bottom:1rem'>"));
                out.printf("<button type='submit' class='btn btn-primary'>🔄 %s</button></form>", "NTP Sync");
            } else {
                out.printf("<div class='alert alert-warning'>⚠️ %s</div>", "Disconnected");
            }
            
            out.print(F("<form action='/save_time' method='POST'>"));
            out.print(F("<div class='form-row'>"));
            out.print(F("<div class='form-group'><label class='form-label'>NTP Server</label><input type='text' name='ntpServer' value='"));
            out.print(config.network.ntpServer);
            out.print(F("' class='form-input'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s (UTC+)</label><input type='number' name='timezone' value='%d' min='-12' max='14' class='form-input'></div>", "Timezone", config.network.timezone);
            out.print(F("</div>"));
            out.printf("<button type='submit' class='btn btn-secondary'>💾 %s</button>", "Save");
            out.print(F("</form></div></div>"));
            
            // RTC Options card
            out.print(F("<div class='card'><div class='card-header'>⚙️ RTC & System Options</div><div class='card-body'>"));
            
            // Write Protection
            if (Rtc) {
                out.print(F("<form action='/rtc_protect' method='POST' style='margin-bottom:1rem'>"));
                out.print(F("<div class='form-group'><label class='form-label'>"));
                out.printf("<input type='checkbox' name='protect' value='1' %s onchange='this.form.submit()'> ", 
                    Rtc->GetIsWriteProtected() ? "checked" : "");
                out.print(F("RTC Write Protected</label>"));
                out.print(F("<p class='form-hint'>When enabled, prevents accidental time changes</p>"));
                out.print(F("</div></form>"));
            }
            
            // Flush Log Buffer
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:1rem;padding:0.75rem;background:var(--bg);border-radius:8px'>"));
            out.print(F("<div><strong>Log Buffer</strong><p class='form-hint' style='margin:0'>Write pending logs to storage</p></div>"));
            out.print(F("<form action='/flush_logs' method='POST'><button type='submit' class='btn btn-secondary'>💾 Flush</button></form>"));
            out.print(F("</div>"));
            
            // Boot Count section
            out.print(F("<div style='padding:0.75rem;background:var(--bg);border-radius:8px'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:0.75rem'>"));
            out.printf("<div><strong>Boot Count:</strong> <span style='font-size:1.25rem;color:var(--primary)'>%d</span></div>", bootCount);
            
            // Read backup value
            uint32_t backupValue = 0;
            File bf = LittleFS.open(BOOTCOUNT_BACKUP_FILE, "r");
            if (bf) { bf.read((uint8_t*)&backupValue, sizeof(backupValue)); bf.close(); }
            out.printf("<div><strong>Backup:</strong> <span style='font-size:1.25rem;color:var(--secondary)'>%d</span></div>", backupValue);
            out.print(F("</div>"));
            
            out.print(F("<div style='display:flex;gap:0.5rem;flex-wrap:wrap'>"));
            out.print(F("<form action='/backup_bootcount' method='POST'><button type='submit' class='btn btn-secondary'>📤 Backup Now</button></form>"));
            out.print(F("<form action='/restore_bootcount' method='POST' onsubmit='return confirm(\"Restore boot count from backup?\")'><button type='submit' class='btn btn-warning'>📥 Restore</button></form>"));
            out.print(F("</div>"));
            out.print(F("<p class='form-hint' style='margin-top:0.5rem'>Backup saves to RTC RAM + flash file</p>"));
            out.print(F("</div>"));
            
            out.print(F("</div></div>"));
        });
    });
    
    server.on("/save_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("ntpServer", true))
            strncpy(config.network.ntpServer, r->getParam("ntpServer", true)->value().c_str(), 64);
        if (r->hasParam("timezone", true))
            config.network.timezone = r->getParam("timezone", true)->value().toInt();
        saveConfig();
        r->redirect("/settings_time");
    });
    
    server.on("/sync_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (syncTimeFromNTP()) {
            rtcValid = true;
            statusMessage = "<div class='alert alert-success'>✅ Time synced successfully!</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>❌ Sync failed - check network connection</div>";
        }
        r->redirect("/settings_time");
    });
    
    server.on("/set_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Block time setting during active measurement cycle
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            statusMessage = "<div class='alert alert-warning'>⚠️ Cannot set time during active measurement</div>";
            r->redirect("/settings_time");
            return;
        }
        
        if (r->hasParam("date", true) && r->hasParam("time", true) && Rtc) {
            String dateStr = r->getParam("date", true)->value();  // YYYY-MM-DD
            String timeStr = r->getParam("time", true)->value();  // HH:MM
            
            int year = dateStr.substring(0, 4).toInt();
            int month = dateStr.substring(5, 7).toInt();
            int day = dateStr.substring(8, 10).toInt();
            int hour = timeStr.substring(0, 2).toInt();
            int minute = timeStr.substring(3, 5).toInt();
            
            Serial.printf("Setting RTC: %04d-%02d-%02d %02d:%02d:00\n", year, month, day, hour, minute);
            
            RtcDateTime dt(year, month, day, hour, minute, 0);
            bool success = false;
            
            // Multiple attempts to set time
            for (int attempt = 0; attempt < 3 && !success; attempt++) {
                Rtc->SetIsWriteProtected(false);
                delay(10);
                Rtc->SetIsRunning(true);
                delay(10);
                Rtc->SetDateTime(dt);
                delay(100);
                
                // Verify
                RtcDateTime verify = Rtc->GetDateTime();
                Serial.printf("Attempt %d: Read back %04d-%02d-%02d\n", 
                    attempt + 1, verify.Year(), verify.Month(), verify.Day());
                
                if (verify.Year() == year && verify.Month() == month && verify.Day() == day) {
                    success = true;
                    rtcValid = true;
                }
            }
            
            if (success) {
                statusMessage = "<div class='alert alert-success'>✅ Time set successfully!</div>";
            } else {
                statusMessage = "<div class='alert alert-error'>❌ Failed to set time - check RTC connection</div>";
            }
        } else {
            statusMessage = "<div class='alert alert-error'>❌ Invalid date/time or RTC not available</div>";
        }
        r->redirect("/settings_time");
    });
    
    // RTC Write Protection toggle
    server.on("/rtc_protect", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (Rtc) {
            bool protect = r->hasParam("protect", true);
            Rtc->SetIsWriteProtected(protect);
            statusMessage = protect ? 
                "<div class='alert alert-success'>🔒 RTC write protection enabled</div>" :
                "<div class='alert alert-success'>🔓 RTC write protection disabled</div>";
        }
        r->redirect("/settings_time");
    });
    
    // Flush log buffer
    server.on("/flush_logs", HTTP_POST, [](AsyncWebServerRequest *r) {
        flushLogBufferToFS();
        statusMessage = "<div class='alert alert-success'>✅ Log buffer flushed to storage</div>";
        r->redirect("/settings_time");
    });
    
    // Backup boot count
    server.on("/backup_bootcount", HTTP_POST, [](AsyncWebServerRequest *r) {
        backupBootCount();
        statusMessage = "<div class='alert alert-success'>✅ Boot count backed up</div>";
        r->redirect("/settings_time");
    });
    
    // Restore boot count from backup
    server.on("/restore_bootcount", HTTP_POST, [](AsyncWebServerRequest *r) {
        uint32_t oldValue = bootCount;
        restoreBootCount();
        char msg[128];
        snprintf(msg, sizeof(msg), "<div class='alert alert-success'>✅ Boot count restored: %d → %d</div>", oldValue, bootCount);
        statusMessage = String(msg);
        r->redirect("/settings_time");
    });
    
    // ========== NETWORK SETTINGS ==========
    server.on("/settings_network", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Network", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("🌐"));
            out.printf("%s</h1>", "Network");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Network");
            
            // Status card
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem'>"));
            out.print(F("<span><strong>Status:</strong> "));
            if (wifiConnectedAsClient) {
                out.printf("%s: %s", "Connected", connectedSSID.c_str());
            } else {
                out.printf("%s", "AP Mode");
            }
            out.print(F("</span>"));
            out.print(F("<span><strong>IP:</strong> "));
            out.print(currentIPAddress);
            out.print(F("</span></div></div></div>"));
            
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            out.print(F("<form action='/save_network' method='POST'>"));
            
            // WiFi Mode card
            out.printf("<div class='card'><div class='card-header'>📡 %s</div><div class='card-body'>", "WiFi Mode");
            out.print(F("<div class='form-group'><select name='wifiMode' id='wifiMode' class='form-input form-select' onchange='toggleMode()'>"));
            out.printf("<option value='0' %s>📶 %s (AP)</option>", config.network.wifiMode == WIFIMODE_AP ? "selected" : "", "AP Mode");
            out.printf("<option value='1' %s>🔗 %s</option>", config.network.wifiMode == WIFIMODE_CLIENT ? "selected" : "", "Client Mode");
            out.print(F("</select></div></div></div>"));
            
            // AP Settings card
            String apDisplay = config.network.wifiMode == WIFIMODE_AP ? "block" : "none";
            out.printf("<div id='apSection' style='display:%s'>", apDisplay.c_str());
            out.printf("<div class='card'><div class='card-header'>📶 %s</div><div class='card-body'>", "AP Mode");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='apSSID' value='%s' placeholder='WaterLogger' class='form-input'></div>", "SSID", config.network.apSSID);
            out.printf("<div class='form-group'><label class='form-label'>%s</label><div style='display:flex;gap:8px'>", "Password");
            out.printf("<input type='password' name='apPassword' id='apPass' value='%s' class='form-input' style='flex:1'>", config.network.apPassword);
            out.print(F("<button type='button' class='btn btn-secondary' onclick=\"togglePass('apPass')\">👁️</button></div></div>"));
            out.print(F("</div></div>"));
            
            // AP Network Properties card
            out.print(F("<div class='card'><div class='card-header'>🔧 Network Properties</div><div class='card-body'>"));
            
            // Current AP info (read-only)
            if (!wifiConnectedAsClient) {
                out.print(F("<div class='mb-1' style='padding:10px;background:var(--bg);border-radius:var(--radius-sm);font-size:0.9rem'>"));
                out.printf("<strong>Current:</strong> IP: %s | ", WiFi.softAPIP().toString().c_str());
                out.printf("Clients: %d", WiFi.softAPgetStationNum());
                out.print(F("</div>"));
            }
            
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>IP Address</label><input type='text' name='apIP' value='%d.%d.%d.%d' class='form-input'></div>",
                config.network.apIP[0], config.network.apIP[1], config.network.apIP[2], config.network.apIP[3]);
            out.printf("<div class='form-group'><label class='form-label'>Gateway</label><input type='text' name='apGateway' value='%d.%d.%d.%d' class='form-input'></div>",
                config.network.apGateway[0], config.network.apGateway[1], config.network.apGateway[2], config.network.apGateway[3]);
            out.printf("<div class='form-group'><label class='form-label'>Subnet Mask</label><input type='text' name='apSubnet' value='%d.%d.%d.%d' class='form-input'></div>",
                config.network.apSubnet[0], config.network.apSubnet[1], config.network.apSubnet[2], config.network.apSubnet[3]);
            out.print(F("</div></div></div>"));
            out.print(F("</div>"));
            
            // Client Settings card
            String clientDisplay = config.network.wifiMode == WIFIMODE_CLIENT ? "block" : "none";
            out.printf("<div id='clientSection' style='display:%s'>", clientDisplay.c_str());
            out.printf("<div class='card'><div class='card-header'>🔗 %s</div><div class='card-body'>", "Client Mode");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><div style='display:flex;gap:8px'>", "SSID");
            out.printf("<input type='text' name='clientSSID' id='clientSSID' value='%s' class='form-input' style='flex:1'>", config.network.clientSSID);
            out.print(F("<button type='button' class='btn btn-secondary' onclick='scanWifi()'>🔍</button></div></div>"));
            out.print(F("<div id='wifiList' style='display:none;margin-bottom:1rem;max-height:200px;overflow-y:auto;border:1px solid var(--border);border-radius:8px'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><div style='display:flex;gap:8px'>", "Password");
            out.printf("<input type='password' name='clientPassword' id='clientPass' value='%s' class='form-input' style='flex:1'>", config.network.clientPassword);
            out.print(F("<button type='button' class='btn btn-secondary' onclick=\"togglePass('clientPass')\">👁️</button></div></div>"));
            out.print(F("</div></div>"));
            
            // Client Network Properties card
            out.print(F("<div class='card'><div class='card-header'>🔧 Network Properties</div><div class='card-body'>"));
            
            // IP fields - show current values when connected, otherwise show static config
            out.print(F("<div class='form-row' id='staticFields'>"));
            if (wifiConnectedAsClient && !config.network.useStaticIP) {
                // Show current DHCP values
                out.printf("<div class='form-group'><label class='form-label'>IP Address</label><input type='text' name='staticIP' id='fldIP' value='%s' class='form-input'></div>",
                    WiFi.localIP().toString().c_str());
                out.printf("<div class='form-group'><label class='form-label'>Gateway</label><input type='text' name='gateway' id='fldGW' value='%s' class='form-input'></div>",
                    WiFi.gatewayIP().toString().c_str());
                out.printf("<div class='form-group'><label class='form-label'>Subnet Mask</label><input type='text' name='subnet' id='fldSN' value='%s' class='form-input'></div>",
                    WiFi.subnetMask().toString().c_str());
                out.printf("<div class='form-group'><label class='form-label'>DNS Server</label><input type='text' name='dns' id='fldDNS' value='%s' class='form-input'></div>",
                    WiFi.dnsIP().toString().c_str());
            } else {
                // Show static config values
                out.printf("<div class='form-group'><label class='form-label'>IP Address</label><input type='text' name='staticIP' id='fldIP' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.staticIP[0], config.network.staticIP[1], config.network.staticIP[2], config.network.staticIP[3]);
                out.printf("<div class='form-group'><label class='form-label'>Gateway</label><input type='text' name='gateway' id='fldGW' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.gateway[0], config.network.gateway[1], config.network.gateway[2], config.network.gateway[3]);
                out.printf("<div class='form-group'><label class='form-label'>Subnet Mask</label><input type='text' name='subnet' id='fldSN' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.subnet[0], config.network.subnet[1], config.network.subnet[2], config.network.subnet[3]);
                out.printf("<div class='form-group'><label class='form-label'>DNS Server</label><input type='text' name='dns' id='fldDNS' value='%d.%d.%d.%d' class='form-input'></div>",
                    config.network.dns[0], config.network.dns[1], config.network.dns[2], config.network.dns[3]);
            }
            out.print(F("</div>"));
            
            // Static IP checkbox below fields
            out.printf("<div class='form-group mt-1'><label class='form-label'><input type='checkbox' name='useStaticIP' id='staticCheck' value='1' %s onchange='toggleStatic()'> Use Static IP (enable editing)</label></div>", 
                config.network.useStaticIP ? "checked" : "");
            
            out.print(F("</div></div>"));
            out.print(F("</div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>💾 Save & Restart</button></form>"));
            
            // JavaScript
            out.print(F("<script>"));
            out.print(F("function toggleMode(){var m=document.getElementById('wifiMode').value;"));
            out.print(F("document.getElementById('apSection').style.display=m=='0'?'block':'none';"));
            out.print(F("document.getElementById('clientSection').style.display=m=='1'?'block':'none';}"));
            out.print(F("function togglePass(id){var e=document.getElementById(id);e.type=e.type=='password'?'text':'password';}"));
            out.print(F("function toggleStatic(){"));
            out.print(F("var en=document.getElementById('staticCheck').checked;"));
            out.print(F("['fldIP','fldGW','fldSN','fldDNS'].forEach(function(id){"));
            out.print(F("var el=document.getElementById(id);"));
            out.print(F("el.disabled=!en;"));
            out.print(F("el.style.opacity=en?'1':'0.5';"));
            out.print(F("el.style.cursor=en?'text':'not-allowed';"));
            out.print(F("});"));
            out.print(F("}toggleStatic();"));  // Call on page load
            out.print(F("var scanRetries=0;"));
            out.print(F("function scanWifi(){"));
            out.print(F("var l=document.getElementById('wifiList');l.innerHTML='<div class=\"list-item\">🔍 Scanning...</div>';l.style.display='block';scanRetries=0;"));
            out.print(F("fetch('/wifi_scan_start').then(()=>setTimeout(checkScanResult,2000));}"));
            out.print(F("function checkScanResult(){"));
            out.print(F("fetch('/wifi_scan_result').then(r=>r.json()).then(d=>{"));
            out.print(F("var l=document.getElementById('wifiList');"));
            out.print(F("if(d.scanning){scanRetries++;if(scanRetries<10){l.innerHTML='<div class=\"list-item\">🔍 Scanning... ('+scanRetries+')</div>';setTimeout(checkScanResult,1000);}else{l.innerHTML='<div class=\"list-item\">⏱️ Scan timeout</div>';}}"));
            out.print(F("else if(d.error){l.innerHTML='<div class=\"list-item\">❌ '+d.error+'</div>';}"));
            out.print(F("else if(d.networks.length==0){l.innerHTML='<div class=\"list-item\">📡 No networks found</div>';}"));
            out.print(F("else{var h='';d.networks.forEach(n=>{h+='<div class=\"list-item\" style=\"cursor:pointer\" onclick=\"selectWifi(\\''+n.ssid.replace(/'/g,\"\\\\'\")+'\\')\">';"));
            out.print(F("h+=(n.secure?'🔒':'📶')+' '+n.ssid+' <small class=\"text-muted\">('+n.rssi+' dBm)</small></div>';});l.innerHTML=h;}"));
            out.print(F("}).catch(e=>{document.getElementById('wifiList').innerHTML='<div class=\"list-item\">❌ Error: '+e+'</div>';});}"));
            out.print(F("function selectWifi(ssid){document.getElementById('clientSSID').value=ssid;document.getElementById('wifiList').style.display='none';}"));
            out.print(F("</script>"));
        });
    });
    
    server.on("/save_network", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("wifiMode", true))
            config.network.wifiMode = (WiFiModeType)r->getParam("wifiMode", true)->value().toInt();
        if (r->hasParam("apSSID", true))
            strncpy(config.network.apSSID, r->getParam("apSSID", true)->value().c_str(), 32);
        if (r->hasParam("apPassword", true))
            strncpy(config.network.apPassword, r->getParam("apPassword", true)->value().c_str(), 64);
        if (r->hasParam("clientSSID", true))
            strncpy(config.network.clientSSID, r->getParam("clientSSID", true)->value().c_str(), 32);
        if (r->hasParam("clientPassword", true))
            strncpy(config.network.clientPassword, r->getParam("clientPassword", true)->value().c_str(), 64);
        config.network.useStaticIP = r->hasParam("useStaticIP", true);
        
        // Parse Static IP settings
        if (r->hasParam("staticIP", true)) {
            String ip = r->getParam("staticIP", true)->value();
            sscanf(ip.c_str(), "%hhu.%hhu.%hhu.%hhu", 
                &config.network.staticIP[0], &config.network.staticIP[1],
                &config.network.staticIP[2], &config.network.staticIP[3]);
        }
        if (r->hasParam("gateway", true)) {
            String gw = r->getParam("gateway", true)->value();
            sscanf(gw.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.gateway[0], &config.network.gateway[1],
                &config.network.gateway[2], &config.network.gateway[3]);
        }
        if (r->hasParam("subnet", true)) {
            String sn = r->getParam("subnet", true)->value();
            sscanf(sn.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.subnet[0], &config.network.subnet[1],
                &config.network.subnet[2], &config.network.subnet[3]);
        }
        if (r->hasParam("dns", true)) {
            String dns = r->getParam("dns", true)->value();
            sscanf(dns.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.dns[0], &config.network.dns[1],
                &config.network.dns[2], &config.network.dns[3]);
        }
        
        // Parse AP IP settings
        if (r->hasParam("apIP", true)) {
            String ip = r->getParam("apIP", true)->value();
            sscanf(ip.c_str(), "%hhu.%hhu.%hhu.%hhu", 
                &config.network.apIP[0], &config.network.apIP[1],
                &config.network.apIP[2], &config.network.apIP[3]);
        }
        if (r->hasParam("apGateway", true)) {
            String gw = r->getParam("apGateway", true)->value();
            sscanf(gw.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.apGateway[0], &config.network.apGateway[1],
                &config.network.apGateway[2], &config.network.apGateway[3]);
        }
        if (r->hasParam("apSubnet", true)) {
            String sn = r->getParam("apSubnet", true)->value();
            sscanf(sn.c_str(), "%hhu.%hhu.%hhu.%hhu",
                &config.network.apSubnet[0], &config.network.apSubnet[1],
                &config.network.apSubnet[2], &config.network.apSubnet[3]);
        }
        
        saveConfig();
        
        sendRestartPage(r, "Device is restarting with new network settings.");
        safeWiFiShutdown();
        delay(100);
        ESP.restart();
    });
    
    // ========== DATALOG SETTINGS ==========
    server.on("/settings_datalog", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Data Log", [](Print& out) {
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("📝"));
            out.printf("%s</h1>", "Data Log");
            out.printf("<div class='breadcrumb'><a href='/settings'>%s</a> / %s</div></div>", "Settings", "Data Log");
            
            if (statusMessage.length() > 0) {
                out.print(statusMessage);
                statusMessage = "";
            }
            
            out.print(F("<form action='/save_datalog' method='POST'>"));
            
            // Active Log File card
            out.printf("<div class='card'><div class='card-header'>📄 %s</div><div class='card-body'>", "Current File");
            out.printf("<div class='form-group'><label class='form-label'>%s</label><select name='currentFile' class='form-input form-select'>", "Select file");
            out.print(generateDatalogFileOptions());
            out.print(F("</select></div>"));
            
            out.print(F("<div class='form-row mt-1'>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='prefix' value='", "Prefix");
            out.print(config.datalog.prefix);
            out.print(F("' placeholder='datalog' class='form-input'></div>"));
            out.printf("<div class='form-group'><label class='form-label'>%s</label><input type='text' name='folder' value='%s' placeholder='/' class='form-input'>", "Folder", config.datalog.folder);
            out.print(F("<p class='text-muted' style='font-size:.75rem;margin-top:2px'>Leave empty for root, or /logs for subfolder</p></div>"));
            out.print(F("</div>"));
            out.printf("<button type='submit' name='action' value='create' class='btn btn-success'>➕ %s</button>", "Create");
            out.print(F("</div></div>"));
            
            // Rotation settings card
            out.printf("<div class='card'><div class='card-header'>🔄 %s</div><div class='card-body'>", "Rotation");
            out.print(F("<p class='text-muted' style='font-size:.8rem;margin-bottom:0.75rem'>How to split log files over time</p>"));
            out.print(F("<div class='form-row'>"));
            out.print(F("<div class='form-group'><select name='rotation' id='rotation' class='form-input form-select' onchange='toggleMaxSize()'>"));
            out.printf("<option value='0' %s>%s</option>", config.datalog.rotation == ROTATION_NONE ? "selected" : "", "None");
            out.printf("<option value='1' %s>%s</option>", config.datalog.rotation == ROTATION_DAILY ? "selected" : "", "Daily");
            out.printf("<option value='2' %s>%s</option>", config.datalog.rotation == ROTATION_WEEKLY ? "selected" : "", "Weekly");
            out.printf("<option value='3' %s>%s</option>", config.datalog.rotation == ROTATION_MONTHLY ? "selected" : "", "Monthly");
            out.printf("<option value='4' %s>By Size</option>", config.datalog.rotation == ROTATION_SIZE ? "selected" : "");
            out.print(F("</select></div>"));
            String maxSizeDisplay = config.datalog.rotation == ROTATION_SIZE ? "block" : "none";
            out.printf("<div class='form-group' id='maxSizeGroup' style='display:%s'><label class='form-label'>%s (KB)</label><input type='number' name='maxSizeKB' value='%d' min='10' max='10000' class='form-input'></div>", maxSizeDisplay.c_str(), "Max Size", config.datalog.maxSizeKB);
            out.print(F("</div>"));
            
            out.print(F("<div class='form-group'><label class='form-label'><input type='checkbox' name='timestampFilename' value='1' "));
            out.print(config.datalog.timestampFilename ? "checked" : "");
            out.print(F("> Timestamp filename</label></div>"));
            
            out.print(F("<div class='form-group'><label class='form-label'><input type='checkbox' name='includeDeviceId' value='1' "));
            out.print(config.datalog.includeDeviceId ? "checked" : "");
            out.print(F("> Device ID in filename</label></div>"));
            out.print(F("</div></div>"));
            
            // Format Configuration card
            out.print(F("<div class='card'><div class='card-header'>⚙️ Log Format</div><div class='card-body'>"));
            out.print(F("<div style='display:flex;flex-wrap:wrap;gap:8px;align-items:flex-end'>"));
            
            // Date format
            out.print(F("<div class='form-group' style='min-width:100px'><label class='form-label'>Date</label>"));
            out.print(F("<select name='dateFormat' id='dfDate' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>Off</option>", config.datalog.dateFormat == DATE_OFF ? "selected" : "");
            out.printf("<option value='1' %s>DD/MM/YYYY</option>", config.datalog.dateFormat == DATE_DDMMYYYY ? "selected" : "");
            out.printf("<option value='2' %s>MM/DD/YYYY</option>", config.datalog.dateFormat == DATE_MMDDYYYY ? "selected" : "");
            out.printf("<option value='3' %s>YYYY-MM-DD</option>", config.datalog.dateFormat == DATE_YYYYMMDD ? "selected" : "");
            out.printf("<option value='4' %s>DD.MM.YYYY</option>", config.datalog.dateFormat == DATE_DDMMYYYY_DOT ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Time format
            out.print(F("<div class='form-group' style='min-width:90px'><label class='form-label'>Start</label>"));
            out.print(F("<select name='timeFormat' id='dfTime' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>HH:MM:SS</option>", config.datalog.timeFormat == TIME_HHMMSS ? "selected" : "");
            out.printf("<option value='1' %s>HH:MM</option>", config.datalog.timeFormat == TIME_HHMM ? "selected" : "");
            out.printf("<option value='2' %s>12h AM/PM</option>", config.datalog.timeFormat == TIME_12H ? "selected" : "");
            out.print(F("</select></div>"));
            
            // End format
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>End</label>"));
            out.print(F("<select name='endFormat' id='dfEnd' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>Time</option>", config.datalog.endFormat == END_TIME ? "selected" : "");
            out.printf("<option value='1' %s>Duration</option>", config.datalog.endFormat == END_DURATION ? "selected" : "");
            out.printf("<option value='2' %s>Off</option>", config.datalog.endFormat == END_OFF ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Boot count
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>Boot#</label>"));
            out.print(F("<select name='includeBootCount' id='dfBoot' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='1' %s>On</option>", config.datalog.includeBootCount ? "selected" : "");
            out.printf("<option value='0' %s>Off</option>", !config.datalog.includeBootCount ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Trigger (always on, just info)
            out.print(F("<div class='form-group' style='min-width:60px'><label class='form-label'>Trigger</label>"));
            out.print(F("<div style='padding:10px 12px;border-radius:var(--radius-sm);background:var(--border);color:var(--text-muted);text-align:center;font-size:1rem'>On</div></div>"));
            
            // Volume format
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>Volume</label>"));
            out.print(F("<select name='volumeFormat' id='dfVol' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='0' %s>L:0,00</option>", config.datalog.volumeFormat == VOL_L_COMMA ? "selected" : "");
            out.printf("<option value='1' %s>L:0.00</option>", config.datalog.volumeFormat == VOL_L_DOT ? "selected" : "");
            out.printf("<option value='2' %s>0.00</option>", config.datalog.volumeFormat == VOL_NUM_ONLY ? "selected" : "");
            out.printf("<option value='3' %s>Off</option>", config.datalog.volumeFormat == VOL_OFF ? "selected" : "");
            out.print(F("</select></div>"));
            
            // Extra presses
            out.print(F("<div class='form-group' style='min-width:80px'><label class='form-label'>FF/PF</label>"));
            out.print(F("<select name='includeExtraPresses' id='dfExtra' class='form-input form-select' onchange='updatePreview()'>"));
            out.printf("<option value='1' %s>On</option>", config.datalog.includeExtraPresses ? "selected" : "");
            out.printf("<option value='0' %s>Off</option>", !config.datalog.includeExtraPresses ? "selected" : "");
            out.print(F("</select></div>"));
            
            out.print(F("</div>"));
            
            // Preview line
            out.print(F("<div class='mt-1'><label class='form-label'>Preview:</label>"));
            out.print(F("<div id='logPreview' style='font-family:monospace;font-size:0.85rem;padding:10px 12px;border-radius:var(--radius-sm);background:var(--bg);color:var(--text);border:1px solid var(--border);overflow-x:auto;white-space:nowrap'></div>"));
            out.print(F("</div></div></div>"));
            
            // Post-Correction card
            out.print(F("<div class='card'><div class='card-header'>🔄 Post-Correction</div><div class='card-body'>"));
            out.printf("<div class='form-group'><label class='form-label'><input type='checkbox' name='postCorrectionEnabled' value='1' %s "
                "onchange=\"document.getElementById('pcFields').style.display=this.checked?'block':'none'\"> Enable Post-Correction</label></div>",
                config.datalog.postCorrectionEnabled ? "checked" : "");
            out.printf("<div id='pcFields' style='display:%s'>", config.datalog.postCorrectionEnabled ? "block" : "none");
            out.print(F("<div class='form-row'>"));
            out.printf("<div class='form-group'><label class='form-label'>PF → FF Threshold (L)</label>"
                "<input type='number' step='0.1' name='pfToFfThreshold' value='%.1f' min='0.1' max='20' class='form-input'>"
                "<p class='form-hint'>PF_BTN with volume ≥ this → corrected to FF_BTN</p></div>", config.datalog.pfToFfThreshold);
            out.printf("<div class='form-group'><label class='form-label'>FF → PF Threshold (L)</label>"
                "<input type='number' step='0.1' name='ffToPfThreshold' value='%.1f' min='0.1' max='20' class='form-input'>"
                "<p class='form-hint'>FF_BTN with volume ≤ this → corrected to PF_BTN</p></div>", config.datalog.ffToPfThreshold);
            out.print(F("</div>"));
            out.printf("<div class='form-group'><label class='form-label'>Button Hold Threshold (ms)</label>"
                "<input type='number' step='50' name='manualPressThresholdMs' value='%d' min='0' max='5000' class='form-input'>"
                "<p class='form-hint'>If button is held longer than this, correction is skipped (extended hold = intentional). 0 = disabled.</p></div>",
                config.datalog.manualPressThresholdMs);
            out.print(F("<p class='form-hint' style='margin-top:0.5rem'>Corrects button identification by comparing measured water volume against thresholds. "
                "Also checks how long the button was held — an extended press releases more water intentionally, so correction is skipped. "
                "Only applies to clean cycles with no extra button presses. Corrections are logged to btn_log.txt.</p>"));
            out.print(F("</div></div></div>"));
            
            // Available files card
            out.printf("<div class='card'><div class='card-header'>📊 %s</div><div class='card-body' style='padding:0'>", "Files");
            if (fsAvailable && activeFS) {
                int fileCount = 0;
                std::function<void(const String&)> listFiles = [&](const String& path) {
                    File dir = activeFS->open(path);
                    if (!dir || !dir.isDirectory()) return;
                    while (File entry = dir.openNextFile()) {
                        String name = String(entry.name());
                        String fullPath = path == "/" ? "/" + name : path + "/" + name;
                        if (entry.isDirectory()) {
                            listFiles(fullPath);
                        } else if (name.endsWith(".txt") || name.endsWith(".log") || name.endsWith(".csv")) {
                            fileCount++;
                            bool isCurrent = (fullPath == String(config.datalog.currentFile));
                            out.print(F("<div class='list-item'>"));
                            out.print(F("<span>"));
                            if (isCurrent) out.print(F("<strong class='text-success'>✓ "));
                            out.print(fullPath);
                            out.print(F(" <small class='text-muted'>("));
                            out.print(formatFileSize(entry.size()));
                            out.print(F(")</small>"));
                            if (isCurrent) out.print(F("</strong>"));
                            out.print(F("</span><span class='btn-group'>"));
                            out.printf("<a href='/download?file=%s' class='btn btn-sm btn-secondary'>📥</a>", fullPath.c_str());
                            if (!isCurrent) {
                                String storage = (config.hardware.storageType == STORAGE_SD_CARD && sdAvailable) ? "sdcard" : "internal";
                                out.printf("<a href='/delete?path=%s&storage=%s&return=datalog' class='btn btn-sm btn-danger' onclick='return confirm(\"Delete?\")'>🗑️</a>", fullPath.c_str(), storage.c_str());
                            }
                            out.print(F("</span></div>"));
                        }
                        entry.close();
                    }
                    dir.close();
                };
                listFiles("/");
                if (fileCount == 0) out.printf("<div class='list-item text-muted'>%s</div>", "No log files");
            }
            out.print(F("</div></div>"));
            
            out.print(F("<button type='submit' class='btn btn-primary btn-block'>💾 Save</button></form>"));
            out.print(F("<script>function toggleMaxSize(){document.getElementById('maxSizeGroup').style.display=document.getElementById('rotation').value==='4'?'block':'none';}"));
            // Preview generator
            out.print(F("function updatePreview(){var p=[];var d=new Date();"));
            out.print(F("var df=document.getElementById('dfDate').value;"));
            out.print(F("var tf=document.getElementById('dfTime').value;"));
            out.print(F("var ef=document.getElementById('dfEnd').value;"));
            out.print(F("var dd=d.getDate().toString().padStart(2,'0'),mm=(d.getMonth()+1).toString().padStart(2,'0'),yy=d.getFullYear();"));
            out.print(F("var hh=d.getHours().toString().padStart(2,'0'),mi=d.getMinutes().toString().padStart(2,'0'),ss=d.getSeconds().toString().padStart(2,'0');"));
            // Date
            out.print(F("if(df==='1')p.push(dd+'/'+mm+'/'+yy);"));
            out.print(F("else if(df==='2')p.push(mm+'/'+dd+'/'+yy);"));
            out.print(F("else if(df==='3')p.push(yy+'-'+mm+'-'+dd);"));
            out.print(F("else if(df==='4')p.push(dd+'.'+mm+'.'+yy);"));
            // Start time
            out.print(F("var tStr='';if(tf==='0')tStr=hh+':'+mi+':'+ss;"));
            out.print(F("else if(tf==='1')tStr=hh+':'+mi;"));
            out.print(F("else{var h=d.getHours()%12||12;tStr=h+':'+mi+':'+ss+(d.getHours()<12?'AM':'PM');}"));
            out.print(F("p.push(tStr);"));
            // End
            out.print(F("if(ef==='0')p.push(tStr);"));
            out.print(F("else if(ef==='1')p.push('45s');"));
            // Boot
            out.print(F("if(document.getElementById('dfBoot').value==='1')p.push('#:1234');"));
            // Trigger
            out.print(F("p.push('FF_BTN');"));
            // Volume
            out.print(F("var vf=document.getElementById('dfVol').value;"));
            out.print(F("if(vf==='0')p.push('L:2,50');"));
            out.print(F("else if(vf==='1')p.push('L:2.50');"));
            out.print(F("else if(vf==='2')p.push('2.50');"));
            // Extra
            out.print(F("if(document.getElementById('dfExtra').value==='1'){p.push('FF0');p.push('PF1');}"));
            out.print(F("document.getElementById('logPreview').textContent=p.join('|');}"));
            out.print(F("updatePreview();"));
            out.print(F("</script>"));
        });
    });
    
    server.on("/save_datalog", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Save all settings first
        if (r->hasParam("currentFile", true))
            strncpy(config.datalog.currentFile, r->getParam("currentFile", true)->value().c_str(), 64);
        if (r->hasParam("prefix", true))
            strncpy(config.datalog.prefix, r->getParam("prefix", true)->value().c_str(), 32);
        if (r->hasParam("folder", true))
            strncpy(config.datalog.folder, r->getParam("folder", true)->value().c_str(), 32);
        if (r->hasParam("rotation", true))
            config.datalog.rotation = (DatalogRotation)r->getParam("rotation", true)->value().toInt();
        if (r->hasParam("maxSizeKB", true))
            config.datalog.maxSizeKB = r->getParam("maxSizeKB", true)->value().toInt();
        config.datalog.timestampFilename = r->hasParam("timestampFilename", true);
        config.datalog.includeDeviceId = r->hasParam("includeDeviceId", true);
        
        // Format settings
        if (r->hasParam("dateFormat", true))
            config.datalog.dateFormat = r->getParam("dateFormat", true)->value().toInt();
        if (r->hasParam("timeFormat", true))
            config.datalog.timeFormat = r->getParam("timeFormat", true)->value().toInt();
        if (r->hasParam("endFormat", true))
            config.datalog.endFormat = r->getParam("endFormat", true)->value().toInt();
        if (r->hasParam("volumeFormat", true))
            config.datalog.volumeFormat = r->getParam("volumeFormat", true)->value().toInt();
        config.datalog.includeBootCount = r->hasParam("includeBootCount", true) && r->getParam("includeBootCount", true)->value() == "1";
        config.datalog.includeExtraPresses = r->hasParam("includeExtraPresses", true) && r->getParam("includeExtraPresses", true)->value() == "1";
        // Post-correction settings
        config.datalog.postCorrectionEnabled = r->hasParam("postCorrectionEnabled", true);
        if (r->hasParam("pfToFfThreshold", true))
            config.datalog.pfToFfThreshold = r->getParam("pfToFfThreshold", true)->value().toFloat();
        if (r->hasParam("ffToPfThreshold", true))
            config.datalog.ffToPfThreshold = r->getParam("ffToPfThreshold", true)->value().toFloat();
        if (r->hasParam("manualPressThresholdMs", true))
            config.datalog.manualPressThresholdMs = r->getParam("manualPressThresholdMs", true)->value().toInt();
        
        saveConfig();
        
        // Check if we should create a new file
        String action = r->hasParam("action", true) ? r->getParam("action", true)->value() : "";
        
        if (action == "create" && fsAvailable && activeFS) {
            // Generate filename based on saved settings
            String folder = String(config.datalog.folder);
            if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
            if (folder.length() > 0 && !folder.endsWith("/")) folder += "/";
            if (folder.length() == 0) folder = "/";
            
            // Create folder if needed
            if (folder != "/" && !activeFS->exists(folder)) {
                activeFS->mkdir(folder);
            }
            
            // Generate new filename
            String newFile = folder + String(config.datalog.prefix);
            
            // Add device ID if enabled
            if (config.datalog.includeDeviceId && strlen(config.deviceId) > 0) {
                newFile += "_";
                newFile += config.deviceId;
            }
            
            // Add timestamp ONLY if enabled
            if (config.datalog.timestampFilename) {
                if (Rtc) {
                    RtcDateTime now = Rtc->GetDateTime();
                    char buf[20];
                    snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d",
                        now.Year(), now.Month(), now.Day(),
                        now.Hour(), now.Minute(), now.Second());
                    newFile += buf;
                } else {
                    newFile += "_";
                    newFile += String(millis());
                }
            }
            
            newFile += ".txt";
            
            // Create empty file
            File f = activeFS->open(newFile, "w");
            if (f) {
                f.close();
                strncpy(config.datalog.currentFile, newFile.c_str(), 64);
                saveConfig();
                statusMessage = "<div class='alert alert-success'>✅ Created: " + newFile + "</div>";
            } else {
                statusMessage = "<div class='alert alert-error'>❌ Failed to create file</div>";
            }
        } else if (action != "create") {
            statusMessage = "<div class='alert alert-success'>✅ " + String("Saved") + "</div>";
        }
        
        r->redirect("/settings_datalog");
    });

    // ========== LIVE MONITOR ==========
    server.on("/live", HTTP_GET, [](AsyncWebServerRequest *r) {
        sendChunkedHtml(r, "Live", [](Print& out) {
            // Page header
            out.print(F("<div class='page-header'><h1>"));
            out.print(icon("📡"));
            out.printf("%s</h1>", "Live");
            out.printf("<div class='breadcrumb'><span id='connStatus' class='text-success'>● %s</span></div>", "Connected");
            out.print(F("</div>"));
            
            // Time card with Mode
            out.print(F("<div class='card'><div class='card-body'>"));
            out.print(F("<div style='display:flex;justify-content:space-between;align-items:flex-start'>"));
            // Mode on left
            out.printf("<div><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Mode");
            out.print(F("<div id='mode' style='font-weight:bold;font-size:1.1rem'>"));
            if (onlineLoggerMode) {
                out.printf("🌐 %s", "Online Logger");
            } else if (apModeTriggered) {
                out.printf("📶 %s", "Web Only");
            } else {
                out.printf("📊 %s", "Logging");
            }
            out.print(F("</div></div>"));
            // Time in center
            out.print(F("<div class='text-center' style='flex:1'>"));
            out.print(F("<div id='time' style='font-size:2.5rem;font-family:monospace;font-weight:bold'>--:--:--</div>"));
            out.print(F("</div>"));
            // Empty right for balance
            out.print(F("<div style='width:100px'></div>"));
            out.print(F("</div></div></div>"));
            
            // Current Cycle card
            out.printf("<div class='card'><div class='card-header'>💧 %s</div><div class='card-body'>", "Current Cycle");
            out.print(F("<div class='stats-grid'>"));
            out.printf("<div class='stat-card'><div class='value' id='liters'>0.00</div><div class='label'>%s</div></div>", "Liters");
            out.printf("<div class='stat-card'><div class='value' id='pulses'>0</div><div class='label'>%s</div></div>", "Pulses");
            out.printf("<div class='stat-card'><div class='value' id='ffCount' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.ffColor, "Extra FF");
            out.printf("<div class='stat-card'><div class='value' id='pfCount' style='color:%s'>0</div><div class='label'>%s</div></div>", config.theme.pfColor, "Extra PF");
            out.print(F("</div>"));
            out.print(F("<div class='mt-1' style='display:flex;justify-content:space-between;font-size:0.85rem'>"));
            out.printf("<span>%s: <strong id='trigger'>-</strong></span>", "Trigger");
            out.printf("<span>%s: <strong id='cycleTime'>0</strong>s</span>", "Duration");
            out.print(F("</div></div></div>"));
            
            // State Machine card
            out.printf("<div class='card'><div class='card-header'>⚙️ %s</div><div class='card-body'>", "State Machine");
            out.print(F("<div style='display:flex;align-items:center;justify-content:center;gap:1.5rem;flex-wrap:wrap'>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "State");
            out.print(F("<div id='state' style='font-weight:bold;padding:8px 16px;border-radius:6px;background:#e0e0e0;font-size:1.1rem'>-</div></div>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Timer");
            out.print(F("<div id='stateRemaining' style='font-size:2rem;font-weight:bold;color:var(--primary)'>-</div></div>"));
            out.print(F("</div>"));
            out.print(F("<div class='mt-1 text-muted' style='font-size:0.75rem;text-align:center'>"));
            out.printf("🔵 IDLE → 🟡 WAIT_FLOW (%ds) → 🟢 MONITORING (%ds idle) → Logging", 
                config.flowMeter.firstLoopMonitoringWindowSecs, config.flowMeter.monitoringWindowSecs);
            out.print(F("</div></div></div>"));
            
            // Button States card
            out.printf("<div class='card'><div class='card-header'>🔘 %s</div><div class='card-body'>", "Button States");
            out.print(F("<div style='display:flex;justify-content:space-around;flex-wrap:wrap;gap:1rem'>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Full Flush Btn");
            out.print(F("<div id='ff' style='padding:8px 16px;border-radius:6px;color:#fff;background:#95a5a6;font-weight:bold'>-</div></div>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "Part Flush Btn");
            out.print(F("<div id='pf' style='padding:8px 16px;border-radius:6px;color:#fff;background:#95a5a6;font-weight:bold'>-</div></div>"));
            out.printf("<div style='text-align:center'><div class='text-muted' style='font-size:0.75rem'>%s</div>", "WiFi Trigger");
            out.print(F("<div id='wifi' style='padding:8px 16px;border-radius:6px;color:#fff;background:#95a5a6;font-weight:bold'>-</div></div>"));
            out.print(F("</div></div></div>"));
            
            // Recent Logs card
            out.printf("<div class='card'><div class='card-header'>📝 %s</div>", "Recent Logs");
            out.printf("<div class='card-body' style='padding:0;overflow-x:auto'><div id='recentLogs' style='font-family:monospace;font-size:0.8rem'>%s...</div></div></div>", "Loading");
            
            // System card (text only - no stat cards)
            out.printf("<div class='card'><div class='card-header'>📊 %s</div><div class='card-body'>", "System");
            // Row 1: Boot, Uptime, Chip
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem'>"));
            out.printf("<span>%s: <strong id='boot'>0</strong></span>", "Boot Count");
            out.printf("<span>%s: <strong id='uptime'>0</strong>s</span>", "Uptime");
            out.printf("<span>%s: <strong>%s</strong></span>", "Chip", ESP.getChipModel());
            out.print(F("</div>"));
            // Row 2: RAM, CPU, Storage
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem;margin-top:0.5rem'>"));
            out.printf("<span>RAM: <strong id='heap'>-</strong> / <strong id='heapTotal'>-</strong></span>");
            out.printf("<span>CPU: <strong>%d</strong> MHz</span>", getCpuFrequencyMhz());
            out.printf("<span>%s: <strong id='storage'>-</strong></span>", "Storage");
            out.print(F("</div>"));
            // Row 3: Network (left), empty (middle), IP (right)
            out.print(F("<div style='display:flex;justify-content:space-between;flex-wrap:wrap;gap:0.5rem;font-size:0.85rem;margin-top:0.5rem'>"));
            out.print(F("<span>"));
            out.print(getNetworkDisplay());
            out.print(F("</span>"));
            out.print(F("<span></span>"));  // Empty middle
            out.print(F("<span>IP: <strong>"));
            out.print(currentIPAddress);
            out.print(F("</strong></span>"));
            out.print(F("</div></div></div>"));
            
            // Hide footer in Live Monitor
            out.print(F("<style>.app-footer{display:none!important}</style>"));
            
            // JavaScript
            out.print(F("<script>"));
            out.print(F("function fmt(b){return b>1048576?(b/1048576).toFixed(1)+'MB':b>1024?(b/1024).toFixed(1)+'KB':b+'B'}"));
            out.print(F("var stateColors={IDLE:'#3498db',WAIT_FLOW:'#f39c12',MONITORING:'#27ae60',DONE:'#e74c3c'};"));
            out.print(F("function upd(){fetch('/api/live').then(r=>r.json()).then(d=>{"));
            out.print(F("document.getElementById('time').textContent=d.time;"));
            out.print(F("document.getElementById('trigger').textContent=d.trigger;"));
            out.print(F("document.getElementById('cycleTime').textContent=d.cycleTime;"));
            out.print(F("document.getElementById('pulses').textContent=d.pulses;"));
            out.print(F("document.getElementById('liters').textContent=d.liters.toFixed(2);"));
            out.print(F("document.getElementById('ffCount').textContent=d.ffCount;"));
            out.print(F("document.getElementById('pfCount').textContent=d.pfCount;"));
            out.print(F("var stEl=document.getElementById('state');stEl.textContent=d.state;stEl.style.background=stateColors[d.state]||'#95a5a6';stEl.style.color='#fff';"));
            out.print(F("var remEl=document.getElementById('stateRemaining');"));
            out.print(F("if(d.stateRemaining>=0){remEl.textContent=d.stateRemaining+'s';}else{remEl.textContent='-';}"));
            out.print(F("document.getElementById('ff').textContent=d.ff?'Pressed':'Released';"));
            out.print(F("document.getElementById('ff').style.background=d.ff?'#27ae60':'#95a5a6';"));
            out.print(F("document.getElementById('pf').textContent=d.pf?'Pressed':'Released';"));
            out.print(F("document.getElementById('pf').style.background=d.pf?'#27ae60':'#95a5a6';"));
            out.print(F("document.getElementById('wifi').textContent=d.wifi?'Pressed':'Released';"));
            out.print(F("document.getElementById('wifi').style.background=d.wifi?'#3498db':'#95a5a6';"));
            out.print(F("document.getElementById('boot').textContent=d.boot;"));
            out.print(F("document.getElementById('heap').textContent=fmt(d.heap);"));
            out.print(F("document.getElementById('heapTotal').textContent=fmt(d.heapTotal);"));
            out.print(F("document.getElementById('uptime').textContent=d.uptime;"));
            out.print(F("if(d.fsTotal)document.getElementById('storage').textContent=fmt(d.fsUsed)+'/'+fmt(d.fsTotal);"));
            out.print(F("var modeEl=document.getElementById('mode');"));
            out.printf("if(d.mode=='online')modeEl.innerHTML='🌐 %s';", "Online Logger");
            out.printf("else if(d.mode=='webonly')modeEl.innerHTML='📶 %s';", "Web Only");
            out.printf("else modeEl.innerHTML='📊 %s';", "Logging");
            out.printf("document.getElementById('connStatus').className='text-success';document.getElementById('connStatus').textContent='● %s';", "Connected");
            out.printf("}).catch(()=>{document.getElementById('connStatus').className='text-danger';document.getElementById('connStatus').textContent='● %s'})}",  "Disconnected");
            
            out.printf("var ffColor='%s',pfColor='%s',otherColor='%s';", 
                config.theme.ffColor, config.theme.pfColor, config.theme.otherColor);
            out.print(F("function hexToRgba(hex,a){var r=parseInt(hex.slice(1,3),16),g=parseInt(hex.slice(3,5),16),b=parseInt(hex.slice(5,7),16);return 'rgba('+r+','+g+','+b+','+a+')';}"));
            
            // JS translation strings for table headers
            out.printf("var lang={time:'%s',trigger:'%s',volume:'%s',noLogs:'%s'};", 
                "Time", "Trigger", "Volume", "Loading");
            
            // Recent logs with Extra FF/PF columns - translated headers
            out.print(F("function updLogs(){fetch('/api/recent_logs').then(r=>r.json()).then(d=>{"));
            out.print(F("var el=document.getElementById('recentLogs');"));
            out.print(F("if(d.logs&&d.logs.length>0){"));
            out.print(F("var html='<table style=\"width:100%;border-collapse:collapse;font-size:0.75rem\">';"));
            out.print(F("html+='<tr style=\"background:var(--bg)\"><th style=\"padding:6px;text-align:left\">'+lang.time+'</th><th>'+lang.trigger+'</th><th>'+lang.volume+'</th><th>+FF</th><th>+PF</th></tr>';"));
            out.print(F("d.logs.forEach(function(l){"));
            out.print(F("var bg=l.trigger.indexOf('FF')>=0?hexToRgba(ffColor,0.15):l.trigger.indexOf('PF')>=0?hexToRgba(pfColor,0.15):hexToRgba(otherColor,0.1);"));
            out.print(F("var tc=l.trigger.indexOf('FF')>=0?ffColor:l.trigger.indexOf('PF')>=0?pfColor:otherColor;"));
            out.print(F("html+='<tr style=\"background:'+bg+'\"><td style=\"padding:6px\">'+l.time+'</td><td style=\"color:'+tc+';font-weight:bold;text-align:center\">'+l.trigger+'</td><td style=\"text-align:center\">'+l.volume+'</td><td style=\"text-align:center\">'+l.ff+'</td><td style=\"text-align:center\">'+l.pf+'</td></tr>';"));
            out.print(F("});html+='</table>';el.innerHTML=html;"));
            out.print(F("}else{el.innerHTML='<div class=\"list-item text-muted\">No log entries yet</div>';}"));
            out.print(F("}).catch(()=>{})}"));
            
            out.print(F("setInterval(upd,500);upd();"));
            out.print(F("setInterval(updLogs,3000);updLogs();"));
            out.print(F("</script>"));
        });
    });
    
    // ========== LIVE API (JSON) ==========
    server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<1024> doc;
        
        // Atomic read of volatile ISR variables
        noInterrupts();
        uint32_t safePulses = pulseCount;
        interrupts();
        
        doc["time"] = getRtcDateTimeString();
        doc["ff"] = digitalRead(config.hardware.pinWakeupFF);
        doc["pf"] = digitalRead(config.hardware.pinWakeupPF);
        doc["wifi"] = digitalRead(config.hardware.pinWifiTrigger);
        doc["pulses"] = safePulses;
        doc["boot"] = bootCount;
        doc["heap"] = ESP.getFreeHeap();
        doc["heapTotal"] = ESP.getHeapSize();
        doc["uptime"] = millis() / 1000;
        
        // Current cycle info
        doc["trigger"] = cycleStartedBy;
        doc["cycleTime"] = (millis() - cycleStartTime) / 1000;
        doc["ffCount"] = highCountFF;
        doc["pfCount"] = highCountPF;
        doc["totalPulses"] = cycleTotalPulses + safePulses;
        
        // State machine info
        const char* stateNames[] = {"IDLE", "WAIT_FLOW", "MONITORING", "DONE"};
        doc["state"] = stateNames[loggingState];
        doc["stateTime"] = (millis() - stateStartTime) / 1000;
        
        // Remaining time in current state
        if (loggingState == STATE_WAIT_FLOW) {
            long remaining = (BUTTON_WAIT_FLOW_MS - (millis() - stateStartTime)) / 1000;
            doc["stateRemaining"] = remaining > 0 ? remaining : 0;
        } else if (loggingState == STATE_MONITORING && lastFlowPulseTime > 0) {
            long remaining = (FLOW_IDLE_TIMEOUT_MS - (millis() - lastFlowPulseTime)) / 1000;
            doc["stateRemaining"] = remaining > 0 ? remaining : 0;
        } else {
            doc["stateRemaining"] = -1;  // Not applicable
        }
        
        // Calculate current flow in liters
        float liters = 0;
        if (config.flowMeter.pulsesPerLiter > 0) {
            liters = (float)safePulses / config.flowMeter.pulsesPerLiter * config.flowMeter.calibrationMultiplier;
        }
        doc["liters"] = liters;
        
        // Mode info
        doc["mode"] = onlineLoggerMode ? "online" : (apModeTriggered ? "webonly" : "logging");
        
        uint64_t used, total; int percent;
        getStorageInfo(used, total, percent);
        doc["fsUsed"] = used;
        doc["fsTotal"] = total;
        
        sendJsonResponse(r, doc);
    });
    
    // ========== CHANGELOG API (always from LittleFS) ==========
    server.on("/api/changelog", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (LittleFS.exists("/changelog.txt")) {
            r->send(LittleFS, "/changelog.txt", "text/plain");
        } else {
            r->send(404, "text/plain", "Changelog not found");
        }
    });
    
    // ========== RECENT LOGS API ==========
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
        
        // Read last 5 lines from file
        File f = activeFS->open(logFile, "r");
        if (!f) {
            doc["error"] = "Cannot open file";
            sendJsonResponse(r, doc);
            return;
        }
        
        // Collect all lines (simple approach for small files)
        std::vector<String> lines;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() > 0) {
                lines.push_back(line);
            }
        }
        f.close();
        
        // Get last 5 lines (newest first)
        int totalLines = (int)lines.size();
        int startIdx = totalLines > 5 ? totalLines - 5 : 0;
        for (int i = totalLines - 1; i >= startIdx; i--) {
            // Parse NEW format: DD/MM/YYYY|WakeTime|SleepTime|#:boot|REASON|L:vol|FFn|PFn
            // Also supports OLD format: DD/MM/YYYY|Time|#:boot|REASON|L:vol|FFn|PFn
            String line = lines[i];
            int p1 = line.indexOf('|');        // after date
            int p2 = line.indexOf('|', p1 + 1); // after wake time
            int p3 = line.indexOf('|', p2 + 1); // after sleep time OR boot count
            int p4 = line.indexOf('|', p3 + 1); // after boot count OR reason
            int p5 = line.indexOf('|', p4 + 1); // after reason OR volume
            int p6 = line.indexOf('|', p5 + 1); // after volume OR FF
            int p7 = line.indexOf('|', p6 + 1); // after FF (new format only)
            
            if (p1 > 0 && p2 > p1 && p3 > p2 && p4 > p3) {
                JsonObject entry = logs.createNestedObject();
                
                // Check if this is new format (has sleep time) or old format
                // New format has 7 delimiters, old has 6
                bool isNewFormat = (p7 > p6);
                
                if (isNewFormat) {
                    // NEW format: Date|WakeTime|SleepTime|#:boot|REASON|L:vol|FFn|PFn
                    String wakeTime = line.substring(p1 + 1, p2);
                    String sleepTime = line.substring(p2 + 1, p3);
                    entry["time"] = line.substring(0, p1) + " " + wakeTime + "-" + sleepTime;
                    entry["trigger"] = line.substring(p4 + 1, p5);  // Reason
                    
                    String volStr = line.substring(p5 + 1, p6);
                    volStr.replace("L:", "");
                    volStr.replace(",", ".");
                    entry["volume"] = volStr + " L";
                    
                    String ffStr = line.substring(p6 + 1, p7);
                    ffStr.replace("FF", "");
                    entry["ff"] = ffStr.toInt();
                    
                    String pfStr = line.substring(p7 + 1);
                    pfStr.replace("PF", "");
                    entry["pf"] = pfStr.toInt();
                } else {
                    // OLD format: Date|Time|#:boot|REASON|L:vol|FFn|PFn
                    entry["time"] = line.substring(0, p2);  // Date + Time
                    entry["trigger"] = line.substring(p3 + 1, p4);  // Reason
                    
                    String volStr = line.substring(p4 + 1, p5);
                    volStr.replace("L:", "");
                    volStr.replace(",", ".");
                    entry["volume"] = volStr + " L";
                    
                    String ffStr = line.substring(p5 + 1, p6);
                    ffStr.replace("FF", "");
                    entry["ff"] = ffStr.toInt();
                    
                    String pfStr = line.substring(p6 + 1);
                    pfStr.replace("PF", "");
                    entry["pf"] = pfStr.toInt();
                }
            }
        }
        
        sendJsonResponse(r, doc);
    });
    
    // ========== STATISTICS (redirect to dashboard) ==========
    
    // Remove the old /api/stats endpoint since we're using direct file download now
    
    // ========== FILE OPERATIONS ==========
    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("file")) {
            r->send(400, "text/plain", "No file");
            return;
        }
        String path = sanitizeFilename(r->getParam("file")->value());
        if (!path.startsWith("/")) path = "/" + path;
        
        // Check which storage to use
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        fs::FS* targetFS = (storage == "sdcard" && sdAvailable) ? (fs::FS*)&SD : 
                        (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
        
        if (targetFS && targetFS->exists(path)) {
            // Extract filename from path
            String filename = path;
            int lastSlash = path.lastIndexOf('/');
            if (lastSlash >= 0) {
                filename = path.substring(lastSlash + 1);
            }
            
            // Send with proper Content-Disposition header
            AsyncWebServerResponse *response = r->beginResponse(*targetFS, path, "application/octet-stream");
            response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
            r->send(response);
        } else {
            r->send(404, "text/plain", "Not found");
        }
    });
    
    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("path")) {
            r->redirect("/files");
            return;
        }
        String path = sanitizeFilename(r->getParam("path")->value());
        if (!path.startsWith("/")) path = "/" + path;
        
        // Get storage from URL or use current
        String storage = currentStorageView;
        if (r->hasParam("storage")) {
            storage = r->getParam("storage")->value();
        }
        
        // Determine which FS to use
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable) {
            targetFS = &SD;
        } else if (storage == "internal" && littleFsAvailable) {
            targetFS = &LittleFS;
        } else if (activeFS) {
            targetFS = activeFS;
        }
        
        bool deleted = false;
        if (targetFS) {
            File f = targetFS->open(path);
            if (f) {
                bool isDir = f.isDirectory();
                f.close();
                if (isDir) {
                    deleteRecursive(*targetFS, path);
                } else {
                    targetFS->remove(path);
                }
                deleted = true;
            }
        }
        
        // Get return directory from param or calculate from path
        String returnDir = r->hasParam("dir") ? r->getParam("dir")->value() : "";
        if (returnDir.isEmpty()) {
            int lastSlash = path.lastIndexOf('/');
            returnDir = lastSlash <= 0 ? "/" : path.substring(0, lastSlash);
        }
        
        // Check where to redirect
        String returnTo = r->hasParam("return") ? r->getParam("return")->value() : "";
        
        if (returnTo == "datalog") {
            statusMessage = deleted ? 
                "<div class='alert alert-success'>✅ Deleted: " + path + "</div>" :
                "<div class='alert alert-error'>❌ Failed to delete</div>";
            r->redirect("/settings_datalog");
        } else {
            statusMessage = deleted ? 
                "<div class='alert alert-success'>✅ Deleted successfully</div>" :
                "<div class='alert alert-error'>❌ Failed to delete</div>";
            r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
        }
    });
    
    // Move/Rename within same storage
    server.on("/move_file", HTTP_GET, [](AsyncWebServerRequest *r) {
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        String srcPath = r->hasParam("src") ? sanitizePath(r->getParam("src")->value()) : "";
        String newName = r->hasParam("newName") ? r->getParam("newName")->value() : "";
        String destDir = r->hasParam("destDir") ? r->getParam("destDir")->value() : "";
        String returnDir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        
        if (srcPath.isEmpty() || newName.isEmpty()) {
            statusMessage = "<div class='alert alert-error'>❌ Invalid parameters</div>";
            r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
            return;
        }
        
        // Determine FS
        fs::FS* fs = nullptr;
        if (storage == "sdcard" && sdAvailable) fs = &SD;
        else if (storage == "internal" && littleFsAvailable) fs = &LittleFS;
        
        if (!fs) {
            statusMessage = "<div class='alert alert-error'>❌ Storage not available</div>";
            r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
            return;
        }
        
        // Build destination path
        String dstPath;
        if (destDir.isEmpty()) {
            // Same directory, just rename
            int lastSlash = srcPath.lastIndexOf('/');
            String srcDir = lastSlash <= 0 ? "/" : srcPath.substring(0, lastSlash);
            dstPath = buildPath(srcDir, newName);
        } else {
            dstPath = buildPath(destDir, newName);
        }
        
        // Perform move/rename
        bool success = fs->rename(srcPath, dstPath);
        
        statusMessage = success ? 
            "<div class='alert alert-success'>✅ Moved to " + dstPath + "</div>" :
            "<div class='alert alert-error'>❌ Move failed</div>";
        
        r->redirect("/files?storage=" + storage + "&dir=" + returnDir + "&edit=1");
    });
    
    // Upload handler
    static String uploadDir = "/";
    static String uploadStorage = "internal";
    server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *r) {
        r->redirect("/files?storage=" + uploadStorage + "&dir=" + uploadDir + "&edit=1");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        static File uploadFile;
        if (!index) {
            uploadDir = request->hasArg("path") ? request->arg("path") : "/";
            if (!uploadDir.startsWith("/")) uploadDir = "/" + uploadDir;
            uploadStorage = currentStorageView;
        }
        String fullPath = buildPath(uploadDir, filename);
        
        fs::FS* targetFS = getCurrentViewFS();
        if (!index && targetFS) {
            Serial.printf("Upload to %s: %s\n", currentStorageView.c_str(), fullPath.c_str());
            uploadFile = targetFS->open(fullPath, "w");
        }
        
        if (uploadFile && len) {
            uploadFile.write(data, len);
        }
        
        if (final && uploadFile) {
            Serial.printf("Upload complete: %d bytes\n", index + len);
            uploadFile.close();
        }
    });
    
    // ========== MKDIR ==========
    server.on("/mkdir", HTTP_GET, [](AsyncWebServerRequest *r) {
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("name") || !targetFS) {
            r->redirect("/files");
            return;
        }
        String dir = r->hasParam("dir") ? r->getParam("dir")->value() : "/";
        String name = sanitizeFilename(r->getParam("name")->value());
        String fullPath = buildPath(dir, name);
        
        if (targetFS->mkdir(fullPath)) {
            statusMessage = "<div class='alert alert-success'>✅ Created: " + fullPath + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>❌ Failed to create folder</div>";
        }
        r->redirect("/files?storage=" + currentStorageView + "&dir=" + dir + "&edit=1");
    });
    
    // ========== RENAME ==========
    server.on("/rename", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!r->hasParam("path")) {
            r->redirect("/files");
            return;
        }
        String path = sanitizeFilename(r->getParam("path")->value());
        
        // Get storage from URL
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        }
        
        sendChunkedHtml(r, "Rename", [path](Print& out) {
            out.print(F("<div class='header'><h1>✏️ Rename</h1><a href='/files' class='btn btn-secondary'>← Cancel</a></div>"));
            out.print(F("<form action='/do_rename' method='GET'>"));
            out.printf("<input type='hidden' name='path' value='%s'>", path.c_str());
            out.printf("<input type='hidden' name='storage' value='%s'>", currentStorageView.c_str());
            
            // Extract filename
            int lastSlash = path.lastIndexOf('/');
            String filename = lastSlash >= 0 ? path.substring(lastSlash + 1) : path;
            
            out.print(F("<div class='section'>"));
            out.printf("<p>Renaming: <strong>%s</strong></p>", path.c_str());
            out.printf("<div class='form-group'><label>New Name</label><input type='text' name='newname' value='%s' required></div>", filename.c_str());
            out.print(F("</div>"));
            out.print(F("<button type='submit' class='btn'>💾 Rename</button></form>"));
        });
    });
    
    server.on("/do_rename", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Get storage from URL
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        }
        
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("path") || !r->hasParam("newname") || !targetFS) {
            r->redirect("/files");
            return;
        }
        
        String oldPath = sanitizeFilename(r->getParam("path")->value());
        String newName = sanitizeFilename(r->getParam("newname")->value());
        
        int lastSlash = oldPath.lastIndexOf('/');
        String dir = lastSlash <= 0 ? "/" : oldPath.substring(0, lastSlash);
        String newPath = buildPath(dir, newName);
        
        if (targetFS->rename(oldPath, newPath)) {
            statusMessage = "<div class='alert alert-success'>✅ Renamed to: " + newPath + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>❌ Rename failed</div>";
        }
        r->redirect("/files?storage=" + currentStorageView + "&dir=" + dir + "&edit=1");
    });
    
    // ========== MOVE ==========
    server.on("/move", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Get storage from URL
        if (r->hasParam("storage")) {
            currentStorageView = r->getParam("storage")->value();
        }
        
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("path") || !r->hasParam("dest") || !targetFS) {
            r->redirect("/files");
            return;
        }
        
        String path = sanitizeFilename(r->getParam("path")->value());
        String dest = sanitizeFilename(r->getParam("dest")->value());
        
        int lastSlash = path.lastIndexOf('/');
        String filename = lastSlash >= 0 ? path.substring(lastSlash + 1) : path;
        String sourceDir = lastSlash <= 0 ? "/" : path.substring(0, lastSlash);
        
        String newPath = buildPath(dest, filename);
        
        if (targetFS->rename(path, newPath)) {
            statusMessage = "<div class='alert alert-success'>✅ Moved to: " + newPath + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>❌ Move failed</div>";
        }
        r->redirect("/files?storage=" + currentStorageView + "&dir=" + sourceDir + "&edit=1");
    });
    
    // ========== CREATE NEW LOG ==========
    server.on("/create_log", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fsAvailable || !activeFS) {
            r->redirect("/settings_datalog");
            return;
        }
        
        // Generate filename with timestamp
        String folder = String(config.datalog.folder);
        if (folder.length() > 0 && !folder.startsWith("/")) folder = "/" + folder;
        if (folder.length() > 0 && !folder.endsWith("/")) folder += "/";
        if (folder.length() == 0) folder = "/";
        
        // Create folder if needed
        if (folder != "/" && !activeFS->exists(folder)) {
            activeFS->mkdir(folder);
        }
        
        // Generate new filename based on settings
        String newFile = folder + String(config.datalog.prefix);
        
        // Add device ID if enabled
        if (config.datalog.includeDeviceId && strlen(config.deviceId) > 0) {
            newFile += "_";
            newFile += config.deviceId;
        }
        
        // Add timestamp if enabled
        if (config.datalog.timestampFilename) {
            if (Rtc) {
                RtcDateTime now = Rtc->GetDateTime();
                char buf[20];
                snprintf(buf, sizeof(buf), "_%04d%02d%02d_%02d%02d%02d",
                    now.Year(), now.Month(), now.Day(),
                    now.Hour(), now.Minute(), now.Second());
                newFile += buf;
            } else {
                newFile += "_";
                newFile += String(millis());
            }
        }
        
        newFile += ".txt";
        
        // Create empty file
        File f = activeFS->open(newFile, "w");
        if (f) {
            f.close();
            strncpy(config.datalog.currentFile, newFile.c_str(), 64);
            saveConfig();
            statusMessage = "<div class='alert alert-success'>✅ Created: " + newFile + "</div>";
        } else {
            statusMessage = "<div class='alert alert-error'>❌ Failed to create log</div>";
        }
        r->redirect("/settings_datalog");
    });
    
    // ========== EXPORT/IMPORT SETTINGS ==========
    server.on("/export_settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        
        // Device settings
        doc["deviceName"] = config.deviceName;
        doc["deviceId"] = config.deviceId;
        doc["forceWebServer"] = config.forceWebServer;
        
        // Theme settings
        JsonObject theme = doc["theme"].to<JsonObject>();
        theme["mode"] = config.theme.mode;
        theme["primaryColor"] = config.theme.primaryColor;
        theme["secondaryColor"] = config.theme.secondaryColor;
        theme["bgColor"] = config.theme.bgColor;
        theme["textColor"] = config.theme.textColor;
        theme["ffColor"] = config.theme.ffColor;
        theme["pfColor"] = config.theme.pfColor;
        theme["otherColor"] = config.theme.otherColor;
        theme["logoSource"] = config.theme.logoSource;
        theme["faviconPath"] = config.theme.faviconPath;
        theme["boardDiagramPath"] = config.theme.boardDiagramPath;
        theme["chartSource"] = config.theme.chartSource;
        theme["chartLocalPath"] = config.theme.chartLocalPath;
        
        // Flow meter settings
        JsonObject flowMeter = doc["flowMeter"].to<JsonObject>();
        flowMeter["pulsesPerLiter"] = config.flowMeter.pulsesPerLiter;
        flowMeter["calibrationMultiplier"] = config.flowMeter.calibrationMultiplier;
        flowMeter["monitoringWindowSecs"] = config.flowMeter.monitoringWindowSecs;
        flowMeter["firstLoopMonitoringWindowSecs"] = config.flowMeter.firstLoopMonitoringWindowSecs;
        flowMeter["blinkDuration"] = config.flowMeter.blinkDuration;
        
        // Datalog settings
        JsonObject datalog = doc["datalog"].to<JsonObject>();
        datalog["rotation"] = config.datalog.rotation;
        datalog["maxSizeKB"] = config.datalog.maxSizeKB;
        datalog["maxEntries"] = config.datalog.maxEntries;
        datalog["folder"] = config.datalog.folder;
        datalog["prefix"] = config.datalog.prefix;
        datalog["dateFormat"] = config.datalog.dateFormat;
        datalog["timeFormat"] = config.datalog.timeFormat;
        datalog["endFormat"] = config.datalog.endFormat;
        datalog["volumeFormat"] = config.datalog.volumeFormat;
        datalog["includeBootCount"] = config.datalog.includeBootCount;
        datalog["includeExtraPresses"] = config.datalog.includeExtraPresses;
        datalog["postCorrectionEnabled"] = config.datalog.postCorrectionEnabled;
        datalog["pfToFfThreshold"] = config.datalog.pfToFfThreshold;
        datalog["ffToPfThreshold"] = config.datalog.ffToPfThreshold;
        datalog["manualPressThresholdMs"] = config.datalog.manualPressThresholdMs;
        
        // Network settings
        JsonObject network = doc["network"].to<JsonObject>();
        network["wifiMode"] = config.network.wifiMode;
        network["apSSID"] = config.network.apSSID;
        network["clientSSID"] = config.network.clientSSID;
        network["ntpServer"] = config.network.ntpServer;
        network["timezone"] = config.network.timezone;
        network["useStaticIP"] = config.network.useStaticIP;
        
        // Hardware settings
        JsonObject hardware = doc["hardware"].to<JsonObject>();
        hardware["storageType"] = config.hardware.storageType;
        hardware["wakeupMode"] = config.hardware.wakeupMode;
        hardware["cpuFreqMHz"] = config.hardware.cpuFreqMHz;
        hardware["debugMode"] = config.hardware.debugMode;
        hardware["defaultStorageView"] = config.hardware.defaultStorageView;
        hardware["debounceMs"] = config.hardware.debounceMs;
        
        String json;
        serializeJsonPretty(doc, json);
        
        AsyncWebServerResponse *response = r->beginResponse(200, "application/json", json);
        String filename = String(config.deviceName) + "_settings.json";
        response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        r->send(response);
    });
    
    // Import settings from JSON file
    static String importBuffer;
    server.on("/import_settings", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Process the buffered JSON
        if (importBuffer.length() == 0) {
            r->send(400, "text/plain", "No data received");
            return;
        }
        
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, importBuffer);
        importBuffer = "";  // Clear buffer
        
        if (err) {
            r->send(400, "text/plain", String("JSON parse error: ") + err.c_str());
            return;
        }
        
        // Import device settings
        if (doc.containsKey("deviceName")) {
            strncpy(config.deviceName, doc["deviceName"] | config.deviceName, 32);
        }
        if (doc.containsKey("forceWebServer")) {
            config.forceWebServer = doc["forceWebServer"] | config.forceWebServer;
        }
        
        // Import theme settings
        if (doc.containsKey("theme")) {
            JsonObject theme = doc["theme"];
            config.theme.mode = theme["mode"] | config.theme.mode;
            strncpy(config.theme.primaryColor, theme["primaryColor"] | config.theme.primaryColor, 8);
            strncpy(config.theme.secondaryColor, theme["secondaryColor"] | config.theme.secondaryColor, 8);
            strncpy(config.theme.bgColor, theme["bgColor"] | config.theme.bgColor, 8);
            strncpy(config.theme.textColor, theme["textColor"] | config.theme.textColor, 8);
            strncpy(config.theme.ffColor, theme["ffColor"] | config.theme.ffColor, 8);
            strncpy(config.theme.pfColor, theme["pfColor"] | config.theme.pfColor, 8);
            strncpy(config.theme.otherColor, theme["otherColor"] | config.theme.otherColor, 8);
            strncpy(config.theme.logoSource, theme["logoSource"] | config.theme.logoSource, 128);
            strncpy(config.theme.faviconPath, theme["faviconPath"] | config.theme.faviconPath, 32);
            strncpy(config.theme.boardDiagramPath, theme["boardDiagramPath"] | config.theme.boardDiagramPath, 64);
            config.theme.chartSource = theme["chartSource"] | config.theme.chartSource;
            strncpy(config.theme.chartLocalPath, theme["chartLocalPath"] | config.theme.chartLocalPath, 64);
        }
        
        // Import flow meter settings
        if (doc.containsKey("flowMeter")) {
            JsonObject fm = doc["flowMeter"];
            config.flowMeter.pulsesPerLiter = fm["pulsesPerLiter"] | config.flowMeter.pulsesPerLiter;
            config.flowMeter.calibrationMultiplier = fm["calibrationMultiplier"] | config.flowMeter.calibrationMultiplier;
            config.flowMeter.monitoringWindowSecs = fm["monitoringWindowSecs"] | config.flowMeter.monitoringWindowSecs;
            config.flowMeter.firstLoopMonitoringWindowSecs = fm["firstLoopMonitoringWindowSecs"] | config.flowMeter.firstLoopMonitoringWindowSecs;
            config.flowMeter.blinkDuration = fm["blinkDuration"] | config.flowMeter.blinkDuration;
        }
        
        // Import datalog settings
        if (doc.containsKey("datalog")) {
            JsonObject dl = doc["datalog"];
            config.datalog.rotation = dl["rotation"] | config.datalog.rotation;
            config.datalog.maxSizeKB = dl["maxSizeKB"] | config.datalog.maxSizeKB;
            config.datalog.maxEntries = dl["maxEntries"] | config.datalog.maxEntries;
            strncpy(config.datalog.folder, dl["folder"] | config.datalog.folder, 32);
            strncpy(config.datalog.prefix, dl["prefix"] | config.datalog.prefix, 32);
            config.datalog.dateFormat = dl["dateFormat"] | config.datalog.dateFormat;
            config.datalog.timeFormat = dl["timeFormat"] | config.datalog.timeFormat;
            config.datalog.endFormat = dl["endFormat"] | config.datalog.endFormat;
            config.datalog.volumeFormat = dl["volumeFormat"] | config.datalog.volumeFormat;
            config.datalog.includeBootCount = dl["includeBootCount"] | config.datalog.includeBootCount;
            config.datalog.includeExtraPresses = dl["includeExtraPresses"] | config.datalog.includeExtraPresses;
            if (dl.containsKey("postCorrectionEnabled"))
                config.datalog.postCorrectionEnabled = dl["postCorrectionEnabled"] | config.datalog.postCorrectionEnabled;
            if (dl.containsKey("pfToFfThreshold"))
                config.datalog.pfToFfThreshold = dl["pfToFfThreshold"] | config.datalog.pfToFfThreshold;
            if (dl.containsKey("ffToPfThreshold"))
                config.datalog.ffToPfThreshold = dl["ffToPfThreshold"] | config.datalog.ffToPfThreshold;
            if (dl.containsKey("manualPressThresholdMs"))
                config.datalog.manualPressThresholdMs = dl["manualPressThresholdMs"] | config.datalog.manualPressThresholdMs;
        }
        
        // Import network settings
        if (doc.containsKey("network")) {
            JsonObject net = doc["network"];
            config.network.wifiMode = net["wifiMode"] | config.network.wifiMode;
            strncpy(config.network.apSSID, net["apSSID"] | config.network.apSSID, 32);
            strncpy(config.network.clientSSID, net["clientSSID"] | config.network.clientSSID, 32);
            strncpy(config.network.ntpServer, net["ntpServer"] | config.network.ntpServer, 64);
            config.network.timezone = net["timezone"] | config.network.timezone;
            config.network.useStaticIP = net["useStaticIP"] | config.network.useStaticIP;
        }
        
        // Import hardware settings
        if (doc.containsKey("hardware")) {
            JsonObject hw = doc["hardware"];
            config.hardware.storageType = hw["storageType"] | config.hardware.storageType;
            config.hardware.wakeupMode = hw["wakeupMode"] | config.hardware.wakeupMode;
            config.hardware.cpuFreqMHz = hw["cpuFreqMHz"] | config.hardware.cpuFreqMHz;
            config.hardware.debugMode = hw["debugMode"] | config.hardware.debugMode;
            config.hardware.defaultStorageView = hw["defaultStorageView"] | config.hardware.defaultStorageView;
            config.hardware.debounceMs = hw["debounceMs"] | config.hardware.debounceMs;
        }
        
        saveConfig();
        r->send(200, "text/plain", "OK");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        // File upload handler - accumulate data
        if (!index) {
            importBuffer = "";
        }
        for (size_t i = 0; i < len; i++) {
            importBuffer += (char)data[i];
        }
    });
    
    // ========== WIFI SCAN ==========
    server.on("/wifi_scan_start", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Block WiFi scan during active measurement cycle
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            r->send(503, "text/plain", "Busy - measurement in progress");
            return;
        }
        // Must be in AP+STA mode to scan while serving AP
        WiFi.mode(WIFI_AP_STA);
        WiFi.scanDelete();
        int result = WiFi.scanNetworks(true);  // async=true
        DBGF("WiFi scan started, result: %d\n", result);
        r->send(200, "text/plain", "OK");
    });
    
    server.on("/wifi_scan_result", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<2048> doc;
        JsonArray networks = doc.createNestedArray("networks");
        
        int n = WiFi.scanComplete();
        DBGF("WiFi scan complete: %d networks\n", n);
        
        if (n == WIFI_SCAN_RUNNING) {
            doc["scanning"] = true;
        } else if (n == WIFI_SCAN_FAILED) {
            doc["error"] = "Scan failed";
        } else if (n >= 0) {
            for (int i = 0; i < n && i < 20; i++) {
                JsonObject net = networks.createNestedObject();
                net["ssid"] = WiFi.SSID(i);
                net["rssi"] = WiFi.RSSI(i);
                net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            WiFi.scanDelete();
        }
        
        sendJsonResponse(r, doc);
    });
    
    server.on("/wifi_test", HTTP_GET, [](AsyncWebServerRequest *r) {
        StaticJsonDocument<256> doc;
        
        // Block WiFi test during active measurement cycle
        if (loggingState != STATE_IDLE && loggingState != STATE_DONE) {
            doc["success"] = false;
            doc["error"] = "Cannot test WiFi during active measurement";
            sendJsonResponse(r, doc);
            return;
        }
        
        if (!r->hasParam("ssid")) {
            doc["success"] = false;
            doc["error"] = "No SSID";
            sendJsonResponse(r, doc);
            return;
        }
        
        String ssid = r->getParam("ssid")->value();
        String pass = r->hasParam("pass") ? r->getParam("pass")->value() : "";
        
        WiFi.disconnect();
        yield();  // Non-blocking
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        
        // Non-blocking wait with yield
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000) {
            yield();  // Allow background tasks
        }
        
        if (WiFi.status() == WL_CONNECTED) {
            doc["success"] = true;
            doc["ip"] = WiFi.localIP().toString();
            WiFi.disconnect();
        } else {
            doc["success"] = false;
            doc["error"] = "Connection failed";
        }
        
        // Restore original mode
        if (config.network.wifiMode == WIFIMODE_AP) {
            startAPMode();
        }
        
        sendJsonResponse(r, doc);
    });
    
    // ========== STATIC FILE HANDLER ==========
    // System files (logo, board, favicon, chart, changelog) - always from LittleFS
    // Other files - from activeFS (SD or LittleFS based on config)
    server.onNotFound([](AsyncWebServerRequest *r) {
        String path = r->url();
        
        // Determine content type
        String contentType = "application/octet-stream";
        if (path.endsWith(".svg")) contentType = "image/svg+xml";
        else if (path.endsWith(".png")) contentType = "image/png";
        else if (path.endsWith(".jpg") || path.endsWith(".jpeg")) contentType = "image/jpeg";
        else if (path.endsWith(".gif")) contentType = "image/gif";
        else if (path.endsWith(".ico")) contentType = "image/x-icon";
        else if (path.endsWith(".css")) contentType = "text/css";
        else if (path.endsWith(".js")) contentType = "application/javascript";
        else if (path.endsWith(".json")) contentType = "application/json";
        else if (path.endsWith(".html") || path.endsWith(".htm")) contentType = "text/html";
        else if (path.endsWith(".txt") || path.endsWith(".log") || path.endsWith(".csv")) contentType = "text/plain";
        
        // System files - ALWAYS check LittleFS first
        bool isSystemFile = path.startsWith("/logo") || path.startsWith("/board") || 
                           path.startsWith("/favicon") || path.startsWith("/chart") ||
                           path == "/changelog.txt" || path == "/style.css";
        
        if (isSystemFile && littleFsAvailable && LittleFS.exists(path)) {
            r->send(LittleFS, path, contentType);
            return;
        }
        
        // Try activeFS (SD or LittleFS based on config)
        if (fsAvailable && activeFS && activeFS->exists(path)) {
            r->send(*activeFS, path, contentType);
            return;
        }
        
        // Fallback to LittleFS for any file
        if (littleFsAvailable && LittleFS.exists(path)) {
            r->send(LittleFS, path, contentType);
            return;
        }
        
        r->send(404, "text/plain", "Not found");
    });
    
    // ========== CUSTOM OTA UPDATE ==========
    server.begin();
    DBGF("Web server started. Free heap: %d\n", ESP.getFreeHeap());
}
