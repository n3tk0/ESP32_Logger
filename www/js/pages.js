/**
 * /www/js/pages.js — dashboard, files, live pages
 * Loaded after core.js. Depends on globals: ST, CFG, currentPage,
 * setEl/setVal/getVal/showToast/showMsg, navigateTo, fmtBytes.
 */
"use strict";

// ============================================================================
// ══ PAGE: DASHBOARD ══
// uPlot replaces Chart.js as the sole chart engine.  uPlot is canvas-based,
// ~50 KB gzipped, and renders the time-series workloads we need much faster
// than Chart.js.  serveStatic auto-serves a `.gz` sibling so a single
// uPlot.iife.min.js.gz on LittleFS is enough.
// ============================================================================
var _uPlotLoading = false;
var _uPlotLoaded  = false;
var _uPlotCbs     = [];

function dbLoadUPlot(cb) {
  if (_uPlotLoaded || typeof uPlot !== "undefined") {
    _uPlotLoaded = true;
    cb();
    return;
  }
  _uPlotCbs.push(cb);
  if (_uPlotLoading) return;
  _uPlotLoading = true;

  var th = ST.theme || CFG.theme || {};
  var preferLocal = th.chartSource === 0 || th.chartSource === "0";
  var localSrc = th.chartLocalPath || "/uPlot.iife.min.js";
  var cdnSrc   = "https://cdn.jsdelivr.net/npm/uplot@1/dist/uPlot.iife.min.js";
  var localCss = "/uPlot.min.css";
  var cdnCss   = "https://cdn.jsdelivr.net/npm/uplot@1/dist/uPlot.min.css";

  function fire() {
    _uPlotLoaded = true;
    _uPlotLoading = false;
    _uPlotCbs.forEach(function (fn) { fn(); });
    _uPlotCbs = [];
  }

  function _giveUp() {
    _uPlotLoading = false;
    var err = document.getElementById("errorMsg");
    if (err) {
      err.innerHTML =
        "<strong>Charts unavailable:</strong> Could not load <code>uPlot.iife.min.js</code>. " +
        "If you removed it from LittleFS, re-upload it via /upload.";
      err.style.display = "block";
    }
    if (typeof showToast === "function") {
      showToast("Failed to load uPlot.iife.min.js", "error");
    }
  }

  // Stylesheet: try preferred source, fall back to the other on error.
  // Best-effort — uPlot renders without its CSS (just loses some grid styling).
  var link = document.createElement("link");
  link.rel  = "stylesheet";
  link.href = preferLocal ? localCss : cdnCss;
  link.onerror = function () {
    var fallbackCss = preferLocal ? cdnCss : localCss;
    if (fallbackCss && link.href !== fallbackCss) {
      var link2 = document.createElement("link");
      link2.rel  = "stylesheet";
      link2.href = fallbackCss;
      document.head.appendChild(link2);
    }
  };
  document.head.appendChild(link);

  // Always try local first; fall back to CDN only on 404 (local missing).
  var s = document.createElement("script");
  s.src = localSrc;
  s.onload = fire;
  s.onerror = function () {
    var isOffline = (ST.wifi === "ap") || (CFG.network && CFG.network.wifiMode === 0);
    if (isOffline) {
      showToast("Offline AP mode: upload uPlot.iife.min.js(.gz) to LittleFS.", "error");
      _giveUp();
      return;
    }
    var s2 = document.createElement("script");
    s2.src = cdnSrc;
    s2.onload = fire;
    s2.onerror = _giveUp;
    document.head.appendChild(s2);
  };
  document.head.appendChild(s);
}

// ============================================================================
// SMART DASHBOARD — sensor cards with uPlot sparklines + /api/latest polling.
// State is held in module-scope vars so dbStopPolling() (called from core.js
// on navigation away) can reach the timer/charts without a global hop.
// ============================================================================
var dbCardCharts = {};   // key = `${id}::${metric}` → uPlot instance
var dbPollTimer  = null;
var dbRangeSec   = 3600; // default 1h, persisted in localStorage
var DB_POLL_MS   = 60000;

function dbStorageKey() { return "dashboard.v1"; }

function dbLoadPrefs() {
  try {
    var s = localStorage.getItem(dbStorageKey());
    if (!s) return {};
    var p = JSON.parse(s) || {};
    if (typeof p.rangeSec === "number") dbRangeSec = p.rangeSec;
    return p;
  } catch (e) { return {}; }
}

function dbSavePrefs(patch) {
  var p = dbLoadPrefs();
  Object.keys(patch || {}).forEach(function (k) { p[k] = patch[k]; });
  try { localStorage.setItem(dbStorageKey(), JSON.stringify(p)); } catch (e) {}
}

function dbCardKey(id, metric) { return id + "::" + metric; }

function dbInit() {
  var prefs = dbLoadPrefs();
  var rangeEl = document.getElementById("db-range");
  if (rangeEl) {
    if (prefs.rangeSec) rangeEl.value = String(prefs.rangeSec);
    dbRangeSec = parseInt(rangeEl.value, 10) || 3600;
  }
  dbLoadUPlot(function () { dbLoadCards(); });
}

function dbRangeChange(ev) {
  var v = parseInt(ev.target.value, 10);
  if (isNaN(v) || v <= 0) return;
  dbRangeSec = v;
  dbSavePrefs({ rangeSec: v });
  dbDestroyAllCharts();
  dbLoadCards();
}

