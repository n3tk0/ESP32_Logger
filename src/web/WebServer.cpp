/**
 * src/web/WebServer.cpp
 * ESP32 Water Logger v4.2.0 – Production audit hardening
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
#include "ApiHandlers.h"
#include "../pipeline/DataPipeline.h"   // fsMutex (FS1)
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <functional>
#include <vector>
#include <math.h>

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

void sendJsonResponse(AsyncWebServerRequest *r, JsonDocument &doc) {
    String json;
    serializeJson(doc, json);
    r->send(200, "application/json", json);
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
// FAILSAFE HTML  (served when /www/index.html is missing)
// ============================================================================
static const char FAILSAFE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Water Logger - Setup Mode</title><style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:-apple-system,sans-serif;background:#f0f4f8;color:#2d3748;min-height:100vh}header{background:#275673;color:#fff;padding:16px 20px}header h1{font-size:1.2rem;display:flex;align-items:center;gap:10px}.badge{background:#e74c3c;color:#fff;border-radius:12px;padding:2px 10px;font-size:.75rem;font-weight:700}.sub{font-size:.8rem;opacity:.8;margin-top:4px}.container{max-width:720px;margin:20px auto;padding:0 14px}.card{background:#fff;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.09);margin-bottom:14px;overflow:hidden}.card-header{padding:13px 18px;border-bottom:1px solid #e2e8f0;font-weight:600;background:#f7fafc;display:flex;justify-content:space-between;align-items:center}.card-body{padding:16px 18px}.drop{border:2px dashed #cbd5e0;border-radius:8px;padding:24px;text-align:center;cursor:pointer;transition:.2s;margin-bottom:10px}.drop:hover,.drop.over{border-color:#275673;background:#ebf4ff}.drop input{display:none}.drop p{color:#718096;font-size:.85rem;margin-top:5px}.btn{display:inline-flex;align-items:center;gap:5px;padding:8px 16px;border:none;border-radius:7px;font-size:.88rem;font-weight:500;cursor:pointer;transition:.15s;text-decoration:none}.btn-primary{background:#275673;color:#fff}.btn-primary:hover{background:#1d4259}.btn-danger{background:#e74c3c;color:#fff}.btn-danger:hover{background:#c0392b}.btn-warn{background:#f39c12;color:#fff}.btn-warn:hover{background:#d68910}.btn-sm{padding:4px 10px;font-size:.78rem}progress{width:100%;height:8px;border-radius:4px;margin-top:8px;display:none}.msg{margin-top:8px;font-size:.88rem;min-height:1.1em}.ok{color:#27ae60}.err{color:#e74c3c}.inf{color:#275673}.file-list{font-size:.85rem}.file-row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid #e2e8f0;gap:6px}.file-row:last-child{border:none}.fname{word-break:break-all;flex:1}.fsize{color:#718096;white-space:nowrap;margin:0 8px}.acts{display:flex;gap:5px;flex-shrink:0}.alert{padding:11px 15px;border-radius:8px;margin-bottom:12px;font-size:.88rem;line-height:1.4}.alert-warn{background:#fef3c7;color:#92400e;border:1px solid #fcd34d}.legacy{background:#fff3cd;border-left:4px solid #f39c12;padding:6px 10px;border-radius:4px;font-size:.8rem;color:#856404;margin-top:4px}input[type=text]{width:100%;padding:7px 11px;border:1px solid #e2e8f0;border-radius:6px;font-size:.88rem}.section-label{font-size:.75rem;font-weight:700;text-transform:uppercase;letter-spacing:.05em;color:#718096;padding:10px 0 4px}.sel{width:100%;padding:7px 10px;border:1px solid #e2e8f0;border-radius:6px;font-size:.88rem;margin-top:4px;background:#fff;color:#2d3748}.fhint{font-size:.78rem;color:#718096;margin-top:4px}.chk-row{display:flex;align-items:center;gap:8px;padding:7px 0;border-bottom:1px solid #e2e8f0;font-size:.88rem}.chk-row:last-child{border:none}.warn-box{background:#fef3c7;border:1px solid #fcd34d;border-radius:7px;padding:11px 14px;font-size:.83rem;color:#92400e;margin-top:8px;display:none}.warn-box.show{display:block}.flabel{font-size:.8rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em;color:#718096;display:block;margin-bottom:2px}.tabs{display:flex;gap:0;border-bottom:2px solid #e2e8f0;margin-bottom:16px}.tab{padding:10px 20px;border:none;background:none;cursor:pointer;font-size:.9rem;color:#718096;font-weight:500;border-bottom:3px solid transparent;margin-bottom:-2px;transition:.15s;display:flex;align-items:center;gap:6px}.tab:hover{color:#275673;background:#f7fafc}.tab.active{color:#275673;border-bottom-color:#275673;font-weight:700}.tab-pane{display:none}.tab-pane.active{display:block}</style></head><body>)HTML"
R"HTML(<header><h1>&#x1F4A7; Water Logger <span class="badge">SETUP MODE</span></h1><div class="sub">Upload UI files to /www/ to restore normal operation &mdash; or bookmark <strong>/setup</strong> for recovery</div><div id="fs-sysinfo" class="sub" style="font-size:.72rem;margin-top:3px;opacity:.8"></div></header><div class="container"><div class="tabs"><button class="tab active" id="tab-btn-setup" onclick="switchTab('setup',this)">&#x2699;&#xFE0F; Setup</button><button class="tab" id="tab-btn-corelogic" onclick="switchTab('corelogic',this)">&#x1F9E9; Core Logic</button></div>)HTML"
R"HTML(<div id="tab-setup" class="tab-pane active"><div class="alert alert-warn">&#x26A0;&#xFE0F; <strong>Normal UI not found.</strong> Upload <code>index.html</code>, <code>web.js</code> and <code>style.css</code> into <code>/www/</code>. If you see a broken page normally, you likely have <strong>old files at the root</strong> &mdash; delete them below. If the main UI is broken, navigate to <code>/setup</code> at any time to return here.</div>)HTML"
R"HTML(<div class="card"><div class="card-header">&#x1F4E4; Upload files to /www/</div><div class="card-body"><div class="drop" id="dropZone" onclick="document.getElementById('fileInput').click()"><input type="file" id="fileInput" multiple>&#x2B06; <strong>Click or drag files here</strong><p>index.html &bull; web.js &bull; style.css &bull; changelog.txt &bull; chart.min.js &bull; etc.</p></div><progress id="prog" value="0" max="100"></progress><div class="msg inf" id="uploadMsg"></div></div></div>)HTML"
R"HTML(<div class="card"><div class="card-header"><span>&#x1F4C1; LittleFS &mdash; All Files</span><button class="btn btn-sm btn-primary" onclick="loadFiles()">&#x21BA; Refresh</button></div><div class="card-body" style="padding:4px 18px 14px"><div id="legacyWarn" style="display:none" class="legacy">&#x26A0;&#xFE0F; <strong>Legacy UI files found at root.</strong> These override /www/ files and cause broken pages. Delete them!</div><div class="section-label">&#x1F4C2; /www/ (new UI files)</div><div class="file-list" id="wwwList">Loading&#x2026;</div><div class="section-label" style="margin-top:10px">&#x1F4C2; / (root &mdash; legacy / system files)</div><div class="file-list" id="rootList">Loading&#x2026;</div></div></div>)HTML"
R"HTML(<div class="card"><div class="card-header">&#x270F;&#xFE0F; Rename / Move File</div><div class="card-body"><div style="display:flex;gap:8px;flex-wrap:wrap"><input type="text" id="renSrc" placeholder="From: e.g. /web.js" style="flex:1;min-width:140px"><input type="text" id="renDst" placeholder="To: e.g. /www/web.js" style="flex:1;min-width:140px"><button class="btn btn-primary" onclick="doRename()">Move</button></div><div class="msg" id="renMsg"></div></div></div>)HTML"
R"HTML(<div class="card"><div class="card-header">&#x1F504; Device Control</div><div class="card-body" style="display:flex;gap:10px;flex-wrap:wrap;align-items:center"><button class="btn btn-primary" onclick="if(confirm('Restart now?'))fetch('/restart').then(function(){setTimeout(function(){location.reload();},5000)})">&#x1F504; Restart</button><button class="btn btn-danger" onclick="doFactoryReset()">&#x1F9F9; Factory Reset</button><div class="msg" id="fsResetMsg" style="flex-basis:100%;margin-top:4px"></div></div></div>)HTML"
R"HTML(<div class="card"><div class="card-header">&#x1F6E0;&#xFE0F; OTA Firmware Update</div><div class="card-body"><p style="font-size:.85rem;color:#718096;margin-bottom:10px">Upload a <code>.bin</code> firmware file compiled for ESP32-C3. The device will restart automatically after a successful flash.</p><div class="drop" id="otaDropZone" onclick="document.getElementById('otaFile').click()"><input type="file" id="otaFile" accept=".bin">&#x2B06; <strong>Click or drag .bin file here</strong><p>Firmware must start with magic byte 0xE9</p></div><progress id="otaProg" value="0" max="100" style="display:none"></progress><div class="msg" id="otaMsg"></div></div></div></div>)HTML"
R"HTML(<div id="tab-corelogic" class="tab-pane"><div class="card"><div class="card-header">&#x2699;&#xFE0F; Operating Mode</div><div class="card-body"><label class="flabel">Mode</label><select id="cl-mode" class="sel"><option value="legacy">Legacy &mdash; Water logger only (deep sleep, original behaviour)</option><option value="continuous">Continuous &mdash; Multi-sensor pipeline (FreeRTOS tasks)</option><option value="hybrid">Hybrid &mdash; Water logger + sensor pipeline</option></select><p class="fhint"><strong>Legacy</strong>: original behaviour, deep-sleep between flush events.<br><strong>Continuous</strong>: all sensors polled continuously; device stays awake.<br><strong>Hybrid</strong>: both modes active simultaneously.</p></div></div>)HTML"
R"HTML(<div class="card"><div class="card-header">&#x1F4A4; Sleep Mode</div><div class="card-body"><label class="flabel">Sleep Mode</label><select id="cl-sleep" class="sel" onchange="fsSlpChk()"><option value="deep">Deep Sleep &mdash; maximum power saving (recommended for battery)</option><option value="light">Light Sleep &mdash; faster wake-up, moderate power use</option><option value="none">No Sleep &mdash; device stays awake, Wi-Fi may disconnect between events</option><option value="online">&#x1F310; Online Mode &mdash; always awake, Wi-Fi + web server permanently active</option></select><div id="cl-slp-warn" class="warn-box">&#x26A0;&#xFE0F; <strong>Online Mode</strong> keeps Wi-Fi and the web server permanently active. Power consumption increases significantly (&asymp;50&ndash;200&thinsp;mA continuously). This mode is recommended <strong>only when connected to mains power</strong>. <strong>Not suitable for battery operation.</strong></div><p class="fhint">Controls how the device behaves between measurement events. Takes effect after restart.</p></div></div>)HTML"
R"HTML(<div class="card"><div class="card-header" style="display:flex;justify-content:space-between;align-items:center"><span>&#x1F50C; Sensors &mdash; Enable / Disable</span><button class="btn btn-sm btn-primary" onclick="fsAddSensor()">+ Add Sensor</button></div><div class="card-body" style="padding:4px 18px 14px"><div id="cl-slist"><span style="color:#718096;font-size:.85rem">Loading&hellip;</span></div></div></div><div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:16px"><button class="btn btn-primary" onclick="fsClSave()">&#x1F4BE; Save &amp; Restart</button><button class="btn btn-warn" onclick="fsClLoad()">&#x21BA; Reload</button><span id="cl-msg" class="msg"></span></div></div></div>)HTML"
R"HTML(<script>
function switchTab(n,b){document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active')});document.querySelectorAll('.tab-pane').forEach(function(p){p.classList.remove('active');p.style.display='none'});b.classList.add('active');var p=document.getElementById('tab-'+n);if(p){p.classList.add('active');p.style.display='block'}if(n==='corelogic'&&!FS_PCFG)fsClLoad()}
var LEG=['/web.js','/style.css','/index.html','/index.htm'];
var dz=document.getElementById('dropZone');dz.addEventListener('dragover',function(e){e.preventDefault();this.classList.add('over')});dz.addEventListener('dragleave',function(){this.classList.remove('over')});dz.addEventListener('drop',function(e){e.preventDefault();this.classList.remove('over');upF(e.dataTransfer.files)});document.getElementById('fileInput').addEventListener('change',function(){upF(this.files)});
function upF(fs){if(!fs||!fs.length)return;var pg=document.getElementById('prog'),mg=document.getElementById('uploadMsg'),i=0;pg.style.display='block';mg.className='msg inf';(function nx(){if(i>=fs.length){mg.textContent='Done! '+fs.length+' file(s) uploaded to /www/.';mg.className='msg ok';pg.style.display='none';document.getElementById('fileInput').value='';ldF();return}var fd=new FormData();fd.append('file',fs[i]);fd.append('path','/www/');var x=new XMLHttpRequest();x.upload.onprogress=function(e){if(e.lengthComputable)pg.value=Math.round(e.loaded/e.total*100)};x.onload=function(){mg.textContent='Uploaded: '+fs[i].name+' ('+(i+1)+'/'+fs.length+')';i++;nx()};x.onerror=function(){mg.textContent='Error: '+fs[i].name;mg.className='msg err';pg.style.display='none'};x.open('POST','/upload');x.send(fd)})()}
function fmB(b){if(!b)return'0 B';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B'}
function fRow(f){var lg=LEG.indexOf(f.path)>=0;return'<div class="file-row"'+(lg?' style="background:#fff8e1"':'')+'><span class="fname">'+(lg?'&#x26A0;&#xFE0F; ':'&#x1F4C4; ')+f.path+(lg?' <span style="color:#e67e22;font-size:.75rem">[LEGACY - DELETE]</span>':'')+'</span><span class="fsize">'+fmB(f.size)+'</span><span class="acts"><a href="/download?file='+encodeURIComponent(f.path)+'&storage=internal" class="btn btn-sm btn-primary">&#x1F4E5;</a> <button class="btn btn-sm btn-danger" data-path="'+f.path+'" onclick="dlF(this.dataset.path)">&#x1F5D1;</button></span></div>'}
function ldF(){var w=document.getElementById('wwwList'),r=document.getElementById('rootList'),wn=document.getElementById('legacyWarn');if(w.innerHTML==='')w.innerHTML='Loading&#x2026;';if(r.innerHTML==='')r.innerHTML='Loading&#x2026;';fetch('/api/filelist?storage=internal&dir=/www/').then(function(r){return r.json()}).then(function(d){var f=d.files||[];if(!f.length){w.innerHTML='<div style="padding:8px 0;color:#718096">Empty &mdash; upload files here</div>';return}w.innerHTML=f.map(function(x){return fRow(x)}).join('')}).catch(function(){w.innerHTML='<span class="err">Error</span>'});fetch('/api/filelist?storage=internal&dir=/').then(function(r){return r.json()}).then(function(d){var f=(d.files||[]).filter(function(x){return!x.isDir});if(!f.length){r.innerHTML='<div style="padding:8px 0;color:#718096">Empty</div>';wn.style.display='none';return}wn.style.display=f.some(function(x){return LEG.indexOf(x.path)>=0})?'block':'none';r.innerHTML=f.map(function(x){return fRow(x)}).join('')}).catch(function(){r.innerHTML='<span class="err">Error</span>'})}
function dlF(p){if(!confirm('Delete '+p+'?'))return;fetch('/delete?path='+encodeURIComponent(p)+'&storage=internal').then(function(r){return r.json()}).then(function(j){if(!j||!j.ok){alert('Delete failed: '+((j&&j.error)?j.error:'unknown error'));return}ldF()}).catch(function(e){alert('Error: '+e)})}
function doRename(){var s=document.getElementById('renSrc').value.trim(),d=document.getElementById('renDst').value.trim(),m=document.getElementById('renMsg');if(!s||!d){m.textContent='Both fields required.';m.className='msg err';return}var p=d.lastIndexOf('/'),nn=d.substring(p+1),dd=p<=0?'/':d.substring(0,p);fetch('/move_file?src='+encodeURIComponent(s)+'&newName='+encodeURIComponent(nn)+'&destDir='+encodeURIComponent(dd)+'&storage=internal').then(function(){m.textContent='Done: '+s+' -> '+d;m.className='msg ok';ldF()}).catch(function(e){m.textContent='Error: '+e;m.className='msg err'})}
ldF();
function doFactoryReset(){if(!confirm('\u26a0\ufe0f FACTORY RESET\n\nThis will erase ALL files on LittleFS (config, UI, logs) and restart.\n\nProceed?'))return;var a=prompt('Type RESET to confirm:');if(a!=='RESET'){alert('Cancelled.');return}var m=document.getElementById('fsResetMsg');if(m){m.textContent='Factory reset in progress\u2026';m.className='msg inf'}fetch('/factory_reset',{method:'POST'}).then(function(r){return r.json()}).then(function(d){if(d.ok){alert('LittleFS formatted. Device is restarting.\nReconnect in ~10 seconds.');setTimeout(function(){location.reload()},10000)}else{if(m){m.textContent='Reset failed: '+(d.error||'unknown');m.className='msg err'}else alert('Reset failed: '+(d.error||'unknown'))}}).catch(function(e){if(m){m.textContent='Error: '+e;m.className='msg err'}else alert('Error: '+e)})}
var odz=document.getElementById('otaDropZone');odz.addEventListener('dragover',function(e){e.preventDefault();this.classList.add('over')});odz.addEventListener('dragleave',function(){this.classList.remove('over')});odz.addEventListener('drop',function(e){e.preventDefault();this.classList.remove('over');var f=e.dataTransfer.files[0];if(f)doOta(f)});document.getElementById('otaFile').addEventListener('change',function(){if(this.files.length)doOta(this.files[0])});
function doOta(file){var m=document.getElementById('otaMsg'),pg=document.getElementById('otaProg');if(!file.name.toLowerCase().endsWith('.bin')){m.textContent='Error: file must be a .bin firmware file.';m.className='msg err';return}if(file.size<10240){m.textContent='Error: file too small (min 10 KB).';m.className='msg err';return}var rd=new FileReader();rd.onload=function(ev){var by=new Uint8Array(ev.target.result);if(by[0]!==0xE9){m.textContent='Error: invalid firmware (wrong magic byte \u2013 expected 0xE9, got 0x'+by[0].toString(16)+').';m.className='msg err';return}pg.style.display='block';pg.value=0;m.textContent='Uploading\u2026';m.className='msg inf';var fd=new FormData();fd.append('firmware',file);var x=new XMLHttpRequest();x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);pg.value=p;m.textContent='Uploading: '+p+'% ('+Math.round(e.loaded/1024)+' / '+Math.round(e.total/1024)+' KB)'}};x.onload=function(){pg.style.display='none';try{var r=JSON.parse(x.responseText);if(r.success){m.textContent='\u2705 '+r.message+' \u2013 reconnect in ~10 seconds.';m.className='msg ok';setTimeout(function(){location.reload()},10000)}else{m.textContent='\u274c '+r.message;m.className='msg err'}}catch(e){m.textContent='\u2705 Firmware sent \u2013 device restarting\u2026';m.className='msg ok';setTimeout(function(){location.reload()},10000)}};x.onerror=function(){pg.style.display='none';m.textContent='\u274c Upload failed \u2013 connection error.';m.className='msg err'};x.open('POST','/do_update');x.send(fd)};rd.readAsArrayBuffer(file.slice(0,4))}
var FS_PCFG=null;
function fsClLoad(){var m=document.getElementById('cl-msg');if(m){m.textContent='Loading\u2026';m.className='msg inf'}fetch('/api/platform_config').then(function(r){return r.json()}).then(function(c){FS_PCFG=c;var mE=document.getElementById('cl-mode');if(mE)mE.value=c.mode||'legacy';var sE=document.getElementById('cl-sleep');if(sE){sE.value=c.sleep_mode||'deep';fsSlpChk()}var sl=document.getElementById('cl-slist');if(sl){var sn=c.sensors||[];if(!sn.length){sl.innerHTML='<span style="color:#718096;font-size:.85rem">No sensors configured.</span>'}else{sl.innerHTML=sn.map(function(s,i){return'<div class="chk-row" style="display:flex;align-items:center;gap:6px"><input type="checkbox" id="fss-'+i+'"'+(s.enabled?' checked':'')+' onchange="fsSensorToggle('+i+',this.checked)"><label for="fss-'+i+'" style="cursor:pointer;flex:1;overflow:hidden;text-overflow:ellipsis"><strong>'+(s.id||s.type)+'</strong> <span style="color:#718096;font-size:.78rem">('+s.type+' \u00b7 '+(s.interface||'')+(s.status==='ok'?' \u2705':s.status==='err'?' \u274C':'')+')</span></label><button class="btn btn-sm btn-secondary" onclick="fsEditSensor('+i+')">&#x270F;&#xFE0F;</button><button class="btn btn-sm btn-danger" onclick="fsRmSensor('+i+')">&#x1F5D1;&#xFE0F;</button></div>'}).join('')}}if(m){m.textContent='';m.className='msg'}}).catch(function(e){if(m){m.textContent='Load failed: '+e;m.className='msg err'}})}
function fsSensorToggle(i,en){if(FS_PCFG&&FS_PCFG.sensors&&FS_PCFG.sensors[i])FS_PCFG.sensors[i].enabled=en}
function fsClAddSensor(){var tp=prompt('Sensor type:\n'+FS_SENSOR_TYPES.map(function(t){return t.value+' \u2014 '+t.label+' ('+t.iface+')'}).join('\n'));if(!tp)return;tp=tp.trim().toLowerCase();var info=FS_SENSOR_TYPES.find(function(t){return t.value===tp});if(!info){alert('Unknown type: '+tp);return}var sid=prompt('Sensor ID (unique name):',tp+'_'+(FS_PCFG&&FS_PCFG.sensors?FS_PCFG.sensors.length+1:1));if(!sid)return;if(!FS_PCFG)FS_PCFG={sensors:[]};if(!FS_PCFG.sensors)FS_PCFG.sensors=[];var ns={id:sid.trim(),type:tp,enabled:true,interface:info.iface};if(info.iface==='i2c'){ns.sda=6;ns.scl=7;ns.read_interval_ms=10000}if(info.iface==='uart'){ns.uart_rx=20;ns.uart_tx=-1;ns.baud=9600}if(info.iface==='pulse'){ns.pin=9;ns.read_interval_ms=5000}if(info.iface==='analog'){ns.pin=0;ns.adc_samples=64;ns.read_interval_ms=1000;if(tp==='zmpt101b')ns.voltage_factor=1.0;if(tp==='zmct103c')ns.current_factor=1.0}FS_PCFG.sensors.push(ns);fsClRenderSensors(FS_PCFG.sensors)}
function fsClEditSensor(i){if(!FS_PCFG||!FS_PCFG.sensors)return;var s=FS_PCFG.sensors[i];var j=prompt('Edit sensor JSON:',JSON.stringify(s));if(!j)return;try{var ns=JSON.parse(j);var PK=['pin','sda','scl','uart_rx','uart_tx','trig_pin','echo_pin'];var used={};FS_PCFG.sensors.forEach(function(x,xi){if(xi===i)return;PK.forEach(function(k){if(x[k]!=null&&x[k]>=0)used[x[k]]=(used[x[k]]||[]).concat(x.id||x.type)})});var cfls=[];PK.forEach(function(k){if(ns[k]!=null&&ns[k]>=0&&used[ns[k]])cfls.push('GPIO'+ns[k]+' ('+k+') used by: '+used[ns[k]].join(', '))});if(cfls.length)alert('\u26a0\ufe0f Pin conflict:\n'+cfls.join('\n'));FS_PCFG.sensors[i]=ns;fsClRenderSensors(FS_PCFG.sensors)}catch(e){alert('Invalid JSON: '+e.message)}}
function fsClRemoveSensor(i){if(!FS_PCFG||!FS_PCFG.sensors)return;if(!confirm('Remove "'+( FS_PCFG.sensors[i].id||FS_PCFG.sensors[i].type)+'"?'))return;FS_PCFG.sensors.splice(i,1);fsClRenderSensors(FS_PCFG.sensors)}
function fsSlpChk(){var s=document.getElementById('cl-sleep'),w=document.getElementById('cl-slp-warn');if(s&&w)w.className=(s.value==='online')?'warn-box show':'warn-box'}
function fsClSave(){var m=document.getElementById('cl-msg');if(!FS_PCFG){if(m){m.textContent='No config \u2014 click Reload first.';m.className='msg err'}return}var mE=document.getElementById('cl-mode'),sE=document.getElementById('cl-sleep');if(mE)FS_PCFG.mode=mE.value;if(sE)FS_PCFG.sleep_mode=sE.value;if(m){m.textContent='Saving\u2026';m.className='msg inf'}var x=new XMLHttpRequest();x.open('POST','/save_platform');x.setRequestHeader('Content-Type','application/json');x.onload=function(){try{var r=JSON.parse(x.responseText);if(r&&r.ok){if(m){m.textContent='\u2705 Saved! Restarting\u2026';m.className='msg ok'}setTimeout(function(){fetch('/api/platform_reload',{method:'POST'}).catch(function(){});setTimeout(function(){location.reload()},5000)},300)}else{if(m){m.textContent='\u274c Save failed';m.className='msg err'}}}catch(e){if(m){m.textContent='\u274c '+e;m.className='msg err'}}};x.onerror=function(){if(m){m.textContent='\u274c Network error';m.className='msg err'}};x.send(JSON.stringify(FS_PCFG))}
function fsAddSensor(){var j=prompt("Add sensor JSON:","{\"type\":\"bme280\",\"enabled\":true}");if(j){try{var o=JSON.parse(j);if(!FS_PCFG)FS_PCFG={};if(!FS_PCFG.sensors)FS_PCFG.sensors=[];FS_PCFG.sensors.push(o);fsClLoad()}catch(e){alert("Invalid JSON: "+e)}}}
function fsEditSensor(i){if(!FS_PCFG||!FS_PCFG.sensors)return;var j=prompt("Edit sensor JSON:",JSON.stringify(FS_PCFG.sensors[i]));if(j){try{FS_PCFG.sensors[i]=JSON.parse(j);fsClLoad()}catch(e){alert("Invalid JSON: "+e)}}}
function fsRmSensor(i){if(!FS_PCFG||!FS_PCFG.sensors)return;if(confirm("Remove sensor?")){FS_PCFG.sensors.splice(i,1);fsClLoad()}}
</script></body></html>)HTML";



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
}

// ============================================================================
// IP ARRAY FORMATTER  (uint8_t[4] → "A.B.C.D")
// ============================================================================
static void fmtIP(const uint8_t* ip, char* buf16) {
    snprintf(buf16, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// ============================================================================
// WEB SERVER SETUP
// ============================================================================
void setupWebServer() {
    Serial.println("Setting up web server...");

    // C2: track web activity for idle power restore
    auto touchActivity = []() { g_lastWebActivity = millis(); };

    bool uiReady = LittleFS.exists("/www/index.html");

    if (uiReady) {
        server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");
        Serial.println("Web UI: serving from /www/");
    } else {
        server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
            if (littleFsAvailable && LittleFS.exists("/www/index.html")) {
                r->send(LittleFS, "/www/index.html", "text/html");
                return;
            }
            r->send_P(200, "text/html", FAILSAFE_HTML);
        });
        Serial.println("Web UI: FAILSAFE mode (upload /www/index.html to restore)");
    }

    server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send_P(200, "text/html", FAILSAFE_HTML);
    });

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
    // API: STATUS
    // Keys consumed by web.js:
    //   applyStatus: device, deviceId, version, time, network, ip, boot, heap,
    //                heapTotal, chip, cpu, mode, theme{...}, freeSketch
    //   sdInit:      device/deviceName, deviceId, defaultStorageView,
    //                forceWebServer, version, boot, mode, heap, cpu, chip
    //   timeInit:    time, boot, rtcRunning, rtcProtected, wifi, ip
    //   netInit:     wifi, network, ip
    // =========================================================================
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
        g_lastWebActivity = millis();   // C2: any poll = active user
        JsonDocument doc;

        // ── Identity ──────────────────────────────────────────────────────────
        doc["device"]         = strlen(config.deviceName) ? config.deviceName : "Water Logger";
        doc["deviceName"]     = doc["device"];   // alias – sdInit uses both
        doc["deviceId"]       = config.deviceId;
        doc["version"]        = getVersionString();
        doc["forceWebServer"] = config.forceWebServer;

        // ── Time / Network ────────────────────────────────────────────────────
        doc["time"]    = getRtcDateTimeString();
        doc["network"] = getNetworkDisplay();
        doc["ip"]      = wifiConnectedAsClient
                         ? WiFi.localIP().toString()
                         : WiFi.softAPIP().toString();

        doc["rssi"]    = wifiConnectedAsClient ? WiFi.RSSI() : -100;

        doc["gateway"] = wifiConnectedAsClient ? WiFi.gatewayIP().toString() : "";
        doc["subnet"]  = wifiConnectedAsClient ? WiFi.subnetMask().toString() : "";
        doc["dns"]     = wifiConnectedAsClient ? WiFi.dnsIP().toString() : "";

        // ── Runtime metrics ───────────────────────────────────────────────────
        doc["boot"]       = bootCount;
        doc["heap"]       = ESP.getFreeHeap();
        doc["heapTotal"]  = ESP.getHeapSize();
        doc["heapPct"]    = (int)(ESP.getFreeHeap() * 100UL / ESP.getHeapSize());
        doc["chip"]       = ESP.getChipModel();
        doc["cpu"]        = getCpuFrequencyMhz();
        doc["mode"]       = getModeDisplay();
        doc["wifi"]       = wifiConnectedAsClient ? "client" : "ap";
        doc["freeSketch"] = ESP.getFreeSketchSpace();

        // ── Storage ───────────────────────────────────────────────────────────
        uint64_t used = 0, total = 0; int pct = 0;
        getStorageInfo(used, total, pct);
        char uBuf[24], tBuf[24];
        snprintf(uBuf, sizeof(uBuf), "%llu", (unsigned long long)used);
        snprintf(tBuf, sizeof(tBuf), "%llu", (unsigned long long)total);
        doc["fsUsed"]             = serialized(String(uBuf));
        doc["fsTotal"]            = serialized(String(tBuf));
        doc["fsPct"]              = pct;
        doc["defaultStorageView"] = config.hardware.defaultStorageView;
        doc["currentFile"]        = getActiveDatalogFile();

        // ── RTC (null-safe) ───────────────────────────────────────────────────
        if (Rtc) {
            doc["rtcProtected"] = Rtc->GetIsWriteProtected();
            doc["rtcRunning"]   = Rtc->GetIsRunning();
        } else {
            doc["rtcProtected"] = false;
            doc["rtcRunning"]   = false;
        }

        // ── Theme (nested) ────────────────────────────────────────────────────
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
        th["chartLocalPath"]    = strlen(config.theme.chartLocalPath) ? config.theme.chartLocalPath : "/chart.min.js";
        th["chartLabelFormat"]  = (int)config.theme.chartLabelFormat;
        th["showIcons"]         = config.theme.showIcons;

        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: LIVE  (polled every 500 ms by SPA live page)
    // =========================================================================
    server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;

        noInterrupts();
        uint32_t safePulses = pulseCount;
        interrupts();

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

        sendJsonResponse(r, doc);
    });

    // =========================================================================
    // API: RECENT LOGS
    // =========================================================================
    server.on("/api/recent_logs", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        JsonArray logs = doc["logs"].to<JsonArray>();

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

        // Efficient tail-read: seek to last ~1KB of file instead of reading every line
        char lastLines[5][160];
        int lCount = 0;
        size_t fSize = f.size();
        const size_t TAIL_BYTES = 1024;

        if (fSize > TAIL_BYTES) {
            f.seek(fSize - TAIL_BYTES);
            f.readStringUntil('\n');   // discard partial first line
        }

        char lineBuf[160];
        while (f.available()) {
            int i = 0;
            while (f.available() && i < 159) {
                char c = f.read();
                if (c == '\n' || c == '\r') break;
                lineBuf[i++] = c;
            }
            lineBuf[i] = '\0';
            // skip empty lines
            if (i > 0) {
                memcpy(lastLines[lCount % 5], lineBuf, i + 1);
                lCount++;
            }
        }
        f.close();

        int count = lCount < 5 ? lCount : 5;
        for (int i = 0; i < count; i++) {
            int idx = (lCount - 1 - i) % 5;
            char* lineStr = lastLines[idx];
            
            char* saveptr;
            char* tokens[10];
            int tCount = 0;
            char* tok = strtok_r(lineStr, "|", &saveptr);
            while (tok && tCount < 10) {
                tokens[tCount++] = tok;
                tok = strtok_r(NULL, "|", &saveptr);
            }

            if (tCount >= 7) {
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
    });

    // =========================================================================
    // API: FILE LIST
    // =========================================================================
    server.on("/api/filelist", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        JsonArray files = doc["files"].to<JsonArray>();

        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        String dir     = r->hasParam("dir")     ? r->getParam("dir")->value()     : "/";
        String filter  = r->hasParam("filter")  ? r->getParam("filter")->value()  : "";
        bool recursive = r->hasParam("recursive");

        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)              targetFS = &SD;
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        else if (littleFsAvailable)                          targetFS = &LittleFS;

        if (targetFS) {
            scanDir(*targetFS, dir, files, filter, recursive);
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
    });

    // =========================================================================
    // API: CHANGELOG
    // =========================================================================
    server.on("/api/changelog", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (LittleFS.exists("/www/changelog.txt"))
            r->send(LittleFS, "/www/changelog.txt", "text/plain");
        else if (LittleFS.exists("/changelog.txt"))
            r->send(LittleFS, "/changelog.txt", "text/plain");
        else
            r->send(404, "text/plain", "Changelog not found. Upload /www/changelog.txt");
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
    // EXPORT SETTINGS
    // Keys consumed by web.js:
    //   sfInit:  flowMeter{pulsesPerLiter, calibrationMultiplier, monitoringWindowSecs,
    //                       firstLoopMonitoringWindowSecs, testMode, blinkDuration}
    //   hwInit:  hardware{storageType, pinSdCS, pinSdMOSI, pinSdMISO, pinSdSCK,
    //                      wakeupMode, debounceMs, pinWifiTrigger, pinWakeupFF,
    //                      pinWakeupPF, pinFlowSensor, pinRtcCE, pinRtcIO, pinRtcSCLK, cpuFreqMHz}
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
    server.on("/export_settings", HTTP_GET, [](AsyncWebServerRequest *r) {
        char ipBuf[16];

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
        th["chartLocalPath"]    = strlen(config.theme.chartLocalPath) ? config.theme.chartLocalPath : "/chart.min.js";
        th["chartLabelFormat"]  = (int)config.theme.chartLabelFormat;
        th["showIcons"]         = config.theme.showIcons;

        // ── Flow Meter ────────────────────────────────────────────────────────
        JsonObject fm = doc["flowMeter"].to<JsonObject>();
        fm["pulsesPerLiter"]                = config.flowMeter.pulsesPerLiter > 0    ? config.flowMeter.pulsesPerLiter    : 450.0f;
        fm["calibrationMultiplier"]         = config.flowMeter.calibrationMultiplier ? config.flowMeter.calibrationMultiplier : 1.0f;
        fm["monitoringWindowSecs"]          = config.flowMeter.monitoringWindowSecs > 0 ? config.flowMeter.monitoringWindowSecs : 3;
        fm["firstLoopMonitoringWindowSecs"] = config.flowMeter.firstLoopMonitoringWindowSecs > 0 ? config.flowMeter.firstLoopMonitoringWindowSecs : 6;
        fm["testMode"]                      = config.flowMeter.testMode;
        fm["blinkDuration"]                 = config.flowMeter.blinkDuration > 0 ? config.flowMeter.blinkDuration : 250;

        // ── Datalog ───────────────────────────────────────────────────────────
        JsonObject dl = doc["datalog"].to<JsonObject>();
        dl["rotation"]               = (int)config.datalog.rotation;
        dl["maxSizeKB"]              = config.datalog.maxSizeKB > 0 ? config.datalog.maxSizeKB : 1024;
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

        // ── Network ───────────────────────────────────────────────────────────
        JsonObject net = doc["network"].to<JsonObject>();
        net["wifiMode"]       = (int)config.network.wifiMode;
        net["apSSID"]         = strlen(config.network.apSSID)         ? config.network.apSSID         : DEFAULT_AP_SSID;
        net["apPassword"]     = config.network.apPassword;
        net["clientSSID"]     = config.network.clientSSID;
        net["clientPassword"] = config.network.clientPassword;
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

        String json;
        serializeJsonPretty(doc, json);
        AsyncWebServerResponse *resp = r->beginResponse(200, "application/json", json);
        String fn = String(strlen(config.deviceName) ? config.deviceName : "device") + "_settings.json";
        resp->addHeader("Content-Disposition", "attachment; filename=\"" + fn + "\"");
        r->send(resp);
    });

    // =========================================================================
    // SAVE ENDPOINTS
    // =========================================================================

    server.on("/save_device", HTTP_POST, [](AsyncWebServerRequest *r) {
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
        saveConfig();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/save_flowmeter", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("pulsesPerLiter", true)) {
            float v = r->getParam("pulsesPerLiter", true)->value().toFloat();
            config.flowMeter.pulsesPerLiter = (v >= 1.0f && isfinite(v)) ? v : 450.0f;
        }
        if (r->hasParam("calibrationMultiplier", true)) {
            float v = r->getParam("calibrationMultiplier", true)->value().toFloat();
            config.flowMeter.calibrationMultiplier = (v > 0.0f && isfinite(v)) ? v : 1.0f;
        }
        if (r->hasParam("monitoringWindowSecs", true))
            config.flowMeter.monitoringWindowSecs = max(1L, r->getParam("monitoringWindowSecs", true)->value().toInt());
        if (r->hasParam("firstLoopWindowSecs", true))
            config.flowMeter.firstLoopMonitoringWindowSecs = max(1L, r->getParam("firstLoopWindowSecs", true)->value().toInt());
        config.flowMeter.testMode = r->hasParam("testMode", true);
        if (r->hasParam("blinkDuration", true))
            config.flowMeter.blinkDuration = max(50L, r->getParam("blinkDuration", true)->value().toInt());
        if (r->hasParam("resetBootCount", true)) { bootCount = 0; backupBootCount(); }
        saveConfig();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/save_hardware", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("storageType", true))    config.hardware.storageType    = (StorageType)r->getParam("storageType", true)->value().toInt();
        if (r->hasParam("wakeupMode", true))     config.hardware.wakeupMode     = (WakeupMode)r->getParam("wakeupMode", true)->value().toInt();
        if (r->hasParam("pinWifiTrigger", true)) config.hardware.pinWifiTrigger = r->getParam("pinWifiTrigger", true)->value().toInt();
        if (r->hasParam("pinWakeupFF", true))    config.hardware.pinWakeupFF    = r->getParam("pinWakeupFF", true)->value().toInt();
        if (r->hasParam("pinWakeupPF", true))    config.hardware.pinWakeupPF    = r->getParam("pinWakeupPF", true)->value().toInt();
        if (r->hasParam("pinFlowSensor", true))  config.hardware.pinFlowSensor  = r->getParam("pinFlowSensor", true)->value().toInt();
        if (r->hasParam("pinRtcCE", true))       config.hardware.pinRtcCE       = r->getParam("pinRtcCE", true)->value().toInt();
        if (r->hasParam("pinRtcIO", true))       config.hardware.pinRtcIO       = r->getParam("pinRtcIO", true)->value().toInt();
        if (r->hasParam("pinRtcSCLK", true))     config.hardware.pinRtcSCLK     = r->getParam("pinRtcSCLK", true)->value().toInt();
        if (r->hasParam("pinSdCS", true))        config.hardware.pinSdCS        = r->getParam("pinSdCS", true)->value().toInt();
        if (r->hasParam("pinSdMOSI", true))      config.hardware.pinSdMOSI      = r->getParam("pinSdMOSI", true)->value().toInt();
        if (r->hasParam("pinSdMISO", true))      config.hardware.pinSdMISO      = r->getParam("pinSdMISO", true)->value().toInt();
        if (r->hasParam("pinSdSCK", true))       config.hardware.pinSdSCK       = r->getParam("pinSdSCK", true)->value().toInt();
        if (r->hasParam("cpuFreqMHz", true))     config.hardware.cpuFreqMHz     = r->getParam("cpuFreqMHz", true)->value().toInt();
        if (r->hasParam("debounceMs", true))     config.hardware.debounceMs     = constrain(r->getParam("debounceMs", true)->value().toInt(), 20, 500);
        if (r->hasParam("debugMode", true))      config.hardware.debugMode      = r->getParam("debugMode", true)->value() == "1";
        saveConfig();
        sendRestartPage(r, "Device is restarting with new hardware settings.");
        shouldRestart = true;
        restartTimer  = millis();
    });

    server.on("/save_theme", HTTP_POST, [](AsyncWebServerRequest *r) {
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
    });

    server.on("/save_datalog", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("currentFile", true))  SAFE_STRNCPY(config.datalog.currentFile, r->getParam("currentFile", true)->value().c_str(), sizeof(config.datalog.currentFile));
        if (r->hasParam("prefix", true))       SAFE_STRNCPY(config.datalog.prefix,      r->getParam("prefix", true)->value().c_str(), sizeof(config.datalog.prefix));
        if (r->hasParam("folder", true))       SAFE_STRNCPY(config.datalog.folder,      r->getParam("folder", true)->value().c_str(), sizeof(config.datalog.folder));
        if (r->hasParam("rotation", true))     config.datalog.rotation    = (DatalogRotation)r->getParam("rotation", true)->value().toInt();
        if (r->hasParam("maxSizeKB", true))    config.datalog.maxSizeKB   = constrain(r->getParam("maxSizeKB", true)->value().toInt(), 10, 10000);
        if (r->hasParam("maxEntries", true))   config.datalog.maxEntries  = constrain(r->getParam("maxEntries", true)->value().toInt(), 10, 100000);
        config.datalog.timestampFilename   = r->hasParam("timestampFilename", true);
        config.datalog.includeDeviceId     = r->hasParam("includeDeviceId", true);
        if (r->hasParam("dateFormat", true))   config.datalog.dateFormat   = r->getParam("dateFormat", true)->value().toInt();
        if (r->hasParam("timeFormat", true))   config.datalog.timeFormat   = r->getParam("timeFormat", true)->value().toInt();
        if (r->hasParam("endFormat", true))    config.datalog.endFormat    = r->getParam("endFormat", true)->value().toInt();
        if (r->hasParam("volumeFormat", true)) config.datalog.volumeFormat = r->getParam("volumeFormat", true)->value().toInt();
        config.datalog.includeBootCount    = r->hasParam("includeBootCount", true)    && r->getParam("includeBootCount", true)->value()    == "1";
        config.datalog.includeExtraPresses = r->hasParam("includeExtraPresses", true) && r->getParam("includeExtraPresses", true)->value() == "1";
        config.datalog.postCorrectionEnabled = r->hasParam("postCorrectionEnabled", true);
        if (r->hasParam("pfToFfThreshold", true))        config.datalog.pfToFfThreshold        = max(0.1f, r->getParam("pfToFfThreshold", true)->value().toFloat());
        if (r->hasParam("ffToPfThreshold", true))        config.datalog.ffToPfThreshold        = max(0.1f, r->getParam("ffToPfThreshold", true)->value().toFloat());
        if (r->hasParam("manualPressThresholdMs", true)) config.datalog.manualPressThresholdMs = r->getParam("manualPressThresholdMs", true)->value().toInt();

        saveConfig();

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
                SAFE_STRNCPY(config.datalog.currentFile, newFile.c_str(), sizeof(config.datalog.currentFile));
                saveConfig();
                r->send(200, "application/json", "{\"ok\":true,\"file\":\"" + newFile + "\"}");
                return;
            } else {
                r->send(500, "application/json", "{\"ok\":false,\"error\":\"Failed to create file\"}");
                return;
            }
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/save_network", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("wifiMode", true))       config.network.wifiMode = (WiFiModeType)r->getParam("wifiMode", true)->value().toInt();
        if (r->hasParam("apSSID", true))         SAFE_STRNCPY(config.network.apSSID,         r->getParam("apSSID", true)->value().c_str(), sizeof(config.network.apSSID));
        if (r->hasParam("apPassword", true))     SAFE_STRNCPY(config.network.apPassword,     r->getParam("apPassword", true)->value().c_str(), sizeof(config.network.apPassword));
        if (r->hasParam("clientSSID", true))     SAFE_STRNCPY(config.network.clientSSID,     r->getParam("clientSSID", true)->value().c_str(), sizeof(config.network.clientSSID));
        if (r->hasParam("clientPassword", true)) SAFE_STRNCPY(config.network.clientPassword, r->getParam("clientPassword", true)->value().c_str(), sizeof(config.network.clientPassword));
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
    });

    server.on("/save_time", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("ntpServer", true)) SAFE_STRNCPY(config.network.ntpServer, r->getParam("ntpServer", true)->value().c_str(), sizeof(config.network.ntpServer));
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
    // FACTORY RESET  – formats LittleFS, erases config, restarts
    // =========================================================================
    server.on("/factory_reset", HTTP_POST, [](AsyncWebServerRequest *r) {
        r->send(200, "application/json", "{\"ok\":true}");
        Serial.println("[FACTORY RESET] Formatting LittleFS…");
        if (fsMutex) xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));   // FS1
        if (LittleFS.format()) {
            Serial.println("[FACTORY RESET] LittleFS formatted OK – restarting");
        } else {
            Serial.println("[FACTORY RESET] LittleFS format FAILED – restarting anyway");
        }
        if (fsMutex) xSemaphoreGive(fsMutex);   // FS1
        safeWiFiShutdown();
        delay(300);
        ESP.restart();
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

    // Accept both GET (legacy web.js compat) and POST (preferred)
    auto deleteHandler = [](AsyncWebServerRequest *r) {
        if (!r->hasParam("path") && !r->hasParam("path", true)) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing path\"}"); return; }
        String path = sanitizePath(r->getParam("path")->value());
        if (path == "/") { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Refusing to delete root\"}"); return; }
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)              targetFS = &SD;
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        else if (activeFS) targetFS = activeFS;
        bool deleted = false;
        if (targetFS && targetFS->exists(path)) {
            File f = targetFS->open(path, FILE_READ);
            bool isDir = f && f.isDirectory();
            if (f) f.close();
            if (fsMutex) xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));   // FS1
            deleted = isDir ? deleteRecursive(*targetFS, path) : targetFS->remove(path);
            if (fsMutex) xSemaphoreGive(fsMutex);
        }
        r->send(200, "application/json", deleted ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Delete failed\"}");
    };
    server.on("/delete", HTTP_GET,  deleteHandler);  // legacy compat
    server.on("/delete", HTTP_POST, deleteHandler);   // preferred

    server.on("/mkdir", HTTP_GET, [](AsyncWebServerRequest *r) {
        fs::FS* targetFS = getCurrentViewFS();
        if (!r->hasParam("name") || !targetFS) { r->send(400, "text/plain", "Missing name"); return; }
        String dir     = r->hasParam("dir")     ? r->getParam("dir")->value()     : "/";
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        if (storage == "sdcard" && sdAvailable) targetFS = &SD;
        else targetFS = &LittleFS;
        String name = sanitizeFilename(r->getParam("name")->value());
        String fp   = buildPath(dir, name);
        if (fsMutex) xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));   // FS1
        bool ok = targetFS->mkdir(fp);
        if (fsMutex) xSemaphoreGive(fsMutex);
        r->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    server.on("/move_file", HTTP_GET, [](AsyncWebServerRequest *r) {
        String storage = r->hasParam("storage") ? r->getParam("storage")->value() : currentStorageView;
        String src     = r->hasParam("src")     ? sanitizePath(r->getParam("src")->value())     : "";
        String newName = r->hasParam("newName") ? r->getParam("newName")->value() : "";
        String destDir = r->hasParam("destDir") ? r->getParam("destDir")->value() : "";
        if (src.isEmpty() || newName.isEmpty()) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing params\"}"); return; }
        fs::FS* targetFS = nullptr;
        if (storage == "sdcard" && sdAvailable)              targetFS = &SD;
        else if (storage == "internal" && littleFsAvailable) targetFS = &LittleFS;
        if (!targetFS) { r->send(400, "application/json", "{\"ok\":false,\"error\":\"No storage\"}"); return; }
        String dstDir  = destDir.isEmpty() ? src.substring(0, src.lastIndexOf('/')) : destDir;
        if (dstDir.isEmpty()) dstDir = "/";
        String dstPath = buildPath(dstDir, newName);
        if (fsMutex) xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));   // FS1
        bool ok = targetFS->rename(src, dstPath);
        if (fsMutex) xSemaphoreGive(fsMutex);
        r->send(200, "application/json", ok ? "{\"ok\":true,\"dst\":\"" + dstPath + "\"}" : "{\"ok\":false}");
    });

    // Upload handler: file handle stored per-request to prevent concurrent corruption
    server.on("/upload", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            // Clean up request-scoped file handle
            File* fp = (File*)r->_tempObject;
            if (fp) { if (*fp) fp->close(); delete fp; r->_tempObject = nullptr; }
            r->send(200, "application/json", "{\"ok\":true}");
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                // Clean up any leftover handle from a previous aborted upload
                File* old = (File*)request->_tempObject;
                if (old) { if (*old) old->close(); delete old; }

                String upDir = request->hasParam("path")
                               ? request->getParam("path")->value()
                               : String("/www/");
                if (!upDir.startsWith("/")) upDir = "/" + upDir;
                while (upDir.length() > 1 && upDir.endsWith("/")) upDir.remove(upDir.length()-1);

                String upStorage = request->hasParam("storage")
                                   ? request->getParam("storage")->value()
                                   : String("internal");

                fs::FS* targetFS = (upStorage == "sdcard" && sdAvailable)
                                   ? (fs::FS*)&SD
                                   : (littleFsAvailable ? (fs::FS*)&LittleFS : nullptr);
                if (!targetFS) { Serial.println("Upload: no filesystem available"); request->_tempObject = nullptr; return; }
                if (upDir != "/") targetFS->mkdir(upDir);

                String upPath = (upDir == "/") ? "/" + filename : upDir + "/" + filename;
                Serial.printf("Upload start [%s]: %s\n", upStorage.c_str(), upPath.c_str());

                if (fsMutex) xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));   // FS1
                File* fp = new File(targetFS->open(upPath, FILE_WRITE));
                if (!fp || !*fp) {
                    Serial.printf("Upload: cannot open %s for write\n", upPath.c_str());
                    if (fp) delete fp;
                    request->_tempObject = nullptr;
                    return;
                }
                request->_tempObject = fp;
            }

            File* fp = (File*)request->_tempObject;
            if (fp && *fp && len) fp->write(data, len);

            if (final) {
                if (fp) {
                    if (*fp) {
                        Serial.printf("Upload done: %s (%u bytes)\n", filename.c_str(), (unsigned)(index + len));
                        fp->close();
                    }
                    delete fp;
                    request->_tempObject = nullptr;
                    if (fsMutex) xSemaphoreGive(fsMutex);   // FS1
                }
            }
        }
    );

    // =========================================================================
    // IMPORT SETTINGS
    // =========================================================================
    static String _importBuf;
    server.on("/import_settings", HTTP_POST,
        [](AsyncWebServerRequest *r) {
            if (_importBuf.isEmpty()) { r->send(400, "text/plain", "No data"); return; }
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, _importBuf);
            _importBuf = String();   // release heap buffer, not just clear
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
                if (fm["monitoringWindowSecs"].is<int>())           config.flowMeter.monitoringWindowSecs         = fm["monitoringWindowSecs"];
                if (fm["firstLoopMonitoringWindowSecs"].is<int>())  config.flowMeter.firstLoopMonitoringWindowSecs= fm["firstLoopMonitoringWindowSecs"];
                else if (fm.containsKey("firstLoopWindow"))         config.flowMeter.firstLoopMonitoringWindowSecs= fm["firstLoopWindow"] | 6;
            }
            if (doc["datalog"].is<JsonObject>()) {
                JsonObject dl = doc["datalog"];
                if (dl["rotation"].is<int>())               config.datalog.rotation               = (DatalogRotation)(int)dl["rotation"];
                if (dl["maxSizeKB"].is<int>())              config.datalog.maxSizeKB              = constrain(dl["maxSizeKB"].as<int>(), 10, 10000);
                if (dl.containsKey("maxEntries"))           config.datalog.maxEntries             = constrain(dl["maxEntries"].as<int>(), 10, 100000);
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
                if (hw.containsKey("debugMode"))        config.hardware.debugMode          = hw["debugMode"].as<bool>();
            }
            saveConfig();
            r->send(200, "text/plain", "OK");
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                _importBuf = "";
                _importBuf.reserve(req->contentLength() > 0 ? req->contentLength() : 4096);
            }
            if (_importBuf.length() + len > 8192) return; // Hard cap
            _importBuf.concat((const char*)data, len);
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
        JsonDocument doc;
        JsonArray nets = doc["networks"].to<JsonArray>();
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
            if (ok) {
                shouldRestart = true;
                restartTimer = millis();
            }
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
    server.onNotFound([touchActivity](AsyncWebServerRequest *r) {
        touchActivity();   // C2: track web activity for idle power management
        String path = r->url();

        if (path.startsWith("/www/")) {
            if (littleFsAvailable && LittleFS.exists(path)) {
                r->send(LittleFS, path, getMime(path));
                return;
            }
            r->send(404, "text/plain", "Not found: " + path);
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
            r->send_P(200, "text/html", FAILSAFE_HTML);
            return;
        }
        r->send(404, "text/plain", "Not found");
    });

    // =========================================================================
    // API: PLATFORM CONFIG  (GET = read, POST = write)
    // Used by Core Logic settings page to manage /platform_config.json
    // =========================================================================
    server.on("/api/platform_config", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fsAvailable || !activeFS || !activeFS->exists("/platform_config.json")) {
            r->send(404, "application/json", "{\"error\":\"platform_config.json not found\"}");
            return;
        }
        r->send(*activeFS, "/platform_config.json", "application/json");
    });

    // POST /save_platform — receives JSON body, writes to /platform_config.json
    // Uses static file handle (only one request at a time on embedded device).
    {
        // Static file handle shared across onRequest + onBody lambdas
        static File s_pcfgFile;

        server.on("/save_platform", HTTP_POST,
            [](AsyncWebServerRequest *r) {
                // onRequest fires AFTER body is fully collected (when using onBody).
                // At this point the file is already closed by onBody; just ACK.
                if (!fsAvailable || !activeFS) {
                    r->send(503, "application/json", "{\"ok\":false,\"error\":\"no fs\"}");
                    return;
                }
                r->send(200, "application/json", "{\"ok\":true}");
            },
            nullptr,
            [](AsyncWebServerRequest *r, uint8_t *data, size_t len,
               size_t index, size_t total) {
                if (!fsAvailable || !activeFS) return;
                if (index == 0) {
                    if (fsMutex) xSemaphoreTake(fsMutex, pdMS_TO_TICKS(5000));   // FS1
                    s_pcfgFile = activeFS->open("/platform_config.json", FILE_WRITE);
                }
                if (s_pcfgFile) {
                    s_pcfgFile.write(data, len);
                }
                if (index + len >= total && s_pcfgFile) {
                    s_pcfgFile.close();
                    if (fsMutex) xSemaphoreGive(fsMutex);   // FS1
                }
            }
        );
    }

    // =========================================================================
    // API: PLATFORM RELOAD — trigger live sensor/exporter reload after save
    // =========================================================================
    server.on("/api/platform_reload", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Signal to main loop / TaskManager to reload configs
        // Full reload requires restart; signal shouldRestart
        shouldRestart = true;
        restartTimer  = millis();
        r->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    });

    server.begin();
    Serial.printf("Web server started. Free heap: %d\n", ESP.getFreeHeap());
}
