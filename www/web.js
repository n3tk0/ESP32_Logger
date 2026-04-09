/**
 * /www/web.js  –  Water Logger SPA JavaScript  v4.1.5
 * Pairs with /www/index.html and WebServer.cpp API endpoints.
 *
 * Architecture:
 *   – On load: fetch /api/status + /export_settings, apply theme, route to hash page
 *   – Pages are hidden/shown via class toggling (no server round-trips)
 *   – Live page polls /api/live every 500ms; logs polled every 3s
 *   – Footer refreshes boot+heap from /api/live; chip/version from /api/status on boot
 *   – All form saves use XHR/fetch to /save_* endpoints
 *   – Settings pages load config from /export_settings (full nested objects)
 */

'use strict';

// ============================================================================
// GLOBALS
// ============================================================================
var ST = {};                // cached /api/status payload
var CFG = {};               // cached /export_settings payload
var dbChart = null;         // Chart.js instance on dashboard
var dbRawData = '';         // raw log text for dashboard
var dbFilteredData = [];    // filtered, parsed rows
var liveTimer = null;       // live page interval
var liveLogsTimer = null;   // live logs interval
var currentPage = '';       // active page id (without 'page-' prefix)
var _toastTimer = null;     // toast auto-hide timer
var currentFilesDir = '/';
var currentFilesStorage = 'internal';
var filesEditMode = false;
var netScanRetries = 0;
var changelogLoaded = false;

// ============================================================================
// BOOTSTRAP
// ============================================================================
window.addEventListener('DOMContentLoaded', function () {
    Promise.all([
        fetch('/api/status').then(function (r) { return r.json(); }).catch(function () { return {}; }),
        fetch('/export_settings').then(function (r) { return r.json(); }).catch(function () { return {}; })
    ]).then(function (results) {
        ST = results[0];
        CFG = results[1];
        applyStatus(ST);
        var hash = location.hash.replace('#', '') || 'dashboard';
        navigateTo(hash);
    });
});

window.addEventListener('hashchange', function () {
    var hash = location.hash.replace('#', '') || 'dashboard';
    navigateTo(hash);
});

// ============================================================================
// STATUS  (apply theme, populate footer, header, etc.)
// ============================================================================
function applyStatus(d) {
    document.title = (d.device || 'Water Logger') + ' – Logger';

    // Device names
    var sn = document.getElementById('sidebarName');
    var hn = document.getElementById('headerName');
    if (sn) sn.textContent = d.device || 'Water Logger';
    if (hn) hn.textContent = d.device || 'Water Logger';

    // Logo
    if (d.theme && d.theme.logoSource) {
        ['sidebarLogo', 'headerLogo'].forEach(function (id) {
            var el = document.getElementById(id);
            if (el) { el.src = d.theme.logoSource; el.style.display = ''; }
        });
    }

    // Favicon
    if (d.theme && d.theme.faviconPath) {
        var link = document.querySelector("link[rel='icon']") || document.createElement('link');
        link.rel = 'icon'; link.href = d.theme.faviconPath;
        document.head.appendChild(link);
    }

    // CSS custom properties (theme colors)
    if (d.theme) {
        var th = d.theme;
        // Theme class on <html> element
        var html = document.getElementById('htmlRoot');
        var m = th.mode;
        var actDark = false;

        if (html) {
            html.classList.remove('theme-light', 'theme-dark', 'theme-auto');
            if (m === 0 || m === '0') html.classList.add('theme-light');
            else if (m === 1 || m === '1') { html.classList.add('theme-dark'); actDark = true; }
            else {
                html.classList.add('theme-auto');
                actDark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
                // Add listener to hot-reload if OS theme changes while in auto mode
                if (!window._actDarkListenerAppended) {
                    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function (e) {
                        if (ST && ST.theme && (ST.theme.mode === 2 || ST.theme.mode === '2')) applyStatus(ST);
                    });
                    window._actDarkListenerAppended = true;
                }
            }
        }

        var vars = ':root{';
        if (th.primaryColor) vars += '--primary:' + th.primaryColor + ';';
        if (th.secondaryColor) vars += '--secondary:' + th.secondaryColor + ';';
        if (actDark) {
            if (th.darkBgColor) vars += '--bg:' + th.darkBgColor + ';';
            if (th.darkTextColor) vars += '--text:' + th.darkTextColor + ';';
        } else {
            if (th.lightBgColor) vars += '--bg:' + th.lightBgColor + ';';
            if (th.lightTextColor) vars += '--text:' + th.lightTextColor + ';';
        }
        vars += '}';
        var style = document.getElementById('themeVars');
        if (style) style.textContent = vars;

        // Dashboard legend dot colors — matches original inline style in .ino
        // Dashboard legend dot colors — matches original inline style in .ino
        setElStyle('db-legendFF', 'background', th.ffColor || 'var(--ff-color)');
        setElStyle('db-legendPF', 'background', th.pfColor || 'var(--pf-color)');
        setElStyle('db-legendOther', 'background', th.otherColor || 'var(--other-color)');
        // Stat card text colors — matches original: style='color:%s'
        setElStyle('db-totalFF', 'color', th.ffColor || 'var(--ff-color)');
        setElStyle('db-totalPF', 'color', th.pfColor || 'var(--pf-color)');
        setElStyle('live-ffCount', 'color', th.ffColor || 'var(--ff-color)');
        setElStyle('live-pfCount', 'color', th.pfColor || 'var(--pf-color)');
    }

    // Footer — matches original .ino footer construction exactly:
    //   footer-grid:    Boot: N | <cpu>MHz | <heap free> / <heap total>
    //   footer-row:     <networkDisplay>   |  IP: <ip>
    //   footer-version: Board: <chip> – Firmware: <version>
    updateFooter(d);

    // Mobile header
    setEl('headerNet', d.network);
    var timePart = (d.time || '').split(' ')[1] || d.time || '--:--';
    setEl('headerTime', timePart);

    // OTA page version badge
    setEl('ota-currentVer', d.version || '-');
}

// Partial footer update — called by both applyStatus() and liveUpdate()
// Only updates fields present in the supplied object
function updateFooter(d) {
    if (d.boot !== undefined && d.boot !== null) setEl('footer-boot', d.boot);
    if (d.cpu !== undefined && d.cpu !== null) setEl('footer-cpu', d.cpu);
    if (d.heap !== undefined && d.heapTotal !== undefined)
        setEl('footer-heap', fmtBytes(d.heap) + ' / ' + fmtBytes(d.heapTotal));
    if (d.network !== undefined && d.network !== null) setEl('footer-net', d.network);
    if (d.ip !== undefined && d.ip !== null) setEl('footer-ip', d.ip);
    if (d.chip !== undefined && d.chip !== null) setEl('footer-chip', d.chip);
    if (d.version !== undefined && d.version !== null) setEl('footer-ver', d.version);
}

// ============================================================================
// NAVIGATION
// ============================================================================
function nav(el) {
    var page = el.getAttribute('data-page');
    location.hash = page;
    return false;
}

function navigateTo(page) {
    // Stop live timers when leaving live page
    if (currentPage === 'live' && page !== 'live') {
        if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
        if (liveLogsTimer) { clearInterval(liveLogsTimer); liveLogsTimer = null; }
    }

    document.querySelectorAll('.page').forEach(function (p) { p.classList.remove('active'); });
    document.querySelectorAll('.nav-item, .bottom-nav a').forEach(function (a) { a.classList.remove('active'); });

    var topPage = page.startsWith('settings') ? 'settings' : page;
    currentPage = page;

    var pageEl = document.getElementById('page-' + page);
    if (pageEl) {
        pageEl.classList.add('active');
    } else {
        var hub = document.getElementById('page-settings');
        if (hub) hub.classList.add('active');
        topPage = 'settings'; currentPage = 'settings';
    }

    document.querySelectorAll('[data-page="' + topPage + '"]').forEach(function (a) { a.classList.add('active'); });

    pageInit(page);
    applySettingsFlash();
}

function showSubpage(page) { location.hash = page; }

function pageInit(page) {
    switch (page) {
        case 'dashboard': dbInit(); break;
        case 'files': filesInit(); break;
        case 'live': liveInit(); break;
        case 'settings_device': sdInit(); break;
        case 'settings_flowmeter': sfInit(); break;
        case 'settings_hardware': hwInit(); break;
        case 'settings_theme': thInit(); break;
        case 'settings_network': netInit(); break;
        case 'settings_time': timeInit(); break;
        case 'settings_datalog': dlInit(); break;
        case 'update': otaInit(); break;
        case 'sensors': sensorsLoad(); break;
        case 'settings_corelogic': clLoad(); break;
        case 'settings_export': expLoad(); break;
    }
}

// ============================================================================
// HELPERS
// ============================================================================
function setEl(id, val) {
    var e = document.getElementById(id);
    if (e && val !== undefined && val !== null) e.textContent = val;
}
function setElStyle(id, prop, val) {
    var e = document.getElementById(id);
    if (e && val) e.style[prop] = val;
}
function setVal(id, val) {
    var e = document.getElementById(id);
    if (e && val !== undefined && val !== null) e.value = val;
}
function setChk(id, val) {
    var e = document.getElementById(id);
    if (e) e.checked = !!val;
}
function getVal(id) {
    var e = document.getElementById(id);
    return e ? e.value : '';
}

function fmtBytes(b) {
    if (!b && b !== 0) return '-';
    if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GB';
    if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
    if (b >= 1024) return (b / 1024).toFixed(1) + ' KB';
    return b + ' B';
}

function hexToRgba(hex, a) {
    if (!hex || hex.length < 7) return 'rgba(149,165,166,' + a + ')';
    var r = parseInt(hex.slice(1, 3), 16),
        g = parseInt(hex.slice(3, 5), 16),
        b = parseInt(hex.slice(5, 7), 16);
    return 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
}

function togglePass(id) {
    var e = document.getElementById(id);
    if (e) e.type = e.type === 'password' ? 'text' : 'password';
}

function showMsg(containerId, html, autoClear) {
    var el = document.getElementById(containerId);
    if (el) {
        el.innerHTML = html;
        if (autoClear) setTimeout(function () { el.innerHTML = ''; }, 4000);
    }
}

var PAGE_MSG_IDS = {
    'settings_device': 'sd-msg',
    'settings_flowmeter': 'sf-msg',
    'settings_hardware': 'hw-msg',
    'settings_theme': 'th-msg',
    'settings_network': 'net-msg',
    'settings_time': 'time-msg',
    'settings_datalog': 'dl-msg'
};

function settingsSave(ev, url, form, restart) {
    if (ev) ev.preventDefault();
    var fd = new FormData(form);
    var xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.onload = function () {
        if (restart) return;
        try {
            var r = JSON.parse(xhr.responseText);
            var msgId = PAGE_MSG_IDS[currentPage] || (currentPage.replace('settings_', '') + '-msg');
            if (r.ok) {
                sessionStorage.setItem('settingsFlash', JSON.stringify({
                    page: currentPage,
                    html: "<div class='alert alert-success'>✅ Settings saved successfully</div>"
                }));
                setTimeout(function () { location.reload(); }, 300);
            } else {
                showMsg(msgId, "<div class='alert alert-error'>❌ " + (r.error || 'Unknown error') + "</div>", true);
            }
        } catch (e) { }
    };
    xhr.onerror = function () {
        var msgId = PAGE_MSG_IDS[currentPage] || (currentPage.replace('settings_', '') + '-msg');
        showMsg(msgId, "<div class='alert alert-error'>❌ Network error</div>", true);
    };
    xhr.send(fd);
}

function applySettingsFlash() {
    var raw = sessionStorage.getItem('settingsFlash');
    if (!raw) return;
    sessionStorage.removeItem('settingsFlash');
    try {
        var f = JSON.parse(raw);
        if (!f || !f.page || !f.html) return;
        var msgId = PAGE_MSG_IDS[f.page] || (f.page.replace('settings_', '') + '-msg');
        showMsg(msgId, f.html, true);
    } catch (e) { }
}

// ============================================================================
// RESTART POPUP
// ============================================================================
function showRestartPopup() {
    setEl('rPopIcon', '🔄');
    setEl('rPopTitle', 'Restart Device?');
    setEl('rPopMsg', 'The device will restart. Any unsaved changes will be lost.');
    document.getElementById('rPopProgress').style.display = 'none';
    document.getElementById('rPopButtons').style.display = 'flex';
    document.getElementById('restartPopup').style.display = 'flex';
}
function closeRestart() {
    document.getElementById('restartPopup').style.display = 'none';
}
function confirmRestart() {
    document.getElementById('rPopButtons').style.display = 'none';
    document.getElementById('rPopProgress').style.display = 'block';
    setEl('rPopIcon', '⏳'); setEl('rPopTitle', 'Restarting…');
    var s = 5, bar = document.getElementById('rPopBar');
    var tick = function () {
        document.getElementById('rPopMsg').innerHTML = 'Redirecting in <strong>' + s + '</strong> seconds…';
        if (bar) bar.style.width = ((5 - s) * 20) + '%';
        if (s <= 0) {
            fetch('/restart').finally(function () { location.hash = 'dashboard'; location.reload(); });
        } else { s--; setTimeout(tick, 1000); }
    };
    tick();
}

// ============================================================================
// ══ PAGE: DASHBOARD ══
// Exact port of original .ino embedded JS:
//   loadData() → fetch('/download?file=...') → processData() → renderChart()
// ============================================================================
var _chartJsLoading = false;
var _chartJsLoaded = false;
var _chartJsCbs = [];

function dbLoadChartJs(cb) {
    if (_chartJsLoaded) { cb(); return; }
    _chartJsCbs.push(cb);
    if (_chartJsLoading) return;
    _chartJsLoading = true;

    var th = ST.theme || CFG.theme || {};
    var src = (th.chartSource === 0 || th.chartSource === '0')
        ? (th.chartLocalPath || '/chart.min.js')
        : 'https://cdn.jsdelivr.net/npm/chart.js';

    function fire() {
        _chartJsLoaded = true; _chartJsLoading = false;
        _chartJsCbs.forEach(function (fn) { fn(); }); _chartJsCbs = [];
    }
    var s = document.createElement('script'); s.src = src; s.onload = fire;
    s.onerror = function () {
        var s2 = document.createElement('script');
        s2.src = 'https://cdn.jsdelivr.net/npm/chart.js'; s2.onload = fire;
        s2.onerror = function () {
            _chartJsLoading = false;
            var err = document.getElementById('errorMsg');
            if (err) { err.textContent = 'Failed to load Chart.js'; err.style.display = 'block'; }
        };
        document.head.appendChild(s2);
    };
    document.head.appendChild(s);
}

function dbInit() {
    dbRestoreFilters();
    dbLoadChartJs(function () {
        // Matches original: generateDatalogFileOptions() via select population
        fetch('/api/filelist?filter=log&recursive=1')
            .then(function (r) { return r.json(); })
            .then(function (d) {
                var sel = document.getElementById('fileSelect');
                if (!sel) return;
                sel.innerHTML = '';
                var curFile = d.currentFile || ST.currentFile || '';
                if (!d.files || !d.files.length) {
                    sel.innerHTML = '<option>No log files found</option>'; return;
                }
                d.files.forEach(function (f) {
                    var opt = document.createElement('option');
                    opt.value = f.path; opt.textContent = f.path;
                    if (curFile && f.path === curFile) opt.selected = true;
                    sel.appendChild(opt);
                });
                dbLoadData(); // matches original: window.onload = loadData
            })
            .catch(function (e) {
                var err = document.getElementById('errorMsg');
                if (err) { err.textContent = 'Error loading file list: ' + e.message; err.style.display = 'block'; }
            });
    });
}


// Matches original: function loadData()
function dbLoadData() {
    var file = getVal('fileSelect');
    if (!file || file === 'No log files found') return;
    var err = document.getElementById('errorMsg');
    if (err) err.style.display = 'none';
    fetch('/download?file=' + encodeURIComponent(file))
        .then(function (r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); })
        .then(function (data) { dbRawData = data; dbApplyFilters(); })
        .catch(function (e) {
            if (err) { err.textContent = 'Error loading: ' + e.message; err.style.display = 'block'; }
        });
}


// Matches original: function applyFilters()
function dbApplyFilters() {
    if (!dbRawData) { dbLoadData(); return; }
    dbSaveFilters();
    dbViewStart = 0; dbViewEnd = -1;  // reset zoom on filter change
    dbProcessData(dbRawData);
}