function dbRefreshNow() {
  var status = document.getElementById("db-poll-status");
  if (status) { status.textContent = "🔄"; status.className = "text-primary"; }
  dbRefreshLatest();
}

function dbDestroyAllCharts() {
  Object.keys(dbCardCharts).forEach(function (k) {
    try { dbCardCharts[k].destroy(); } catch (e) {}
  });
  dbCardCharts = {};
}

function dbStartPolling() {
  if (dbPollTimer) return;
  dbPollTimer = setInterval(dbRefreshLatest, DB_POLL_MS);
}

function dbStopPolling() {
  if (dbPollTimer) { clearInterval(dbPollTimer); dbPollTimer = null; }
  dbDestroyAllCharts();
}

// Build the card grid from /api/sensors, then fetch a sparkline series
// (/api/data) per (sensor, metric) and seed each card chart.  Once cards
// are mounted, /api/latest polling drives the value labels in place.
function dbLoadCards() {
  var grid    = document.getElementById("db-cards");
  var loading = document.getElementById("db-loading");
  var empty   = document.getElementById("db-empty");
  if (!grid) return;
  if (empty)   empty.style.display   = "none";
  if (loading) loading.style.display = "";
  grid.style.display = "none";

  fetch("/api/sensors")
    .then(function (r) { return r.json(); })
    .then(function (d) {
      var sensors = (d && d.sensors) || [];
      // Each sensor exposes one or more metrics — we render one card per
      // (sensor, metric) pair so the user can see the full breakdown.
      var pairs = [];
      sensors.forEach(function (s) {
        if (s.status === "disabled") return;
        (s.metrics || []).forEach(function (m) {
          pairs.push({ id: s.id, type: s.type, name: s.name, metric: m });
        });
      });

      if (loading) loading.style.display = "none";
      if (!pairs.length) {
        if (empty) empty.style.display = "";
        return;
      }

      grid.innerHTML = pairs.map(dbBuildCardHtml).join("");
      grid.style.display = "";

      // Pull the spark series in parallel — chart creation is cheap so we
      // start polling /api/latest once they're all mounted.
      Promise.all(pairs.map(dbLoadSparkSeries))
        .then(function () {
          dbRefreshLatest();
          dbStartPolling();
        });
    })
    .catch(function (e) {
      if (loading) loading.style.display = "none";
      var err = document.getElementById("errorMsg");
      if (err) {
        err.textContent = "Failed to load sensor list: " + e;
        err.style.display = "block";
      }
    });
}

function dbBuildCardHtml(p) {
  var key = dbCardKey(p.id, p.metric);
  return ''
    + '<div class="sensor-card-mini" data-key="' + esc(key) + '">'
    +   '<div class="sensor-card-mini-header">'
    +     '<span class="sensor-card-mini-name" title="' + esc(p.id) + '">' + esc(p.name || p.id) + '</span>'
    +     '<span class="sensor-card-mini-metric">' + esc(p.metric) + '</span>'
    +   '</div>'
    +   '<div class="sensor-card-mini-value">'
    +     '<span class="value" data-card-value="' + esc(key) + '">--</span>'
    +     '<span class="unit" data-card-unit="' + esc(key) + '"></span>'
    +   '</div>'
    +   '<div class="sensor-card-mini-spark" data-card-spark="' + esc(key) + '"></div>'
    +   '<div class="sensor-card-mini-foot">'
    +     '<span class="ts" data-card-ts="' + esc(key) + '">—</span>'
    +     '<span class="quality" data-card-q="' + esc(key) + '"></span>'
    +   '</div>'
    + '</div>';
}

function dbLoadSparkSeries(p) {
  var key  = dbCardKey(p.id, p.metric);
  var now  = Math.floor(Date.now() / 1000);
  var from = now - dbRangeSec;
  // Pick a bucket that yields ~30 spark points across the chosen range.
  var bucket = "raw";
  if (dbRangeSec >= 86400)      bucket = "1h";
  else if (dbRangeSec >= 43200) bucket = "1h";
  else if (dbRangeSec >= 3600)  bucket = "5m";
  else if (dbRangeSec >= 900)   bucket = "1m";
  var url = "/api/data?sensor=" + encodeURIComponent(p.id)
          + "&metric=" + encodeURIComponent(p.metric)
          + "&from=" + from + "&to=" + now
          + "&agg=" + bucket + "&mode=lttb&limit=60";
  return fetchWithTimeout(url)
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      var pts = (d && d.data) || [];
      var xs = pts.map(function (pt) { return pt.ts; });
      var ys = pts.map(function (pt) { return pt.v; });
      dbMountSparkChart(key, xs, ys);
    })
    .catch(function () { /* card stays without spark; foot shows '—' */ });
}

function dbMountSparkChart(key, xs, ys) {
  var host = document.querySelector('[data-card-spark="' + key + '"]');
  if (!host || typeof uPlot === "undefined") return;
  if (dbCardCharts[key]) {
    try { dbCardCharts[key].destroy(); } catch (e) {}
  }
  if (!xs.length) {
    host.innerHTML = '<span class="sensor-card-mini-empty">no data</span>';
    return;
  }
  host.innerHTML = "";
  var rootStyle = getComputedStyle(document.documentElement);
  var stroke = rootStyle.getPropertyValue("--primary").trim() || "#275673";
  dbCardCharts[key] = new uPlot({
    width:  host.clientWidth || 200,
    height: host.clientHeight || 48,
    pxAlign: false,
    cursor: { show: false },
    legend: { show: false },
    scales: { x: { time: true }, y: {} },
    axes:   [{ show: false }, { show: false }],
    series: [
      {},
      { stroke: stroke, width: 1.5, points: { show: false }, fill: stroke + "22" },
    ],
  }, [xs, ys], host);
}

