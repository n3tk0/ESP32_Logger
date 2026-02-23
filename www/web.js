/**
 * /www/web.js  –  Water Logger SPA JavaScript
 * Pairs with /www/index.html and WebServer.cpp API endpoints.
 *
 * Architecture:
 *   – On load: fetch /api/status, apply theme, populate fields, route to hash page
 *   – Pages are hidden/shown via class toggling (no server round-trips)
 *   – Live page polls /api/live every 500ms; logs polled every 3s
 *   – All form saves use XHR/fetch to their respective /save_* endpoints
 */

'use strict';

// ============================================================================
// GLOBALS
// ============================================================================
var ST = {};                // cached /api/status payload
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
window.addEventListener('DOMContentLoaded', function() {
    // Fetch status and then route
    fetch('/api/status')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            ST = d;
            applyStatus(d);
            var hash = location.hash.replace('#','') || 'dashboard';
            navigateTo(hash);
        })
        .catch(function() {
            navigateTo('dashboard');
        });
});

window.addEventListener('hashchange', function() {
    var hash = location.hash.replace('#','') || 'dashboard';
    navigateTo(hash);
});

// ============================================================================
// STATUS  (apply theme, populate footer, etc.)
// ============================================================================
function applyStatus(d) {
    document.title = (d.device || 'Water Logger') + ' – Logger';

    // Names
    var sn = document.getElementById('sidebarName');
    var hn = document.getElementById('headerName');
    if (sn) sn.textContent = d.device || 'Water Logger';
    if (hn) hn.textContent = d.device || 'Water Logger';

    // Logo
    if (d.theme && d.theme.logoSource) {
        ['sidebarLogo','headerLogo'].forEach(function(id) {
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
        var vars = ':root{';
        if (th.primaryColor)   vars += '--primary:'   + th.primaryColor   + ';';
        if (th.secondaryColor) vars += '--secondary:' + th.secondaryColor + ';';
        if (th.bgColor)        vars += '--bg:'        + th.bgColor        + ';';
        if (th.textColor)      vars += '--text:'      + th.textColor      + ';';
        vars += '}';
        var style = document.getElementById('themeVars');
        if (style) style.textContent = vars;

        // Theme class
        var html = document.getElementById('htmlRoot');
        if (html) {
            html.classList.remove('theme-light','theme-dark');
            if (th.mode === 0) html.classList.add('theme-light');
            else if (th.mode === 1) html.classList.add('theme-dark');
            else {
                var dark = window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
                html.classList.add(dark ? 'theme-dark' : 'theme-light');
            }
        }

        // Dashboard legend colors
        setElStyle('db-legendFF',    'background', th.ffColor);
        setElStyle('db-legendPF',    'background', th.pfColor);
        setElStyle('db-legendOther', 'background', th.otherColor);
        setElStyle('db-totalFF',     'color',      th.ffColor);
        setElStyle('db-totalPF',     'color',      th.pfColor);
        setElStyle('live-ffCount',   'color',      th.ffColor);
        setElStyle('live-pfCount',   'color',      th.pfColor);
    }

    // Footer
    setEl('footer-boot', d.boot);
    setEl('footer-cpu',  d.cpu);
    setEl('footer-heap', fmtBytes(d.heap) + ' / ' + fmtBytes(d.heapTotal));
    setEl('footer-net',  d.network);
    setEl('footer-ip',   d.ip);
    setEl('footer-chip', d.chip);
    setEl('footer-ver',  d.version);

    // Header
    setEl('headerNet',  d.network);
    setEl('headerTime', (d.time || '').split(' ')[1] || '--:--');
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
    // Stop live timers if leaving live page
    if (currentPage === 'live' && page !== 'live') {
        if (liveTimer)     { clearInterval(liveTimer);     liveTimer     = null; }
        if (liveLogsTimer) { clearInterval(liveLogsTimer); liveLogsTimer = null; }
    }

    // Hide all pages
    document.querySelectorAll('.page').forEach(function(p) { p.classList.remove('active'); });
    // Deactivate nav links
    document.querySelectorAll('.nav-item, .bottom-nav a').forEach(function(a) { a.classList.remove('active'); });

    // Handle subpage routing (settings sub-pages)
    var topPage = page.startsWith('settings') ? 'settings' : page;
    currentPage = page;

    var pageEl = document.getElementById('page-' + page);
    if (pageEl) {
        pageEl.classList.add('active');
    } else {
        // Fallback to settings hub if subpage not found
        var hub = document.getElementById('page-settings');
        if (hub) hub.classList.add('active');
        topPage = 'settings'; currentPage = 'settings';
    }

    // Activate nav for top-level page
    document.querySelectorAll('[data-page="' + topPage + '"]').forEach(function(a) { a.classList.add('active'); });

    // Page-specific init
    pageInit(page);
}

function showSubpage(page) {
    location.hash = page;
}

function pageInit(page) {
    switch (page) {
        case 'dashboard':       dbInit();        break;
        case 'files':           filesInit();     break;
        case 'live':            liveInit();      break;
        case 'settings_device':     sdInit();    break;
        case 'settings_flowmeter':  sfInit();    break;
        case 'settings_hardware':   hwInit();    break;
        case 'settings_theme':      thInit();    break;
        case 'settings_network':    netInit();   break;
        case 'settings_time':       timeInit();  break;
        case 'settings_datalog':    dlInit();    break;
        case 'settings_update':                  break; // no init needed
    }
}

// ============================================================================
// HELPERS
// ============================================================================
function setEl(id, val) { var e = document.getElementById(id); if (e && val !== undefined) e.textContent = val; }
function setElStyle(id, prop, val) { var e = document.getElementById(id); if (e && val) e.style[prop] = val; }
function setVal(id, val) { var e = document.getElementById(id); if (e && val !== undefined) e.value = val; }
function setChk(id, val) { var e = document.getElementById(id); if (e) e.checked = !!val; }
function getVal(id) { var e = document.getElementById(id); return e ? e.value : ''; }

function fmtBytes(b) {
    if (!b && b !== 0) return '-';
    if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
    if (b >= 1024)    return (b / 1024).toFixed(1) + ' KB';
    return b + ' B';
}

function hexToRgba(hex, a) {
    var r = parseInt(hex.slice(1,3),16), g = parseInt(hex.slice(3,5),16), b = parseInt(hex.slice(5,7),16);
    return 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
}

function togglePass(id) {
    var e = document.getElementById(id);
    if (e) e.type = e.type === 'password' ? 'text' : 'password';
}

function showMsg(containerId, html, autoClear) {
    var el = document.getElementById(containerId);
    if (el) { el.innerHTML = html; if (autoClear) setTimeout(function(){el.innerHTML='';}, 4000); }
}

// Generic save via XHR with FormData
function settingsSave(ev, url, form, restart) {
    if (ev) ev.preventDefault();
    var fd = new FormData(form);
    var xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.onload = function() {
        if (restart) return; // page will auto-reload/redirect from server
        try {
            var r = JSON.parse(xhr.responseText);
            if (r.ok) showMsg(currentPage.replace('settings_','') + '-msg',
                "<div class='alert alert-success'>✅ Saved</div>", true);
        } catch(e) {}
    };
    xhr.send(fd);
}

// ============================================================================
// RESTART POPUP
// ============================================================================
function showRestartPopup() {
    setEl('rPopIcon',  '🔄');
    setEl('rPopTitle', 'Restart Device?');
    setEl('rPopMsg',   'The device will restart. Any unsaved changes will be lost.');
    document.getElementById('rPopProgress').style.display  = 'none';
    document.getElementById('rPopButtons').style.display   = 'flex';
    document.getElementById('restartPopup').style.display  = 'flex';
}
function closeRestart() {
    document.getElementById('restartPopup').style.display = 'none';
}
function confirmRestart() {
    document.getElementById('rPopButtons').style.display  = 'none';
    document.getElementById('rPopProgress').style.display = 'block';
    setEl('rPopIcon',  '⏳');
    setEl('rPopTitle', 'Restarting…');
    var s = 5, bar = document.getElementById('rPopBar');
    var tick = function() {
        document.getElementById('rPopMsg').innerHTML = 'Redirecting in <strong>' + s + '</strong> seconds…';
        if (bar) bar.style.width = ((5 - s) * 20) + '%';
        if (s <= 0) {
            fetch('/restart').finally(function() { location.hash = 'dashboard'; });
        } else { s--; setTimeout(tick, 1000); }
    };
    tick();
}

// ============================================================================
// ══ PAGE: DASHBOARD ══
// ============================================================================
function dbInit() {
    // Populate file selector
    fetch('/api/filelist?filter=log&recursive=1')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            var sel = document.getElementById('db-fileSelect');
            if (!sel) return;
            sel.innerHTML = '';
            if (!d.files || !d.files.length) {
                sel.innerHTML = '<option>No log files found</option>';
                return;
            }
            d.files.forEach(function(f) {
                var opt = document.createElement('option');
                opt.value = f.path; opt.textContent = f.path;
                if (ST.currentFile && f.path === ST.currentFile) opt.selected = true;
                sel.appendChild(opt);
            });
            dbLoadData();
        });
}