// Matches original: function processData(data) — exact port of .ino embedded JS
function dbProcessData(data) {
    var lines = data.trim().split('\n');
    var filtered = [];
    var startVal = getVal('startDate');
    var endVal = getVal('endDate');
    var filterType = getVal('eventFilter');
    var pressType = getVal('pressFilter');
    var excZ = document.getElementById('excludeZero') && document.getElementById('excludeZero').checked;
    var tVol = 0, tFF = 0, tPF = 0;

    lines.forEach(function (line) {
        var p = line.split('|');
        if (p.length < 2) return;
        var dateStr = '', timeStr = '', endStr = '', boot = '', reason = '', vol = 0, ff = 0, pf = 0, i = 0;

        // Auto-detect date format (DD/MM/YYYY ┬╖ DD.MM.YYYY ┬╖ YYYY-MM-DD)
        if (p[0].match(/\d{2}[\/\.\-]\d{2}[\/\.\-]\d{4}/) || p[0].match(/\d{4}\-\d{2}\-\d{2}/)) { dateStr = p[0]; i = 1; }
        if (p[i] && p[i].indexOf(':') >= 0) { timeStr = p[i]; i++; }
        if (p[i] && (p[i].indexOf(':') >= 0 || p[i].match(/^\d+s$/))) { endStr = p[i]; i++; }
        if (p[i] && p[i].indexOf('#:') === 0) { boot = p[i].substring(2); i++; }
        if (p[i] && (p[i].indexOf('FF') >= 0 || p[i].indexOf('PF') >= 0 || p[i] === 'IDLE')) { reason = p[i]; i++; }
        if (p[i]) { var vs = p[i].replace('L:', '').replace(',', '.'); vol = parseFloat(vs) || 0; i++; }
        if (p[i] && p[i].indexOf('FF') === 0) { ff = parseInt(p[i].replace('FF', '')) || 0; i++; }
        if (p[i] && p[i].indexOf('PF') === 0) { pf = parseInt(p[i].replace('PF', '')) || 0; }

        var entryDate = '';
        if (dateStr) {
            var m;
            if ((m = dateStr.match(/(\d{2})[\/\.](\d{2})[\/\.](\d{4})/))) entryDate = m[3] + '-' + m[2] + '-' + m[1];
            else if ((m = dateStr.match(/(\d{4})\-(\d{2})\-(\d{2})/))) entryDate = m[1] + '-' + m[2] + '-' + m[3];
        }

        // Filters — exact logic from original .ino
        if (startVal && entryDate && entryDate < startVal) return;
        if (endVal && entryDate && entryDate > endVal) return;
        if (filterType === 'BTN' && reason.indexOf('FF') < 0 && reason.indexOf('PF') < 0) return;
        if (filterType === 'FF' && reason.indexOf('FF') < 0) return;
        if (filterType === 'PF' && reason.indexOf('PF') < 0) return;
        if (pressType === 'EXTRA' && ff === 0 && pf === 0) return;
        if (pressType === 'NONE' && (ff > 0 || pf > 0)) return;
        if (excZ && vol === 0) return;

        tFF += ff; tPF += pf; tVol += vol;
        var fullTime = timeStr + (endStr ? '-' + endStr : '');
        filtered.push({ date: dateStr || 'N/A', time: timeStr, fullTime: fullTime, boot: boot, vol: vol, reason: reason, ff: ff, pf: pf });
    });

    dbFilteredData = filtered;
    // Element IDs match original: totalVol, eventCount, totalFF, totalPF
    setEl('db-totalVol', tVol.toFixed(2) + ' L');
    setEl('db-eventCount', filtered.length);
    setEl('db-totalFF', tFF);
    setEl('db-totalPF', tPF);
    dbRenderChart(filtered);
}

// Matches original: function renderChart(data)
function dbRenderChart(data) {
    var ctx = document.getElementById('chart');
    if (!ctx) return;
    if (typeof Chart === 'undefined') { dbLoadChartJs(function () { dbRenderChart(data); }); return; }
    if (dbChart) { dbChart.destroy(); dbChart = null; }

    // Apply zoom window
    if (data.length > 0 && dbViewEnd >= 0) {
        var s = Math.max(0, Math.min(dbViewStart, data.length - 1));
        var e = Math.max(s, Math.min(dbViewEnd, data.length - 1));
        data = data.slice(s, e + 1);
    }

    var th = ST.theme || CFG.theme || {};
    var rootStyle = getComputedStyle(document.documentElement);
    var ffColor = th.ffColor || rootStyle.getPropertyValue('--ff-color').trim() || '#275673';
    var pfColor = th.pfColor || rootStyle.getPropertyValue('--pf-color').trim() || '#7eb0d5';
    var otherColor = th.otherColor || rootStyle.getPropertyValue('--other-color').trim() || '#a0aec0';

    var clr = data.map(function (d) {
        if (d.reason.indexOf('FF') >= 0) return ffColor;
        if (d.reason.indexOf('PF') >= 0) return pfColor;
        return otherColor;
    });

    // Matches original: var lblFmt = config.theme.chartLabelFormat
    var lblFmt = th.chartLabelFormat !== undefined ? th.chartLabelFormat : 0;
    var lbls = data.map(function (d) {
        if (lblFmt === 1) return d.boot ? '#' + d.boot : '#?';
        if (lblFmt === 2) return d.date + ' ' + d.time + (d.boot ? ' #' + d.boot : '');
        return d.date + ' ' + d.time;
    });

    var threshEl = document.getElementById('db-threshold');
    var threshVal = threshEl ? parseFloat(threshEl.value) : NaN;
    var datasets = [{ type: 'bar', label: 'Liters (L)', data: data.map(function (d) { return d.vol; }), backgroundColor: clr, borderWidth: 0 }];
    if (!isNaN(threshVal) && threshVal > 0) {
        datasets.push({
            type: 'line',
            label: 'Threshold',
            data: data.map(function () { return threshVal; }),
            borderColor: '#e74c3c',
            borderWidth: 2,
            borderDash: [6, 3],
            pointRadius: 0,
            fill: false
        });
    }

    dbChart = new Chart(ctx, {
        type: 'bar',
        data: { labels: lbls, datasets: datasets },
        options: {
            responsive: true, maintainAspectRatio: false,
            plugins: {
                tooltip: {
                    callbacks: {
                        afterLabel: function (c) {
                            if (c.datasetIndex !== 0) return [];
                            var d = data[c.dataIndex];
                            return ['Trigger: ' + d.reason, 'Boot: ' + (d.boot || 'N/A'), 'Extra FF: ' + d.ff, 'Extra PF: ' + d.pf];
                        }
                    }
                }
            },
            scales: { y: { beginAtZero: true, title: { display: true, text: 'Liters' } } }
        }
    });
}