function dbRefreshLatest() {
  fetchWithTimeout("/api/latest")
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      var status = document.getElementById("db-poll-status");
      if (status) {
        status.textContent = "✅ " + new Date().toLocaleTimeString();
        status.className = "text-success";
      }
      var items = (d && d.items) || [];
      items.forEach(function (it) {
        var key = dbCardKey(it.id, it.metric);
        var v = document.querySelector('[data-card-value="' + key + '"]');
        var u = document.querySelector('[data-card-unit="' + key + '"]');
        var t = document.querySelector('[data-card-ts="' + key + '"]');
        var q = document.querySelector('[data-card-q="' + key + '"]');
        if (v) v.textContent = (it.value !== undefined && it.value !== null)
          ? Number(it.value).toPrecision(4) : "--";
        if (u) u.textContent = it.unit ? " " + it.unit : "";
        if (t) t.textContent = it.ts ? new Date(it.ts * 1000).toLocaleTimeString() : "—";
        if (q) {
          q.textContent =
            it.q === 1 ? "" :
            it.q === 2 ? "est" :
            it.q === 3 ? "err" : "?";
          q.className = "quality" + (it.q === 3 ? " text-danger" : "");
        }
      });
    })
    .catch(function () {
      var status = document.getElementById("db-poll-status");
      if (status) {
        status.textContent = "⚠️ offline";
        status.className = "text-warning";
      }
    });
}

// ============================================================================
// LEGACY LOG VIEWER (per-fill flowmeter pipe-delimited TXT, PLATFORM_LEGACY)
// Hosted on its own page (`#logs`); reuses the existing dbLoadData /
// dbApplyFilters / dbExportCSV / dbRenderChart pipeline below.
// ============================================================================
function logsInit() {
  dbLoadUPlot(function () {
    fetch("/api/filelist?filter=log&recursive=1")
      .then(function (r) { return r.json(); })
      .then(function (d) {
        var sel = document.getElementById("fileSelect");
        if (!sel) return;
        sel.innerHTML = "";
        var curFile = d.currentFile || ST.currentFile || "";
        if (!d.files || !d.files.length) {
          sel.innerHTML = "<option>No log files found</option>";
          return;
        }
        d.files.forEach(function (f) {
          var opt = document.createElement("option");
          opt.value = f.path;
          opt.textContent = f.path;
          if (curFile && f.path === curFile) opt.selected = true;
          sel.appendChild(opt);
        });
        dbLoadData();
      })
      .catch(function (e) {
        var err = document.getElementById("logsErrorMsg");
        if (err) {
          err.textContent = "Error loading file list: " + e.message;
          err.style.display = "block";
        }
      });
  });
}

// Matches original: function loadData()
function dbLoadData() {
  var file = getVal("fileSelect");
  if (!file || file === "No log files found") return;
  var err = document.getElementById("logsErrorMsg");
  if (err) err.style.display = "none";
  fetch("/download?file=" + encodeURIComponent(file))
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.text();
    })
    .then(function (data) {
      dbRawData = data;
      dbApplyFilters();
    })
    .catch(function (e) {
      if (err) {
        err.textContent = "Error loading: " + e.message;
        err.style.display = "block";
      }
    });
}

// Matches original: function applyFilters()
function dbApplyFilters() {
  if (!dbRawData) {
    dbLoadData();
    return;
  }
  dbProcessData(dbRawData);
}