function dbLoadData() {
    var file = getVal('db-fileSelect');
    if (!file) return;
    var err = document.getElementById('db-errorMsg');
    if (err) err.style.display = 'none';
    fetch('/download?file=' + encodeURIComponent(file))
        .then(function(r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.text();
        })
        .then(function(data) { dbRawData = data; dbApplyFilters(); })
        .catch(function(e) {
            if (err) { err.textContent = 'Error loading: ' + e.message; err.style.display = 'block'; }
        });
}

function dbApplyFilters() { if (!dbRawData) { dbLoadData(); return; } dbProcessData(dbRawData); }

function dbProcessData(data) {
    var lines = data.trim().split('\n');
    var filtered = [];
    var startVal = getVal('db-startDate'), endVal = getVal('db-endDate');
    var filterType = getVal('db-eventFilter'), pressType = getVal('db-pressFilter');
    var excZ = document.getElementById('db-excludeZero') && document.getElementById('db-excludeZero').checked;
    var tVol = 0, tFF = 0, tPF = 0;

    lines.forEach(function(line) {
        var p = line.split('|'); if (p.length < 2) return;
        var dateStr='', timeStr='', endStr='', boot='', reason='', vol=0, ff=0, pf=0, i=0;
        if (p[0].match(/\d{2}[\/\.\-]\d{2}[\/\.\-]\d{4}/) || p[0].match(/\d{4}\-\d{2}\-\d{2}/)) { dateStr=p[0]; i=1; }
        if (p[i] && p[i].indexOf(':') >= 0)  { timeStr=p[i]; i++; }
        if (p[i] && (p[i].indexOf(':') >= 0 || p[i].match(/^\d+s$/))) { endStr=p[i]; i++; }
        if (p[i] && p[i].indexOf('#:') === 0) { boot=p[i].substring(2); i++; }
        if (p[i] && (p[i].indexOf('FF')>=0 || p[i].indexOf('PF')>=0 || p[i]==='IDLE')) { reason=p[i]; i++; }
        if (p[i]) { var vs=p[i].replace('L:','').replace(',','.'); vol=parseFloat(vs)||0; i++; }
        if (p[i] && p[i].indexOf('FF')===0) { ff=parseInt(p[i].replace('FF',''))||0; i++; }
        if (p[i] && p[i].indexOf('PF')===0) { pf=parseInt(p[i].replace('PF',''))||0; }

        var entryDate='';
        if (dateStr) {
            var m;
            if ((m=dateStr.match(/(\d{2})[\/\.](\d{2})[\/\.](\d{4})/))) entryDate=m[3]+'-'+m[2]+'-'+m[1];
            else if ((m=dateStr.match(/(\d{4})\-(\d{2})\-(\d{2})/))) entryDate=m[1]+'-'+m[2]+'-'+m[3];
        }

        if (startVal && entryDate && entryDate < startVal) return;
        if (endVal   && entryDate && entryDate > endVal)   return;
        if (filterType==='BTN' && reason.indexOf('FF')<0 && reason.indexOf('PF')<0) return;
        if (filterType==='FF'  && reason.indexOf('FF')<0) return;
        if (filterType==='PF'  && reason.indexOf('PF')<0) return;
        if (pressType==='EXTRA' && ff===0 && pf===0) return;
        if (pressType==='NONE'  && (ff>0 || pf>0))  return;
        if (excZ && vol===0) return;

        tFF+=ff; tPF+=pf; tVol+=vol;
        var fullTime = timeStr + (endStr ? '-'+endStr : '');
        filtered.push({date:dateStr||'N/A',time:timeStr,fullTime:fullTime,boot:boot,vol:vol,reason:reason,ff:ff,pf:pf});
    });

    dbFilteredData = filtered;
    setEl('db-totalVol',   tVol.toFixed(2) + ' L');
    setEl('db-eventCount', filtered.length);
    setEl('db-totalFF',    tFF);
    setEl('db-totalPF',    tPF);
    dbRenderChart(filtered);
}