// Matches original: function exportCSV()
function dbExportCSV() {
    if (!dbFilteredData.length) { alert('No data to export'); return; }
    var csv = 'Date,Time,Boot,Volume (L),Trigger,Extra FF,Extra PF\n';
    dbFilteredData.forEach(function (d) {
        csv += d.date + ',' + d.fullTime + ',' + (d.boot || '') + ',' + d.vol.toFixed(2) + ',' + d.reason + ',' + d.ff + ',' + d.pf + '\n';
    });
    // Filename: deviceId_filters_date.csv  — exact match to original .ino
    var f = ST.deviceId || CFG.deviceId || 'logger';
    var ft = getVal('eventFilter'); if (ft !== 'ALL') f += '_' + ft;
    var pt = getVal('pressFilter'); if (pt !== 'ALL') f += '_' + pt;
    var excZ = document.getElementById('excludeZero');
    if (excZ && excZ.checked) f += '_noZero';
    var sd = getVal('startDate'); if (sd) f += '_from' + sd;
    var ed = getVal('endDate'); if (ed) f += '_to' + ed;
    f += '_' + new Date().toISOString().slice(0, 10) + '.csv';
    var blob = new Blob([csv], { type: 'text/csv' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a'); a.href = url; a.download = f; a.click();
    URL.revokeObjectURL(url);
}

// ============================================================================
// ══ PAGE: FILES ══
// ============================================================================
function filesInit() {
    filesEditMode = false;
    currentFilesDir = '/';
    var hw = CFG.hardware || {};
    currentFilesStorage = (hw.defaultStorageView === 1) ? 'sdcard' : 'internal';
    var list = document.getElementById('list');
    if (list) list.innerHTML = "<div class='list-item text-muted'>Loading…</div>";
    filesRender();
}

function filesRender() {
    var tabs = document.getElementById('tabs');
    if (tabs) {
        tabs.innerHTML =
            '<a onclick="filesSetStorage(\'internal\')" class="btn ' + (currentFilesStorage === 'internal' ? 'btn-primary' : 'btn-secondary') + '">💾 Internal</a> ' +
            '<a onclick="filesSetStorage(\'sdcard\')"   class="btn ' + (currentFilesStorage === 'sdcard' ? 'btn-primary' : 'btn-secondary') + '">💳 SD Card</a>';
    }

    fetch('/api/filelist?storage=' + currentFilesStorage + '&dir=' + encodeURIComponent(currentFilesDir))
        .then(function (r) { return r.json(); })
        .then(function (d) {
            var pct = d.percent || 0;
            setEl('files-usage', fmtBytes(d.used) + ' / ' + fmtBytes(d.total));
            setEl('files-pct', pct + '%');
            var bar = document.getElementById('bar');
            if (bar) {
                bar.style.width = pct + '%';
                bar.className = 'progress-bar' + (pct >= 90 ? ' progress-bar-danger' : pct >= 70 ? ' progress-bar-warning' : ' progress-bar-success');
            }

            setEl('files-dirLabel', '📂 [' + (currentFilesStorage === 'sdcard' ? 'SD' : 'Int') + '] ' + (currentFilesDir === '/' ? 'Root' : currentFilesDir));
            var upBtn = document.getElementById('upBtn');
            if (upBtn) upBtn.style.display = currentFilesDir === '/' ? 'none' : '';
            var et = document.getElementById('editToggle');
            if (et) et.textContent = filesEditMode ? '✖️ Done' : '✏️ Edit';
            var tools = document.getElementById('editTools');
            if (tools) tools.style.display = filesEditMode ? 'block' : 'none';

            var list = document.getElementById('list');
            if (!list) return;
            var files = d.files || [];
            if (!files.length) { list.innerHTML = "<div class='list-item text-muted'>Empty</div>"; return; }

            var html = '';
            files.forEach(function (f) {
                var safePath = f.path.replace(/'/g, "\\'");
                var safeName = f.name.replace(/'/g, "\\'");
                var actions = '';
                if (f.isDir) {
                    actions = "<a onclick=\"filesEnterDir('" + safePath + "')\" class='btn btn-sm btn-secondary'>📂 Open</a>";
                } else {
                    actions += "<a href='/download?file=" + encodeURIComponent(f.path) + "&storage=" + currentFilesStorage + "' class='btn btn-sm btn-secondary'>📥</a>";
                    if (filesEditMode) {
                        actions += " <button onclick=\"showMovePopup('" + safePath + "','" + safeName + "')\" class='btn btn-sm btn-secondary'>✂️</button>";
                        // Use data-path to avoid quote-in-attribute bug
                        actions += " <button data-path=\"" + f.path + "\" onclick=\"filesDelete(this.dataset.path)\" class='btn btn-sm btn-danger'>🗑️</button>";
                    }
                }
                html += "<div class='list-item'><span>" + (f.isDir ? '📁 ' : '📄 ') + f.name +
                    (f.isDir ? '' : ' <small class="text-muted">(' + fmtBytes(f.size) + ')</small>') +
                    "</span><span class='btn-group'>" + actions + "</span></div>";
            });
            list.innerHTML = html;
        })
        .catch(function (e) {
            var list = document.getElementById('list');
            if (list) list.innerHTML = "<div class='list-item' style='color:red'>Error: " + e + "</div>";
        });
}


function filesSetStorage(s) { currentFilesStorage = s; currentFilesDir = '/'; filesRender(); }
function filesEnterDir(d) { currentFilesDir = d; filesRender(); }
function filesGoUp() {
    var p = currentFilesDir.lastIndexOf('/');
    currentFilesDir = p <= 0 ? '/' : currentFilesDir.substring(0, p);
    filesRender();
}
function filesToggleEdit() { filesEditMode = !filesEditMode; filesRender(); }

function filesDelete(path) {
    if (!confirm('Delete ' + path + '?')) return;
    fetch('/delete?path=' + encodeURIComponent(path) + '&storage=' + currentFilesStorage)
        .then(function () { filesRender(); })
        .catch(function (e) { alert('Error: ' + e); });
}

function filesUpload() {
    var inp = document.getElementById('fileInput');
    if (!inp || !inp.files.length) return;
    var files = inp.files, i = 0;
    var prog = document.getElementById('uploadProg');
    var bar = document.getElementById('uploadBar');
    var pct = document.getElementById('uploadPct');
    if (prog) prog.style.display = 'block';
    function next() {
        if (i >= files.length) {
            if (prog) prog.style.display = 'none';
            if (bar) bar.style.width = '0%';
            inp.value = ''; filesRender(); return;
        }
        var fd = new FormData();
        fd.append('file', files[i]);
        fd.append('path', currentFilesDir);
        fd.append('storage', currentFilesStorage);
        var xhr = new XMLHttpRequest();
        xhr.upload.onprogress = function (ev) {
            if (ev.lengthComputable) { var p = Math.round(ev.loaded / ev.total * 100); if (bar) bar.style.width = p + '%'; if (pct) pct.textContent = p + '%'; }
        };
        xhr.onload = function () { i++; next(); };
        xhr.onerror = function () { alert('Upload failed: ' + files[i].name); if (prog) prog.style.display = 'none'; };
        xhr.open('POST', '/upload'); xhr.send(fd);
    }
    next();
}

function filesMkdir() {
    var name = document.getElementById('newFolder');
    if (!name || !name.value.trim()) return;
    fetch('/mkdir?name=' + encodeURIComponent(name.value.trim()) + '&dir=' + encodeURIComponent(currentFilesDir) + '&storage=' + currentFilesStorage)
        .then(function () { name.value = ''; filesRender(); });
}

var mvSrcPath = '';
function showMovePopup(path, name) {
    mvSrcPath = path;
    var inp = document.getElementById('name'); if (inp) inp.value = name;
    document.getElementById('movePopup').style.display = 'flex';
}
function filesApplyMove() {
    var newName = getVal('name').trim(), destDir = getVal('dest');
    if (!newName) return;
    var url = '/move_file?src=' + encodeURIComponent(mvSrcPath) + '&newName=' + encodeURIComponent(newName) + '&storage=' + currentFilesStorage;
    if (destDir) url += '&destDir=' + encodeURIComponent(destDir);
    fetch(url).then(function () { document.getElementById('movePopup').style.display = 'none'; filesRender(); })
        .catch(function (e) { alert('Error: ' + e); });
}

// ============================================================================
// ══ PAGE: LIVE ══
// Matches original: function upd() polling /api/live every 500ms
//                   function updLogs() polling /api/recent_logs every 3s
// ============================================================================
function liveInit() {
    if (ST.chip) setEl('live-chip', ST.chip);
    if (ST.cpu) setEl('live-cpu', ST.cpu);
    if (ST.ip) setEl('live-ip', ST.ip);
    if (ST.network) setEl('live-net', ST.network);

    var hint = document.getElementById('stateHint');
    if (hint) {
        var fm = CFG.flowMeter || {};
        var fl = fm.firstLoopMonitoringWindowSecs || '?';
        var win = fm.monitoringWindowSecs || '?';
        hint.textContent = '🔧 IDLE → 🟡 WAIT_FLOW (' + fl + 's) → 🟢 MONITORING (' + win + 's idle) → Logging';
    }

    var rateEl = document.getElementById('live-refresh-rate');
    var rate = rateEl ? parseInt(rateEl.value, 10) || 500 : 500;

    liveUpdate();
    liveLogsUpdate();
    liveTimer = setInterval(liveUpdate, rate);
    liveLogsTimer = setInterval(liveLogsUpdate, 3000);
}

function liveSetRate() {
    var rateEl = document.getElementById('live-refresh-rate');
    if (!rateEl) return;
    var rate = parseInt(rateEl.value, 10) || 500;
    if (liveTimer) { clearInterval(liveTimer); liveTimer = null; }
    liveTimer = setInterval(liveUpdate, rate);
}

// Matches original: function upd()
function liveUpdate() {
    fetch('/api/live')
        .then(function (r) { return r.json(); })
        .then(function (d) {
            var conn = document.getElementById('conn');
            if (conn) { conn.textContent = '● Connected'; conn.className = 'text-success'; }

            setEl('live-time', d.time);
            setEl('live-trigger', d.trigger);
            setEl('live-cycleTime', d.cycleTime);
            setEl('live-pulses', d.pulses);
            setEl('live-liters', parseFloat(d.liters || 0).toFixed(2));
            setEl('live-ffCount', d.ffCount);
            setEl('live-pfCount', d.pfCount);
            setEl('live-boot', d.boot);
            setEl('live-heap', fmtBytes(d.heap));
            setEl('live-heapTotal', fmtBytes(d.heapTotal));
            setEl('live-uptime', d.uptime);
            if (d.fsTotal) setEl('live-storage', fmtBytes(d.fsUsed) + '/' + fmtBytes(d.fsTotal));

            // State machine colors — exact from original .ino
            var stColors = { IDLE: '#3498db', WAIT_FLOW: '#f39c12', MONITORING: '#27ae60', DONE: '#e74c3c' };
            var stEl = document.getElementById('state');
            if (stEl) { stEl.textContent = d.state; stEl.style.background = stColors[d.state] || '#95a5a6'; stEl.style.color = '#fff'; }
            var remEl = document.getElementById('stateRem');
            if (remEl) remEl.textContent = d.stateRemaining >= 0 ? d.stateRemaining + 's' : '-';

            // Button states
            liveBtn('live-ff', d.ff, 'Pressed', 'Released', '#27ae60', '#95a5a6');
            liveBtn('live-pf', d.pf, 'Pressed', 'Released', '#27ae60', '#95a5a6');
            liveBtn('live-wifi', d.wifi, 'Pressed', 'Released', '#3498db', '#95a5a6');

            // Mode display — matches original getModeDisplay()
            var modeEl = document.getElementById('mode');
            if (modeEl) {
                if (d.mode === 'online') modeEl.innerHTML = '🌐 Online Logger';
                else if (d.mode === 'webonly') modeEl.innerHTML = '📡 Web Only';
                else modeEl.innerHTML = '📊 Logging';
            }

            // Live header time update
            if (d.time) setEl('headerTime', d.time.split(' ')[1] || d.time);

            // Footer live refresh — boot count + heap (chip/version stay from applyStatus)
            updateFooter({ boot: d.boot, heap: d.heap, heapTotal: d.heapTotal });
        })
        .catch(function () {
            var conn = document.getElementById('conn');
            if (conn) { conn.textContent = '● Disconnected'; conn.className = 'text-danger'; }
        });
}


function liveBtn(id, pressed, txtOn, txtOff, colorOn, colorOff) {
    var el = document.getElementById(id); if (!el) return;
    el.textContent = pressed ? txtOn : txtOff;
    el.style.background = pressed ? colorOn : colorOff;
}

// Matches original: function updLogs()
function liveLogsUpdate() {
    fetch('/api/recent_logs')
        .then(function (r) { return r.json(); })
        .then(function (d) {
            var el = document.getElementById('logs'); if (!el) return;
            var th = ST.theme || CFG.theme || {};
            var ffC = th.ffColor || '#3498db', pfC = th.pfColor || '#e74c3c', otC = th.otherColor || '#95a5a6';
            if (d.logs && d.logs.length) {
                var html = '<table style="width:100%;border-collapse:collapse;font-size:.75rem">';
                html += '<tr style="background:var(--bg)"><th style="padding:6px;text-align:left">Time</th><th>Trigger</th><th>Volume</th><th>+FF</th><th>+PF</th></tr>';
                d.logs.forEach(function (l) {
                    var color = l.trigger.indexOf('FF') >= 0 ? ffC : l.trigger.indexOf('PF') >= 0 ? pfC : otC;
                    var bg = hexToRgba(color, 0.15);
                    html += '<tr style="background:' + bg + '">' +
                        '<td style="padding:6px">' + l.time + '</td>' +
                        '<td style="color:' + color + ';font-weight:bold;text-align:center">' + l.trigger + '</td>' +
                        '<td style="text-align:center">' + l.volume + '</td>' +
                        '<td style="text-align:center">' + l.ff + '</td>' +
                        '<td style="text-align:center">' + l.pf + '</td></tr>';
                });
                html += '</table>';
                el.innerHTML = html;
            } else {
                el.innerHTML = "<div class='list-item text-muted'>No log entries yet</div>";
            }
        })
        .catch(function () { });
}

// ============================================================================
// ══ SETTINGS: DEVICE ══
// ============================================================================
function sdInit() {
    // Reset changelog state on every page enter so re-navigation works cleanly
    changelogLoaded = false;
    var clEl = document.getElementById('changelog');
    var chev = document.getElementById('changelogChevron');
    if (clEl) clEl.classList.add('hidden');
    if (chev) chev.style.transform = '';

    fetch('/api/status').then(function (r) { return r.json(); }).then(function (d) {
        ST = d;
        setVal('sd-devName', d.device || d.deviceName);
        setVal('sd-devId', d.deviceId);
        setVal('sd-defaultStorage', d.defaultStorageView !== undefined ? d.defaultStorageView : 0);
        setChk('sd-forceWS', d.forceWebServer);

        // System Info card — matches original .ino "System Info" card content
        var info = document.getElementById('sysInfo');
        if (info) {
            info.innerHTML =
                '<div><strong>Firmware</strong><div class="text-primary">' + (d.version || '-') + '</div></div>' +
                '<div><strong>Boot Count</strong><div>' + (d.boot || 0) + '</div></div>' +
                '<div><strong>Mode</strong><div>' + (d.mode || '-') + '</div></div>' +
                '<div><strong>Free Heap</strong><div>' + fmtBytes(d.heap) + '</div></div>' +
                '<div><strong>CPU</strong><div>' + (d.cpu || '-') + ' MHz</div></div>' +
                '<div><strong>Chip</strong><div>' + (d.chip || '-') + '</div></div>';
        }
    });
}


function regenDevId() {
    if (!confirm('Generate new ID based on MAC address?')) return;
    fetch('/api/regen-id', { method: 'POST' })
        .then(function (r) { return r.text(); })
        .then(function (id) {
            var inp = document.getElementById('sd-devId');
            if (inp) { inp.value = id.trim(); inp.disabled = false; }
            alert('New ID generated: ' + id.trim() + '. Click Save to apply.');
        })
        .catch(function (e) { alert('Error: ' + e); });
}

// Matches original: function toggleManualId(id)
function toggleManualId(id) {
    var el = document.getElementById(id);
    if (el) el.disabled = !el.disabled;
}

// ============================================================================
// ══ CHANGELOG ══
// Matches original .ino Device page inline JS:
//   fetch('/api/changelog') → render ## sections → first block highlighted
// ============================================================================

// Matches original: onclick="changelogToggle()" on card-header
function changelogToggle() {
    var el = document.getElementById('changelog');
    if (!el) return;

    var isHidden = el.classList.contains('hidden');
    if (isHidden) {
        el.classList.remove('hidden');
        if (!changelogLoaded) changelogLoad();
    } else {
        el.classList.add('hidden');
    }

    // Rotate chevron indicator — matches original .ino #sd-changelogChevron
    var chev = document.getElementById('changelogChevron');
    if (chev) chev.style.transform = el.classList.contains('hidden') ? '' : 'rotate(180deg)';
}

// Used by error close button inside changelog body
function changelogClose(ev) {
    if (ev) ev.stopPropagation();
    var el = document.getElementById('changelog');
    if (el) el.classList.add('hidden');
    var chev = document.getElementById('changelogChevron');
    if (chev) chev.style.transform = '';
}

// Matches original .ino Device page:
//   fetch('/api/changelog').then(r=>r.ok?r.text():...).then(txt=>{ render ## sections })
function changelogLoad() {
    var el = document.getElementById('changelog'); if (!el) return;
    el.innerHTML = "<div class='text-muted' style='padding:.5rem'>Loading…</div>";

    fetch('/api/changelog')
        .then(function (r) {
            if (!r.ok) throw new Error('not found');
            return r.text();
        })
        .then(function (txt) {
            changelogLoaded = true;           // mark loaded — re-open skips fetch

            var html = '';
            var lines = txt.trim().split('\n');
            var inVer = false;
            var currentMarked = false;
            var hasEntries = false;           // local var — not leaked to global scope

            lines.forEach(function (rawLine) {
                var line = rawLine.trim(); if (!line) return;
                if (line.indexOf('##') === 0) {
                    hasEntries = true;
                    if (inVer) html += '</ul></div>';
                    var ver = line.substring(2).trim();
                    var isCur = !currentMarked; if (isCur) currentMarked = true;
                    html += '<div style="margin-top:.5rem;padding:.5rem;border-radius:4px;' +
                        (isCur ? 'background:var(--primary);color:#fff' : 'background:var(--border);color:var(--text-muted)') + '">' +
                        '<strong>' + ver + '</strong>' +
                        '<ul style="margin:.5rem 0 0 1rem;padding:0;font-size:.9rem">';
                    inVer = true;
                } else if (line.indexOf('-') === 0 && inVer) {
                    html += '<li>' + line.substring(1).trim() + '</li>';
                }
            });

            if (inVer) html += '</ul></div>';
            if (!hasEntries) html += "<div class='text-muted'>No entries found.</div>";
            el.innerHTML = html;
        })
        .catch(function () {
            changelogLoaded = false;          // allow retry on next open
            el.innerHTML =
                "<div style='display:flex;justify-content:flex-end;margin-bottom:.5rem'>" +
                "<button type='button' class='btn btn-secondary btn-sm' onclick='changelogClose(event)'>✖ Close</button></div>" +
                "<div class='alert alert-warning'>Changelog not found. Upload <code>/changelog.txt</code> to LittleFS.</div>";
        });
}


// ============================================================================
// ══ SETTINGS: FLOW METER ══
// ============================================================================
function sfInit() {
    fetch('/export_settings').then(function (r) { return r.json(); }).then(function (d) {
        CFG = d; var fm = d.flowMeter || {};
        setVal('sf-ppl', fm.pulsesPerLiter);
        setVal('sf-cal', fm.calibrationMultiplier);
        setVal('sf-win', fm.monitoringWindowSecs);
        setVal('sf-fl', fm.firstLoopMonitoringWindowSecs);
        setChk('sf-test', fm.testMode);
        setVal('sf-blink', fm.blinkDuration);
        fetch('/api/status').then(function (r2) { return r2.json(); }).then(function (s) { setEl('sf-boot', s.boot); });
    });
}


// ============================================================================
// ══ SETTINGS: HARDWARE ══
// ============================================================================
function hwInit() {
    fetch('/export_settings').then(function (r) { return r.json(); }).then(function (d) {
        CFG = d; var hw = d.hardware || {}, th = ST.theme || CFG.theme || {};
        setVal('hw-storage', hw.storageType !== undefined ? hw.storageType : 0);
        var sdP = document.getElementById('sdPins'); if (sdP) sdP.style.display = hw.storageType == 1 ? 'block' : 'none';
        setVal('hw-sdCS', hw.pinSdCS); setVal('hw-sdMOSI', hw.pinSdMOSI);
        setVal('hw-sdMISO', hw.pinSdMISO); setVal('hw-sdSCK', hw.pinSdSCK);
        setVal('hw-wakeup', hw.wakeupMode !== undefined ? hw.wakeupMode : 0);
        setVal('hw-debounce', hw.debounceMs || 100);
        setVal('hw-pinWifi', hw.pinWifiTrigger); setVal('hw-pinFF', hw.pinWakeupFF);
        setVal('hw-pinPF', hw.pinWakeupPF); setVal('hw-pinFlow', hw.pinFlowSensor);
        setVal('hw-rtcCE', hw.pinRtcCE); setVal('hw-rtcIO', hw.pinRtcIO);
        setVal('hw-rtcCLK', hw.pinRtcSCLK); setVal('hw-cpu', hw.cpuFreqMHz || 80);
        if (th.boardDiagramPath) {
            var card = document.getElementById('boardDiagramCard');
            var img = document.getElementById('boardDiagram');
            if (card) card.style.display = 'block'; if (img) img.src = th.boardDiagramPath;
        }
    });
}


// ============================================================================
// ══ SETTINGS: THEME ══
// ============================================================================
function thInit() {
    fetch('/export_settings').then(function (r) { return r.json(); }).then(function (d) {
        CFG = d; var th = d.theme || {};
        var mode = th.mode !== undefined ? th.mode : 0;
        setVal('th-mode', mode);
        setChk('th-icons', th.showIcons);

        var isDark = (mode === 1 || (mode === 2 && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches));

        setVal('th-primary', th.primaryColor || '#275673');
        setVal('th-secondary', th.secondaryColor || '#4a5568');
        window._thData = th;
        function updateColorPickers(m) {
            var isD = (m === 1 || (m === 2 && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches));
            setVal('th-bg', isD ? (window._thData.darkBgColor || '#0f172a') : (window._thData.lightBgColor || '#f0f2f5'));
            setVal('th-text', isD ? (window._thData.darkTextColor || '#e2e8f0') : (window._thData.lightTextColor || '#2d3748'));
        }
        updateColorPickers(mode);

        var modeSelect = document.getElementById('th-mode');
        if (modeSelect && !window._modeListenerAppended) {
            modeSelect.addEventListener('change', function (e) {
                updateColorPickers(parseInt(e.target.value));
            });
            window._modeListenerAppended = true;
        }
        setVal('th-ff', th.ffColor || '#275673');
        setVal('th-pf', th.pfColor || '#7eb0d5');
        setVal('th-other', th.otherColor || '#a0aec0');
        setVal('th-bar', th.storageBarColor || '#27ae60');
        setVal('th-bar70', th.storageBar70Color || '#f39c12');
        setVal('th-bar90', th.storageBar90Color || '#e74c3c');
        setVal('th-barB', th.storageBarBorder || '#cccccc');
        setVal('th-logo', th.logoSource); setVal('th-favicon', th.faviconPath);
        setVal('th-board', th.boardDiagramPath);
        setVal('th-chartSrc', th.chartSource !== undefined ? th.chartSource : 0);
        var pr = document.getElementById('chartPathRow');
        if (pr) pr.style.display = (th.chartSource == 0 || !th.chartSource) ? 'block' : 'none';
        setVal('th-chartPath', th.chartLocalPath);
        setVal('th-labelFmt', th.chartLabelFormat !== undefined ? th.chartLabelFormat : 0);
    });
}

function themeSave(e, form) {
    e.preventDefault();
    var fd = new FormData(form);

    var mode = parseInt(fd.get('themeMode') || '0');
    var isDark = (mode === 1 || (mode === 2 && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches));

    var currentBg = fd.get('bgColor') || '';
    var currentText = fd.get('textColor') || '';
    fd.delete('bgColor');
    fd.delete('textColor');

    if (isDark) {
        fd.append('darkBgColor', currentBg);
        fd.append('darkTextColor', currentText);
        if (window._thData && window._thData.lightBgColor) fd.append('lightBgColor', window._thData.lightBgColor);
        if (window._thData && window._thData.lightTextColor) fd.append('lightTextColor', window._thData.lightTextColor);
    } else {
        fd.append('lightBgColor', currentBg);
        fd.append('lightTextColor', currentText);
        if (window._thData && window._thData.darkBgColor) fd.append('darkBgColor', window._thData.darkBgColor);
        if (window._thData && window._thData.darkTextColor) fd.append('darkTextColor', window._thData.darkTextColor);
    }

    var defs = {
        'primaryColor': '#275673',
        'secondaryColor': '#4a5568',
        'lightBgColor': '#f0f2f5',
        'lightTextColor': '#2d3748',
        'darkBgColor': '#0f172a',
        'darkTextColor': '#e2e8f0',
        'ffColor': '#275673',
        'pfColor': '#7eb0d5',
        'otherColor': '#a0aec0',
        'storageBarColor': '#27ae60',
        'storageBar70Color': '#f39c12',
        'storageBar90Color': '#e74c3c',
        'storageBarBorder': '#cccccc'
    };

    for (var key in defs) {
        var val = fd.get(key);
        if (val && val.toLowerCase() === defs[key].toLowerCase()) {
            fd.set(key, '');
        }
    }

    var btn = form.querySelector('button[type="submit"]');
    var old = btn.innerHTML;
    btn.innerHTML = 'Saving...';
    btn.disabled = true;

    fetch('/save_theme', { method: 'POST', body: fd })
        .then(function (r) { return r.json(); })
        .then(function (d) {
            btn.innerHTML = old;
            btn.disabled = false;
            if (d.ok) {
                var m = document.getElementById('th-msg');
                if (m) m.innerHTML = '<div class="alert alert-success mt-1 mb-1">Theme saved! Rebooting...</div>';
                setTimeout(function () { location.reload(); }, 1000);
            } else {
                alert('Save failed.');
            }
        }).catch(function (err) {
            btn.innerHTML = old;
            btn.disabled = false;
            alert('Error: ' + err);
        });
}

function themeRestoreDefault() {
    if (!confirm('Are you sure you want to restore the default theme colors? This will wipe your custom choices.')) return;
    var fd = new FormData();
    fd.append('themeMode', '0');
    fd.append('primaryColor', '');
    fd.append('secondaryColor', '');
    fd.append('lightBgColor', '');
    fd.append('lightTextColor', '');
    fd.append('darkBgColor', '');
    fd.append('darkTextColor', '');
    fd.append('ffColor', '');
    fd.append('pfColor', '');
    fd.append('otherColor', '');
    fd.append('storageBarColor', '');
    fd.append('storageBar70Color', '');
    fd.append('storageBar90Color', '');
    fd.append('storageBarBorder', '');
    fetch('/save_theme', { method: 'POST', body: fd }).then(function (r) { return r.json(); }).then(function (d) {
        if (d.ok) {
            alert('Theme restored to defaults! Rebooting...');
            location.reload();
        } else {
            alert('Failed to restore theme defaults.');
        }
    }).catch(function () {
        alert('Theme restored to defaults! Rebooting...');
        location.reload();
    });
}

// ============================================================================
// ══ SETTINGS: NETWORK ══
// ============================================================================
function netInit() {
    fetch('/api/status').then(function (r) { return r.json(); }).then(function (d) {
        ST = d;
        setEl('net-status', d.wifi === 'client' ? 'Connected: ' + (d.network || '') : 'AP Mode');
        var rssiVal = d.rssi !== undefined ? d.rssi : -100;
        var rSvg = getRssiInfo(rssiVal);
        var textEl = document.getElementById('net-rssi');
        if (textEl) {
            textEl.innerText = d.rssi !== undefined ? d.rssi + ' dBm' : '-';
            textEl.style.color = ''; // Remove inline color
        }
        var iconEl = document.getElementById('net-rssi-icon');
        if (iconEl) {
            iconEl.innerHTML = rSvg;
            iconEl.style.color = ''; // Remove inline color
        }
        if (d.wifi === 'client') {
            setVal('net-ip2-current', d.ip || '');
            setVal('net-gw-current', d.gateway || '');
            setVal('net-sn-current', d.subnet || '');
            setVal('net-dns-current', d.dns || '');
        }
        netToggleStatic(); // Trigger UI update based on new loaded current properties
    });
    fetch('/export_settings').then(function (r) { return r.json(); }).then(function (d) {
        CFG = d; var net = d.network || {};
        setVal('net-mode', net.wifiMode !== undefined ? net.wifiMode : 0);
        netToggleMode();
        setVal('net-apSSID', net.apSSID); setVal('net-apPass', net.apPassword || '');
        setVal('net-apIP', net.apIP || ''); setVal('net-apGW', net.apGateway || '');
        setVal('net-apSN', net.apSubnet || '');
        setVal('net-cSSID', net.clientSSID); setVal('net-cPass', net.clientPassword || '');
        setChk('net-staticCheck', net.useStaticIP);
        setVal('net-ip2', net.staticIP || '0.0.0.0'); setVal('net-gw', net.gateway || '0.0.0.0');
        setVal('net-sn', net.subnet || '0.0.0.0'); setVal('net-dns', net.dns || '0.0.0.0');
        netToggleStatic();
    });
}

function getRssiInfo(rssi) {
    var bars = 0, cls = 'text-muted';
    if (rssi >= -50) { bars = 4; cls = 'text-success'; }
    else if (rssi >= -70) { bars = 3; cls = 'text-primary'; }
    else if (rssi >= -80) { bars = 2; cls = 'text-warning'; }
    else if (rssi >= -90) { bars = 1; cls = 'text-danger'; }

    var svg = '<svg width="16" height="14" viewBox="0 0 16 14" class="' + cls + '" fill="currentColor" style="vertical-align:middle">';
    svg += '<rect x="0" y="10" width="3" height="4" rx="1" fill="' + (bars >= 1 ? 'currentColor' : '#ccc') + '"/>';
    svg += '<rect x="4" y="7" width="3" height="7" rx="1" fill="' + (bars >= 2 ? 'currentColor' : '#ccc') + '"/>';
    svg += '<rect x="8" y="4" width="3" height="10" rx="1" fill="' + (bars >= 3 ? 'currentColor' : '#ccc') + '"/>';
    svg += '<rect x="12" y="0" width="3" height="14" rx="1" fill="' + (bars >= 4 ? 'currentColor' : '#ccc') + '"/>';
    svg += '</svg>';
    return svg;
}

// Matches original: function toggleMode()
function netToggleMode() {
    var m = getVal('net-mode');
    var ap = document.getElementById('apSection');
    var cl = document.getElementById('clientSection');
    if (ap) ap.style.display = m === '0' ? 'block' : 'none';
    if (cl) cl.style.display = m === '1' ? 'block' : 'none';
}

// Matches original: function toggleStatic()
function netToggleStatic() {
    var en = document.getElementById('staticCheck') && document.getElementById('staticCheck').checked;

    // Manage which values are in the text inputs: current connection vs static config
    if (!en && ST && ST.wifi === 'client') {
        // Not using static, and we are connected: show current DHCP details
        setVal('net-ip2', getVal('net-ip2-current'));
        setVal('net-gw', getVal('net-gw-current'));
        setVal('net-sn', getVal('net-sn-current'));
        setVal('net-dns', getVal('net-dns-current'));
    } else if (CFG && CFG.network) {
        // Enforce restoring CFG when we switch back to 'static'
        setVal('net-ip2', CFG.network.staticIP || '0.0.0.0');
        setVal('net-gw', CFG.network.gateway || '0.0.0.0');
        setVal('net-sn', CFG.network.subnet || '0.0.0.0');
        setVal('net-dns', CFG.network.dns || '0.0.0.0');
    }

    ['net-ip2', 'net-gw', 'net-sn', 'net-dns'].forEach(function (id) {
        var el = document.getElementById(id); if (!el) return;
        el.disabled = !en; el.style.opacity = en ? '1' : '0.5'; el.style.cursor = en ? 'text' : 'not-allowed';
    });
}


// Matches original: function scanWifi() / function checkScanResult()
function netScanWifi() {
    var list = document.getElementById('wifiList'); if (!list) return;
    list.innerHTML = "<div class='list-item'>🔍 Scanning…</div>"; list.style.display = 'block';
    netScanRetries = 0;
    fetch('/wifi_scan_start').then(function () { setTimeout(netCheckScan, 2000); });
}
function netCheckScan() {
    fetch('/wifi_scan_result').then(function (r) { return r.json(); }).then(function (d) {
        var list = document.getElementById('wifiList'); if (!list) return;
        if (d.scanning) {
            netScanRetries++;
            if (netScanRetries < 10) { list.innerHTML = "<div class='list-item'>🔍 Scanning… (" + netScanRetries + ")</div>"; setTimeout(netCheckScan, 1000); }
            else list.innerHTML = "<div class='list-item'>⏱️ Scan timeout</div>";
        } else if (d.error) {
            list.innerHTML = "<div class='list-item'>❌ " + d.error + "</div>";
        } else if (!d.networks || !d.networks.length) {
            list.innerHTML = "<div class='list-item'>📶 No networks found</div>";
        } else {
            var h = '';
            d.networks.forEach(function (n) {
                var safe = n.ssid.replace(/'/g, "\\'");
                h += "<div class='list-item' style='cursor:pointer' onclick=\"document.getElementById('net-cSSID').value='" + safe + "';document.getElementById('wifiList').style.display='none'\">";
                h += (n.secure ? '🔒' : '📡') + ' ' + n.ssid + ' <small class="text-muted">(' + n.rssi + ' dBm)</small></div>';
            });
            list.innerHTML = h;
        }
    }).catch(function (e) { var l = document.getElementById('wifiList'); if (l) l.innerHTML = "<div class='list-item'>❌ Error: " + e + "</div>"; });
}

// ============================================================================
// ══ SETTINGS: TIME ══
// ============================================================================
function timeInit() {
    fetch('/api/status').then(function (r) { return r.json(); }).then(function (d) {
        ST = d;
        setEl('time-rtcTime', d.time || '--:--:--');
        setEl('time-boot', d.boot);
        var bak = document.getElementById('bootBak'); if (bak) bak.textContent = '-';
        var status = document.getElementById('rtcStatus');
        if (status) {
            status.className = !d.rtcRunning ? 'alert alert-error' : 'alert alert-success';
            status.innerHTML = !d.rtcRunning ? '❌ RTC Error' : '✅ RTC OK';
        }
        var detail = document.getElementById('rtcDetail');
        if (detail) detail.textContent = 'Protected: ' + (d.rtcProtected ? 'Yes' : 'No') + ' | Running: ' + (d.rtcRunning ? 'Yes' : 'No');
        setChk('time-rtcProt', d.rtcProtected);
        var ntpSt = document.getElementById('ntpStatus');
        if (ntpSt) {
            ntpSt.className = d.wifi === 'client' ? 'alert alert-success' : 'alert alert-warning';
            ntpSt.innerHTML = d.wifi === 'client' ? '✅ WiFi Connected – NTP available' : '⚠️ Not connected (AP mode) – NTP unavailable';
        }
        var dateEl = document.getElementById('date');
        if (dateEl && !dateEl.value) dateEl.value = new Date().toISOString().slice(0, 10);
    });
    fetch('/export_settings').then(function (r) { return r.json(); }).then(function (d) {
        CFG = d; var net = d.network || {};
        setVal('time-ntp', net.ntpServer || 'pool.ntp.org');
        setVal('time-tz', net.timezone !== undefined ? net.timezone : 0);
    });
}


function timeSetManual(ev) {
    ev.preventDefault();
    var form = ev.target; var fd = new FormData(form);
    fetch('/set_time', { method: 'POST', body: fd }).then(function (r) { return r.json(); }).then(function (d) {
        showMsg('time-msg', d.ok ? "<div class='alert alert-success'>✅ Time set!</div>" : "<div class='alert alert-error'>❌ " + (d.error || 'Failed') + "</div>", true);
        if (d.ok) timeInit();
    });
}

function timeSyncNTP(ev) {
    if (ev) ev.preventDefault();
    fetch('/sync_time', { method: 'POST' }).then(function (r) { return r.json(); }).then(function (d) {
        showMsg('time-msg', d.ok ? "<div class='alert alert-success'>✅ Time synced!</div>" : "<div class='alert alert-error'>❌ NTP sync failed</div>", true);
        if (d.ok) timeInit();
    });
}

function timeRtcProtect(ev) {
    if (ev) ev.preventDefault();
    var fd = new FormData();
    var chk = document.getElementById('time-rtcProt'); if (chk && chk.checked) fd.append('protect', '1');
    fetch('/rtc_protect', { method: 'POST', body: fd });
}
function timeFlushLogs() {
    fetch('/flush_logs', { method: 'POST' }).then(function () { showMsg('time-msg', "<div class='alert alert-success'>✅ Log buffer flushed</div>", true); });
}
function timeBackupBoot() {
    fetch('/backup_bootcount', { method: 'POST' }).then(function () { showMsg('time-msg', "<div class='alert alert-success'>✅ Boot count backed up</div>", true); });
}
function timeRestoreBoot() {
    fetch('/restore_bootcount', { method: 'POST' }).then(function (r) { return r.json(); }).then(function (d) {
        showMsg('time-msg', d.ok ? "<div class='alert alert-success'>✅ Restored: " + d.old + " → " + d['new'] + "</div>" : "<div class='alert alert-error'>❌ Restore failed</div>", true);
        if (d.ok) timeInit();
    });
}


// ============================================================================
// ══ SETTINGS: DATALOG ══
// ============================================================================
function dlInit() {
    fetch('/api/filelist?filter=log&recursive=1').then(function (r) { return r.json(); }).then(function (d) {
        var sel = document.getElementById('curFile'); if (!sel) return;
        sel.innerHTML = '';
        var curFile = d.currentFile || '';
        (d.files || []).forEach(function (f) {
            var opt = document.createElement('option'); opt.value = f.path; opt.textContent = f.path;
            sel.appendChild(opt);
        });
        fetch('/export_settings').then(function (r2) { return r2.json(); }).then(function (cfg) {
            CFG = cfg; var dl = cfg.datalog || {};
            if (curFile) sel.value = curFile;
            setVal('dl-prefix', dl.prefix || 'datalog');
            setVal('dl-folder', dl.folder || '');
            setVal('dl-rotation', dl.rotation !== undefined ? dl.rotation : 0);
            var msGrp = document.getElementById('maxSizeGroup');
            if (msGrp) msGrp.style.display = dl.rotation == 4 ? 'block' : 'none';
            setVal('dl-maxSize', dl.maxSizeKB || 500);
            setChk('dl-tsFile', dl.timestampFilename || false);
            setChk('dl-devId', dl.includeDeviceId || false);
            setVal('dl-date', dl.dateFormat !== undefined ? dl.dateFormat : 1);
            setVal('dl-time', dl.timeFormat !== undefined ? dl.timeFormat : 0);
            setVal('dl-end', dl.endFormat !== undefined ? dl.endFormat : 0);
            setVal('dl-boot', dl.includeBootCount ? '1' : '0');
            setVal('dl-vol', dl.volumeFormat !== undefined ? dl.volumeFormat : 0);
            setVal('dl-extra', dl.includeExtraPresses ? '1' : '0');
            setChk('dl-pcEnabled', dl.postCorrectionEnabled);
            var pcF = document.getElementById('pcFields');
            if (pcF) pcF.style.display = dl.postCorrectionEnabled ? 'block' : 'none';
            setVal('dl-pfff', dl.pfToFfThreshold);
            setVal('dl-ffpf', dl.ffToPfThreshold);
            setVal('dl-hold', dl.manualPressThresholdMs);
            dlUpdatePreview();
        });
    });
    dlLoadFiles();
}

function dlLoadFiles() {
    var el = document.getElementById('dl-files'); if (!el) return;
    fetch('/api/filelist?filter=log&recursive=1').then(function (r) { return r.json(); }).then(function (d) {
        var files = d.files || [], curFile = d.currentFile || '';
        if (!files.length) { el.innerHTML = "<div class='list-item text-muted'>No log files</div>"; return; }
        var html = '';
        files.forEach(function (f) {
            var isCur = f.path === curFile;
            html += "<div class='list-item'><span>" +
                (isCur ? "<strong class='text-success'>✓ " : '') +
                f.path + ' <small class="text-muted">(' + fmtBytes(f.size) + ')</small>' +
                (isCur ? '</strong>' : '') +
                "</span><span class='btn-group'>" +
                "<a href='/download?file=" + encodeURIComponent(f.path) + "' class='btn btn-sm btn-secondary'>📥</a>";
            if (!isCur) {
                // data-path prevents quote-in-attribute bug
                html += " <button data-path=\"" + f.path + "\" onclick=\"dlDeleteFile(this.dataset.path)\" class='btn btn-sm btn-danger'>🗑️</button>";
            }
            html += '</span></div>';
        });
        el.innerHTML = html;
    });
}


// Uses storage=internal explicitly — matches original failsafe fix
function dlDeleteFile(path) {
    if (!confirm('Delete ' + path + '?')) return;
    fetch('/delete?path=' + encodeURIComponent(path) + '&storage=internal').then(function () { dlLoadFiles(); });
}

// Matches original: function updatePreview()
function dlUpdatePreview() {
    var p = [], d = new Date();
    var df = getVal('dl-date'), tf = getVal('dl-time'), ef = getVal('dl-end');
    var dd = String(d.getDate()).padStart(2, '0'), mm = String(d.getMonth() + 1).padStart(2, '0'), yy = d.getFullYear();
    var hh = String(d.getHours()).padStart(2, '0'), mi = String(d.getMinutes()).padStart(2, '0'), ss = String(d.getSeconds()).padStart(2, '0');

    if (df === '1') p.push(dd + '/' + mm + '/' + yy);
    else if (df === '2') p.push(mm + '/' + dd + '/' + yy);
    else if (df === '3') p.push(yy + '-' + mm + '-' + dd);
    else if (df === '4') p.push(dd + '.' + mm + '.' + yy);

    var tStr = '';
    if (tf === '0') tStr = hh + ':' + mi + ':' + ss;
    else if (tf === '1') tStr = hh + ':' + mi;
    else { var h12 = d.getHours() % 12 || 12; tStr = h12 + ':' + mi + ':' + ss + (d.getHours() < 12 ? 'AM' : 'PM'); }
    p.push(tStr);
    if (ef === '0') p.push(tStr);
    else if (ef === '1') p.push('45s');
    if (getVal('dl-boot') === '1') p.push('#:1234');
    p.push('FF_BTN');
    var vf = getVal('dl-vol');
    if (vf === '0') p.push('L:2,50');
    else if (vf === '1') p.push('L:2.50');
    else if (vf === '2') p.push('2.50');
    if (getVal('dl-extra') === '1') { p.push('FF0'); p.push('PF1'); }
    setEl('dl-preview', p.join('|'));
}

// ============================================================================
// ══ SETTINGS: IMPORT / EXPORT ══
// ============================================================================
function settingsImport() {
    var file = document.getElementById('importFile');
    if (!file || !file.files.length) return;
    var fd = new FormData(); fd.append('settings', file.files[0]);
    var prog = document.getElementById('importProg'), bar = document.getElementById('importBar'), pct = document.getElementById('importPct');
    if (prog) prog.style.display = 'block';
    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function (ev) { if (ev.lengthComputable) { var p = Math.round(ev.loaded / ev.total * 100); if (bar) bar.style.width = p + '%'; if (pct) pct.textContent = p + '%'; } };
    xhr.onload = function () { if (xhr.status === 200) { alert('Settings imported!'); location.reload(); } else { alert('Import failed: ' + xhr.responseText); if (prog) prog.style.display = 'none'; } };
    xhr.onerror = function () { alert('Import failed'); if (prog) prog.style.display = 'none'; };
    xhr.open('POST', '/import_settings'); xhr.send(fd);
}

// ============================================================================
// ══ OTA UPDATE ══
// ============================================================================
function otaInit() {
    fetch('/api/status').then(function (r) { return r.json(); }).then(function (d) {
        ST = d;
        setEl('ota-currentVer', d.version || '--');
        setEl('fwDevice', d.chip || '--');
        setEl('fwHeap', fmtBytes(d.heap));
        setEl('fwStorage', fmtBytes(d.freeSketch));
    });
}

function hwToggleSD() { var sd = document.getElementById('sdPins'), st = document.getElementById('hw-storage'); if (sd && st) sd.style.display = st.value === '1' ? 'block' : 'none'; }

function otaFileSelected() {
    var fileInput = document.getElementById('fwFile');
    var uploadBtn = document.getElementById('otaUploadBtn');
    var fileInfo = document.getElementById('otaFileInfo');
    var file = fileInput.files[0];

    if (!file) {
        uploadBtn.disabled = true;
        fileInfo.style.display = 'none';
        return;
    }

    var errors = [];
    if (!file.name.toLowerCase().endsWith('.bin')) errors.push('File must be a .bin file');
    if (file.size < 10000) errors.push('File too small (min 10KB)');

    if (errors.length > 0) {
        fileInfo.innerHTML = '<span style="color:#c00">❌ ' + errors.join('<br>') + '</span>';
        fileInfo.style.display = 'block';
        uploadBtn.disabled = true;
        return;
    }

    var reader = new FileReader();
    reader.onload = function (e) {
        var arr = new Uint8Array(e.target.result);
        if (arr[0] !== 0xE9) {
            fileInfo.innerHTML = '<span style="color:#c00">❌ Invalid firmware file (wrong magic byte)</span>';
            fileInfo.style.display = 'block';
            uploadBtn.disabled = true;
            return;
        }
        fileInfo.innerHTML = '<span style="color:#080">✅ ' + file.name + ' (' + Math.round(file.size / 1024) + 'KB)</span>';
        fileInfo.style.display = 'block';
        uploadBtn.disabled = false;
    };
    reader.readAsArrayBuffer(file.slice(0, 4));
}

function otaShowPopup(icon, title, msg, showProgress, showClose) {
    var p = document.getElementById('popup');
    if (p) p.style.display = 'flex';
    setEl('popupIcon', icon);
    setEl('popupTitle', title);
    var elMsg = document.getElementById('popupMsg');
    if (elMsg) elMsg.innerHTML = msg;
    var elProg = document.getElementById('popupProgress');
    if (elProg) elProg.style.display = showProgress ? 'block' : 'none';
    var elClose = document.getElementById('popupClose');
    if (elClose) elClose.style.display = showClose ? 'inline-block' : 'none';
}

function otaUpdatePopupProgress(pct, text) {
    var bar = document.getElementById('popupBar');
    if (bar) bar.style.width = pct + '%';
    setEl('popupCounter', text);
}

function otaUpload() {
    var fileInput = document.getElementById('fwFile');
    var uploadBtn = document.getElementById('otaUploadBtn');
    var progressDiv = document.getElementById('otaProgress');
    var progressBar = document.getElementById('otaProgressBar');
    var progressText = document.getElementById('otaProgressText');

    if (!fileInput || !fileInput.files.length) return;
    var file = fileInput.files[0];

    uploadBtn.disabled = true;
    fileInput.disabled = true;
    otaShowPopup('📤', 'Uploading...', 'Please wait while firmware is being uploaded.', true, false);

    if (progressDiv) progressDiv.style.display = 'block';

    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function (e) {
        if (e.lengthComputable) {
            var pct = Math.round(e.loaded / e.total * 100);
            if (progressBar) progressBar.style.width = pct + '%';
            if (progressText) progressText.textContent = pct + '%';
            otaUpdatePopupProgress(pct, Math.round(e.loaded / 1024) + ' / ' + Math.round(e.total / 1024) + ' KB');
        }
    };
    xhr.onload = function () {
        if (progressDiv) progressDiv.style.display = 'none';

        if (xhr.status === 200) {
            try {
                var resp = JSON.parse(xhr.responseText);
                if (resp.success) {
                    var seconds = 5;
                    var tick = function () {
                        otaShowPopup('✅', 'Update Complete!', 'Device will restart...<br>Redirecting in <strong>' + seconds + '</strong> seconds', true, false);
                        otaUpdatePopupProgress((5 - seconds) * 20, '');
                        if (seconds <= 0) {
                            window.location.href = '/';
                            window.location.reload();
                        } else {
                            seconds--;
                            setTimeout(tick, 1000);
                        }
                    };
                    tick();
                } else {
                    otaShowPopup('❌', 'Update Failed', resp.message || 'Unknown error', false, true);
                    uploadBtn.disabled = false;
                    fileInput.disabled = false;
                }
            } catch (e) {
                // Failsafe if not JSON
                var seconds = 5;
                var tick = function () {
                    otaShowPopup('✅', 'Update sent', 'Device is restarting...<br>Redirecting in <strong>' + seconds + '</strong> seconds', true, false);
                    otaUpdatePopupProgress((5 - seconds) * 20, '');
                    if (seconds <= 0) {
                        window.location.href = '/';
                        window.location.reload();
                    } else {
                        seconds--;
                        setTimeout(tick, 1000);
                    }
                };
                tick();
            }
        } else {
            otaShowPopup('❌', 'Upload Error', 'Server returned: ' + xhr.statusText, false, true);
            uploadBtn.disabled = false;
            fileInput.disabled = false;
        }
    };
    xhr.onerror = function () {
        if (progressDiv) progressDiv.style.display = 'none';
        otaShowPopup('❌', 'Connection Error', 'Could not connect to device', false, true);
        uploadBtn.disabled = false;
        fileInput.disabled = false;
    };

    var formData = new FormData();
    formData.append('firmware', file);
    xhr.open('POST', '/do_update');
    xhr.send(formData);
}

function dlToggleMaxSize() { var mg = document.getElementById('maxSizeGroup'), rot = document.getElementById('dl-rotation'); if (mg && rot) mg.style.display = rot.value === '4' ? 'block' : 'none'; }
function closePopup() { var p = document.getElementById('popup'); if (p) p.style.display = 'none'; }

// ============================================================================
// PLATFORM CONFIG  (platform_config.json management)
// ============================================================================
var PCFG = null;  // cached platform config object

function pcfgLoad(cb) {
    fetch('/api/platform_config')
        .then(function(r){ return r.ok ? r.json() : null; })
        .then(function(d){ PCFG = d || { version:1, mode:'legacy', sensors:[], aggregation:{}, export:{}, storage:{} }; if(cb) cb(PCFG); })
        .catch(function(){ PCFG = { version:1, mode:'legacy', sensors:[], aggregation:{}, export:{}, storage:{} }; if(cb) cb(PCFG); });
}

function pcfgSave(obj, cb) {
    var body = JSON.stringify(obj, null, 2);
    fetch('/save_platform', {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: body
    }).then(function(r){ return r.json(); })
      .then(function(d){ if(cb) cb(d.ok, d.error || ''); })
      .catch(function(e){ if(cb) cb(false, String(e)); });
}

// ============================================================================
// AGGREGATION SETTINGS  (in Datalog page)
// ============================================================================
// Load stored aggregation prefs from localStorage (client-side only)
(function aggSettingsInit() {
    window.addEventListener('DOMContentLoaded', function() {
        var b = localStorage.getItem('agg_bucket') || '5m';
        var m = localStorage.getItem('agg_mode')   || 'lttb';
        var l = parseInt(localStorage.getItem('agg_limit') || '500', 10);
        var eb = document.getElementById('agg-bucket');
        var em = document.getElementById('agg-mode');
        var el = document.getElementById('agg-limit');
        if(eb) eb.value = b;
        if(em) em.value = m;
        if(el) el.value = l;
    });
})();

function aggSettingsSave() {
    var b = (document.getElementById('agg-bucket')||{}).value || '5m';
    var m = (document.getElementById('agg-mode')||{}).value   || 'lttb';
    var l = parseInt((document.getElementById('agg-limit')||{}).value || '500', 10);
    localStorage.setItem('agg_bucket', b);
    localStorage.setItem('agg_mode',   m);
    localStorage.setItem('agg_limit',  String(l));
    var msg = document.getElementById('agg-msg');
    if(msg){ msg.textContent = '✅ Saved (used when viewing sensor charts)'; msg.style.color='green'; }
}

function aggGetPrefs() {
    return {
        bucket: localStorage.getItem('agg_bucket') || '5m',
        mode:   localStorage.getItem('agg_mode')   || 'lttb',
        limit:  parseInt(localStorage.getItem('agg_limit') || '500', 10)
    };
}

// ============================================================================
// SENSORS PAGE
// ============================================================================
var sensorChart = null;

function sensorsLoad() {
    var grid = document.getElementById('sensors-grid');
    var msg  = document.getElementById('sensors-msg');
    if(msg)  msg.textContent = 'Loading…';
    if(grid) grid.innerHTML  = '';

    fetch('/api/sensors')
        .then(function(r){ return r.ok ? r.json() : null; })
        .then(function(d) {
            if(!d || !d.sensors || d.sensors.length === 0) {
                if(msg) msg.textContent = 'No sensors registered. Set mode to Continuous in Core Logic settings and configure sensors.';
                return;
            }
            if(msg) msg.textContent = '';
            if(grid) {
                grid.innerHTML = d.sensors.map(function(s) {
                    var statusClass = s.status === 'ok' ? 'badge-ok' : (s.status === 'disabled' ? 'badge-dis' : 'badge-err');
                    var ts = s.last_read_ts ? new Date(s.last_read_ts * 1000).toLocaleTimeString() : '—';
                    return '<div class="sensor-card">'
                        + '<div class="sensor-card-header">'
                        +   '<span class="sensor-name">' + s.name + '</span>'
                        +   '<span class="badge ' + statusClass + '">' + s.status + '</span>'
                        + '</div>'
                        + '<div class="sensor-meta">'
                        +   '<span>ID: <code>' + s.id + '</code></span>'
                        +   '<span>Type: <code>' + s.type + '</code></span>'
                        +   '<span>Last: ' + ts + '</span>'
                        + '</div>'
                        + '<div class="sensor-metrics">'
                        +   (s.metrics || []).map(function(m){ return '<span class="metric-tag">' + m + '</span>'; }).join('')
                        + '</div>'
                        + '</div>';
                }).join('');
            }

            // Populate chart sensor selector
            var sel = document.getElementById('sc-sensor');
            if(sel) {
                sel.innerHTML = '<option value="">— select sensor —</option>'
                    + d.sensors.map(function(s){
                        return '<option value="' + s.id + '">' + s.name + '</option>';
                      }).join('');
            }
        })
        .catch(function(e) {
            if(msg) msg.textContent = 'Failed to load sensors: ' + e;
        });
}

function sensorChartLoad() {
    var sid    = (document.getElementById('sc-sensor')||{}).value;
    var metric = (document.getElementById('sc-metric')||{}).value;
    var agg    = (document.getElementById('sc-agg')||{}).value   || '5m';
    var mode   = (document.getElementById('sc-mode')||{}).value  || 'lttb';
    var range  = parseInt((document.getElementById('sc-range')||{}).value || '86400', 10);
    var msg    = document.getElementById('sc-msg');

    if(!sid) return;

    // Update metric dropdown when sensor changes
    var metricSel = document.getElementById('sc-metric');
    if(metricSel && !metric) {
        // load first available metric from API sensors cache
        if(msg) msg.textContent = 'Select a metric…';
        return;
    }

    var now  = Math.floor(Date.now() / 1000);
    var from = now - range;
    var url  = '/api/data?sensor=' + encodeURIComponent(sid)
             + '&metric=' + encodeURIComponent(metric)
             + '&from=' + from + '&to=' + now
             + '&agg=' + agg + '&mode=' + mode + '&limit=500';

    if(msg) msg.textContent = 'Loading…';

    fetch(url)
        .then(function(r){ return r.ok ? r.json() : null; })
        .then(function(d) {
            if(!d || !d.data || d.data.length === 0) {
                if(msg) msg.textContent = 'No data for selected period.';
                return;
            }
            if(msg) msg.textContent = d.count + ' points · ' + d.agg + ' · ' + d.mode;

            var labels = d.data.map(function(pt){ return new Date(pt.ts * 1000).toLocaleTimeString(); });
            var values = d.data.map(function(pt){ return pt.v; });

            var ctx = document.getElementById('sensorChart');
            if(!ctx) return;

            if(sensorChart) sensorChart.destroy();
            sensorChart = new Chart(ctx, {
                type: 'line',
                data: {
                    labels: labels,
                    datasets: [{
                        label: sid + ' / ' + metric,
                        data: values,
                        borderColor: '#275673',
                        backgroundColor: 'rgba(39,86,115,0.08)',
                        borderWidth: 2,
                        pointRadius: d.data.length > 100 ? 0 : 3,
                        tension: 0.3,
                        fill: true
                    }]
                },
                options: {
                    responsive: true,
                    animation: false,
                    plugins: {
                        legend: { display: true },
                        tooltip: { mode: 'index', intersect: false }
                    },
                    scales: {
                        x: { ticks: { maxTicksLimit: 10, maxRotation: 45 } },
                        y: { beginAtZero: false }
                    }
                }
            });
        })
        .catch(function(e){
            if(msg) msg.textContent = 'Error: ' + e;
        });
}

// Update metric selector when sensor changes
document.addEventListener('DOMContentLoaded', function(){
    var sensorSel = document.getElementById('sc-sensor');
    if(!sensorSel) return;
    sensorSel.addEventListener('change', function(){
        var sid = this.value;
        var metricSel = document.getElementById('sc-metric');
        if(!metricSel || !sid) return;
        // Fetch sensor list to get metrics
        fetch('/api/sensors').then(function(r){ return r.json(); }).then(function(d){
            var s = (d.sensors || []).find(function(s){ return s.id === sid; });
            if(s && s.metrics) {
                metricSel.innerHTML = s.metrics.map(function(m){
                    return '<option value="' + m + '">' + m + '</option>';
                }).join('');
            }
        }).catch(function(){});
    });
});

// ============================================================================
// CORE LOGIC PAGE  (platform_config.json editor)
// ============================================================================
var CL_SENSOR_TYPES = [
    // I2C
    { value: 'bme280',        label: 'BME280 (temp/humidity/pressure)',        iface: 'i2c'     },
    { value: 'bme688',        label: 'BME688 (temp/humidity/pressure/gas)',    iface: 'i2c'     },
    { value: 'ens160',        label: 'ENS160 (TVOC/eCO2)',                     iface: 'i2c'     },
    { value: 'sgp30',         label: 'SGP30 (TVOC/eCO2)',                      iface: 'i2c'     },
    { value: 'scd4x',         label: 'SCD40/41 (CO2/temp/humidity)',           iface: 'i2c'     },
    { value: 'bh1750',        label: 'BH1750 (ambient light)',                 iface: 'i2c'     },
    { value: 'veml6075',      label: 'VEML6075 (UV index A+B)',                iface: 'i2c'     },
    { value: 'veml7700',      label: 'VEML7700 (high-accuracy lux)',           iface: 'i2c'     },
    // UART
    { value: 'sds011',        label: 'SDS011 (PM2.5/PM10)',                    iface: 'uart'    },
    { value: 'pms5003',       label: 'PMS5003 (PM1/PM2.5/PM10)',               iface: 'uart'    },
    // Pulse
    { value: 'yfs201',        label: 'YF-S201/YF-S403 (water flow)',           iface: 'pulse'   },
    { value: 'rain',          label: 'Rain gauge (tipping bucket)',             iface: 'pulse'   },
    { value: 'wind',          label: 'Wind speed (anemometer)',                 iface: 'pulse'   },
    // Analog (ADC)
    { value: 'zmpt101b',      label: 'ZMPT101B (AC voltage)',                  iface: 'analog'  },
    { value: 'zmct103c',      label: 'ZMCT103C (AC current)',                  iface: 'analog'  },
    { value: 'soil_moisture', label: 'Soil moisture (capacitive ADC)',         iface: 'analog'  },
    // OneWire
    { value: 'ds18b20',       label: 'DS18B20 (1-Wire temperature probe)',     iface: 'onewire' },
    // GPIO
    { value: 'hcsr04',        label: 'HC-SR04 (ultrasonic distance)',          iface: 'gpio'    }
];

// XIAO ESP32-C3 — exposed GPIO pins with board labels.
// Only ADC-capable pins (GPIO 0-5) should be used for analog sensors.
var CL_GPIO_PINS = [
    { gpio: 0,  label: 'GPIO0  — D0/A0',      adc: true  },
    { gpio: 1,  label: 'GPIO1  — D1/A1',      adc: true  },
    { gpio: 2,  label: 'GPIO2  — D2/A2',      adc: true  },
    { gpio: 3,  label: 'GPIO3  — D3/A3',      adc: true  },
    { gpio: 4,  label: 'GPIO4  — D4/A4',      adc: true  },
    { gpio: 5,  label: 'GPIO5  — D5/A5',      adc: true  },
    { gpio: 6,  label: 'GPIO6  — D4/SDA',     adc: false },
    { gpio: 7,  label: 'GPIO7  — D5/SCL',     adc: false },
    { gpio: 8,  label: 'GPIO8  — D8/SCK',     adc: false },
    { gpio: 9,  label: 'GPIO9  — D9/MISO',    adc: false },
    { gpio: 10, label: 'GPIO10 — D10/MOSI',   adc: false },
    { gpio: 20, label: 'GPIO20 — D7/RX',      adc: false },
    { gpio: 21, label: 'GPIO21 — D6/TX',      adc: false }
];

// Known system/reserved pins on XIAO ESP32-C3 default config — shown as warnings.
var CL_SYSTEM_PINS = {
    2:  'WiFi trigger',
    3:  'Wakeup FF btn',
    4:  'Wakeup PF btn',
    5:  'RTC CE',
    6:  'RTC IO / SDA',
    7:  'RTC SCLK / SCL',
    10: 'SD CS',
    21: 'Flow sensor'
};

// Returns a map { gpioNum: [sensorId, ...] } of pins in use,
// excluding the sensor at excludeIdx (so editing a sensor doesn't block its own pins).
function clGetUsedPins(excludeIdx) {
    var used = {};
    if(!PCFG || !PCFG.sensors) return used;
    PCFG.sensors.forEach(function(s, i) {
        if(i === excludeIdx) return;
        [s.sda, s.scl, s.pin, s.uart_rx, s.uart_tx].forEach(function(p) {
            if(p !== undefined && p !== null && p >= 0) {
                if(!used[p]) used[p] = [];
                used[p].push(s.id || s.type);
            }
        });
    });
    return used;
}

// Single source of truth for sleep-config defaults (mirrors Logger.ino initial values).
var CL_SLEEP_DEFAULTS = {
    cont_idle_timeout_ms:    300000,
    cont_idle_cpu_mhz:       80,
    cont_modem_sleep:        true,
    hyb_idle_before_sleep_ms: 120000,
    hyb_sleep_duration_ms:   60000,
    hyb_active_window_ms:    30000
};

function clLoad() {
    var msg = document.getElementById('cl-msg');
    if(msg) { msg.textContent = ''; msg.className = ''; }
    pcfgLoad(function(cfg) {
        // Mode
        var modeEl = document.getElementById('cl-mode');
        if(modeEl) modeEl.value = cfg.mode || 'legacy';

        // Aggregation defaults
        var agg = cfg.aggregation || {};
        var amEl = document.getElementById('cl-aggmode');   if(amEl) amEl.value = agg.default_mode || 'lttb';
        var abEl = document.getElementById('cl-aggbucket'); if(abEl) abEl.value = String(agg.default_bucket_min || 5);
        var mpEl = document.getElementById('cl-maxpoints'); if(mpEl) mpEl.value = agg.max_points || 500;
        var rtEl = document.getElementById('cl-retention'); if(rtEl) rtEl.value = agg.raw_retention_days || 7;

        // Export quick-enables
        var exp = cfg.export || {};
        var mqttEl = document.getElementById('cl-exp-mqtt');if(mqttEl) mqttEl.checked = !!(exp.mqtt && exp.mqtt.enabled);
        var httpEl = document.getElementById('cl-exp-http');if(httpEl) httpEl.checked = !!(exp.http && exp.http.enabled);
        var scEl   = document.getElementById('cl-exp-sc');  if(scEl)   scEl.checked   = !!(exp.sensor_community && exp.sensor_community.enabled);
        var osmEl  = document.getElementById('cl-exp-osm'); if(osmEl)  osmEl.checked  = !!(exp.opensensemap && exp.opensensemap.enabled);

        // Sleep settings
        var sl   = cfg.sleep || {};
        var cont = sl.continuous || {};
        var hyb  = sl.hybrid    || {};
        var ciEl = document.getElementById('cl-cont-idle');  if(ciEl) ciEl.value   = cont.idle_timeout_ms      || CL_SLEEP_DEFAULTS.cont_idle_timeout_ms;
        var ccEl = document.getElementById('cl-cont-cpu');   if(ccEl) ccEl.value   = String(cont.idle_cpu_mhz  || CL_SLEEP_DEFAULTS.cont_idle_cpu_mhz);
        var cmEl = document.getElementById('cl-cont-modem'); if(cmEl) cmEl.checked = cont.modem_sleep !== false;
        var hiEl = document.getElementById('cl-hyb-idle');   if(hiEl) hiEl.value   = hyb.idle_before_sleep_ms  || CL_SLEEP_DEFAULTS.hyb_idle_before_sleep_ms;
        var hsEl = document.getElementById('cl-hyb-sleep');  if(hsEl) hsEl.value   = hyb.sleep_duration_ms     || CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms;
        var haEl = document.getElementById('cl-hyb-active'); if(haEl) haEl.value   = hyb.active_window_ms      || CL_SLEEP_DEFAULTS.hyb_active_window_ms;

        // Show/hide sleep panel according to selected mode
        clUpdateSleepPanel();

        // Sensor list
        clRenderSensors(cfg.sensors || []);
    });
}

function clRenderSensors(sensors) {
    var list = document.getElementById('cl-sensors-list');
    if(!list) return;
    if(!sensors || sensors.length === 0) {
        list.innerHTML = '<p class="text-muted" style="padding:1rem">No sensors configured. Click <strong>+ Add Sensor</strong> to begin.</p>';
        return;
    }
    var n = sensors.length;
    list.innerHTML = sensors.map(function(s, i) {
        var typeLabel = (CL_SENSOR_TYPES.find(function(t){ return t.value === s.type; }) || {}).label || s.type;
        var pinInfo   = s.interface === 'i2c'     ? 'SDA:' + (s.sda||'?') + ' SCL:' + (s.scl||'?')
                      : s.interface === 'uart'    ? 'RX:' + (s.uart_rx||'?')
                      : s.interface === 'pulse'   ? 'Pin:' + (s.pin||'?')
                      : s.interface === 'analog'  ? 'ADC:' + (s.pin||'?')
                      : s.interface === 'onewire' ? 'Pin:' + (s.pin||'?')
                      : s.interface === 'gpio'    ? ('trig:' + (s.trig_pin||'?') + ' echo:' + (s.echo_pin||'?'))
                      : '';
        return '<div class="sensor-list-row" style="display:flex;align-items:center;gap:6px;padding:8px 16px;border-bottom:1px solid var(--border)">'
            + '<label style="display:flex;align-items:center;gap:6px;cursor:pointer;flex:0 0 auto">'
            +   '<input type="checkbox" onchange="clToggleSensor(' + i + ',this.checked)"'
            +   (s.enabled ? ' checked' : '') + '>'
            +   '<span style="font-size:.75rem;color:var(--text-muted)">' + (s.enabled ? 'ON' : 'OFF') + '</span>'
            + '</label>'
            + '<div style="flex:1;min-width:0">'
            +   '<div style="font-weight:600">' + (s.id || s.type) + '</div>'
            +   '<div style="font-size:.78rem;color:var(--text-muted)">' + typeLabel + ' · ' + pinInfo + '</div>'
            + '</div>'
            + '<div style="display:flex;gap:3px;flex-shrink:0">'
            +   '<button type="button" class="btn btn-sm btn-secondary" onclick="clMoveSensor(' + i + ',-1)" title="Move up"'    + (i === 0   ? ' disabled' : '') + '>▲</button>'
            +   '<button type="button" class="btn btn-sm btn-secondary" onclick="clMoveSensor(' + i + ',1)"  title="Move down"'  + (i === n-1 ? ' disabled' : '') + '>▼</button>'
            +   '<button type="button" class="btn btn-sm btn-secondary" onclick="clDupSensor('  + i + ')"   title="Duplicate">⧉</button>'
            +   '<button type="button" class="btn btn-sm btn-secondary" onclick="clPingSensor(\'' + (s.id||s.type) + '\')" title="Test / read now">🔍</button>'
            +   '<button type="button" class="btn btn-sm btn-secondary" onclick="clEditSensor(' + i + ')"   title="Edit">✏️</button>'
            +   '<button type="button" class="btn btn-sm btn-danger"    onclick="clRemoveSensor(' + i + ')" title="Remove">🗑</button>'
            + '</div>'
        + '</div>';
    }).join('');
}

function clToggleSensor(idx, enabled) {
    if(!PCFG || !PCFG.sensors) return;
    PCFG.sensors[idx].enabled = enabled;
}

function clRemoveSensor(idx) {
    if(!PCFG || !PCFG.sensors) return;
    if(!confirm('Remove sensor "' + (PCFG.sensors[idx].id || PCFG.sensors[idx].type) + '"?')) return;
    PCFG.sensors.splice(idx, 1);
    clRenderSensors(PCFG.sensors);
}

function clMoveSensor(idx, dir) {
    if(!PCFG || !PCFG.sensors) return;
    var j = idx + dir;
    if(j < 0 || j >= PCFG.sensors.length) return;
    var tmp = PCFG.sensors[idx];
    PCFG.sensors[idx] = PCFG.sensors[j];
    PCFG.sensors[j] = tmp;
    clRenderSensors(PCFG.sensors);
}

function clDupSensor(idx) {
    if(!PCFG || !PCFG.sensors) return;
    var copy = JSON.parse(JSON.stringify(PCFG.sensors[idx]));
    copy.id = copy.id + '_copy';
    PCFG.sensors.splice(idx + 1, 0, copy);
    clRenderSensors(PCFG.sensors);
}

// ============================================================================
// SENSOR ADD POPUP
// ============================================================================
var SAP_selectedType = null;

function clAddSensor() {
    if(!PCFG) PCFG = { sensors: [] };
    if(!PCFG.sensors) PCFG.sensors = [];
    SAP_selectedType = null;

    var grid = document.getElementById('sap-type-grid');
    if(grid) {
        grid.innerHTML = CL_SENSOR_TYPES.map(function(t) {
            // Extract the interface tag and description separately for nicer display
            var ifaceBadge = '<span style="font-size:.7rem;background:var(--bg);border-radius:4px;padding:1px 5px;opacity:.8">'
                           + t.iface + '</span>';
            return '<button type="button" id="sap-btn-' + t.value + '" class="btn btn-secondary"'
                + ' onclick="sapSelectType(\'' + t.value + '\')"'
                + ' style="text-align:left;padding:.5rem .6rem;display:flex;flex-direction:column;gap:2px;height:auto">'
                + '<span style="font-weight:700;font-size:.88rem">' + t.value + ' ' + ifaceBadge + '</span>'
                + '<span style="font-size:.75rem;opacity:.75;white-space:normal">' + t.label + '</span>'
                + '</button>';
        }).join('');
    }

    var idEl = document.getElementById('sap-id');
    if(idEl) idEl.value = '';

    document.getElementById('sensor-add-popup').style.display = 'flex';
}

function sapSelectType(type) {
    SAP_selectedType = type;
    CL_SENSOR_TYPES.forEach(function(t) {
        var btn = document.getElementById('sap-btn-' + t.value);
        if(btn) {
            btn.className = 'btn ' + (t.value === type ? 'btn-primary' : 'btn-secondary');
        }
    });
    var idEl = document.getElementById('sap-id');
    if(idEl && !idEl.value.trim()) {
        var count = PCFG && PCFG.sensors ? PCFG.sensors.length + 1 : 1;
        idEl.value = type + '_' + count;
    }
}

function sapClose() {
    document.getElementById('sensor-add-popup').style.display = 'none';
    SAP_selectedType = null;
}

function sapConfirm() {
    if(!SAP_selectedType) { alert('Please select a sensor type first.'); return; }
    var id = (document.getElementById('sap-id').value || '').trim();
    if(!id) { alert('Please enter a sensor ID.'); return; }
    var info = CL_SENSOR_TYPES.find(function(t){ return t.value === SAP_selectedType; });
    if(!info) return;
    var newS = { id: id, type: SAP_selectedType, enabled: true, interface: info.iface };
    if(info.iface === 'i2c')    { newS.sda = 6; newS.scl = 7; newS.read_interval_ms = 10000; }
    if(info.iface === 'uart')   { newS.uart_rx = 20; newS.uart_tx = -1; newS.baud = 9600; }
    if(info.iface === 'pulse')  { newS.pin = 9; newS.read_interval_ms = 5000; }
    if(info.iface === 'analog') {
        newS.pin = 0; newS.adc_samples = 64; newS.read_interval_ms = 1000;
        if(SAP_selectedType === 'zmpt101b')      { newS.voltage_factor = 1.0; }
        if(SAP_selectedType === 'zmct103c')      { newS.current_factor = 1.0; }
        if(SAP_selectedType === 'soil_moisture') { newS.dry_value = 3300; newS.wet_value = 1500; newS.calibration = { moisture: { offset: 0.0, scale: 1.0 } }; }
    }
    if(info.iface === 'onewire') { newS.pin = 2; newS.resolution = 12; newS.read_interval_ms = 5000; newS.calibration = { temperature: { offset: 0.0, scale: 1.0 } }; }
    if(info.iface === 'gpio' && SAP_selectedType === 'hcsr04') { newS.trig_pin = 5; newS.echo_pin = 4; newS.max_distance_cm = 400; newS.read_interval_ms = 1000; newS.calibration = { distance: { offset: 0.0, scale: 1.0 } }; }
    PCFG.sensors.push(newS);
    sapClose();
    clRenderSensors(PCFG.sensors);
    // Open edit popup for the new sensor so user can fine-tune it immediately
    clEditSensor(PCFG.sensors.length - 1);
}

// ============================================================================
// SENSOR EDIT POPUP
// ============================================================================
var SEP_idx = null;

function _sepEsc(str) {
    return String(str === undefined || str === null ? '' : str)
        .replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function _sepRow(label, inputHtml, hint) {
    return '<div class="form-group" style="margin-bottom:.7rem">'
        + '<label class="form-label" style="margin-bottom:.3rem">' + label + '</label>'
        + inputHtml
        + (hint ? '<p class="form-hint" style="margin-top:.2rem">' + hint + '</p>' : '')
        + '</div>';
}

function _sepNumInput(id, val, min, max, step) {
    return '<input type="number" id="' + id + '" class="form-input" value="' + _sepEsc(val) + '"'
        + (min !== undefined ? ' min="' + min + '"' : '')
        + (max !== undefined ? ' max="' + max + '"' : '')
        + (step !== undefined ? ' step="' + step + '"' : '')
        + '>';
}

// Build a GPIO pin <select>.
// allowNone = true adds a "— Not connected —" option for value -1.
// adcOnly   = true hides non-ADC pins (for analog sensors).
// usedPins  = map returned by clGetUsedPins(). Used pins are shown disabled + labelled.
function _sepPinSelect(elemId, currentVal, usedPins, allowNone, adcOnly) {
    var opts = '';
    if(allowNone) {
        var selNone = (currentVal === -1 || currentVal === undefined || currentVal === null) ? ' selected' : '';
        opts += '<option value="-1"' + selNone + '>— Not connected —</option>';
    }
    CL_GPIO_PINS.forEach(function(p) {
        if(adcOnly && !p.adc) return;
        var usedBy    = usedPins ? usedPins[p.gpio] : null;
        var sysLabel  = CL_SYSTEM_PINS[p.gpio];
        var isSelected = (Number(currentVal) === p.gpio);
        var isDisabled = usedBy && !isSelected;
        var suffix = '';
        if(usedBy)   suffix += '  ✗ ' + usedBy.join(', ');
        if(sysLabel) suffix += '  ⚠ ' + sysLabel;
        opts += '<option value="' + p.gpio + '"'
            + (isSelected  ? ' selected' : '')
            + (isDisabled  ? ' disabled' : '')
            + '>' + p.label + suffix + '</option>';
    });
    return '<select id="' + elemId + '" class="form-input form-select">' + opts + '</select>';
}

function clEditSensor(idx) {
    if(!PCFG || !PCFG.sensors) return;
    SEP_idx = idx;
    var s     = PCFG.sensors[idx];
    var iface = s.interface || 'i2c';
    var typeInfo  = CL_SENSOR_TYPES.find(function(t){ return t.value === s.type; }) || {};
    var usedPins  = clGetUsedPins(idx);
    var html = '';

    // ── Basic fields ──────────────────────────────────────────────────────────
    html += _sepRow('Sensor ID',
        '<input type="text" id="sep-id" class="form-input" value="' + _sepEsc(s.id || '') + '">');
    html += _sepRow('Type',
        '<input type="text" class="form-input" value="' + _sepEsc((typeInfo.label || s.type) + ' [' + iface + ']') + '"'
        + ' readonly style="background:var(--bg);cursor:default;opacity:.75">');
    html += '<div class="form-group" style="margin-bottom:.7rem">'
        + '<label style="display:flex;align-items:center;gap:.5rem;cursor:pointer">'
        + '<input type="checkbox" id="sep-enabled"' + (s.enabled ? ' checked' : '') + '>'
        + '<span class="form-label" style="margin:0">Enabled</span>'
        + '</label></div>';

    // ── Interface-specific fields ─────────────────────────────────────────────
    if(iface === 'i2c') {
        html += '<div class="form-row" style="gap:.5rem">';
        html += '<div>' + _sepRow('SDA Pin', _sepPinSelect('sep-sda', s.sda !== undefined ? s.sda : 6, usedPins, false, false)) + '</div>';
        html += '<div>' + _sepRow('SCL Pin', _sepPinSelect('sep-scl', s.scl !== undefined ? s.scl : 7, usedPins, false, false)) + '</div>';
        html += '</div>';
        html += _sepRow('Read Interval (ms)', _sepNumInput('sep-interval', s.read_interval_ms || 10000, 100, undefined, 100));
        if(s.address !== undefined) {
            html += _sepRow('I²C Address (hex/dec)',
                '<input type="text" id="sep-address" class="form-input" value="' + _sepEsc(s.address) + '">',
                'e.g. 0x76 or 118');
        }
        // BME688-specific
        if(s.type === 'bme688') {
            html += '<div class="form-row" style="gap:.5rem">';
            html += '<div>' + _sepRow('Heater Temp (°C)', _sepNumInput('sep-heater_temp', s.heater_temp || 320, 100, 400)) + '</div>';
            html += '<div>' + _sepRow('Heater Duration (ms)', _sepNumInput('sep-heater_duration_ms', s.heater_duration_ms || 150, 1, 4032)) + '</div>';
            html += '</div>';
        }
        // BH1750-specific
        if(s.type === 'bh1750') {
            html += _sepRow('Measurement Mode',
                '<select id="sep-mode" class="form-input form-select">'
                + ['H','H2','L'].map(function(m) {
                    return '<option value="' + m + '"' + (s.mode === m ? ' selected' : '') + '>' + m
                        + (m === 'H' ? ' — 1 lx res (default)' : m === 'H2' ? ' — 0.5 lx res' : ' — 4 lx res') + '</option>';
                }).join('') + '</select>');
        }
        // VEML7700-specific
        if(s.type === 'veml7700') {
            var gainOpts = [{v:0,l:'1× (default)'},{v:1,l:'2×'},{v:2,l:'1/8×'},{v:3,l:'1/4×'}];
            var intOpts  = [25, 50, 100, 200, 400, 800];
            html += '<div class="form-row" style="gap:.5rem">';
            html += '<div>' + _sepRow('Gain',
                '<select id="sep-gain" class="form-input form-select">'
                + gainOpts.map(function(g){ return '<option value="'+g.v+'"'+(s.gain===g.v?' selected':'')+'>'+g.l+'</option>'; }).join('')
                + '</select>') + '</div>';
            html += '<div>' + _sepRow('Integration (ms)',
                '<select id="sep-integration_ms" class="form-input form-select">'
                + intOpts.map(function(t){ return '<option value="'+t+'"'+(s.integration_ms===t?' selected':'')+'>'+t+' ms</option>'; }).join('')
                + '</select>') + '</div>';
            html += '</div>';
        }

    } else if(iface === 'uart') {
        html += '<div class="form-row" style="gap:.5rem">';
        html += '<div>' + _sepRow('RX Pin', _sepPinSelect('sep-uart_rx', s.uart_rx !== undefined ? s.uart_rx : 20, usedPins, false, false)) + '</div>';
        html += '<div>' + _sepRow('TX Pin', _sepPinSelect('sep-uart_tx', s.uart_tx !== undefined ? s.uart_tx : -1, usedPins, true,  false),
            'Set to "Not connected" if the sensor only uses RX.') + '</div>';
        html += '</div>';
        var baudOpts = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];
        html += _sepRow('Baud Rate',
            '<select id="sep-baud" class="form-input form-select">'
            + baudOpts.map(function(b){
                return '<option value="' + b + '"' + (s.baud === b ? ' selected' : '') + '>' + b + '</option>';
            }).join('') + '</select>');

    } else if(iface === 'pulse') {
        html += _sepRow('Signal Pin', _sepPinSelect('sep-pin', s.pin !== undefined ? s.pin : 9, usedPins, false, false));
        html += _sepRow('Read Interval (ms)', _sepNumInput('sep-interval', s.read_interval_ms || 5000, 100, undefined, 100));
        if(s.pulses_per_liter !== undefined) {
            html += _sepRow('Pulses per Liter', _sepNumInput('sep-pulses_per_liter', s.pulses_per_liter, 0, undefined, 0.1));
        }

    } else if(iface === 'analog') {
        html += _sepRow('ADC Pin',
            '<div id="sep-pin-wrap">' + _sepPinSelect('sep-pin', s.pin !== undefined ? s.pin : 0, usedPins, false, true) + '</div>');
        html += '<div style="margin-top:-.4rem;margin-bottom:.6rem">'
            + '<label style="display:flex;align-items:center;gap:.4rem;font-size:.82rem;color:var(--text-muted);cursor:pointer">'
            + '<input type="checkbox" id="sep-adc-only" checked onchange="sepRebuildPinSel()">'
            + 'Show ADC-capable pins only (GPIO 0–5)</label></div>';
        html += _sepRow('ADC Samples (averaged)', _sepNumInput('sep-adc_samples', s.adc_samples || 64, 1, 1024),
            'More samples → lower noise, slower reading.');
        html += _sepRow('Read Interval (ms)', _sepNumInput('sep-interval', s.read_interval_ms || 1000, 100, undefined, 100));
        if(s.type === 'zmpt101b') {
            html += _sepRow('Voltage Factor', _sepNumInput('sep-voltage_factor', s.voltage_factor !== undefined ? s.voltage_factor : 1.0, 0, undefined, 0.0001),
                'Multiplier: ADC units → Volts RMS. Calibrate against known AC source.');
        } else if(s.type === 'zmct103c') {
            html += _sepRow('Current Factor', _sepNumInput('sep-current_factor', s.current_factor !== undefined ? s.current_factor : 1.0, 0, undefined, 0.0001),
                'Multiplier: ADC units → Amperes RMS. Calibrate against known load.');
        } else if(s.type === 'soil_moisture') {
            html += '<div class="form-row" style="gap:.5rem">';
            html += '<div>' + _sepRow('Dry ADC Value', _sepNumInput('sep-dry_value', s.dry_value || 3300, 0, 4095)) + '</div>';
            html += '<div>' + _sepRow('Wet ADC Value', _sepNumInput('sep-wet_value', s.wet_value || 1500, 0, 4095)) + '</div>';
            html += '</div>';
        }

    } else if(iface === 'onewire') {
        html += _sepRow('Data Pin', _sepPinSelect('sep-pin', s.pin !== undefined ? s.pin : 2, usedPins, false, false));
        html += _sepRow('Resolution',
            '<select id="sep-resolution" class="form-input form-select">'
            + [9, 10, 11, 12].map(function(r){
                return '<option value="' + r + '"' + (s.resolution === r ? ' selected' : '') + '>' + r + ' bit'
                    + (r === 9 ? ' (93ms, ±0.5°C)' : r === 10 ? ' (187ms, ±0.25°C)' : r === 11 ? ' (375ms, ±0.125°C)' : ' (750ms, ±0.0625°C, default)') + '</option>';
            }).join('') + '</select>');
        html += _sepRow('Read Interval (ms)', _sepNumInput('sep-interval', s.read_interval_ms || 5000, 500, undefined, 100));

    } else if(iface === 'gpio') {
        // HC-SR04 has trig + echo as separate pins
        if(s.type === 'hcsr04' || s.trig_pin !== undefined) {
            html += '<div class="form-row" style="gap:.5rem">';
            html += '<div>' + _sepRow('Trigger Pin', _sepPinSelect('sep-trig_pin', s.trig_pin !== undefined ? s.trig_pin : 5, usedPins, false, false)) + '</div>';
            html += '<div>' + _sepRow('Echo Pin',    _sepPinSelect('sep-echo_pin', s.echo_pin  !== undefined ? s.echo_pin  : 4, usedPins, false, false)) + '</div>';
            html += '</div>';
            html += _sepRow('Max Distance (cm)', _sepNumInput('sep-max_distance_cm', s.max_distance_cm || 400, 10, 600));
        } else {
            html += _sepRow('Pin', _sepPinSelect('sep-pin', s.pin !== undefined ? s.pin : 0, usedPins, false, false));
        }
        html += _sepRow('Read Interval (ms)', _sepNumInput('sep-interval', s.read_interval_ms || 1000, 100, undefined, 100));

    } else {
        html += _sepRow('Pin', _sepPinSelect('sep-pin', s.pin !== undefined ? s.pin : 0, usedPins, false, false));
        if(s.read_interval_ms !== undefined) {
            html += _sepRow('Read Interval (ms)', _sepNumInput('sep-interval', s.read_interval_ms, 100, undefined, 100));
        }
    }

    // ── Calibration sub-form ─────────────────────────────────────────────────
    var calObj = s.cal || s.calibration;
    if(calObj && typeof calObj === 'object' && !Array.isArray(calObj)) {
        var calKeys = Object.keys(calObj).filter(function(k) { return typeof calObj[k] === 'object'; });
        if(calKeys.length) {
            html += '<div style="border-top:1px solid var(--border);margin-top:.75rem;padding-top:.75rem">';
            html += '<div style="font-size:.75rem;font-weight:700;text-transform:uppercase;letter-spacing:.05em;color:var(--text-muted);margin-bottom:.5rem">Calibration</div>';
            calKeys.forEach(function(metric) {
                var c = calObj[metric];
                html += '<div style="font-size:.82rem;font-weight:600;color:var(--text-muted);margin-bottom:.2rem">' + metric + '</div>';
                html += '<div class="form-row" style="gap:.5rem;margin-bottom:.6rem">';
                html += '<div>' + _sepRow('Offset', _sepNumInput('sep-cal-' + metric + '-offset', c.offset !== undefined ? c.offset : 0, undefined, undefined, 0.00001)) + '</div>';
                html += '<div>' + _sepRow('Scale',  _sepNumInput('sep-cal-' + metric + '-scale',  c.scale  !== undefined ? c.scale  : 1, undefined, undefined, 0.00001)) + '</div>';
                html += '</div>';
            });
            html += '</div>';
        }
    }

    document.getElementById('sep-fields').innerHTML = html;
    document.getElementById('sensor-edit-popup').style.display = 'flex';
}

function sepClose() {
    document.getElementById('sensor-edit-popup').style.display = 'none';
    SEP_idx = null;
}

function sepConfirm() {
    if(SEP_idx === null || !PCFG || !PCFG.sensors) return;
    var s = PCFG.sensors[SEP_idx];

    var idEl = document.getElementById('sep-id');
    var enEl = document.getElementById('sep-enabled');
    if(idEl) s.id      = idEl.value.trim();
    if(enEl) s.enabled = enEl.checked;

    var iface = s.interface;

    function _int(id)   { var e = document.getElementById(id); return e && e.value !== '' ? parseInt(e.value,  10) : undefined; }
    function _flt(id)   { var e = document.getElementById(id); return e && e.value !== '' ? parseFloat(e.value)    : undefined; }
    function _str(id)   { var e = document.getElementById(id); return e ? e.value.trim()                           : undefined; }
    function _set(key, val) { if(val !== undefined && !isNaN(Number(val))) s[key] = val; }
    function _has(id)   { return !!document.getElementById(id); }

    if(iface === 'i2c') {
        _set('sda',              _int('sep-sda'));
        _set('scl',              _int('sep-scl'));
        _set('read_interval_ms', _int('sep-interval'));
        var addr = _str('sep-address'); if(addr !== undefined && addr !== '') s.address = addr;
        // BME688
        if(_has('sep-heater_temp'))        _set('heater_temp',         _int('sep-heater_temp'));
        if(_has('sep-heater_duration_ms')) _set('heater_duration_ms',  _int('sep-heater_duration_ms'));
        // BH1750
        if(_has('sep-mode'))               { var m = _str('sep-mode'); if(m) s.mode = m; }
        // VEML7700
        if(_has('sep-gain'))               _set('gain',                _int('sep-gain'));
        if(_has('sep-integration_ms'))     _set('integration_ms',      _int('sep-integration_ms'));

    } else if(iface === 'uart') {
        _set('uart_rx', _int('sep-uart_rx'));
        _set('uart_tx', _int('sep-uart_tx'));
        _set('baud',    _int('sep-baud'));

    } else if(iface === 'pulse') {
        _set('pin',              _int('sep-pin'));
        _set('read_interval_ms', _int('sep-interval'));
        if(_has('sep-pulses_per_liter')) _set('pulses_per_liter', _flt('sep-pulses_per_liter'));

    } else if(iface === 'analog') {
        _set('pin',              _int('sep-pin'));
        _set('adc_samples',      _int('sep-adc_samples'));
        _set('read_interval_ms', _int('sep-interval'));
        if(_has('sep-voltage_factor')) _set('voltage_factor', _flt('sep-voltage_factor'));
        if(_has('sep-current_factor')) _set('current_factor', _flt('sep-current_factor'));
        if(_has('sep-dry_value'))      _set('dry_value',      _int('sep-dry_value'));
        if(_has('sep-wet_value'))      _set('wet_value',      _int('sep-wet_value'));

    } else if(iface === 'onewire') {
        _set('pin',              _int('sep-pin'));
        _set('resolution',       _int('sep-resolution'));
        _set('read_interval_ms', _int('sep-interval'));

    } else if(iface === 'gpio') {
        _set('read_interval_ms', _int('sep-interval'));
        if(_has('sep-trig_pin'))        _set('trig_pin',        _int('sep-trig_pin'));
        if(_has('sep-echo_pin'))        _set('echo_pin',        _int('sep-echo_pin'));
        if(_has('sep-max_distance_cm')) _set('max_distance_cm', _flt('sep-max_distance_cm'));
        if(_has('sep-pin'))             _set('pin',             _int('sep-pin'));

    } else {
        _set('pin',              _int('sep-pin'));
        _set('read_interval_ms', _int('sep-interval'));
    }

    // ── Calibration sub-form ─────────────────────────────────────────────────
    var calObj = s.cal || s.calibration;
    var calKey = s.cal ? 'cal' : 'calibration';
    if(calObj && typeof calObj === 'object') {
        Object.keys(calObj).forEach(function(metric) {
            var offEl = document.getElementById('sep-cal-' + metric + '-offset');
            var sclEl = document.getElementById('sep-cal-' + metric + '-scale');
            if(offEl || sclEl) {
                if(!s[calKey])           s[calKey] = {};
                if(!s[calKey][metric])   s[calKey][metric] = {};
                if(offEl) s[calKey][metric].offset = parseFloat(offEl.value);
                if(sclEl) s[calKey][metric].scale  = parseFloat(sclEl.value);
            }
        });
    }

    PCFG.sensors[SEP_idx] = s;
    sepClose();
    clRenderSensors(PCFG.sensors);
}

function clSave() {
    var msg = document.getElementById('cl-msg');
    if(!PCFG) { if(msg){ msg.textContent='❌ No config loaded'; msg.className='alert alert-danger'; } return; }

    // Read form values back into PCFG
    var modeEl = document.getElementById('cl-mode'); if(modeEl) PCFG.mode = modeEl.value;

    if(!PCFG.aggregation) PCFG.aggregation = {};
    var amEl = document.getElementById('cl-aggmode');   if(amEl) PCFG.aggregation.default_mode = amEl.value;
    var abEl = document.getElementById('cl-aggbucket'); if(abEl) PCFG.aggregation.default_bucket_min = parseInt(abEl.value,10);
    var mpEl = document.getElementById('cl-maxpoints'); if(mpEl) PCFG.aggregation.max_points = parseInt(mpEl.value,10);
    var rtEl = document.getElementById('cl-retention'); if(rtEl) PCFG.aggregation.raw_retention_days = parseInt(rtEl.value,10);

    if(!PCFG.export) PCFG.export = {};
    if(!PCFG.export.mqtt) PCFG.export.mqtt = {};
    if(!PCFG.export.http) PCFG.export.http = {};
    if(!PCFG.export.sensor_community) PCFG.export.sensor_community = {};
    if(!PCFG.export.opensensemap) PCFG.export.opensensemap = {};

    var mqttEl = document.getElementById('cl-exp-mqtt'); if(mqttEl) PCFG.export.mqtt.enabled = mqttEl.checked;
    var httpEl = document.getElementById('cl-exp-http'); if(httpEl) PCFG.export.http.enabled = httpEl.checked;
    var scEl   = document.getElementById('cl-exp-sc');   if(scEl)   PCFG.export.sensor_community.enabled = scEl.checked;
    var osmEl  = document.getElementById('cl-exp-osm');  if(osmEl)  PCFG.export.opensensemap.enabled = osmEl.checked;

    // Sleep settings
    if(!PCFG.sleep) PCFG.sleep = {};
    if(!PCFG.sleep.continuous) PCFG.sleep.continuous = {};
    if(!PCFG.sleep.hybrid)     PCFG.sleep.hybrid     = {};
    var ciEl = document.getElementById('cl-cont-idle');  if(ciEl) PCFG.sleep.continuous.idle_timeout_ms    = parseInt(ciEl.value, 10) || CL_SLEEP_DEFAULTS.cont_idle_timeout_ms;
    var ccEl = document.getElementById('cl-cont-cpu');   if(ccEl) PCFG.sleep.continuous.idle_cpu_mhz       = parseInt(ccEl.value, 10) || CL_SLEEP_DEFAULTS.cont_idle_cpu_mhz;
    var cmEl = document.getElementById('cl-cont-modem'); if(cmEl) PCFG.sleep.continuous.modem_sleep        = cmEl.checked;
    var hiEl = document.getElementById('cl-hyb-idle');   if(hiEl) PCFG.sleep.hybrid.idle_before_sleep_ms   = parseInt(hiEl.value, 10) || CL_SLEEP_DEFAULTS.hyb_idle_before_sleep_ms;
    var hsEl = document.getElementById('cl-hyb-sleep');  if(hsEl) PCFG.sleep.hybrid.sleep_duration_ms      = parseInt(hsEl.value, 10) || CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms;
    var haEl = document.getElementById('cl-hyb-active'); if(haEl) PCFG.sleep.hybrid.active_window_ms       = parseInt(haEl.value, 10) || CL_SLEEP_DEFAULTS.hyb_active_window_ms;

    if(msg){ msg.textContent='Saving…'; msg.className=''; }

    pcfgSave(PCFG, function(ok, err) {
        if(ok) {
            if(msg){ msg.textContent='✅ Saved! Restarting device…'; msg.className=''; }
            // Trigger restart so new mode takes effect
            setTimeout(function(){
                fetch('/api/platform_reload', { method: 'POST' }).catch(function(){});
            }, 500);
        } else {
            if(msg){ msg.textContent='❌ Save failed: ' + err; msg.className=''; }
        }
    });
}

// Show/hide the Power & Sleep card and its sub-panels based on selected mode.
function clUpdateSleepPanel() {
    var modeEl  = document.getElementById('cl-mode');
    var mode    = modeEl ? modeEl.value : 'legacy';
    var card    = document.getElementById('cl-sleep-card');
    var contDiv = document.getElementById('cl-sleep-cont');
    var hybDiv  = document.getElementById('cl-sleep-hyb');
    if(card)    card.style.display    = (mode === 'continuous' || mode === 'hybrid') ? '' : 'none';
    if(contDiv) contDiv.style.display = (mode === 'continuous') ? '' : 'none';
    if(hybDiv)  hybDiv.style.display  = (mode === 'hybrid')     ? '' : 'none';
    clUpdateHybCycle();
}

// Update the hybrid cycle summary label (sleep + active = total).
function clUpdateHybCycle() {
    var lbl  = document.getElementById('cl-hyb-cycle-label');
    if(!lbl) return;
    var hsEl = document.getElementById('cl-hyb-sleep');
    var haEl = document.getElementById('cl-hyb-active');
    var sleepMs  = parseInt(hsEl ? hsEl.value : CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms,   10) || CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms;
    var activeMs = parseInt(haEl ? haEl.value : CL_SLEEP_DEFAULTS.hyb_active_window_ms,    10) || CL_SLEEP_DEFAULTS.hyb_active_window_ms;
    var totalMs  = sleepMs + activeMs;
    lbl.textContent = `${(sleepMs/1000).toFixed(0)}s sleep + ${(activeMs/1000).toFixed(0)}s active = ${(totalMs/1000).toFixed(0)}s per cycle`;
}

// ============================================================================
// EXPORT PAGE
// ============================================================================
function expLoad() {
    pcfgLoad(function(cfg) {
        var exp = cfg.export || {};

        // MQTT
        var m = exp.mqtt || {};
        _setVal('exp-mqtt-en',       m.enabled  || false,  true);
        _setVal('exp-mqtt-host',     m.broker   || '');
        _setVal('exp-mqtt-port',     m.port     || 1883);
        _setVal('exp-mqtt-prefix',   m.topic_prefix || 'waterlogger');
        _setVal('exp-mqtt-clientid', m.client_id || '');
        _setVal('exp-mqtt-user',     m.username  || '');
        _setVal('exp-mqtt-pass',     m.password  || '');
        _setVal('exp-mqtt-interval', m.interval_ms || 60000);
        _setVal('exp-mqtt-retain',   m.retain || false, true);

        // HTTP
        var h = exp.http || {};
        _setVal('exp-http-en',       h.enabled || false, true);
        _setVal('exp-http-url',      h.url || '');
        _setVal('exp-http-auth',     (h.headers && h.headers.Authorization) || '');
        _setVal('exp-http-interval', h.interval_ms || 60000);

        // Sensor.Community
        var sc = exp.sensor_community || {};
        _setVal('exp-sc-en',       sc.enabled || false, true);
        _setVal('exp-sc-interval', sc.interval_ms || 145000);

        // openSenseMap
        var osm = exp.opensensemap || {};
        _setVal('exp-osm-en',    osm.enabled || false, true);
        _setVal('exp-osm-boxid', osm.box_id || '');
        _setVal('exp-osm-token', osm.access_token || '');

        // OSM sensor IDs grid
        var ids = osm.sensor_ids || {};
        var osmDiv = document.getElementById('exp-osm-ids');
        if(osmDiv) {
            var metrics = ['temperature','humidity','pressure','pm25','pm10','tvoc','eco2','flow_rate','rain_total','wind_speed'];
            osmDiv.innerHTML = '<div class="form-row" style="flex-wrap:wrap">'
                + metrics.map(function(m){
                    return '<div class="form-group" style="min-width:180px">'
                         + '<label class="form-label">' + m + '</label>'
                         + '<input type="text" id="osm-id-' + m + '" class="form-input" value="'
                         + (ids[m] || '') + '" placeholder="sensor ID…">'
                         + '</div>';
                  }).join('')
                + '</div>';
        }
    });
}

function _setVal(id, val, isCheck) {
    var el = document.getElementById(id);
    if(!el) return;
    if(isCheck) el.checked = !!val;
    else el.value = val;
}

// ============================================================================
// ══ TOAST NOTIFICATIONS ══
// ============================================================================
function showToast(msg, type) {
    var el = document.getElementById('toast');
    if (!el) return;
    if (_toastTimer) { clearTimeout(_toastTimer); _toastTimer = null; }
    el.textContent = msg;
    el.className = 'toast-show' + (type ? ' toast-' + type : '');
    _toastTimer = setTimeout(function () {
        el.className = '';
        _toastTimer = null;
    }, 3500);
}

// ============================================================================
// ══ THEME QUICK TOGGLE ══
// ============================================================================
function quickThemeToggle() {
    var html = document.getElementById('htmlRoot');
    if (!html) return;
    var isDark = html.classList.contains('theme-dark') ||
        (html.classList.contains('theme-auto') &&
         window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
    var next = isDark ? 'theme-light' : 'theme-dark';
    html.classList.remove('theme-light', 'theme-dark', 'theme-auto');
    html.classList.add(next);
    localStorage.setItem('quickTheme', next);
    var btn = document.getElementById('themeToggleBtn');
    if (btn) btn.textContent = next === 'theme-dark' ? '☀️' : '🌙';
}

// Restore quick theme override on load (before applyStatus overwrites it)
(function () {
    var qt = localStorage.getItem('quickTheme');
    if (!qt) return;
    var html = document.getElementById('htmlRoot');
    if (!html) return;
    html.classList.remove('theme-light', 'theme-dark', 'theme-auto');
    html.classList.add(qt);
    // Update button icon once DOM ready
    window.addEventListener('DOMContentLoaded', function () {
        var btn = document.getElementById('themeToggleBtn');
        if (btn) btn.textContent = qt === 'theme-dark' ? '☀️' : '🌙';
    });
}());

// ============================================================================
// ══ KEYBOARD SHORTCUTS ══
// ============================================================================
document.addEventListener('keydown', function (e) {
    // Ignore when typing in inputs/textareas
    var tag = (e.target && e.target.tagName) ? e.target.tagName.toLowerCase() : '';
    if (tag === 'input' || tag === 'textarea' || tag === 'select') return;
    // Ignore modifier keys
    if (e.ctrlKey || e.altKey || e.metaKey) return;

    switch (e.key) {
        case '?':
            var kbp = document.getElementById('kbPopup');
            if (kbp) kbp.style.display = kbp.style.display === 'flex' ? 'none' : 'flex';
            break;
        case 'Escape':
            // Close any open popup-overlay
            document.querySelectorAll('.popup-overlay').forEach(function (p) {
                if (p.style.display !== 'none') p.style.display = 'none';
            });
            break;
        case '1': location.hash = 'dashboard'; break;
        case '2': location.hash = 'files'; break;
        case '3': location.hash = 'live'; break;
        case '4': location.hash = 'sensors'; break;
        case '5': location.hash = 'settings_device'; break;
        case 'r':
        case 'R':
            pageInit(currentPage);
            showToast('Page reloaded', 'success');
            break;
        case 'd':
        case 'D':
            quickThemeToggle();
            break;
    }
});

// ============================================================================
// ══ CHART PNG EXPORT ══
// ============================================================================
function dbExportPNG() {
    if (!dbChart) { showToast('No chart to export', 'warning'); return; }
    var url = dbChart.toBase64Image('image/png', 1);
    var a = document.createElement('a');
    var f = (ST.deviceId || CFG.deviceId || 'logger') + '_chart_' + new Date().toISOString().slice(0, 10) + '.png';
    a.href = url; a.download = f; a.click();
}

// ============================================================================
// ══ SENSOR TEST / PING ══
// ============================================================================
var _sensorPingEl = null;

function clPingSensor(id) {
    if (!_sensorPingEl) {
        _sensorPingEl = document.createElement('div');
        _sensorPingEl.className = 'popup-overlay';
        _sensorPingEl.style.display = 'none';
        _sensorPingEl.onclick = function (e) { if (e.target === this) this.style.display = 'none'; };
        _sensorPingEl.innerHTML =
            '<div class="popup-content" style="max-width:340px">'
            + '<div id="spr-title" style="font-size:1.1rem;font-weight:700;margin-bottom:.8rem"></div>'
            + '<div id="spr-body"></div>'
            + '<div style="text-align:right;margin-top:1rem">'
            + '<button class="btn btn-secondary" onclick="document.querySelector(\'.popup-overlay[data-ping]\').style.display=\'none\'">Close</button>'
            + '</div></div>';
        _sensorPingEl.setAttribute('data-ping', '1');
        document.body.appendChild(_sensorPingEl);
    }
    document.getElementById('spr-title').textContent = '🔍 ' + id;
    document.getElementById('spr-body').innerHTML = '<p style="color:var(--text-muted)">Reading…</p>';
    _sensorPingEl.style.display = 'flex';

    fetch('/api/sensors/read_now?id=' + encodeURIComponent(id))
        .then(function (r) { return r.json(); })
        .then(function (d) {
            var body = document.getElementById('spr-body');
            if (!body) return;
            if (d.error) {
                body.innerHTML = '<p style="color:var(--danger,#e74c3c)">❌ ' + d.error + '</p>';
                return;
            }
            var rows = (d.readings || []).map(function (r) {
                return '<div style="display:flex;justify-content:space-between;padding:.35rem 0;border-bottom:1px solid var(--border)">'
                    + '<span style="color:var(--text-muted)">' + r.metric + '</span>'
                    + '<strong>' + Number(r.value).toFixed(4)
                    + ' <span style="font-weight:400;color:var(--text-muted)">' + (r.unit || '') + '</span></strong>'
                    + '</div>';
            }).join('');
            body.innerHTML = rows || '<p style="color:var(--text-muted)">No readings returned.</p>';
        })
        .catch(function (e) {
            var body = document.getElementById('spr-body');
            if (body) body.innerHTML = '<p style="color:var(--danger,#e74c3c)">❌ ' + e.message + '</p>';
        });
}

// ============================================================================
// ══ DASHBOARD: ZOOM / PAN ══
// ============================================================================
var dbViewStart = 0;
var dbViewEnd   = -1;  // -1 means "show all"

function dbZoom(dir) {
    // dir=-1 → zoom in (fewer bars), dir=+1 → zoom out (more bars)
    var total = dbFilteredData.length;
    if (total === 0) return;
    var start = dbViewStart;
    var end   = dbViewEnd < 0 ? total - 1 : dbViewEnd;
    var visible = end - start + 1;
    var step = Math.max(1, Math.round(visible * 0.25));
    if (dir < 0) {  // zoom in: shrink from both ends
        start += step;
        end   -= step;
        if (start >= end) { start = Math.max(0, Math.round((total - 1) / 2)); end = start; }
    } else {        // zoom out: expand toward full range
        start = Math.max(0, start - step);
        end   = Math.min(total - 1, end + step);
    }
    dbViewStart = start;
    dbViewEnd   = end;
    dbRenderChart(dbFilteredData);
}

function dbZoomReset() {
    dbViewStart = 0;
    dbViewEnd   = -1;
    dbRenderChart(dbFilteredData);
}

// ============================================================================
// ══ DASHBOARD: PERSISTENT FILTERS ══
// ============================================================================
function dbSaveFilters() {
    try {
        localStorage.setItem('dbFilters', JSON.stringify({
            startDate:   getVal('startDate'),
            endDate:     getVal('endDate'),
            eventFilter: getVal('eventFilter'),
            pressFilter: getVal('pressFilter'),
            excludeZero: !!(document.getElementById('excludeZero') || {}).checked
        }));
    } catch (e) {}
}

function dbRestoreFilters() {
    try {
        var pf = JSON.parse(localStorage.getItem('dbFilters') || 'null');
        if (!pf) return;
        if (pf.startDate)   setVal('startDate',   pf.startDate);
        if (pf.endDate)     setVal('endDate',      pf.endDate);
        if (pf.eventFilter) setVal('eventFilter',  pf.eventFilter);
        if (pf.pressFilter) setVal('pressFilter',  pf.pressFilter);
        if (pf.excludeZero) setChk('excludeZero',  pf.excludeZero);
    } catch (e) {}
}

// ============================================================================
// ══ SETTINGS FILTER / SEARCH ══
// ============================================================================
function settingsFilter(query) {
    var q = (query || '').trim().toLowerCase();
    var grid = document.getElementById('settings-grid');
    var noMatch = document.getElementById('settings-no-match');
    if (!grid) return;
    var cards = grid.querySelectorAll('.setting-card');
    var visible = 0;
    cards.forEach(function (card) {
        var title = (card.querySelector('.title') || {}).textContent || '';
        var icon  = (card.querySelector('.icon')  || {}).textContent || '';
        var match = !q || (title + ' ' + icon).toLowerCase().indexOf(q) >= 0;
        card.style.display = match ? '' : 'none';
        if (match) visible++;
    });
    if (noMatch) noMatch.style.display = visible === 0 ? 'block' : 'none';
}

// ============================================================================
// ══ UNSAVED CHANGES TRACKING ══
// ============================================================================
var _dirtyPage = null;

function _markDirty() { _dirtyPage = currentPage; }
function _clearDirty() { _dirtyPage = null; }

// Override navigateTo to warn about unsaved changes on settings pages
(function () {
    var _origNavigateTo = navigateTo;
    navigateTo = function (page) {
        if (_dirtyPage && _dirtyPage !== page && _dirtyPage.startsWith('settings')) {
            if (!confirm('You have unsaved changes on this settings page. Leave without saving?')) return;
        }
        _clearDirty();
        _origNavigateTo(page);
    };
}());

// Attach dirty tracking to all settings forms after page loads
document.addEventListener('DOMContentLoaded', function () {
    // Delegated listener: any input/change in a .page element marks dirty
    document.querySelectorAll('.page').forEach(function (page) {
        if (!page.id || !page.id.startsWith('page-settings')) return;
        page.addEventListener('change', function () {
            if (currentPage && currentPage.startsWith('settings')) _markDirty();
        });
        page.addEventListener('input', function () {
            if (currentPage && currentPage.startsWith('settings')) _markDirty();
        });
    });
});

// ADC-only pin filter rebuild for analog sensor edit popup
function sepRebuildPinSel() {
    var wrap = document.getElementById('sep-pin-wrap');
    if (!wrap) return;
    var adcOnly = !!(document.getElementById('sep-adc-only') || {}).checked;
    var pinEl = document.getElementById('sep-pin');
    var current = pinEl ? parseInt(pinEl.value, 10) : 0;
    var used = clGetUsedPins(SEP_idx);
    wrap.innerHTML = _sepPinSelect('sep-pin', current, used, false, adcOnly);
}

function expSave() {
    var msg = document.getElementById('exp-msg');
    if(!PCFG) PCFG = {};
    if(!PCFG.export) PCFG.export = {};

    // MQTT
    PCFG.export.mqtt = {
        enabled:     !!(document.getElementById('exp-mqtt-en')||{}).checked,
        broker:      ((document.getElementById('exp-mqtt-host')||{}).value || ''),
        port:        parseInt(((document.getElementById('exp-mqtt-port')||{}).value || '1883'), 10),
        topic_prefix:((document.getElementById('exp-mqtt-prefix')||{}).value || 'waterlogger'),
        client_id:   ((document.getElementById('exp-mqtt-clientid')||{}).value || ''),
        username:    ((document.getElementById('exp-mqtt-user')||{}).value || ''),
        password:    ((document.getElementById('exp-mqtt-pass')||{}).value || ''),
        interval_ms: parseInt(((document.getElementById('exp-mqtt-interval')||{}).value || '60000'), 10),
        retain:      !!(document.getElementById('exp-mqtt-retain')||{}).checked,
        qos: 0
    };

    // HTTP
    var authVal = ((document.getElementById('exp-http-auth')||{}).value || '');
    PCFG.export.http = {
        enabled:     !!(document.getElementById('exp-http-en')||{}).checked,
        url:         ((document.getElementById('exp-http-url')||{}).value || ''),
        method:      'POST',
        headers:     authVal ? { Authorization: authVal } : {},
        interval_ms: parseInt(((document.getElementById('exp-http-interval')||{}).value || '60000'), 10)
    };

    // Sensor.Community
    PCFG.export.sensor_community = {
        enabled:     !!(document.getElementById('exp-sc-en')||{}).checked,
        interval_ms: parseInt(((document.getElementById('exp-sc-interval')||{}).value || '145000'), 10)
    };

    // openSenseMap
    var ids = {};
    ['temperature','humidity','pressure','pm25','pm10','tvoc','eco2','flow_rate','rain_total','wind_speed'].forEach(function(m){
        var v = ((document.getElementById('osm-id-' + m)||{}).value || '').trim();
        if(v) ids[m] = v;
    });
    PCFG.export.opensensemap = {
        enabled:      !!(document.getElementById('exp-osm-en')||{}).checked,
        box_id:       ((document.getElementById('exp-osm-boxid')||{}).value || ''),
        access_token: ((document.getElementById('exp-osm-token')||{}).value || ''),
        sensor_ids:   ids
    };

    if(msg){ msg.textContent='Saving…'; msg.className=''; }
    pcfgSave(PCFG, function(ok, err) {
        if(ok) {
            if(msg){ msg.textContent='✅ Saved! Restarting…'; msg.className=''; }
            setTimeout(function(){ fetch('/api/platform_reload', {method:'POST'}).catch(function(){}); }, 500);
        } else {
            if(msg){ msg.textContent='❌ ' + err; msg.className=''; }
        }
    });
}