// Matches original: function processData(data) — exact port of .ino embedded JS
function dbProcessData(data) {
  var lines = data.trim().split("\n");
  var filtered = [];
  var startVal = getVal("startDate");
  var endVal = getVal("endDate");
  var filterType = getVal("eventFilter");
  var pressType = getVal("pressFilter");
  var excZ =
    document.getElementById("excludeZero") &&
    document.getElementById("excludeZero").checked;
  var tVol = 0,
    tFF = 0,
    tPF = 0;

  lines.forEach(function (line) {
    var p = line.split("|");
    if (p.length < 2) return;
    var dateStr = "",
      timeStr = "",
      endStr = "",
      boot = "",
      reason = "",
      vol = 0,
      ff = 0,
      pf = 0,
      i = 0;

    // Auto-detect date format (DD/MM/YYYY ┬╖ DD.MM.YYYY ┬╖ YYYY-MM-DD)
    if (
      p[0].match(/\d{2}[\/\.\-]\d{2}[\/\.\-]\d{4}/) ||
      p[0].match(/\d{4}\-\d{2}\-\d{2}/)
    ) {
      dateStr = p[0];
      i = 1;
    }
    if (p[i] && p[i].indexOf(":") >= 0) {
      timeStr = p[i];
      i++;
    }
    if (p[i] && (p[i].indexOf(":") >= 0 || p[i].match(/^\d+s$/))) {
      endStr = p[i];
      i++;
    }
    if (p[i] && p[i].indexOf("#:") === 0) {
      boot = p[i].substring(2);
      i++;
    }
    if (
      p[i] &&
      (p[i].indexOf("FF") >= 0 || p[i].indexOf("PF") >= 0 || p[i] === "IDLE")
    ) {
      reason = p[i];
      i++;
    }
    if (p[i]) {
      var vs = p[i].replace("L:", "").replace(",", ".");
      vol = parseFloat(vs) || 0;
      i++;
    }
    if (p[i] && p[i].indexOf("FF") === 0) {
      ff = parseInt(p[i].replace("FF", "")) || 0;
      i++;
    }
    if (p[i] && p[i].indexOf("PF") === 0) {
      pf = parseInt(p[i].replace("PF", "")) || 0;
    }

    var entryDate = "";
    if (dateStr) {
      var m;
      if ((m = dateStr.match(/(\d{2})[\/\.](\d{2})[\/\.](\d{4})/)))
        entryDate = m[3] + "-" + m[2] + "-" + m[1];
      else if ((m = dateStr.match(/(\d{4})\-(\d{2})\-(\d{2})/)))
        entryDate = m[1] + "-" + m[2] + "-" + m[3];
    }

    // Filters — exact logic from original .ino
    if (startVal && entryDate && entryDate < startVal) return;
    if (endVal && entryDate && entryDate > endVal) return;
    if (
      filterType === "BTN" &&
      reason.indexOf("FF") < 0 &&
      reason.indexOf("PF") < 0
    )
      return;
    if (filterType === "FF" && reason.indexOf("FF") < 0) return;
    if (filterType === "PF" && reason.indexOf("PF") < 0) return;
    if (pressType === "EXTRA" && ff === 0 && pf === 0) return;
    if (pressType === "NONE" && (ff > 0 || pf > 0)) return;
    if (excZ && vol === 0) return;

    tFF += ff;
    tPF += pf;
    tVol += vol;
    var fullTime = timeStr + (endStr ? "-" + endStr : "");
    filtered.push({
      date: dateStr || "N/A",
      time: timeStr,
      fullTime: fullTime,
      boot: boot,
      vol: vol,
      reason: reason,
      ff: ff,
      pf: pf,
    });
  });

  dbFilteredData = filtered;
  // Element IDs match original: totalVol, eventCount, totalFF, totalPF
  setEl("db-totalVol", tVol.toFixed(2) + " L");
  setEl("db-eventCount", filtered.length);
  setEl("db-totalFF", tFF);
  setEl("db-totalPF", tPF);
  dbRenderChart(filtered);
}

// uPlot port of the legacy bar chart.  uPlot is a time-series engine, so
// each entry becomes a (x, volume) point with vertical bars drawn via the
// `paths` builder.  Per-bar coloring (FF / PF / other) is preserved via a
// custom paths function that emits one stroke per bar.
function dbRenderChart(data) {
  var ctx = document.getElementById("chart");
  if (!ctx) return;
  if (typeof uPlot === "undefined") {
    dbLoadUPlot(function () { dbRenderChart(data); });
    return;
  }
  if (dbChart) { dbChart.destroy(); dbChart = null; }

  var th = ST.theme || CFG.theme || {};
  var rootStyle = getComputedStyle(document.documentElement);
  var ffColor =
    th.ffColor || rootStyle.getPropertyValue("--ff-color").trim() || "#275673";
  var pfColor =
    th.pfColor || rootStyle.getPropertyValue("--pf-color").trim() || "#7eb0d5";
  var otherColor =
    th.otherColor || rootStyle.getPropertyValue("--other-color").trim() || "#a0aec0";

  // Synthetic x-axis: bar index → ts within session.  Real RTC times exist on
  // each entry but the legacy TXT logs don't always carry full ISO dates, so
  // we sequence them.  Chunk F's smart dashboard reads from CSV and uses
  // real epochs.
  var xs = data.map(function (_, i) { return i; });
  var ys = data.map(function (d) { return d.vol; });
  var clr = data.map(function (d) {
    if (d.reason.indexOf("FF") >= 0) return ffColor;
    if (d.reason.indexOf("PF") >= 0) return pfColor;
    return otherColor;
  });

  // uPlot custom paths: vertical bars from baseline to value, one color per bar.
  function barPaths(u, seriesIdx) {
    var data = u.data, idxs = data[0], vals = data[seriesIdx];
    var fill = new Path2D();
    var bw = Math.max(1, Math.floor((u.bbox.width / Math.max(1, idxs.length)) * 0.7));
    for (var i = 0; i < idxs.length; i++) {
      if (vals[i] == null) continue;
      var cx = u.valToPos(idxs[i], "x", true);
      var y0 = u.valToPos(0,       "y", true);
      var y1 = u.valToPos(vals[i], "y", true);
      fill.rect(cx - bw / 2, Math.min(y0, y1), bw, Math.abs(y1 - y0));
    }
    return { fill: fill, stroke: null };
  }

  // Per-bar fill: uPlot doesn't natively colorize per-point in a single
  // series, so draw each colored group as its own series using `bands`.
  // For simplicity here we use a single series with the FF color and emit
  // distinct colored series only when the dataset has both kinds.
  var colorGroups = {};
  data.forEach(function (d, i) {
    var c = clr[i];
    if (!colorGroups[c]) colorGroups[c] = [];
    colorGroups[c].push(i);
  });

  var lblFmt = th.chartLabelFormat !== undefined ? th.chartLabelFormat : 0;
  function formatLabel(i) {
    var d = data[i] || {};
    if (lblFmt === 1) return d.boot ? "#" + d.boot : "#?";
    if (lblFmt === 2) return d.date + " " + d.time + (d.boot ? " #" + d.boot : "");
    return d.date + " " + d.time;
  }

  var series = [{}, { label: "Liters (L)", stroke: ffColor, fill: ffColor, paths: barPaths }];
  var seriesData = [xs, ys];

  ctx.innerHTML = "";
  dbChart = new uPlot({
    width: ctx.clientWidth || 600,
    height: ctx.clientHeight || 320,
    scales: { x: { time: false }, y: { range: function (u, lo, hi) { return [0, hi || 1]; } } },
    axes: [
      {
        values: function (u, splits) {
          return splits.map(function (i) {
            var ii = Math.round(i);
            return (ii >= 0 && ii < data.length) ? formatLabel(ii) : "";
          });
        },
        rotate: 45,
      },
      { label: "Liters" },
    ],
    series: series,
    cursor: { drag: { x: false, y: false } },
    legend: { show: true },
  }, seriesData, ctx);

  // Bars are uniformly colored via `paths`; per-bar coloring is too costly
  // for a single-series uPlot. For Chunk F's smart dashboard we'll move to
  // proper time-series scatter/line which doesn't need per-point colors.
  void colorGroups;  // kept for potential future overlay-by-color rendering
  void pfColor; void otherColor;
}

