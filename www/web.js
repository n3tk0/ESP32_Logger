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

    dbChart = new Chart(ctx, {
        type: 'bar',
        data: { labels: lbls, datasets: [{ label: 'Liters (L)', data: data.map(function (d) { return d.vol; }), backgroundColor: clr, borderWidth: 0 }] },
        options: {
            responsive: true, maintainAspectRatio: false,
            plugins: {
                tooltip: {
                    callbacks: {
                        afterLabel: function (c) {
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

    liveUpdate();
    liveLogsUpdate();
    liveTimer = setInterval(liveUpdate, 500);
    liveLogsTimer = setInterval(liveLogsUpdate, 3000);
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
        setEl('net-ip', d.ip);
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