function dbRenderChart(data) {
    var ctx = document.getElementById('db-chart');
    if (!ctx) return;
    if (dbChart) { dbChart.destroy(); dbChart = null; }
    var th = ST.theme || {};
    var ffColor = th.ffColor || '#3498db', pfColor = th.pfColor || '#e74c3c', otherColor = th.otherColor || '#95a5a6';
    var clr = data.map(function(d) {
        if (d.reason.indexOf('FF')>=0) return ffColor;
        if (d.reason.indexOf('PF')>=0) return pfColor;
        return otherColor;
    });
    var lblFmt = th.chartLabelFormat || 0;
    var lbls = data.map(function(d) {
        if (lblFmt===1) return d.boot ? '#'+d.boot : '#?';
        if (lblFmt===2) return d.date+' '+d.time+(d.boot?' #'+d.boot:'');
        return d.date+' '+d.time;
    });
    if (typeof Chart === 'undefined') return; // Chart.js not loaded
    dbChart = new Chart(ctx, {
        type: 'bar',
        data: { labels: lbls, datasets: [{ label: 'Liters (L)', data: data.map(function(d){return d.vol;}), backgroundColor: clr, borderWidth: 0 }] },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: { tooltip: { callbacks: { afterLabel: function(c) {
                var d = data[c.dataIndex];
                return ['Trigger: '+d.reason, 'Boot: '+(d.boot||'N/A'), 'Extra FF: '+d.ff, 'Extra PF: '+d.pf];
            }}}},
            scales: { y: { beginAtZero:true, title: { display:true, text:'Liters' }}}
        }
    });
}

function dbExportCSV() {
    if (!dbFilteredData.length) { alert('No data to export'); return; }
    var csv = 'Date,Time,Boot,Volume (L),Trigger,Extra FF,Extra PF\n';
    dbFilteredData.forEach(function(d) {
        csv += d.date+','+d.fullTime+','+(d.boot||'')+','+d.vol.toFixed(2)+','+d.reason+','+d.ff+','+d.pf+'\n';
    });
    var f = (ST.deviceId || 'logger');
    var ft = getVal('db-eventFilter'); if (ft!=='ALL') f+='_'+ft;
    var pt = getVal('db-pressFilter'); if (pt!=='ALL') f+='_'+pt;
    var sd = getVal('db-startDate'); if (sd) f+='_from'+sd;
    var ed = getVal('db-endDate');   if (ed) f+='_to'+ed;
    f += '_'+new Date().toISOString().slice(0,10)+'.csv';
    var blob = new Blob([csv], {type:'text/csv'});
    var url  = URL.createObjectURL(blob);
    var a = document.createElement('a'); a.href=url; a.download=f; a.click();
    URL.revokeObjectURL(url);
}

// ============================================================================
// ══ PAGE: FILES ══
// ============================================================================
function filesInit() {
    filesEditMode = false;
    currentFilesDir = '/';
    // Default storage from status
    currentFilesStorage = (ST.defaultStorageView === 1) ? 'sdcard' : 'internal';
    filesRender();
}

