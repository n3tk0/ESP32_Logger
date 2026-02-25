// Comprehensive Blueprint Generator
// Maps ALL APIs, HTML elements, JS functions, and cross-references them
const fs = require('fs');

const html = fs.readFileSync('C:/Users/n3Tk0/Documents/GitHub/Water_logger/www/index.html', 'utf8');
const webserver = fs.readFileSync('C:/Users/n3Tk0/Documents/GitHub/Water_logger/src/web/WebServer.cpp', 'utf8');
const styleCss = fs.readFileSync('C:/Users/n3Tk0/Documents/GitHub/Water_logger/www/style.css', 'utf8');

const out = [];
out.push('# Water Logger – Full API & JS Blueprint\n');

// ═════════════════════════════════════════════════
// 1. SERVER API ENDPOINTS
// ═════════════════════════════════════════════════
out.push('## 1. Server API Endpoints (WebServer.cpp)\n');

const endpointRe = /server\.on\("([^"]+)",\s*HTTP_(\w+)/g;
let m;
const endpoints = [];
while ((m = endpointRe.exec(webserver)) !== null) {
    endpoints.push({ path: m[1], method: m[2] });
}
out.push('| Path | Method |');
out.push('|------|--------|');
endpoints.forEach(e => out.push(`| \`${e.path}\` | ${e.method} |`));

// ═════════════════════════════════════════════════
// 2. API RESPONSE FIELDS
// ═════════════════════════════════════════════════
out.push('\n## 2. API Response Fields\n');