// Matches original: function exportCSV()
function dbExportCSV() {
  if (!dbFilteredData.length) {
    showToast("No data to export", "error");
    return;
  }
  var csv = "Date,Time,Boot,Volume (L),Trigger,Extra FF,Extra PF\n";
  dbFilteredData.forEach(function (d) {
    csv +=
      d.date +
      "," +
      d.fullTime +
      "," +
      (d.boot || "") +
      "," +
      d.vol.toFixed(2) +
      "," +
      d.reason +
      "," +
      d.ff +
      "," +
      d.pf +
      "\n";
  });
  // Filename: deviceId_filters_date.csv  — exact match to original .ino
  var f = ST.deviceId || CFG.deviceId || "logger";
  var ft = getVal("eventFilter");
  if (ft !== "ALL") f += "_" + ft;
  var pt = getVal("pressFilter");
  if (pt !== "ALL") f += "_" + pt;
  var excZ = document.getElementById("excludeZero");
  if (excZ && excZ.checked) f += "_noZero";
  var sd = getVal("startDate");
  if (sd) f += "_from" + sd;
  var ed = getVal("endDate");
  if (ed) f += "_to" + ed;
  f += "_" + new Date().toISOString().slice(0, 10) + ".csv";
  var blob = new Blob([csv], { type: "text/csv" });
  var url = URL.createObjectURL(blob);
  var a = document.createElement("a");
  a.href = url;
  a.download = f;
  a.click();
  URL.revokeObjectURL(url);
}

// ============================================================================
// ══ PAGE: FILES ══
// ============================================================================
function filesInit() {
  filesEditMode = false;
  currentFilesDir = "/";
  var hw = CFG.hardware || {};
  currentFilesStorage = hw.defaultStorageView === 1 ? "sdcard" : "internal";
  var list = document.getElementById("list");
  if (list && list.innerHTML.trim().length === 0) list.innerHTML = "<div class='list-item text-muted'>Loading…</div>";
  filesRender();
}