function filesRender() {
    // Storage tabs
    var tabs = document.getElementById('files-tabs');
    if (tabs) {
        tabs.innerHTML = '';
        if (ST.fsTotal || ST.sdAvailable || true) { // always show
            var btn1 = '<a onclick="filesSetStorage(\'internal\')" class="btn ' + (currentFilesStorage==='internal'?'btn-primary':'btn-secondary') + '">💾 Internal</a> ';
            var btn2 = '<a onclick="filesSetStorage(\'sdcard\')" class="btn ' + (currentFilesStorage==='sdcard'?'btn-primary':'btn-secondary') + '">💳 SD Card</a>';
            tabs.innerHTML = btn1 + btn2;
        }
    }

    // Storage info
    fetch('/api/filelist?storage=' + currentFilesStorage + '&dir=' + encodeURIComponent(currentFilesDir))
        .then(function(r) { return r.json(); })
        .then(function(d) {
            // Bar
            var pct = d.percent || 0;
            setEl('files-usage', fmtBytes(d.used) + ' / ' + fmtBytes(d.total));
            setEl('files-pct', pct + '%');
            var bar = document.getElementById('files-bar');
            if (bar) {
                bar.style.width = pct + '%';
                bar.className = 'progress-bar' + (pct>=90?' progress-bar-danger':pct>=70?' progress-bar-warning':' progress-bar-success');
            }

            // Dir label
            setEl('files-dirLabel', '📂 [' + (currentFilesStorage==='sdcard'?'SD':'Int') + '] ' + (currentFilesDir==='/'?'Root':currentFilesDir));

            // Up button
            var upBtn = document.getElementById('files-upBtn');
            if (upBtn) upBtn.style.display = currentFilesDir === '/' ? 'none' : '';

            // Edit toggle
            var et = document.getElementById('files-editToggle');
            if (et) et.textContent = filesEditMode ? '✖️ Done' : '✏️ Edit';
            var tools = document.getElementById('files-editTools');
            if (tools) tools.style.display = filesEditMode ? 'block' : 'none';

            // File list
            var list = document.getElementById('files-list');
            if (!list) return;
            var files = d.files || [];
            if (!files.length) { list.innerHTML = "<div class='list-item text-muted'>Empty</div>"; return; }
            var html = '';
            files.forEach(function(f) {
                var actions = '';
                if (f.isDir) {
                    actions = "<a onclick=\"filesEnterDir('" + f.path.replace(/'/g,"\\'") + "')\" class='btn btn-sm btn-secondary'>📂 Open</a>";
                } else {
                    actions += "<a href='/download?file=" + encodeURIComponent(f.path) + "&storage=" + currentFilesStorage + "' class='btn btn-sm btn-secondary'>📥</a> ";
                    if (filesEditMode) {
                        actions += "<button onclick=\"showMovePopup('" + f.path.replace(/'/g,"\\'") + "','" + f.name.replace(/'/g,"\\'") + "')\" class='btn btn-sm btn-secondary'>✂️</button> ";
                        actions += "<button onclick=\"filesDelete('" + f.path.replace(/'/g,"\\'") + "')\" class='btn btn-sm btn-danger'>🗑️</button>";
                    }
                }
                html += "<div class='list-item'><span>" + (f.isDir ? '📂 ':'📄 ') + f.name + (f.isDir ? '' : ' <small class=\"text-muted\">(' + fmtBytes(f.size) + ')</small>') + "</span><span class='btn-group'>" + actions + "</span></div>";
            });
            list.innerHTML = html;
        })
        .catch(function() {
            var list = document.getElementById('files-list');
            if (list) list.innerHTML = "<div class='list-item' style='color:red'>Error loading file list</div>";
        });
}

function filesSetStorage(s) { currentFilesStorage = s; currentFilesDir = '/'; filesRender(); }
function filesEnterDir(d)    { currentFilesDir = d; filesRender(); }
function filesGoUp() {
    var p = currentFilesDir.lastIndexOf('/');
    currentFilesDir = p <= 0 ? '/' : currentFilesDir.substring(0, p);
    filesRender();
}
function filesToggleEdit() { filesEditMode = !filesEditMode; filesRender(); }

function filesDelete(path) {
    if (!confirm('Delete ' + path + '?')) return;
    fetch('/delete?path=' + encodeURIComponent(path) + '&storage=' + currentFilesStorage)
        .then(function() { filesRender(); })
        .catch(function(e) { alert('Error: ' + e); });
}

function filesUpload() {
    var inp = document.getElementById('files-fileInput');
    if (!inp || !inp.files.length) return;
    var files = inp.files, i = 0;
    var prog = document.getElementById('files-uploadProg');
    var bar  = document.getElementById('files-uploadBar');
    var pct  = document.getElementById('files-uploadPct');
    if (prog) prog.style.display = 'block';
    function next() {
        if (i >= files.length) { if(prog) prog.style.display='none'; inp.value=''; filesRender(); return; }
        var fd = new FormData();
        fd.append('file', files[i]);
        fd.append('path', currentFilesDir);
        fd.append('storage', currentFilesStorage);
        var xhr = new XMLHttpRequest();
        xhr.upload.onprogress = function(ev) {
            if (ev.lengthComputable) {
                var p = Math.round(ev.loaded / ev.total * 100);
                if (bar) bar.style.width = p + '%';
                if (pct) pct.textContent = p + '%';
            }
        };
        xhr.onload = function() { i++; next(); };
        xhr.onerror = function() { alert('Upload failed: ' + files[i].name); if(prog) prog.style.display='none'; };
        xhr.open('POST', '/upload');
        xhr.send(fd);
    }
    next();
}

function filesMkdir() {
    var name = document.getElementById('files-newFolder');
    if (!name || !name.value.trim()) return;
    fetch('/mkdir?name=' + encodeURIComponent(name.value.trim()) + '&dir=' + encodeURIComponent(currentFilesDir) + '&storage=' + currentFilesStorage)
        .then(function() { name.value = ''; filesRender(); });
}

var mvSrcPath = '';
function showMovePopup(path, name) {
    mvSrcPath = path;
    var inp = document.getElementById('mv-name');
    if (inp) inp.value = name;
    document.getElementById('movePopup').style.display = 'flex';
}
function filesApplyMove() {
    var newName = getVal('mv-name').trim();
    var destDir = getVal('mv-dest');
    if (!newName) return;
    var url = '/move_file?src=' + encodeURIComponent(mvSrcPath) + '&newName=' + encodeURIComponent(newName) + '&storage=' + currentFilesStorage;
    if (destDir) url += '&destDir=' + encodeURIComponent(destDir);
    fetch(url)
        .then(function() { document.getElementById('movePopup').style.display='none'; filesRender(); })
        .catch(function(e) { alert('Error: ' + e); });
}

// ============================================================================
// ══ PAGE: LIVE ══
// ============================================================================
function liveInit() {
    // Load initial status values
    if (ST.chip) setEl('live-chip', ST.chip);
    if (ST.cpu)  setEl('live-cpu',  ST.cpu);
    if (ST.ip)   setEl('live-ip',   ST.ip);
    if (ST.network) setEl('live-net', ST.network);

    // Hint with timing from status
    var hint = document.getElementById('live-stateHint');
    if (hint && ST.flowMeter) {
        hint.textContent = '🔵 IDLE → 🟡 WAIT_FLOW (' + ST.flowMeter.firstLoop + 's) → 🟢 MONITORING (' + ST.flowMeter.window + 's idle) → Logging';
    }

    liveUpdate();
    liveLogsUpdate();
    liveTimer     = setInterval(liveUpdate,     500);
    liveLogsTimer = setInterval(liveLogsUpdate, 3000);
}

function liveUpdate() {
    fetch('/api/live')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            var conn = document.getElementById('live-conn');
            if (conn) { conn.textContent = '● Connected'; conn.className = 'text-success'; }

            setEl('live-time',       d.time);
            setEl('live-trigger',    d.trigger);
            setEl('live-cycleTime',  d.cycleTime);
            setEl('live-pulses',     d.pulses);
            setEl('live-liters',     parseFloat(d.liters).toFixed(2));
            setEl('live-ffCount',    d.ffCount);
            setEl('live-pfCount',    d.pfCount);
            setEl('live-boot',       d.boot);
            setEl('live-heap',       fmtBytes(d.heap));
            setEl('live-heapTotal',  fmtBytes(d.heapTotal));
            setEl('live-uptime',     d.uptime);
            if (d.fsTotal) setEl('live-storage', fmtBytes(d.fsUsed) + '/' + fmtBytes(d.fsTotal));

            // State
            var stColors = {IDLE:'#3498db', WAIT_FLOW:'#f39c12', MONITORING:'#27ae60', DONE:'#e74c3c'};
            var stEl = document.getElementById('live-state');
            if (stEl) { stEl.textContent = d.state; stEl.style.background = stColors[d.state] || '#95a5a6'; stEl.style.color = '#fff'; }
            var remEl = document.getElementById('live-stateRem');
            if (remEl) remEl.textContent = d.stateRemaining >= 0 ? d.stateRemaining + 's' : '-';

            // Buttons
            liveBtn('live-ff',   d.ff,   'Pressed','Released', '#27ae60','#95a5a6');
            liveBtn('live-pf',   d.pf,   'Pressed','Released', '#27ae60','#95a5a6');
            liveBtn('live-wifi', d.wifi, 'Pressed','Released', '#3498db','#95a5a6');

            // Mode
            var modeEl = document.getElementById('live-mode');
            if (modeEl) {
                if (d.mode === 'online')  modeEl.innerHTML = '🌐 Online Logger';
                else if (d.mode === 'webonly') modeEl.innerHTML = '📶 Web Only';
                else modeEl.innerHTML = '📊 Logging';
            }
        })
        .catch(function() {
            var conn = document.getElementById('live-conn');
            if (conn) { conn.textContent = '● Disconnected'; conn.className = 'text-danger'; }
        });
}