// /api/status fields
const statusFields = [];
const statusSection = webserver.match(/server\.on\("\/api\/status"[\s\S]*?sendJsonResponse/);
if (statusSection) {
    const docFields = statusSection[0].match(/doc\["(\w+)"\]/g) || [];
    docFields.forEach(f => {
        const name = f.match(/doc\["(\w+)"\]/)[1];
        if (!statusFields.includes(name)) statusFields.push(name);
    });
    const thFields = statusSection[0].match(/th\["(\w+)"\]/g) || [];
    thFields.forEach(f => {
        const name = f.match(/th\["(\w+)"\]/)[1];
        if (!statusFields.includes('theme.' + name)) statusFields.push('theme.' + name);
    });
}
out.push('### /api/status');
out.push('```');
statusFields.forEach(f => out.push(f));
out.push('```\n');

// /api/live fields
const liveFields = [];
const liveSection = webserver.match(/server\.on\("\/api\/live"[\s\S]*?sendJsonResponse/);
if (liveSection) {
    const docFields = liveSection[0].match(/doc\["(\w+)"\]/g) || [];
    docFields.forEach(f => {
        const name = f.match(/doc\["(\w+)"\]/)[1];
        if (!liveFields.includes(name)) liveFields.push(name);
    });
}
out.push('### /api/live');
out.push('```');
liveFields.forEach(f => out.push(f));
out.push('```\n');

// /export_settings fields
const exportFields = [];
const exportSection = webserver.match(/server\.on\("\/export_settings"[\s\S]*?r->send\(resp\)/);
if (exportSection) {
    const sections = ['flowMeter', 'datalog', 'network', 'hardware', 'theme'];
    sections.forEach(sec => {
        const re = new RegExp(`(${sec.charAt(0)}[a-z])\\["(\\w+)"\\]`, 'g');
        let mm;
        while ((mm = re.exec(exportSection[0])) !== null) {
            const field = sec + '.' + mm[2];
            if (!exportFields.includes(field)) exportFields.push(field);
        }
    });
    // Top-level fields
    const topRe = /doc\["(\w+)"\]/g;
    while ((mm = topRe.exec(exportSection[0])) !== null) {
        if (!exportFields.includes(mm[1])) exportFields.push(mm[1]);
    }
}
out.push('### /export_settings');
out.push('```');
exportFields.forEach(f => out.push(f));
out.push('```\n');

// /api/filelist fields
out.push('### /api/filelist');
out.push('```');
out.push('files[] { name, path, isDir, size }');
out.push('used, total, percent');
out.push('currentFile');
out.push('Params: ?storage=internal|sdcard&dir=/&filter=log&recursive=1');
out.push('```\n');

// ═════════════════════════════════════════════════
// 3. HTML PAGES
// ═════════════════════════════════════════════════
out.push('## 3. HTML Pages\n');

const pageIds = [...html.matchAll(/id="(page-[^"]+)"/g)].map(m => m[1]);
out.push('| Page ID | Line |');
out.push('|---------|------|');
pageIds.forEach(pid => {
    const idx = html.indexOf(`id="${pid}"`);
    const line = html.substring(0, idx).split('\n').length;
    out.push(`| \`${pid}\` | ${line} |`);
});

// ═════════════════════════════════════════════════
// 4. PAGES MAP vs HTML
// ═════════════════════════════════════════════════
out.push('\n## 4. PAGES Map (JS Router)\n');
const pagesMatch = html.match(/var PAGES\s*=\s*\{([^}]+)\}/);
if (pagesMatch) {
    const pairs = pagesMatch[1].match(/'?[\w-]+'?\s*:\s*'[^']+'/g) || [];
    out.push('| Hash | Element ID | Exists in HTML |');
    out.push('|------|-----------|----------------|');
    pairs.forEach(p => {
        const parts = p.match(/'?([\w-]+)'?\s*:\s*'([^']+)'/);
        if (parts) {
            const exists = html.includes(`id="${parts[2]}"`);
            out.push(`| \`${parts[1]}\` | \`${parts[2]}\` | ${exists ? '✅' : '❌'} |`);
        }
    });
}

// ═════════════════════════════════════════════════
// 5. onEnter MAP
// ═════════════════════════════════════════════════
out.push('\n## 5. onEnter Function Map\n');
const onEnterMatch = html.match(/function onEnter[\s\S]*?\{[\s\S]*?\{([\s\S]*?)\}\[h\]/);
if (onEnterMatch) {
    const fnPairs = onEnterMatch[1].match(/'[\w-]+'\s*:\s*\w+/g) || [];
    out.push('| Page Hash | Function | Function Exists |');
    out.push('|-----------|----------|----------------|');
    fnPairs.forEach(p => {
        const parts = p.match(/'([\w-]+)'\s*:\s*(\w+)/);
        if (parts) {
            const fnExists = html.includes(`function ${parts[2]}`);
            out.push(`| \`${parts[1]}\` | \`${parts[2]}\` | ${fnExists ? '✅' : '❌'} |`);
        }
    });
}

// ═════════════════════════════════════════════════
// 6. ALL JS FUNCTIONS
// ═════════════════════════════════════════════════
out.push('\n## 6. All JS Functions\n');
const scriptContent = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const allFuncs = [...scriptContent.matchAll(/function\s+(\w+)\s*\(/g)].map(m => m[1]);
out.push('Total functions: ' + allFuncs.length);
out.push('```');
allFuncs.forEach(f => out.push(f));
out.push('```\n');

// ═════════════════════════════════════════════════
// 7. ALL JS getElementById CALLS vs HTML IDs
// ═════════════════════════════════════════════════
out.push('\n## 7. Element ID Cross-Reference\n');
const jsIds = [...new Set([...scriptContent.matchAll(/getElementById\('([^']+)'\)/g)].map(m => m[1]))];
const htmlIds = [...new Set([...html.matchAll(/id="([^"]+)"/g)].map(m => m[1]))];

const missing = jsIds.filter(id => !htmlIds.includes(id));
out.push('### JS references NOT in HTML (' + missing.length + ')');
if (missing.length) {
    out.push('| ID | Referenced By |');
    out.push('|----|--------------|');
    missing.forEach(id => {
        const lines = scriptContent.split('\n');
        const refs = lines.filter(l => l.includes(`getElementById('${id}')`))
            .map(l => l.trim().substring(0, 80));
        out.push(`| \`${id}\` | ${refs[0] || '?'} |`);
    });
} else {
    out.push('✅ All JS getElementById references have matching HTML elements\n');
}

// ═════════════════════════════════════════════════
// 8. ALL API CALLS FROM JS
// ═════════════════════════════════════════════════
out.push('\n## 8. API Calls from JS\n');
const fetchCalls = [...scriptContent.matchAll(/fetch\('([^']+)'/g)].map(m => m[1]);
const uniqueFetches = [...new Set(fetchCalls.map(f => f.split('?')[0]))];
out.push('| JS Fetch Path | Server Endpoint Exists |');
out.push('|--------------|----------------------|');
uniqueFetches.forEach(f => {
    const serverHas = endpoints.some(e => e.path === f);
    out.push(`| \`${f}\` | ${serverHas ? '✅' : '❌'} |`);
});

// ═════════════════════════════════════════════════
// 9. FORM ACTIONS vs SERVER ENDPOINTS
// ═════════════════════════════════════════════════
out.push('\n## 9. Form Actions vs Server Endpoints\n');
const formActions = [...html.matchAll(/action="([^"]+)"/g)].map(m => m[1]);
const uniqueActions = [...new Set(formActions)];
out.push('| Form Action | Server Exists | Method |');
out.push('|------------|--------------|--------|');
uniqueActions.forEach(a => {
    const ep = endpoints.find(e => e.path === a);
    out.push(`| \`${a}\` | ${ep ? '✅' : '❌'} | ${ep ? ep.method : '?'} |`);
});

// ═════════════════════════════════════════════════
// 10. KNOWN ISSUES DETECTION
// ═════════════════════════════════════════════════
out.push('\n## 10. Detected Issues\n');
const issues = [];

// Check for textContent with HTML entities
const tcEntities = [...scriptContent.matchAll(/\.textContent\s*=\s*[^;]*&#x/g)];
if (tcEntities.length) {
    issues.push('⚠️ `.textContent` used with HTML entities (&#x...) — entities won\'t render. Use `.innerHTML` instead. Found ' + tcEntities.length + ' instances.');
}

// Check for OTA code outside script
const rawOTA = /^\/\/ ═══.*OTA UPLOAD/m.test(html.replace(/<script>[\s\S]*?<\/script>/g, ''));
if (rawOTA) issues.push('❌ OTA JavaScript code is outside <script> tags — renders as text');

// Check script tag balance
const sOpen = (html.match(/<script>/g) || []).length;
const sClose = (html.match(/<\/script>/g) || []).length;
if (sOpen !== sClose) issues.push('❌ Unbalanced script tags: ' + sOpen + ' opens, ' + sClose + ' closes');

// Check DOMContentLoaded count
const domReady = (scriptContent.match(/DOMContentLoaded/g) || []).length;
if (domReady > 2) issues.push('⚠️ ' + domReady + ' DOMContentLoaded listeners (expected ≤2)');

// Check arrToIP
if (scriptContent.includes("!Array.isArray(a))?'':a.join('.')")) {
    issues.push('⚠️ arrToIP only handles arrays — /export_settings returns IPs as strings');
}

// Check footer
if (!html.includes('app-footer') || !html.includes('class="footer-')) {
    issues.push('⚠️ Footer HTML missing');
}

// Check network passwords
const snOnEnter = scriptContent.match(/function sn_onEnter[\s\S]*?(?=\nfunction )/);
if (snOnEnter && !snOnEnter[0].includes('apPassword')) {
    issues.push('⚠️ sn_onEnter does not load apPassword/clientPassword');
}

// Check RTC status update
const stOnEnter = scriptContent.match(/function st_onEnter[\s\S]*?(?=\n\/\/ ═)/);
if (stOnEnter && !stOnEnter[0].includes('rtcStatus')) {
    issues.push('⚠️ st_onEnter does not update #rtcStatus element');
}

// Check ota_onEnter populates fwHeap
const otaOnEnter = scriptContent.match(/function ota_onEnter[\s\S]*?(?=\n\/\/ )/);
if (otaOnEnter && !otaOnEnter[0].includes('fwHeap')) {
    issues.push('⚠️ ota_onEnter does not populate #fwHeap');
}

// Check datalog file list
if (scriptContent.includes("fetch('/api/filelist?storage=internal&dir=/')") &&
    scriptContent.includes('sdl_onEnter')) {
    issues.push('⚠️ sdl_onEnter fetches files only from root dir — log files may be in subfolders');
}

// Check toast textContent vs innerHTML
if (scriptContent.includes('t.textContent=msg;') && scriptContent.includes('toast(')) {
    issues.push('⚠️ toast() uses textContent — HTML entities in messages won\'t render');
}

// Check changelog toggle
const changelogToggle = scriptContent.match(/function toggleChangelog[\s\S]*?(?=\nfunction )/);
if (changelogToggle && changelogToggle[0].includes("'sd_changelog'")) {
    issues.push('⚠️ toggleChangelog references sd_changelog but HTML only has changelog');
}

// Check hidden class with !important
if (styleCss.includes('.hidden') && styleCss.includes('display: none !important')) {
    if (html.includes('class="card-body hidden"')) {
        issues.push('⚠️ .hidden uses !important but JS sets style.display which can\'t override it');
    }
}

if (issues.length === 0) {
    out.push('✅ No issues detected\n');
} else {
    issues.forEach((issue, i) => out.push((i + 1) + '. ' + issue));
}

// ═════════════════════════════════════════════════
// 11. PER-PAGE ELEMENT AUDIT
// ═════════════════════════════════════════════════
out.push('\n## 11. Per-Page Element Audit\n');

const pages = [
    { hash: 'dashboard', onEnter: 'db_onEnter', ids: ['chartLegend', 'fileSelect', 'dbError', 'dbTotalVol', 'dbCount', 'dbFF', 'dbPF', 'dbChart'] },
    { hash: 'live', onEnter: 'live_onEnter', ids: ['time', 'trigger', 'cycleTime', 'pulses', 'liters', 'ffCount', 'pfCount', 'boot', 'uptime', 'heap', 'heapTotal', 'storage', 'stEl', 'stateRemaining', 'ff', 'pf', 'wifi', 'mode', 'connStatus', 'recentLogs', 'hdrTime'] },
    { hash: 'files', onEnter: 'files_onEnter', ids: ['storageTabs', 'storageInfo', 'dirTitle', 'fileList', 'btnUp', 'btnEdit', 'btnDone', 'editTools', 'uploadForm', 'fileInput', 'mkdirForm', 'newFolder', 'uploadProg', 'uploadBar', 'uploadPct', 'movePopup', 'mvName', 'mvDest'] },
    { hash: 'settings-device', onEnter: 'sd_onEnter', ids: ['deviceName', 'devIdInput', 'defaultStorageView', 'forceWebServer', 'siFirmware', 'siBoot', 'siMode', 'siHeap', 'siCpu', 'siStorage', 'changelog', 'sd_clContent'] },
    { hash: 'settings-flowmeter', onEnter: 'sfm_onEnter', ids: ['bootCount', 'pulsesPerLiter', 'calibrationMultiplier', 'monitoringWindowSecs', 'firstLoopWindowSecs', 'blinkDuration', 'testMode'] },
    { hash: 'settings-hardware', onEnter: 'sh_onEnter', ids: ['storageType', 'wakeupMode', 'debounceMs', 'pinWifiTrigger', 'pinWakeupFF', 'pinWakeupPF', 'pinFlowSensor', 'pinRtcCE', 'pinRtcIO', 'pinRtcSCLK', 'pinSdCS', 'pinSdMOSI', 'pinSdMISO', 'pinSdSCK', 'cpuFreqMHz', 'sdPins', 'boardDiagram', 'boardDiagramCard'] },
    { hash: 'settings-datalog', onEnter: 'sdl_onEnter', ids: ['prefix', 'folder', 'maxSizeKB', 'pfToFfThreshold', 'ffToPfThreshold', 'manualPressThresholdMs', 'rotation', 'dfDate', 'dfTime', 'dfEnd', 'dfBoot', 'dfVol', 'dfExtra', 'timestampFilename', 'includeDeviceId', 'postCorrectionEnabled', 'pcFields', 'maxSizeGroup', 'logPreview', 'currentFile'] },
    { hash: 'settings-theme', onEnter: 'sth_onEnter', ids: ['themeMode', 'primaryColor', 'secondaryColor', 'bgColor', 'textColor', 'ffColor', 'pfColor', 'otherColor', 'storageBarColor', 'storageBar70Color', 'storageBar90Color', 'storageBarBorder', 'logoSource', 'faviconPath', 'boardDiagramPath', 'chartLocalPath', 'showIcons', 'chartSource', 'chartLabelFormat', 'chartLocalPathInput'] },
    { hash: 'settings-network', onEnter: 'sn_onEnter', ids: ['wifiStatus', 'wifiIP', 'wifiMode', 'apSSID', 'apPass', 'apIP', 'apGateway', 'apSubnet', 'clientSSID', 'clientPass', 'fldIP', 'fldGW', 'fldSN', 'fldDNS', 'staticCheck', 'apSection', 'clientSection', 'wifiList'] },
    { hash: 'settings-time', onEnter: 'st_onEnter', ids: ['rtcTime', 'rtcStatus', 'rtcMeta', 'bootCount', 'rtcProtect', 'ntpStatus', 'ntpBtn', 'ntpServer', 'timezone', 'ntpForm'] },
    { hash: 'update', onEnter: 'ota_onEnter', ids: ['fwVersion', 'fwDevice', 'fwHeap', 'fwStorage', 'otaForm', 'fwFile', 'popup', 'popupIcon', 'popupTitle', 'popupMsg', 'popupProgress', 'popupBar'] }
];

pages.forEach(page => {
    out.push(`### ${page.hash} → ${page.onEnter}`);
    const missingIds = page.ids.filter(id => !html.includes(`id="${id}"`));
    if (missingIds.length) {
        out.push('Missing elements: ' + missingIds.map(id => '`' + id + '`').join(', '));
    } else {
        out.push('✅ All elements present');
    }
    out.push('');
});


const result = out.join('\n');
fs.writeFileSync('C:/Users/n3Tk0/.gemini/antigravity/brain/ab7d0d95-aa8f-4b99-bbe7-2e79f14d46de/blueprint.md', result, 'utf8');
console.log('Blueprint written to blueprint.md');
console.log('\nIssues found:', issues.length);
issues.forEach(i => console.log('  ' + i));