function filesRender() {
  var tabs = document.getElementById("tabs");
  if (tabs) {
    tabs.innerHTML =
      '<button data-click="filesSetStorage" data-args=\'["internal"]\' class="' +
      (currentFilesStorage === "internal" ? "active" : "") +
      '"><span data-icon="microchip"></span> LittleFS</button>' +
      '<button data-click="filesSetStorage" data-args=\'["sdcard"]\' class="' +
      (currentFilesStorage === "sdcard" ? "active" : "") +
      '"><span data-icon="hard-drive"></span> SD Card</button>';
  }

  fetch(
    "/api/filelist?storage=" +
      currentFilesStorage +
      "&dir=" +
      encodeURIComponent(currentFilesDir),
  )
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      var pct = d.percent || 0;
      setEl("files-usage", fmtBytes(d.used) + " / " + fmtBytes(d.total));
      setEl("files-pct", pct + "%");
      var bar = document.getElementById("bar");
      if (bar) {
        bar.style.width = pct + "%";
        bar.className = pct >= 70 ? "warn" : "";
      }

      var lbl = document.getElementById("files-dirLabel");
      if (lbl) {
        lbl.innerHTML =
          '<span class="mono">' +
          (currentFilesStorage === "sdcard" ? "SD:" : "FS:") +
          "</span> " + esc(currentFilesDir);
      }
      var upBtn = document.getElementById("upBtn");
      if (upBtn) upBtn.style.display = currentFilesDir === "/" ? "none" : "";
      var btnEdit = document.getElementById("btnEdit");
      var btnDone = document.getElementById("btnDone");
      if (btnEdit) btnEdit.style.display = filesEditMode ? "none" : "";
      if (btnDone) btnDone.style.display = filesEditMode ? "" : "none";
      var tools = document.getElementById("editTools");
      if (tools) tools.style.display = filesEditMode ? "block" : "none";

      var list = document.getElementById("list");
      if (!list) return;
      var files = d.files || [];
      if (!files.length) {
        list.innerHTML = "";
        list.appendChild(emptyState({
          icon: "folder",
          title: "No files",
          msg: "This directory is empty. Upload a file or create a subfolder to get started."
        }));
        return;
      }

      var rows = "";
      if (d.truncated) {
        rows +=
          '<tr><td colspan="5" style="color:var(--warn);font-size:11.5px;padding:6px 14px">' +
          "Listing truncated at 500 entries — refine with a subfolder." +
          "</td></tr>";
      }
      files.forEach(function (f) {
        var icon = f.isDir
          ? '<span data-icon="folder"></span>'
          : (/\.gz$/i.test(f.name)
              ? '<span data-icon="file-archive"></span>'
              : (/\.(jsonl?|csv|txt|log)$/i.test(f.name)
                  ? '<span data-icon="file-text"></span>'
                  : '<span data-icon="file"></span>'));
        var nameCell = f.isDir
          ? '<a class="fname dir" data-click="filesEnterDir" data-args="' +
            esc(JSON.stringify([f.path])) + '">' + esc(f.name) + "</a>"
          : '<span class="fname">' + esc(f.name) + "</span>";
        var actions = "";
        if (!f.isDir) {
          actions +=
            '<a class="btn-mini" title="Download" href="/download?file=' +
            encodeURIComponent(f.path) +
            "&storage=" + currentFilesStorage + '">' +
            '<span data-icon="download"></span></a>';
        }
        if (filesEditMode) {
          if (!f.isDir) {
            actions +=
              '<button class="btn-mini" title="Move/Rename" data-click="showMovePopup" data-args="' +
              esc(JSON.stringify([f.path, f.name])) + '">' +
              '<span data-icon="pencil"></span></button>';
          }
          actions +=
            '<button class="btn-mini warn" title="Delete" data-click="filesDelete" data-args="' +
            esc(JSON.stringify([f.path])) + '">' +
            '<span data-icon="trash-2"></span></button>';
        }
        rows +=
          '<tr><td style="width:32px">' + icon + "</td>" +
          "<td>" + nameCell + "</td>" +
          "<td>" + (f.isDir ? "—" : fmtBytes(f.size)) + "</td>" +
          "<td>" + (f.modified ? esc(f.modified) : "") + "</td>" +
          '<td><div class="row-acts">' + actions + "</div></td></tr>";
      });
      list.innerHTML =
        '<table class="ftable">' +
        "<thead><tr><th></th><th>Name</th><th>Size</th><th>Modified</th><th></th></tr></thead>" +
        "<tbody>" + rows + "</tbody></table>";
      if (window.Icons && Icons.swap) Icons.swap(list);
    })
    .catch(function (e) {
      var list = document.getElementById("list");
      if (list)
        list.innerHTML =
          '<div style="padding:14px;color:var(--err)">Error: ' + esc(String(e)) + "</div>";
    });
}

function filesSetStorage(s) {
  currentFilesStorage = s;
  currentFilesDir = "/";
  filesRender();
}
function filesEnterDir(d) {
  currentFilesDir = d;
  filesRender();
}
function filesGoUp() {
  var p = currentFilesDir.lastIndexOf("/");
  currentFilesDir = p <= 0 ? "/" : currentFilesDir.substring(0, p);
  filesRender();
}
function filesToggleEdit() {
  filesEditMode = !filesEditMode;
  filesRender();
}

function filesDelete(path) {
  if (!confirm("Delete " + path + "?")) return;
  getCsrfToken().then(function (token) {
    var url = "/delete?path=" + encodeURIComponent(path) +
              "&storage=" + currentFilesStorage +
              (token ? "&csrf=" + encodeURIComponent(token) : "");
    fetch(url, { method: "POST" })
      .then(function (r) {
        if (r.status === 403) {
          window.__csrfToken = null;
          throw new Error("CSRF token rejected — refresh page");
        }
        filesRender();
      })
      .catch(function (e) {
        showToast("Error: " + e, "error");
      });
  });
}

function filesUpload() {
  var inp = document.getElementById("fileInput");
  if (!inp || !inp.files.length) return;
  var files = inp.files,
    i = 0;
  var prog = document.getElementById("uploadProg");
  var bar = document.getElementById("uploadBar");
  var pct = document.getElementById("uploadPct");
  if (prog) prog.style.display = "block";
  function next() {
    if (i >= files.length) {
      if (prog) prog.style.display = "none";
      if (bar) bar.style.width = "0%";
      inp.value = "";
      filesRender();
      return;
    }
    var fd = new FormData();
    fd.append("path", currentFilesDir);
    fd.append("storage", currentFilesStorage);
    fd.append("file", files[i]);
    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function (ev) {
      if (ev.lengthComputable) {
        var p = Math.round((ev.loaded / ev.total) * 100);
        if (bar) bar.style.width = p + "%";
        if (pct) pct.textContent = p + "%";
      }
    };
    xhr.onload = function () {
      i++;
      next();
    };
    xhr.onerror = function () {
      showToast("Upload failed: " + files[i].name, "error");
      if (prog) prog.style.display = "none";
    };
    xhr.open(
      "POST",
      "/upload?path=" +
        encodeURIComponent(currentFilesDir) +
        "&storage=" +
        encodeURIComponent(currentFilesStorage),
    );
    xhr.send(fd);
  }
  next();
}

function filesMkdir() {
  var name = document.getElementById("newFolder");
  if (!name || !name.value.trim()) return;
  getCsrfToken().then(function (token) {
    var url = "/mkdir?name=" + encodeURIComponent(name.value.trim()) +
              "&dir=" + encodeURIComponent(currentFilesDir) +
              "&storage=" + currentFilesStorage +
              (token ? "&csrf=" + encodeURIComponent(token) : "");
    fetch(url, { method: "POST" })
      .then(function (r) {
        if (r.status === 403) {
          window.__csrfToken = null;
          throw new Error("CSRF token rejected — refresh page");
        }
        name.value = "";
        filesRender();
      })
      .catch(function (e) { showToast("Error: " + e, "error"); });
  });
}