function liveBtn(id, pressed, txtOn, txtOff, colorOn, colorOff) {
    var el = document.getElementById(id);
    if (!el) return;
    el.textContent = pressed ? txtOn : txtOff;
    el.style.background = pressed ? colorOn : colorOff;
}

function liveLogsUpdate() {
    fetch('/api/recent_logs')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            var el = document.getElementById('live-logs');
            if (!el) return;
            var th = ST.theme || {};
            var ffC = th.ffColor || '#3498db', pfC = th.pfColor || '#e74c3c', otC = th.otherColor || '#95a5a6';
            if (d.logs && d.logs.length) {
                var html = '<table style="width:100%;border-collapse:collapse;font-size:.75rem">';
                html += '<tr style="background:var(--bg)"><th style="padding:6px;text-align:left">Time</th><th>Trigger</th><th>Volume</th><th>+FF</th><th>+PF</th></tr>';
                d.logs.forEach(function(l) {
                    var color = l.trigger.indexOf('FF')>=0 ? ffC : l.trigger.indexOf('PF')>=0 ? pfC : otC;
                    var bg = hexToRgba(color, 0.15);
                    html += '<tr style="background:'+bg+'"><td style="padding:6px">'+l.time+'</td><td style="color:'+color+';font-weight:bold;text-align:center">'+l.trigger+'</td><td style="text-align:center">'+l.volume+'</td><td style="text-align:center">'+l.ff+'</td><td style="text-align:center">'+l.pf+'</td></tr>';
                });
                html += '</table>';
                el.innerHTML = html;
            } else {
                el.innerHTML = "<div class='list-item text-muted'>No log entries yet</div>";
            }
        })
        .catch(function() {});
}

// ============================================================================
// ══ SETTINGS: DEVICE ══
// ============================================================================
function sdInit() {
    fetch('/api/status').then(function(r){return r.json();}).then(function(d) {
        ST = d;
        setVal('sd-devName', d.device);
        setVal('sd-devId',   d.deviceId);
        setVal('sd-defaultStorage', d.defaultStorageView || 0);
        setChk('sd-forceWS', d.forceWebServer);

        // System info
        var info = document.getElementById('sd-sysInfo');
        if (info) {
            info.innerHTML =
                '<div><strong>Firmware</strong><div class="text-primary">' + d.version + '</div></div>' +
                '<div><strong>Boot Count</strong><div>' + d.boot + '</div></div>' +
                '<div><strong>Mode</strong><div>' + d.mode + '</div></div>' +
                '<div><strong>Free Heap</strong><div>' + fmtBytes(d.heap) + '</div></div>' +
                '<div><strong>CPU</strong><div>' + d.cpu + ' MHz</div></div>' +
                '<div><strong>Chip</strong><div>' + d.chip + '</div></div>';
        }
    });
}

function regenDevId() {
    if (!confirm('Generate new ID based on MAC address?')) return;
    fetch('/api/regen-id', {method:'POST'})
        .then(function(r){return r.text();})
        .then(function(id) {
            var inp = document.getElementById('sd-devId');
            if (inp) { inp.value = id; inp.disabled = false; }
            alert('New ID generated: ' + id + '. Click Save to apply.');
        })
        .catch(function(e) { alert('Error: ' + e); });
}

function changelogToggle() {
    var el = document.getElementById('sd-changelog');
    if (!el) return;
    el.classList.toggle('hidden');
    if (!changelogLoaded) { changelogLoad(); changelogLoaded = true; }
}

function changelogLoad() {
    var el = document.getElementById('sd-changelog');
    fetch('/api/changelog')
        .then(function(r) { if (!r.ok) throw new Error('not found'); return r.text(); })
        .then(function(txt) {
            var html = '', lines = txt.trim().split('\n'), inVer = false;
            lines.forEach(function(line) {
                line = line.trim(); if (!line) return;
                if (line.startsWith('##')) {
                    if (inVer) html += '</ul></div>';
                    var ver = line.substring(2).trim();
                    var isCur = ver.indexOf('Current')>=0 || lines.indexOf(line)<3;
                    html += '<div style="margin-top:.5rem;padding:.5rem;' + (isCur?'background:var(--primary);color:#fff':'background:var(--bg)') + ';border-radius:4px">';
                    html += '<strong>' + ver + '</strong><ul style="margin:.5rem 0 0 1rem;padding:0;font-size:.9rem">';
                    inVer = true;
                } else if (line.startsWith('-') && inVer) {
                    html += '<li>' + line.substring(1).trim() + '</li>';
                }
            });
            if (inVer) html += '</ul></div>';
            if (el) el.innerHTML = html;
        })
        .catch(function() {
            if (el) el.innerHTML = "<div class='alert alert-warning'>Changelog not found. Upload /changelog.txt</div>";
        });
}

// ============================================================================
// ══ SETTINGS: FLOW METER ══
// ============================================================================
function sfInit() {
    fetch('/api/status').then(function(r){return r.json();}).then(function(d) {
        var fm = d.flowMeter || {};
        setVal('sf-ppl',   fm.pulsesPerLiter);
        setVal('sf-cal',   fm.calibrationMultiplier);
        setVal('sf-win',   fm.monitoringWindowSecs);
        setVal('sf-fl',    fm.firstLoopMonitoringWindowSecs);
        setChk('sf-test',  fm.testMode);
        setVal('sf-blink', fm.blinkDuration);
        setEl('sf-boot',   d.boot);
    });
}

// ============================================================================
// ══ SETTINGS: HARDWARE ══
// ============================================================================
function hwInit() {
    fetch('/api/status').then(function(r){return r.json();}).then(function(d) {
        ST = d;
        var hw = d.hardware || {};
        var th = d.theme   || {};

        setVal('hw-storage',  hw.storageType || 0);
        document.getElementById('hw-sdPins').style.display = hw.storageType == 1 ? 'block' : 'none';
        setVal('hw-sdCS',    hw.pinSdCS);
        setVal('hw-sdMOSI',  hw.pinSdMOSI);
        setVal('hw-sdMISO',  hw.pinSdMISO);
        setVal('hw-sdSCK',   hw.pinSdSCK);
        setVal('hw-wakeup',  hw.wakeupMode || 0);
        setVal('hw-debounce',hw.debounceMs || 50);
        setVal('hw-pinWifi', hw.pinWifiTrigger);
        setVal('hw-pinFF',   hw.pinWakeupFF);
        setVal('hw-pinPF',   hw.pinWakeupPF);
        setVal('hw-pinFlow', hw.pinFlowSensor);
        setVal('hw-rtcCE',   hw.pinRtcCE);
        setVal('hw-rtcIO',   hw.pinRtcIO);
        setVal('hw-rtcCLK',  hw.pinRtcSCLK);
        setVal('hw-cpu',     hw.cpuFreqMHz || 80);

        // Board diagram
        if (th.boardDiagramPath) {
            var card = document.getElementById('hw-boardDiagramCard');
            var img  = document.getElementById('hw-boardDiagram');
            if (card) card.style.display = 'block';
            if (img)  img.src = th.boardDiagramPath;
        }
    });
}

// ============================================================================
// ══ SETTINGS: THEME ══
// ============================================================================
function thInit() {
    fetch('/api/status').then(function(r){return r.json();}).then(function(d) {
        var th = d.theme || {};
        setVal('th-mode',      th.mode || 0);
        setChk('th-icons',     th.showIcons);
        setVal('th-primary',   th.primaryColor);
        setVal('th-secondary', th.secondaryColor);
        setVal('th-bg',        th.bgColor);
        setVal('th-text',      th.textColor);
        setVal('th-ff',        th.ffColor);
        setVal('th-pf',        th.pfColor);
        setVal('th-other',     th.otherColor);
        setVal('th-bar',       th.storageBarColor);
        setVal('th-bar70',     th.storageBar70Color);
        setVal('th-bar90',     th.storageBar90Color);
        setVal('th-barB',      th.storageBarBorder);
        setVal('th-logo',      th.logoSource);
        setVal('th-favicon',   th.faviconPath);
        setVal('th-board',     th.boardDiagramPath);
        setVal('th-chartSrc',  th.chartSource || 0);
        document.getElementById('th-chartPathRow').style.display = (th.chartSource==0||!th.chartSource) ? 'block' : 'none';
        setVal('th-chartPath', th.chartLocalPath);
        setVal('th-labelFmt',  th.chartLabelFormat || 0);
    });
}

// ============================================================================
// ══ SETTINGS: NETWORK ══
// ============================================================================
function netInit() {
    fetch('/api/status').then(function(r){return r.json();}).then(function(d) {
        ST = d;
        setEl('net-status', d.wifi === 'client' ? 'Connected: ' + d.network : 'AP Mode');
        setEl('net-ip', d.ip);
    });
    // Fetch network-specific config from export
    fetch('/export_settings')
        .then(function(r){return r.json();})
        .then(function(d) {
            var net = d.network || {};
            setVal('net-mode', net.wifiMode || 0);
            netToggleMode();
            setVal('net-apSSID', net.apSSID);
            setVal('net-apPass', net.apPassword || '');
            setVal('net-apIP',   net.apIP || '192.168.4.1');
            setVal('net-apGW',   net.apGateway || '192.168.4.1');
            setVal('net-apSN',   net.apSubnet || '255.255.255.0');
            setVal('net-cSSID',  net.clientSSID);
            setVal('net-cPass',  net.clientPassword || '');
            setChk('net-staticCheck', net.useStaticIP);
            setVal('net-ip2',  net.staticIP  || '');
            setVal('net-gw',   net.gateway   || '');
            setVal('net-sn',   net.subnet    || '');
            setVal('net-dns',  net.dns       || '');
            netToggleStatic();
        });
}

function netToggleMode() {
    var m = getVal('net-mode');
    document.getElementById('net-apSection').style.display     = m==='0' ? 'block' : 'none';
    document.getElementById('net-clientSection').style.display = m==='1' ? 'block' : 'none';
}

function netToggleStatic() {
    var en = document.getElementById('net-staticCheck') && document.getElementById('net-staticCheck').checked;
    ['net-ip2','net-gw','net-sn','net-dns'].forEach(function(id) {
        var el = document.getElementById(id);
        if (el) { el.disabled = !en; el.style.opacity = en ? '1' : '0.5'; el.style.cursor = en ? 'text' : 'not-allowed'; }
    });
}