var mvSrcPath = "";
function showMovePopup(path, name) {
  mvSrcPath = path;
  var inp = document.getElementById("name");
  if (inp) inp.value = name;
  document.getElementById("movePopup").style.display = "flex";
}
function filesApplyMove() {
  var newName = getVal("name").trim(),
    destDir = getVal("dest");
  if (!newName) return;
  var url =
    "/move_file?src=" +
    encodeURIComponent(mvSrcPath) +
    "&newName=" +
    encodeURIComponent(newName) +
    "&storage=" +
    currentFilesStorage;
  if (destDir) url += "&destDir=" + encodeURIComponent(destDir);
  getCsrfToken().then(function (token) {
    if (token) url += "&csrf=" + encodeURIComponent(token);
    fetch(url, { method: "POST" })
      .then(function (r) {
        if (r.status === 403) {
          window.__csrfToken = null;
          throw new Error("CSRF token rejected — refresh page");
        }
        document.getElementById("movePopup").style.display = "none";
        filesRender();
      })
      .catch(function (e) {
        showToast("Error: " + e, "error");
      });
  });
}

// ============================================================================
// ══ PAGE: LIVE ══
// Matches original: function upd() polling /api/live every 500ms
//                   function updLogs() polling /api/recent_logs every 3s
// ============================================================================
function liveInit() {
  if (ST.chip) setEl("live-chip", ST.chip);
  if (ST.cpu) setEl("live-cpu", ST.cpu);
  if (ST.ip) setEl("live-ip", ST.ip);
  if (ST.network) setEl("live-net", ST.network);

  var hint = document.getElementById("stateHint");
  if (hint) {
    var fm = CFG.flowMeter || {};
    var fl = fm.firstLoopMonitoringWindowSecs || "?";
    var win = fm.monitoringWindowSecs || "?";
    hint.textContent =
      "🔧 IDLE → 🟡 WAIT_FLOW (" +
      fl +
      "s) → 🟢 MONITORING (" +
      win +
      "s idle) → Logging";
  }

  // Prefer Server-Sent Events; fall back to polling on error / unsupported.
  liveStartTransport();
  liveLogsUpdate();
  liveLogsTimer = setInterval(liveLogsUpdate, 3000);
}

function liveStartTransport() {
  // Always do one immediate fetch so the page is populated before the first
  // SSE tick (server pushes at 1 Hz).
  liveUpdate();

  if (typeof EventSource === "undefined") {
    liveStartPolling(500);
    return;
  }
  try {
    liveES = new EventSource("/api/events");
  } catch (e) {
    liveStartPolling(500);
    return;
  }
  liveES.addEventListener("live", function (ev) {
    try { liveRender(JSON.parse(ev.data)); } catch (e) {}
  });
  liveES.onerror = function () {
    // Browser will auto-retry, but surface the disconnect and degrade to
    // polling if the SSE channel never recovers.
    var conn = document.getElementById("conn");
    if (conn) {
      conn.textContent = "● Reconnecting…";
      conn.className = "text-warning";
    }
    if (!liveTimer) liveStartPolling(1000);
  };
}

function liveStartPolling(rate) {
  if (liveTimer) { clearInterval(liveTimer); }
  liveTimer = setInterval(liveUpdate, rate || 500);
}

function liveSetRate() {
    var rateEl = document.getElementById('live-refresh-rate');
    if (!rateEl) return;
    var rate = parseInt(rateEl.value, 10) || 500;
    // Manual rate override → close SSE and use polling at the chosen interval.
    if (liveES) { try { liveES.close(); } catch (e) {} liveES = null; }
    liveStartPolling(rate);
}

// Polling fallback — kept identical in shape to the original upd() so the
// liveRender() body works for both EventSource and fetch results.
function liveUpdate() {
  fetchWithTimeout("/api/live")
    .then(function (r) {
      return r.json();
    })
    .then(liveRender)
    .catch(function () {
      var conn = document.getElementById("conn");
      if (conn) {
        conn.textContent = "● Disconnected";
        conn.className = "text-danger";
      }
    });
}