function netScanWifi() {
    var list = document.getElementById('net-wifiList');
    if (!list) return;
    list.innerHTML = "<div class='list-item'>🔍 Scanning…</div>"; list.style.display = 'block';
    netScanRetries = 0;
    fetch('/wifi_scan_start').then(function() { setTimeout(netCheckScan, 2000); });
}
function netCheckScan() {
    fetch('/wifi_scan_result').then(function(r){return r.json();}).then(function(d) {
        var list = document.getElementById('net-wifiList'); if (!list) return;
        if (d.scanning) {
            netScanRetries++;
            if (netScanRetries < 10) { list.innerHTML = "<div class='list-item'>🔍 Scanning… (" + netScanRetries + ")</div>"; setTimeout(netCheckScan, 1000); }
            else list.innerHTML = "<div class='list-item'>⏱️ Scan timeout</div>";
        } else if (d.error) {
            list.innerHTML = "<div class='list-item'>❌ " + d.error + "</div>";
        } else if (!d.networks || !d.networks.length) {
            list.innerHTML = "<div class='list-item'>📡 No networks found</div>";
        } else {
            var h = '';
            d.networks.forEach(function(n) {
                h += "<div class='list-item' style='cursor:pointer' onclick=\"document.getElementById('net-cSSID').value='" + n.ssid.replace(/'/g,"\\'") + "';document.getElementById('net-wifiList').style.display='none'\">";
                h += (n.secure?'🔒':'📶') + ' ' + n.ssid + ' <small class="text-muted">(' + n.rssi + ' dBm)</small></div>';
            });
            list.innerHTML = h;
        }
    }).catch(function(e) {
        var list = document.getElementById('net-wifiList');
        if (list) list.innerHTML = "<div class='list-item'>❌ Error: " + e + "</div>";
    });
}

// ============================================================================
// ══ SETTINGS: TIME ══
// ============================================================================
function timeInit() {
    fetch('/api/status').then(function(r){return r.json();}).then(function(d) {
        ST = d;
        setEl('time-rtcTime', d.time || '--:--:--');
        setEl('time-boot',    d.boot);

        var status = document.getElementById('time-rtcStatus');
        if (status) {
            if (!d.rtcRunning) status.innerHTML = "<div class='alert alert-error'>❌ RTC Error</div>";
            else status.innerHTML = "<div class='alert alert-success'>✅ RTC OK</div>";
        }
        var detail = document.getElementById('time-rtcDetail');
        if (detail) detail.textContent = 'Protected: ' + (d.rtcProtected?'Yes':'No') + ' | Running: ' + (d.rtcRunning?'Yes':'No');
        setChk('time-rtcProt', d.rtcProtected);

        // NTP status
        var ntpSt = document.getElementById('time-ntpStatus');
        if (ntpSt) ntpSt.innerHTML = d.wifi==='client' ? "<div class='alert alert-success'>✅ Connected</div>" : "<div class='alert alert-warning'>⚠️ Not connected (AP mode)</div>";

        // Set today as default date
        var dateEl = document.getElementById('time-date');
        if (dateEl && !dateEl.value) dateEl.value = new Date().toISOString().slice(0,10);
    });

    fetch('/export_settings').then(function(r){return r.json();}).then(function(d) {
        var net = d.network || {};
        setVal('time-ntp', net.ntpServer || 'pool.ntp.org');
        setVal('time-tz',  net.timezone  || 0);
    });
}

function timeSetManual(ev) {
    ev.preventDefault();
    var fd = new FormData();
    fd.append('date', getVal('time-date'));
    fd.append('time', getVal('time-time'));
    fetch('/set_time', {method:'POST', body:fd})
        .then(function(r){return r.json();})
        .then(function(d) {
            showMsg('time-msg', d.ok ? "<div class='alert alert-success'>✅ Time set successfully!</div>" : "<div class='alert alert-error'>❌ " + (d.error||'Failed') + "</div>", true);
        });
}
function timeSyncNTP(ev) {
    if (ev) ev.preventDefault();
    fetch('/sync_time', {method:'POST'})
        .then(function(r){return r.json();})
        .then(function(d) {
            showMsg('time-msg', d.ok ? "<div class='alert alert-success'>✅ Time synced!</div>" : "<div class='alert alert-error'>❌ Sync failed</div>", true);
            if (d.ok) timeInit();
        });
}
function timeRtcProtect(ev) {
    if (ev) ev.preventDefault();
    var fd = new FormData();
    if (document.getElementById('time-rtcProt') && document.getElementById('time-rtcProt').checked) fd.append('protect', '1');
    fetch('/rtc_protect', {method:'POST', body:fd});
}
function timeFlushLogs() {
    fetch('/flush_logs', {method:'POST'})
        .then(function() { showMsg('time-msg', "<div class='alert alert-success'>✅ Log buffer flushed</div>", true); });
}
function timeBackupBoot() {
    fetch('/backup_bootcount', {method:'POST'})
        .then(function() { showMsg('time-msg', "<div class='alert alert-success'>✅ Boot count backed up</div>", true); });
}
function timeRestoreBoot() {
    fetch('/restore_bootcount', {method:'POST'}).then(function(r){return r.json();}).then(function(d) {
        showMsg('time-msg', d.ok ? "<div class='alert alert-success'>✅ Restored: " + d.old + " → " + d.new + "</div>" : "<div class='alert alert-error'>❌ Restore failed</div>", true);
        timeInit();
    });
}

// ============================================================================
// ══ SETTINGS: DATALOG ══
// ============================================================================
function dlInit() {
    // Load log file list
    fetch('/api/filelist?filter=log&recursive=1').then(function(r){return r.json();}).then(function(d) {
        var sel = document.getElementById('dl-curFile'); if (!sel) return;
        sel.innerHTML = '';
        (d.files||[]).forEach(function(f) {
            var opt = document.createElement('option');
            opt.value = f.path; opt.textContent = f.path;
            sel.appendChild(opt);
        });
        // Load full config to select current
        fetch('/export_settings').then(function(r){return r.json();}).then(function(cfg) {
            var dl = cfg.datalog || {};
            if (dl.currentFile) sel.value = dl.currentFile;
            setVal('dl-prefix',    dl.prefix   || 'datalog');
            setVal('dl-folder',    dl.folder   || '');
            setVal('dl-rotation',  dl.rotation || 0);
            document.getElementById('dl-maxSizeGroup').style.display = dl.rotation==4 ? 'block':'none';
            setVal('dl-maxSize',   dl.maxSizeKB || 500);
            setChk('dl-tsFile',    dl.timestampFilename);
            setChk('dl-devId',     dl.includeDeviceId);
            setVal('dl-date',      dl.dateFormat  || 0);
            setVal('dl-time',      dl.timeFormat  || 0);
            setVal('dl-end',       dl.endFormat   || 0);
            setVal('dl-boot',      dl.includeBootCount ? '1' : '0');
            setVal('dl-vol',       dl.volumeFormat || 0);
            setVal('dl-extra',     dl.includeExtraPresses ? '1' : '0');
            setChk('dl-pcEnabled', dl.postCorrectionEnabled);
            document.getElementById('dl-pcFields').style.display = dl.postCorrectionEnabled ? 'block':'none';
            setVal('dl-pfff',      dl.pfToFfThreshold);
            setVal('dl-ffpf',      dl.ffToPfThreshold);
            setVal('dl-hold',      dl.manualPressThresholdMs);
            dlUpdatePreview();
        });
    });

    // Files list
    dlLoadFiles();
}

function dlLoadFiles() {
    var el = document.getElementById('dl-files'); if (!el) return;
    fetch('/api/filelist?filter=log&recursive=1').then(function(r){return r.json();}).then(function(d) {
        var files = d.files || [];
        if (!files.length) { el.innerHTML = "<div class='list-item text-muted'>No log files</div>"; return; }
        var curFile = ST.currentFile || getVal('dl-curFile');
        var html = '';
        files.forEach(function(f) {
            var isCur = f.path === curFile;
            html += "<div class='list-item'><span>" + (isCur ? "<strong class='text-success'>✔ " : '') + f.path + ' <small class="text-muted">(' + fmtBytes(f.size) + ')</small>' + (isCur ? '</strong>' : '') + "</span>";
            html += "<span class='btn-group'><a href='/download?file=" + encodeURIComponent(f.path) + "' class='btn btn-sm btn-secondary'>📥</a>";
            if (!isCur) html += " <button onclick=\"dlDeleteFile('" + f.path.replace(/'/g,"\\'") + "')\" class='btn btn-sm btn-danger'>🗑️</button>";
            html += '</span></div>';
        });
        el.innerHTML = html;
    });
}

function dlDeleteFile(path) {
    if (!confirm('Delete ' + path + '?')) return;
    fetch('/delete?path=' + encodeURIComponent(path))
        .then(function() { dlLoadFiles(); });
}

function dlUpdatePreview() {
    var p = [], d = new Date();
    var df = getVal('dl-date'), tf = getVal('dl-time'), ef = getVal('dl-end');
    var dd = String(d.getDate()).padStart(2,'0'), mm = String(d.getMonth()+1).padStart(2,'0'), yy = d.getFullYear();
    var hh = String(d.getHours()).padStart(2,'0'), mi = String(d.getMinutes()).padStart(2,'0'), ss = String(d.getSeconds()).padStart(2,'0');

    if (df==='1') p.push(dd+'/'+mm+'/'+yy);
    else if (df==='2') p.push(mm+'/'+dd+'/'+yy);
    else if (df==='3') p.push(yy+'-'+mm+'-'+dd);
    else if (df==='4') p.push(dd+'.'+mm+'.'+yy);

    var tStr = '';
    if (tf==='0') tStr = hh+':'+mi+':'+ss;
    else if (tf==='1') tStr = hh+':'+mi;
    else { var h = d.getHours()%12||12; tStr = h+':'+mi+':'+ss+(d.getHours()<12?'AM':'PM'); }
    p.push(tStr);

    if (ef==='0') p.push(tStr);
    else if (ef==='1') p.push('45s');

    if (getVal('dl-boot')==='1') p.push('#:1234');
    p.push('FF_BTN');

    var vf = getVal('dl-vol');
    if (vf==='0') p.push('L:2,50');
    else if (vf==='1') p.push('L:2.50');
    else if (vf==='2') p.push('2.50');

    if (getVal('dl-extra')==='1') { p.push('FF0'); p.push('PF1'); }

    setEl('dl-preview', p.join('|'));
}

// ============================================================================
// ══ SETTINGS: IMPORT / EXPORT ══
// ============================================================================
function settingsImport() {
    var file = document.getElementById('importFile');
    if (!file || !file.files.length) return;
    var fd = new FormData();
    fd.append('settings', file.files[0]);
    var prog = document.getElementById('importProg');
    var bar  = document.getElementById('importBar');
    var pct  = document.getElementById('importPct');
    if (prog) prog.style.display = 'block';
    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function(ev) {
        if (ev.lengthComputable) {
            var p = Math.round(ev.loaded / ev.total * 100);
            if (bar) bar.style.width = p + '%';
            if (pct) pct.textContent = p + '%';
        }
    };
    xhr.onload = function() {
        if (xhr.status === 200) { alert('Settings imported successfully!'); location.reload(); }
        else { alert('Import failed: ' + xhr.responseText); if (prog) prog.style.display = 'none'; }
    };
    xhr.onerror = function() { alert('Import failed'); if (prog) prog.style.display = 'none'; };
    xhr.open('POST', '/import_settings');
    xhr.send(fd);
}

// ============================================================================
// ══ OTA UPDATE ══
// ============================================================================
function otaUpload() {
    var file = document.getElementById('ota-file');
    if (!file || !file.files.length) { alert('Select a .bin file first'); return; }
    var fd = new FormData();
    fd.append('update', file.files[0]);
    var prog = document.getElementById('ota-prog');
    var bar  = document.getElementById('ota-bar');
    var pct  = document.getElementById('ota-pct');
    var msg  = document.getElementById('ota-msg');
    if (prog) prog.style.display = 'block';
    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function(ev) {
        if (ev.lengthComputable) {
            var p = Math.round(ev.loaded / ev.total * 100);
            if (bar) bar.style.width = p + '%';
            if (pct) pct.textContent = p + '%';
        }
    };
    xhr.onload = function() {
        try {
            var r = JSON.parse(xhr.responseText);
            if (msg) msg.innerHTML = r.success ? "<div class='alert alert-success'>✅ " + r.message + "</div>" : "<div class='alert alert-error'>❌ " + r.message + "</div>";
        } catch(e) {
            if (msg) msg.innerHTML = "<div class='alert alert-success'>✅ Update sent, device restarting…</div>";
        }
    };
    xhr.onerror = function() {
        if (msg) msg.innerHTML = "<div class='alert alert-error'>❌ Upload failed</div>";
    };
    xhr.open('POST', '/do_update');
    xhr.send(fd);
}