function liveRender(d) {
  if (!d) return;
  var conn = document.getElementById("conn");
  if (conn) {
    conn.textContent = "● Connected";
    conn.className = "text-success";
  }

  setEl("live-time", d.time);
  setEl("live-trigger", d.trigger);
  setEl("live-cycleTime", d.cycleTime);
  setEl("live-pulses", d.pulses);
  setEl("live-liters", parseFloat(d.liters || 0).toFixed(2));
  setEl("live-ffCount", d.ffCount);
  setEl("live-pfCount", d.pfCount);
  setEl("live-boot", d.boot);
  setEl("live-heap", fmtBytes(d.heap));
  setEl("live-heapTotal", fmtBytes(d.heapTotal));
  setEl("live-uptime", d.uptime);
  if (d.fsTotal)
    setEl("live-storage", fmtBytes(d.fsUsed) + "/" + fmtBytes(d.fsTotal));

  var stClasses = {
    IDLE:       "sm-idle",
    WAIT_FLOW:  "sm-wait-flow",
    MONITORING: "sm-monitoring",
    DONE:       "sm-done",
  };
  var stEl = document.getElementById("state");
  if (stEl) {
    stEl.textContent = d.state || "–";
    stEl.className = stClasses[d.state] || "";
  }
  var remEl = document.getElementById("stateRem");
  if (remEl)
    remEl.textContent = d.stateRemaining >= 0 ? d.stateRemaining + "s" : "-";

  liveBtn("live-ff",   d.ff,   "Pressed", "Released", "live-on",   "live-idle");
  liveBtn("live-pf",   d.pf,   "Pressed", "Released", "live-on",   "live-idle");
  liveBtn("live-wifi", d.wifi, "Pressed", "Released", "live-wifi", "live-idle");

  var modeEl = document.getElementById("mode");
  if (modeEl) {
    if (d.mode === "online") modeEl.innerHTML = "🌐 Online Logger";
    else if (d.mode === "webonly") modeEl.innerHTML = "📡 Web Only";
    else modeEl.innerHTML = "📊 Logging";
  }

  if (d.time) setEl("headerTime", d.time.split(" ")[1] || d.time);
  updateFooter({ boot: d.boot, heap: d.heap, heapTotal: d.heapTotal });
}

function liveBtn(id, pressed, txtOn, txtOff, clsOn, clsOff) {
  var el = document.getElementById(id);
  if (!el) return;
  el.textContent = pressed ? txtOn : txtOff;
  el.className = "badge " + (pressed ? clsOn : clsOff);
}

// Enrol markup-reachable handlers (data-click / data-change / data-input /
// data-submit).  See core.js::Handlers for why the whitelist exists.
registerHandlers({
  dbRangeChange: dbRangeChange,
  dbRefreshNow: dbRefreshNow,
  dbLoadData: dbLoadData,
  dbApplyFilters: dbApplyFilters,
  dbExportCSV: dbExportCSV,
  filesSetStorage: filesSetStorage,
  filesEnterDir: filesEnterDir,
  filesGoUp: filesGoUp,
  filesToggleEdit: filesToggleEdit,
  filesDelete: filesDelete,
  filesUpload: filesUpload,
  filesMkdir: filesMkdir,
  showMovePopup: showMovePopup,
  filesApplyMove: filesApplyMove,
  liveSetRate: liveSetRate,
  liveLogsFilter: liveLogsFilter,
  liveLogsFilterClear: liveLogsFilterClear,
});

// Matches original: function updLogs()
// Phase 5c-3: caches the rendered logs in `_liveLogsCache` so the filter
// input can re-render without re-fetching.  liveLogsFilter() reads the
// cache; liveLogsFilterClear() resets the input + re-renders.
var _liveLogsCache = [];
function liveLogsUpdate() {
  fetchWithTimeout("/api/recent_logs")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      _liveLogsCache = (d && d.logs) || [];
      _liveLogsRender();
    })
    .catch(function () {});
}

function _liveLogsRender() {
  var el = document.getElementById("logs");
  if (!el) return;
  var th = ST.theme || CFG.theme || {};
  var ffC = th.ffColor || "#3498db",
    pfC = th.pfColor || "#e74c3c",
    otC = th.otherColor || "#95a5a6";

  var filterEl = document.getElementById("logsFilter");
  var query = filterEl ? filterEl.value.trim().toLowerCase() : "";
  var rows = _liveLogsCache;
  if (query) {
    rows = rows.filter(function (l) {
      return (
        String(l.time).toLowerCase().indexOf(query) >= 0 ||
        String(l.trigger).toLowerCase().indexOf(query) >= 0 ||
        String(l.volume).toLowerCase().indexOf(query) >= 0 ||
        String(l.ff).toLowerCase().indexOf(query) >= 0 ||
        String(l.pf).toLowerCase().indexOf(query) >= 0
      );
    });
  }

  if (!rows.length) {
    el.innerHTML = "";
    el.appendChild(emptyState({
      icon: "activity",
      title: query ? "No matches" : "No log entries yet",
      msg: query
        ? "Try a different search term, or clear the filter."
        : "Log entries appear here after the first wakeup with flow."
    }));
    return;
  }

  var html =
    '<table style="width:100%;border-collapse:collapse;font-size:.75rem">';
  html +=
    '<tr style="background:var(--bg)"><th style="padding:6px;text-align:left">Time</th><th>Trigger</th><th>Volume</th><th>+FF</th><th>+PF</th></tr>';
  rows.forEach(function (l) {
    var color =
      l.trigger.indexOf("FF") >= 0
        ? ffC
        : l.trigger.indexOf("PF") >= 0
          ? pfC
          : otC;
    var bg = hexToRgba(color, 0.15);
    html +=
      '<tr style="background:' + bg + '">' +
      '<td style="padding:6px">' + esc(l.time) + "</td>" +
      '<td style="color:' + color + ';font-weight:bold;text-align:center">' + esc(l.trigger) + "</td>" +
      '<td style="text-align:center">' + esc(l.volume) + "</td>" +
      '<td style="text-align:center">' + esc(l.ff) + "</td>" +
      '<td style="text-align:center">' + esc(l.pf) + "</td></tr>";
  });
  html += "</table>";
  el.innerHTML = html;
}

function liveLogsFilter() { _liveLogsRender(); }
function liveLogsFilterClear() {
  var f = document.getElementById("logsFilter");
  if (f) f.value = "";
  _liveLogsRender();
}

