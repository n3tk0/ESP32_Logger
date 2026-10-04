/**
 * /www/js/sensors.js — sensors live grid, Core Logic editor,
 * platform_config.json IO, settings import/export.
 * Loaded after settings.js. Depends on core globals + CFG.
 */
"use strict";

// spT(key, fallback, vars): translated string with an explicit English
// fallback, same guarded pattern used across the rest of the app (see
// firstrun.js / core.js / nodes.js). Namespace is "sensorsPage" —
// registered in www/i18n/sensors.js. Interpolates {var} into the fallback
// too, so callers can rely on vars being applied either way.
function spT(key, fallback, vars) {
  if (window.I18n) return I18n.t("sensorsPage." + key, vars);
  var s = fallback;
  if (vars) {
    for (var k in vars) {
      if (Object.prototype.hasOwnProperty.call(vars, k)) {
        s = s.split("{" + k + "}").join(String(vars[k]));
      }
    }
  }
  return s;
}

// ============================================================================
// PLATFORM CONFIG  (platform_config.json management)
// ============================================================================
var PCFG = null; // cached platform config object
var _pcfgFetch = null; // in-flight Promise (dedup concurrent loads)

var _PCFG_DEFAULT = { version: 1, mode: "legacy", sensors: [], aggregation: {}, export: {}, storage: {} };

function pcfgLoad(cb) {
  if (_pcfgFetch) { _pcfgFetch.then(function () { if (cb) cb(PCFG); }); return; }
  _pcfgFetch = fetchWithTimeout("/api/platform_config", {}, 15000)
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) { PCFG = d || Object.assign({}, _PCFG_DEFAULT); })
    .catch(function () { PCFG = Object.assign({}, _PCFG_DEFAULT); })
    .finally(function () { _pcfgFetch = null; if (cb) cb(PCFG); });
}

function pcfgSave(obj, cb) {
  var body = JSON.stringify(obj, null, 2);
  postWithCsrf("/save_platform", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: body,
  }, 30000)
    .then(function (r) { return r.json(); })
    .then(function (d) { if (cb) cb(d.ok, d.error || ""); })
    .catch(function (e) { if (cb) cb(false, String(e)); });
}

// ============================================================================
// SENSORS PAGE
// ============================================================================
var sensorChart = null;

function sensorsLoad() {
  var grid = document.getElementById("sensors-grid");
  var msg = document.getElementById("sensors-msg");
  if (msg) msg.textContent = window.I18n ? I18n.t("common.loading") : "Loading…";
  if (grid) grid.innerHTML = "";

  // Phase 5c-4 — short relative-time formatter for sensor freshness pills.
  // Falls through "5s" → "3m" → "2h" → "1d" so the staleness signal stays
  // legible at a glance.
  function _sensorFmtAge(ms) {
    var s = Math.round(ms / 1000);
    if (s < 60)   return s + "s";
    var m = Math.round(s / 60);
    if (m < 60)   return m + "m";
    var h = Math.round(m / 60);
    if (h < 24)   return h + "h";
    var d = Math.round(h / 24);
    return d + "d";
  }

  // Force a fresh fetch here — this is the Sensors page main load, the
  // operator is explicitly looking at this data and expects it current.
  getSensors({ maxAgeMs: 0 })
    .catch(function () { return null; })
    .then(function (d) {
      if (!d || !d.sensors || d.sensors.length === 0) {
        if (msg)
          msg.textContent = spT(
            "noSensorsRegistered",
            "No sensors registered. Set mode to Continuous in Core Logic settings and configure sensors."
          );
        var sub = document.getElementById("sensors-sub");
        if (sub) sub.textContent = spT("sensorsCount", "{n} sensors", { n: 0 });
        return;
      }
      if (msg) msg.textContent = "";

      // Update page subtitle with live counts
      var nowMs = Date.now();
      var errCount = d.sensors.filter(function(s) { return s && (s.status === "err" || s.status === "error"); }).length;
      var okCount  = d.sensors.filter(function(s) { return s && s.status === "ok"; }).length;
      var sub = document.getElementById("sensors-sub");
      if (sub) {
        var parts = [spT("activeCount", "{n} active", { n: okCount })];
        if (errCount) parts.push(spT("erroredCount", "{n} errored", { n: errCount }));
        sub.textContent = parts.join(" · ");
      }

      if (grid) {
        var html = [];
        d.sensors.forEach(function (s) {
          if (!s) return;
          var metrics = (s.metrics && s.metrics.length > 0) ? s.metrics : [""];
          
          metrics.forEach(function (m, mIdx) {
            // Sparkline path from `s.spark` for primary metric.
            // Secondary metrics get a placeholder with .s-spark-lazy.
            var sparkSvg = "";
            var spark = (mIdx === 0) ? (s.spark || []) : [];
            
            if (spark.length >= 2) {
              var min = Infinity, max = -Infinity;
              for (var i = 0; i < spark.length; i++) {
                var n = +spark[i];
                if (n < min) min = n;
                if (n > max) max = n;
              }
              var range = max - min;
              if (range < 1e-9) range = 1;
              var stepX = 100 / (spark.length - 1);
              var pts = "";
              for (var j = 0; j < spark.length; j++) {
                var x = (j * stepX).toFixed(1);
                var y = (32 - ((+spark[j] - min) / range) * 28).toFixed(1);
                pts += (j ? " " : "") + x + "," + y;
              }
              sparkSvg =
                '<svg class="s-spark" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true">' +
                '<polyline points="' + pts + '" fill="none" stroke="currentColor" stroke-width="1.4"></polyline>' +
                "</svg>";
            } else if (mIdx > 0 && m) {
              sparkSvg = '<svg class="s-spark s-spark-lazy" data-sensor="' + esc(s.id) + '" data-metric="' + esc(m) + '" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true"></svg>';
            } else {
              sparkSvg = '<svg class="s-spark" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true"></svg>';
            }

            // Metric Value
            var lv = m && s.last_values ? s.last_values[m] : null;
            var val = "", unit = "", ts = 0;
            if (lv !== undefined && lv !== null) {
              if (typeof lv === "object") {
                val  = lv.v !== undefined ? String(lv.v) : "";
                unit = lv.u || "";
                ts   = lv.ts || 0;
              } else {
                val = String(lv);
              }
            }

            // Card-level staleness
            var stateClass = "";
            var ageStr = "—";
            var sleeping = false;
            var refMs = 0;
            if (ts) refMs = ts * 1000;
            else if (s.last_read_ts) refMs = s.last_read_ts * 1000;
            // Freshness window = data_interval_ms (the work period for a
            // duty-cycled sensor; == poll interval otherwise). A periodic sensor
            // between wake cycles is "sleeping" (working as intended), not stale.
            var freshMs = s.data_interval_ms || s.read_interval_ms;
            if (refMs && freshMs) {
              var ageMs = nowMs - refMs;
              ageStr = spT("ago", "{t} ago", { t: _sensorFmtAge(ageMs) });
              if (ageMs > freshMs * 2) {
                stateClass = " stale";
              } else if (s.periodic && ageMs > s.read_interval_ms) {
                sleeping = true;
                ageStr = spT("sleepingAge", "sleeping · {age}", { age: ageStr });
              }
            }
            if (s.status === "err" || s.status === "error") stateClass = " err";
            if (s.status === "disabled") stateClass = " dis";

            var badgeClass =
              s.status === "ok" ? "ok" :
              s.status === "disabled" ? "dim" : "err";
            var badgeText =
              s.status === "ok" ? "OK" :
              s.status === "disabled" ? "OFF" : "ERR";

            // Only show transport if it adds information beyond the type/id the
            // user already sees.  "sds011 · sds011" is redundant; "sds011 · UART1" is useful.
            var rawTransport = s.transport || "";
            var transport = (rawTransport && rawTransport !== s.id && rawTransport !== s.type)
                            ? rawTransport : "";

            // s-metrics: error detail chip only (each metric is its own card)
            var errChip = "";
            if ((s.status === "err" || s.status === "error") && s.status_detail) {
              errChip = '<div class="s-metrics"><span class="badge err" style="font-size:10px">' +
                        esc(s.status_detail) + '</span></div>';
            }

            var ageRefMs = s.last_read_ms || 0;
            var ageIcon = "", ageColor = "inherit";
            if (stateClass === " err")        { ageIcon = "⊘"; ageColor = "var(--err)"; }
            else if (stateClass === " stale") { ageIcon = "⚠"; ageColor = "var(--warn)"; }
            else if (sleeping)                { ageIcon = "☾"; ageColor = "var(--text-3)"; }
            else if (ageRefMs && stateClass !== " dis") { ageIcon = "✓"; ageColor = "var(--ok)"; }

            var cardName = esc(s.name) + (m && metrics.length > 1 ? " (" + esc(m) + ")" : "");
            
            html.push(
              '<div class="sensor' + stateClass + '" data-sensor-name="' + esc((cardName + ' ' + (s.id || '')).toLowerCase().trim()) + '" data-sid="' + esc(s.id || '') + '">' +
                '<div class="s-head">' +
                  '<div>' +
                    '<div class="s-name">' + cardName + '</div>' +
                    '<div class="s-id">' + esc(s.id) +
                      (transport ? ' · ' + esc(transport) : (s.type && s.type !== s.id ? ' · ' + esc(s.type) : '')) + '</div>' +
                  '</div>' +
                  '<span class="badge ' + badgeClass + '">' + badgeText + '</span>' +
                '</div>' +
                '<div class="s-val">' +
                  '<span class="n">' + (val ? esc(val) : "—") + '</span>' +
                  (unit ? '<span class="u">' + esc(unit) + '</span>' : '') +
                '</div>' +
                sparkSvg +
                errChip +
                '<div class="s-foot">' +
                  '<span style="color:' + ageColor + '">' + ageIcon + ' ' + ageStr + '</span>' +
                  (transport ? '<span>' + esc(transport) + '</span>' : '') +
                '</div>' +
              '</div>'
            );
          });
        });
        grid.innerHTML = html.join("");

        // Fetch missing sparklines for secondary metrics. Throttle to a small
        // concurrency — ESPAsyncWebServer has a tiny connection pool, so firing
        // one /api/data request per metric at once causes timeouts/contention.
        var lazySparks = [].slice.call(grid.querySelectorAll(".s-spark-lazy"));
        function _drawLazySpark(svg) {
          var sId = svg.getAttribute("data-sensor");
          var mId = svg.getAttribute("data-metric");
          if (!sId || !mId) return Promise.resolve();
          var now  = Math.floor(Date.now() / 1000);
          var from = now - 3600; // last 1 hour
          var url = "/api/data?sensor=" + encodeURIComponent(sId)
                  + "&metric=" + encodeURIComponent(mId)
                  + "&from=" + from + "&to=" + now
                  + "&agg=raw&mode=lttb&limit=32";
          return fetchWithTimeout(url, {}, 5000)
            .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
            .then(function (res) {
              if (!res || !res.data || res.data.length < 2) return;
              var min = Infinity, max = -Infinity, ys = [];
              res.data.forEach(function (pt) {
                if (!pt || pt.v === undefined) return;
                var val = Number(pt.v);
                if (!isNaN(val)) { if (val < min) min = val; if (val > max) max = val; ys.push(val); }
              });
              if (ys.length < 2) return;
              var range = max - min;
              if (range < 1e-9) range = 1;
              var stepX = 100 / (ys.length - 1), pts = "";
              for (var j = 0; j < ys.length; j++) {
                var x = (j * stepX).toFixed(1);
                var y = (32 - ((ys[j] - min) / range) * 28).toFixed(1);
                pts += (j ? " " : "") + x + "," + y;
              }
              svg.innerHTML = '<polyline points="' + pts + '" fill="none" stroke="currentColor" stroke-width="1.4"></polyline>';
            })
            .catch(function (err) { console.error("Failed to fetch sparkline data:", err); });
        }
        // Worker pool: at most MAX_PARALLEL requests in flight; each worker pulls
        // the next pending sparkline when its request settles.
        var MAX_PARALLEL = 3, qi = 0;
        function _nextSpark() {
          if (qi >= lazySparks.length) return;
          _drawLazySpark(lazySparks[qi++]).then(_nextSpark);
        }
        for (var w = 0; w < Math.min(MAX_PARALLEL, lazySparks.length); w++) _nextSpark();
      }

      // Populate chart sensor selectors (primary + overlay)
      var sensorOpts =
        d.sensors
          .map(function (s) {
            return '<option value="' + esc(s.id) + '">' + esc(s.name) + "</option>";
          })
          .join("");
      var sel = document.getElementById("sc-sensor");
      if (sel) {
        sel.innerHTML = '<option value="">' + esc(spT("optSensor", "— select sensor —")) + '</option>' + sensorOpts;
      }
      var sel2 = document.getElementById("sc-sensor2");
      if (sel2) {
        sel2.innerHTML = '<option value="">' + esc(spT("optNone", "— none —")) + '</option>' + sensorOpts;
      }
    })
    .catch(function (e) {
      if (msg) msg.textContent = spT("failedLoadSensors", "Failed to load sensors: {e}", { e: e });
    });
}

function sensorsFilter() {
  var q = (document.getElementById("sensors-filter") || {}).value || "";
  q = q.toLowerCase().trim();
  var cards = document.querySelectorAll("#sensors-grid .sensor");
  cards.forEach(function (card) {
    var name = card.getAttribute("data-sensor-name") || "";
    card.style.display = (!q || name.indexOf(q) !== -1) ? "" : "none";
  });
}

// ── Sensor chart history from the data log ───────────────────────────────
// /api/data serves the in-memory ring only, and a request sees just its
// newest 300 readings across every sensor: a few minutes. Anything older is
// in the data log, one TIMER row per aggregation interval with a column per
// logged metric (src/storage/DatalogFormat.h). So the chart reads the active
// log file, and the files rotation moved aside that can still hold rows of
// the range, and puts the ring's readings after them.

var _scLogCache = {};                   // storage|path -> { size, text }
var _scLogInflight = {};                // storage|path -> Promise<text>
var _scDlQueue = [], _scDlActive = 0;
var SC_DL_PARALLEL = 2;                 // the web server's connection pool is tiny
var SC_MAX_ARCHIVED = 12;               // older files read at most per load
var SC_LABEL_MAX = 23;                  // DatalogCol::label is char[24]
var SC_BASE_FIELDS = ["Date", "Start", "End", "Duration", "Boot", "Trigger", "Volume", "FF", "PF"];

function _scDlNext() {
  while (_scDlActive < SC_DL_PARALLEL && _scDlQueue.length) {
    var job = _scDlQueue.shift();
    _scDlActive++;
    job().finally(function () { _scDlActive--; _scDlNext(); });
  }
}

// A file's text; "" when it cannot be read. Only a successful read is
// cached (by size: a rotated file never changes, the active one grows), and
// a file already on its way is not asked for twice.
function _scLogText(path, size, storage) {
  var key = storage + "|" + path;
  var c = _scLogCache[key];
  if (c && c.size === size) return Promise.resolve(c.text);
  if (_scLogInflight[key]) return _scLogInflight[key];
  var p = new Promise(function (resolve) {
    _scDlQueue.push(function () {
      return fetchWithTimeout("/download?file=" + encodeURIComponent(path) +
                              "&storage=" + encodeURIComponent(storage), {}, 30000)
        .then(function (r) {
          if (!r.ok) throw new Error("HTTP " + r.status);
          return r.text();
        })
        .then(function (t) { _scLogCache[key] = { size: size, text: t }; resolve(t); })
        .catch(function () { resolve(""); })
        .finally(function () { delete _scLogInflight[key]; });
    });
    _scDlNext();
  });
  _scLogInflight[key] = p;
  return p;
}

// ── The device's clock ──
// Rows carry the device's local time. Its zone is in the settings (a whole
// hour offset and a summer time rule, src/utils/PosixTz.h), so a row is
// turned into an epoch the way the device turned the epoch into the row,
// whatever zone the browser is in.
function _scNthSunday(y, mon, n) {           // n = -1: the last one
  if (n < 0) {
    var last = new Date(Date.UTC(y, mon + 1, 0));
    return last.getUTCDate() - last.getUTCDay();
  }
  var first = new Date(Date.UTC(y, mon, 1)).getUTCDay();
  return 1 + ((7 - first) % 7) + 7 * (n - 1);
}

// Seconds east of UTC at `epoch`; null when the settings do not say.
function _scDeviceOffset(epoch) {
  var net = (window.CFG && CFG.network) || null;
  if (!net || net.timezone === undefined) {
    return (window.ST && ST.utcOffset !== undefined) ? +ST.utcOffset : null;
  }
  var std = (+net.timezone || 0) * 3600, rule = +net.dstRule || 0;
  if (rule === 2) return std;
  if (rule === 3) return std + (+net.dstOffsetHours || 1) * 3600;
  var y = new Date(epoch * 1000).getUTCFullYear(), on, off;
  if (rule === 1) {
    on  = Date.UTC(y, 2,  _scNthSunday(y, 2, 2), 2) / 1000 - std;
    off = Date.UTC(y, 10, _scNthSunday(y, 10, 1), 2) / 1000 - std - 3600;
  } else {
    on  = Date.UTC(y, 2, _scNthSunday(y, 2, -1), 1) / 1000;
    off = Date.UTC(y, 9, _scNthSunday(y, 9, -1), 1) / 1000;
  }
  return std + (epoch >= on && epoch < off ? 3600 : 0);
}

// Device-local wall time -> epoch.
function _scLocalToEpoch(y, mo, d, h, mi, s) {
  var naive = Date.UTC(y, mo - 1, d, h, mi, s) / 1000;
  var off = _scDeviceOffset(naive);
  if (off === null) return Math.floor(new Date(y, mo - 1, d, h, mi, s).getTime() / 1000);
  // The offset depends on the instant; one refinement settles it.
  return naive - _scDeviceOffset(naive - off);
}

// The last moment a file moved aside can hold, from the suffix archive()
// gave it: _YYYY-MM-DD (daily/weekly), _YYYY-MM (monthly), or
// _YYYYMMDD-HHMMSS (size or header change, the time of the row that did
// not fit). null = not one of those.
function _scArchiveEnd(suffix) {
  var m = suffix.match(/^(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})/);
  if (m) return _scLocalToEpoch(+m[1], +m[2], +m[3], +m[4], +m[5], +m[6]);
  m = suffix.match(/^(\d{4})-(\d{2})(?:-(\d{2}))?/);
  if (!m) return null;
  var y = +m[1], mo = +m[2];
  return m[3] ? _scLocalToEpoch(y, mo, +m[3] + 1, 0, 0, 0)
              : _scLocalToEpoch(y, mo + 1, 1, 0, 0, 0);
}

function _scDirOf(path) {
  var i = path.lastIndexOf("/");
  return i > 0 ? path.substring(0, i) : "/";
}

// The data log files that can hold rows at or after `from`, newest first.
// Only the log's own folder is listed: a recursive listing walks the whole
// card and stops at its entry limit.
function _scLogFiles(from) {
  var hw = (window.CFG && CFG.hardware) || {};
  var storage = +hw.storageType === 1 ? "sdcard" : "internal";
  var known = (window.ST && ST.currentFile) ||
              (window.CFG && CFG.datalog && CFG.datalog.currentFile) || "";
  var url = "/api/filelist?filter=log&storage=" + storage +
            (known ? "&dir=" + encodeURIComponent(_scDirOf(known)) : "&recursive=1");
  return fetchWithTimeout(url, {}, 15000)
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      var active = (d && d.currentFile) || "";
      if (!d || !d.files || !active) return [];
      var dir = _scDirOf(active);
      var base = active.substring(active.lastIndexOf("/") + 1);
      var dot = base.lastIndexOf(".");
      var stem = dot > 0 ? base.substring(0, dot) : base;
      var ext = dot > 0 ? base.substring(dot) : "";
      var out = [], archived = [];
      d.files.forEach(function (f) {
        if (f.isDir || _scDirOf(f.path) !== dir) return;
        var name = f.path.substring(f.path.lastIndexOf("/") + 1);
        if (f.path === active) { out.push({ path: f.path, size: f.size }); return; }
        if (name.indexOf(stem + "_") !== 0 || (ext && name.slice(-ext.length) !== ext)) return;
        var end = _scArchiveEnd(name.substring(stem.length + 1, name.length - ext.length));
        if (end !== null && end >= from) archived.push({ path: f.path, size: f.size, end: end });
      });
      archived.sort(function (a, b) { return b.end - a.end; });
      return out.concat(archived.slice(0, SC_MAX_ARCHIVED)).map(function (f) {
        f.storage = storage;
        return f;
      });
    })
    .catch(function () { return []; });
}

// A row's Date field as [y, m, d], or null. The row's own separators decide
// where they can (YYYY-MM-DD, DD.MM.YYYY); for d/m/y vs m/d/y the setting
// does, unless the row cannot be read that way — the setting may have been
// changed since the row was written, and the header does not record it.
function _scRowDate(s, dateFormat) {
  var m;
  if ((m = s.match(/^(\d{4})-(\d{2})-(\d{2})$/))) return [+m[1], +m[2], +m[3]];
  if ((m = s.match(/^(\d{2})\.(\d{2})\.(\d{4})$/))) return [+m[3], +m[2], +m[1]];
  if (!(m = s.match(/^(\d{2})\/(\d{2})\/(\d{4})$/))) return null;
  var a = +m[1], b = +m[2], y = +m[3];
  var dmy = [y, b, a], mdy = [y, a, b];
  var pick = dateFormat === 2 ? mdy : dmy, alt = dateFormat === 2 ? dmy : mdy;
  if (pick[1] >= 1 && pick[1] <= 12) return pick;
  return alt[1] >= 1 && alt[1] <= 12 ? alt : null;
}

function _scClock(s) {
  var t = s.match(/^(\d{1,2}):(\d{2})(?::(\d{2}))?\s*(AM|PM)?$/i);
  if (!t) return null;
  var h = +t[1];
  if (t[4]) h = (h % 12) + (t[4].toUpperCase() === "PM" ? 12 : 0);
  return [h, +t[2], +(t[3] || 0)];
}

// The header label a column gets, as the firmware writes it.
function _scLabel(s) {
  return String(s).substring(0, SC_LABEL_MAX).replace(/[|\[\]\x00-\x1f]/g, "_");
}

// Points {ts, v} of one sensor metric in one data log file's text.
// `want` = { labels, index }: the labels the column may carry, and its
// position among the logged columns (-1 = unknown).
function _scParseLog(text, want, from, to, dateFormat, winSec, stats) {
  var lines = text.split("\n");
  var head = (lines[0] || "").replace(/\r$/, "").split("|");
  if (/\d/.test(head[0] || "")) return [];          // no header: no sensor columns
  var iDate = head.indexOf("Date"), iStart = head.indexOf("Start");
  var iEnd = head.indexOf("End"), iDur = head.indexOf("Duration");
  var iTrig = head.indexOf("Trigger");
  var nBase = 0;
  while (nBase < head.length && SC_BASE_FIELDS.indexOf(head[nBase]) >= 0) nBase++;
  // A column that is not averaged carries its mode after its label,
  // "Gust[max]" — how THIS file's rows were combined (dlFormatHeader).
  var modes = head.map(function () { return "avg"; });
  for (var h = nBase; h < head.length; h++) {
    var mm = /^(.*)\[(avg|min|max|last|sum)\]$/.exec(head[h]);
    if (mm) { head[h] = mm[1]; modes[h] = mm[2]; }
  }
  // The column: at its position when the label there matches (labels need
  // not be unique), else the one column carrying the label.
  var col = -1;
  if (want.index >= 0 && want.labels.indexOf(head[nBase + want.index]) >= 0) {
    col = nBase + want.index;
  } else {
    for (var k = 0; k < want.labels.length && col < 0; k++) {
      var first = head.indexOf(want.labels[k], nBase);
      if (first >= 0 && head.indexOf(want.labels[k], first + 1) < 0) col = first;
    }
  }
  if (col < 0 || iStart < 0) return [];
  // Rows combined another way than the series is — the column's mode was
  // changed since this file was written — are not the same kind of number,
  // and are left out rather than drawn as if they were.
  if (modes[col] !== want.mode) { stats.otherMode = true; return []; }
  if (iDate < 0) { stats.noDate = true; return []; }
  var pts = [];
  for (var i = 1; i < lines.length; i++) {
    var p = lines[i].replace(/\r$/, "").split("|");
    if (p.length <= col || p[col] === "") continue;
    var v = parseFloat(p[col]);
    if (!isFinite(v)) continue;
    var dt = _scRowDate(p[iDate], dateFormat), cl = _scClock(p[iStart]);
    if (!dt || !cl) continue;
    var ts = _scLocalToEpoch(dt[0], dt[1], dt[2], cl[0], cl[1], cl[2]);
    // A TIMER row's time is the start of the window it averages: place it
    // in the middle, by the row's own End or Duration when it has one.
    if (iTrig >= 0 && p[iTrig] === "TIMER") {
      var win = winSec, e;
      if (iEnd >= 0 && (e = _scClock(p[iEnd] || ""))) {
        win = ((e[0] - cl[0]) * 3600 + (e[1] - cl[1]) * 60 + (e[2] - cl[2]) + 86400) % 86400;
      } else if (iDur >= 0 && /^\d+s$/.test(p[iDur] || "")) {
        win = parseInt(p[iDur], 10);
      }
      ts += Math.floor(win / 2);
    }
    if (ts < from || ts > to) continue;
    pts.push({ ts: ts, v: v });
  }
  return pts;
}

// The data log column of one sensor metric, from the settings: the labels it
// may carry, its position among the logged columns (-1 = not logged), and
// how it is combined — the logged column's mode, or, for a column switched
// off, the mode it was listed with, which is what its files were written by.
// Known without reading any file, so a series has its mode even when the
// history cannot be read.
function _scColumn(sid, metric) {
  var dl = (window.CFG && CFG.datalog) || {};
  var cols = (dl.sensorCols && dl.sensorCols.cols) || [];
  var want = { labels: [], index: -1, mode: "avg" }, n = 0, listed = null;
  cols.forEach(function (c) {
    var mine = c.s === sid && c.m === metric;
    if (mine) {
      want.labels.push(_scLabel(c.l || (c.s + "_" + c.m)));
      if (listed === null) listed = DATALOG_AGGS.indexOf(c.a) >= 0 ? c.a : "avg";
    }
    if (!c.off) {
      if (mine && want.index < 0) {
        want.index = n;
        want.mode = DATALOG_AGGS.indexOf(c.a) >= 0 ? c.a : "avg";
      }
      n++;
    }
  });
  if (want.index < 0 && listed !== null) want.mode = listed;
  var dflt = _scLabel(sid + "_" + metric);          // the label a column gets by default
  if (want.labels.indexOf(dflt) < 0) want.labels.push(dflt);
  return want;
}

function _scLogHistory(sid, metric, from, to) {
  var dl = (window.CFG && CFG.datalog) || {};
  var want = _scColumn(sid, metric);
  var dateFormat = +dl.dateFormat;
  var lg = (window.CFG && CFG.logger) || {};
  var winSec = +lg.aggregationIntervalSec || 60;
  var stats = { noDate: false, otherMode: false };
  return _scLogFiles(from).then(function (files) {
    return Promise.all(files.map(function (f) {
      return _scLogText(f.path, f.size, f.storage).then(function (t) {
        return _scParseLog(t, want, from, to, dateFormat, winSec, stats);
      });
    }));
  }).then(function (lists) {
    var all = [].concat.apply([], lists);
    all.sort(function (a, b) { return a.ts - b.ts; });
    return { pts: all, noDate: stats.noDate, otherMode: stats.otherMode };
  });
}

var SC_BUCKET_SEC = { "5m": 300, "1h": 3600, "1d": 86400, raw: 0 };
// Wider buckets when the chosen one gives more than SC_MAX_POINTS.
var SC_BUCKET_LADDER = [300, 900, 1800, 3600, 10800, 21600, 43200, 86400];
var SC_MAX_POINTS = 250;

function _scBucketName(sec) {
  if (!sec) return "raw";
  if (sec % 86400 === 0) return (sec / 86400) + "d";
  if (sec % 3600 === 0) return (sec / 3600) + "h";
  return (sec / 60) + "m";
}

// How the data log combined each interval's readings for a column ("a" in
// its sensorCols entry; DATALOG_AGGS in core.js). The chart combines its
// buckets the same way, so a column logged as the highest of each minute is
// charted as the highest of each hour — not as the average of the highs.

// Largest-Triangle-Three-Buckets down to `n` points: what "raw" is drawn
// with when there are too many points, so a short spike is kept rather than
// averaged into a wider bucket.
function _scLttb(pts, n) {
  if (pts.length <= n || n < 3) return pts;
  var out = [pts[0]], every = (pts.length - 2) / (n - 2), a = 0;
  for (var i = 0; i < n - 2; i++) {
    var s = Math.floor((i + 1) * every) + 1, e = Math.min(Math.floor((i + 2) * every) + 1, pts.length);
    var ax = 0, ay = 0;
    for (var j = s; j < e; j++) { ax += pts[j].ts; ay += pts[j].v; }
    ax /= (e - s) || 1; ay /= (e - s) || 1;
    var rs = Math.floor(i * every) + 1, re = Math.floor((i + 1) * every) + 1, best = -1, pick = rs;
    for (var r = rs; r < re; r++) {
      var area = Math.abs((pts[a].ts - ax) * (pts[r].v - pts[a].v) -
                          (pts[a].ts - pts[r].ts) * (ay - pts[a].v));
      if (area > best) { best = area; pick = r; }
    }
    out.push(pts[pick]);
    a = pick;
  }
  out.push(pts[pts.length - 1]);
  return out;
}

// Buckets the points by `sec` (0 = as they are) on the device's local
// clock, so 1d is the device's day; one value per bucket by `mode`, the
// column's own. Returns { pts, sec } with the width actually used.
function _scAggregate(pts, sec, mode) {
  if (sec > 0 && pts.length) {
    var off = _scDeviceOffset(pts[pts.length - 1].ts) || 0;
    var out = [], cur = null;
    pts.forEach(function (p) {
      var b = Math.floor((p.ts + off) / sec) * sec - off;
      if (!cur || cur.ts !== b) {
        cur = { ts: b, sum: 0, n: 0, min: Infinity, max: -Infinity, last: 0 };
        out.push(cur);
      }
      cur.sum += p.v; cur.n++;
      if (p.v < cur.min) cur.min = p.v;
      if (p.v > cur.max) cur.max = p.v;
      cur.last = p.v;
    });
    pts = out.map(function (c) {
      var v = mode === "min" ? c.min : mode === "max" ? c.max :
              mode === "last" ? c.last : mode === "sum" ? c.sum : c.sum / c.n;
      return { ts: c.ts, v: Math.round(v * 100) / 100 };
    });
  }
  if (pts.length > SC_MAX_POINTS) {
    // Not for a sum: LTTB keeps some points and drops the rest, and a
    // dropped interval's total would vanish. A sum widens its buckets.
    if (!sec && mode !== "sum") return { pts: _scLttb(pts, SC_MAX_POINTS), sec: 0 };
    var wider = SC_BUCKET_LADDER.filter(function (w) { return w > sec; })[0];
    if (wider) return _scAggregate(pts, wider, mode);
    pts = pts.slice(pts.length - SC_MAX_POINTS);
  }
  return { pts: pts, sec: sec };
}

// The data log's rows, then the ring's readings after them.
//
// A SUM COLUMN'S ROWS ARE TOTALS and the ring's readings are single ones: the
// ring holds only its newest minutes, and a total made from part of an
// interval, joined to the rows, would dip wherever the two meet. So a sum
// column is drawn from its rows alone. Every other mode joins the ring's
// readings from where the ring starts: a highest, lowest, last or average of
// single readings is the same kind of number as the rows'.
function _scJoin(log, ring, mode) {
  if (mode === "sum") return log;
  var firstRing = ring.length ? ring[0].ts : Infinity;
  return log.filter(function (p) { return p.ts < firstRing; }).concat(ring);
}

// One chart series: data log history, then the ring's readings after it,
// bucketed by the column's own aggregation.
function _scSeries(sid, metric, from, to, agg) {
  var mode = _scColumn(sid, metric).mode;
  var ringUrl = "/api/data?sensor=" + encodeURIComponent(sid) +
    "&metric=" + encodeURIComponent(metric) +
    "&from=" + from + "&to=" + to + "&agg=raw&mode=raw&limit=300";
  var ring = fetchWithTimeout(ringUrl, {}, 15000)
    .then(function (r) { return r.ok ? r.json() : null; })
    .catch(function () { return null; });
  var hist = _scLogHistory(sid, metric, from, to)
    .catch(function () { return { pts: [], noDate: false, otherMode: false }; });
  return Promise.all([ring, hist]).then(function (res) {
    var rd = (res[0] && res[0].data) || [];
    var unit = rd.length ? (rd[0].unit || "") : "";
    if (!unit) {
      var s = ((_sensorsCache && _sensorsCache.sensors) || []).find(function (x) { return x.id === sid; });
      var lv = s && s.last_values && s.last_values[metric];
      if (lv && typeof lv === "object") unit = lv.u || "";
    }
    var pts = _scJoin(res[1].pts, rd.map(function (p) { return { ts: p.ts, v: p.v }; }), mode);
    var a = _scAggregate(pts, SC_BUCKET_SEC[agg] || 0, mode);
    return {
      agg: _scBucketName(a.sec), mode: mode, count: a.pts.length, noDate: res[1].noDate,
      otherMode: res[1].otherMode,
      data: a.pts.map(function (p) { return { ts: p.ts, v: p.v, unit: unit }; }),
    };
  });
}

var _scLoadSeq = 0;

function sensorChartLoad() {
  var sid = (document.getElementById("sc-sensor") || {}).value;
  var metric = (document.getElementById("sc-metric") || {}).value;
  var agg = (document.getElementById("sc-agg") || {}).value || "5m";
  var range = parseInt(
    (document.getElementById("sc-range") || {}).value || "86400",
    10,
  );
  var msg = document.getElementById("sc-msg");

  if (!sid) return;

  // Update metric dropdown when sensor changes
  var metricSel = document.getElementById("sc-metric");
  if (metricSel && !metric) {
    if (msg) msg.textContent = spT("selectMetricPrompt", "Select a metric…");
    return;
  }

  var now = Math.floor(Date.now() / 1000);
  var from = now - range;

  // Secondary overlay sensor
  var sid2 = (document.getElementById("sc-sensor2") || {}).value;
  var metric2 = (document.getElementById("sc-metric2") || {}).value;

  if (msg) msg.textContent = window.I18n ? I18n.t("common.loading") : "Loading…";

  // Primary (and optionally secondary) series: data log history + ring.
  var myLoad = ++_scLoadSeq;
  var fetches = [_scSeries(sid, metric, from, now, agg)];
  if (sid2 && metric2) fetches.push(_scSeries(sid2, metric2, from, now, agg));

  Promise.all(fetches)
    .then(function (results) {
      if (myLoad !== _scLoadSeq) return;   // a newer selection is loading
      var d1 = results[0];
      var d2 = results.length > 1 ? results[1] : null;

      if (!d1 || !d1.data || d1.data.length === 0) {
        if (msg) msg.textContent = spT("noDataPeriod", "No data for selected period.") +
          (d1 && d1.noDate ? " " + spT("historyNeedsDate", "History needs the Date field in the data log.") : "");
        return;
      }

      var unit1 = d1.data[0].unit || "";
      var unit2 = d2 && d2.data && d2.data.length > 0 ? (d2.data[0].unit || "") : "";
      var hasDual = d2 && d2.data && d2.data.length > 0;

      // Build unified timestamp labels from primary series
      var labels = d1.data.map(function (pt) {
        return new Date(pt.ts * 1000).toLocaleTimeString();
      });
      var values1 = d1.data.map(function (pt) { return pt.v; });

      // For the secondary series, align data by timestamp to primary labels
      var values2 = [];
      if (hasDual) {
        // Build a lookup map from ts -> value for secondary
        var tsMap = {};
        d2.data.forEach(function (pt) { tsMap[pt.ts] = pt.v; });

        // For each primary timestamp, find the closest secondary point
        values2 = d1.data.map(function (pt) {
          if (tsMap[pt.ts] !== undefined) return tsMap[pt.ts];
          // Find nearest secondary point within ±bucket window
          var closest = null, bestDist = Infinity;
          d2.data.forEach(function (p2) {
            var dist = Math.abs(p2.ts - pt.ts);
            if (dist < bestDist) { bestDist = dist; closest = p2.v; }
          });
          // Only include if within 2x the aggregation window
          var maxDist = range / labels.length * 2;
          return bestDist <= maxDist ? closest : null;
        });
      }

      // Compute and display CURRENT / Min / Avg / Max / Pts stats
      var fmt = function(v) { return v != null ? (Math.round(v * 10) / 10) + (unit1 ? " " + unit1 : "") : "—"; };
      var lastVal = values1[values1.length - 1];
      var minVal = Infinity, maxVal = -Infinity, sumVal = 0, cntVal = 0;
      for (var vi = 0; vi < values1.length; vi++) {
        var vv = values1[vi];
        if (vv == null) continue;
        if (vv < minVal) minVal = vv;
        if (vv > maxVal) maxVal = vv;
        sumVal += vv; cntVal++;
      }
      var avgVal = cntVal ? sumVal / cntVal : null;
      var elCur = document.getElementById("sc-current");
      var elMin = document.getElementById("sc-min");
      var elAvg = document.getElementById("sc-avg");
      var elMax = document.getElementById("sc-max");
      var elPts = document.getElementById("sc-pts");
      if (elCur) elCur.textContent = fmt(lastVal);
      if (elMin) elMin.textContent = minVal !== Infinity ? fmt(minVal) : "—";
      if (elAvg) elAvg.textContent = fmt(avgVal);
      if (elMax) elMax.textContent = maxVal !== -Infinity ? fmt(maxVal) : "—";
      if (elPts) elPts.textContent = d1.count !== undefined ? d1.count : "—";

      // The bucket and how it was combined — the data log column's own
      // choice (Settings → Data log), which is where it is changed.
      var infoStr = d1.agg + " · " + datalogAggLabel(d1.mode).toLowerCase();
      if (hasDual) {
        infoStr += spT("plusOverlay", " + overlay");
        // The overlay is combined by ITS column's mode, which can differ.
        if (d2.mode !== d1.mode) infoStr += " (" + datalogAggLabel(d2.mode).toLowerCase() + ")";
      }
      if (d1.otherMode || (d2 && d2.otherMode))
        infoStr += " · " + spT("historyOtherMode", "Rows logged with another aggregation are not shown.");
      if (d1.noDate) infoStr += " · " + spT("historyNeedsDate", "History needs the Date field in the data log.");
      if (msg) msg.textContent = infoStr;

      var ctx = document.getElementById("sensorChart");
      if (!ctx) return;

      function render() {
        if (sensorChart) { sensorChart.destroy(); sensorChart = null; }

        // uPlot wants epoch-seconds for time scale; convert from API ts.
        var xs = d1.data.map(function (pt) { return pt.ts; });

        var series = [
          { label: spT("timeLabel", "Time") },
          {
            label: sid + " / " + metric + (unit1 ? " (" + unit1 + ")" : ""),
            stroke: "#275673",
            fill: "rgba(39,86,115,0.08)",
            width: 2,
            points: { show: d1.data.length <= 100 },
            scale: "y",
          },
        ];
        var seriesData = [xs, values1];

        if (hasDual) {
          series.push({
            label: sid2 + " / " + metric2 + (unit2 ? " (" + unit2 + ")" : ""),
            stroke: "#e67e22",
            width: 2,
            dash: [5, 3],
            points: { show: d2.data.length <= 100 },
            scale: "y2",
          });
          seriesData.push(values2);
        }

        var axes = [
          {},
          { scale: "y", label: metric + (unit1 ? " (" + unit1 + ")" : "") },
        ];
        if (hasDual) {
          axes.push({
            scale: "y2",
            side: 1,
            grid: { show: false },
            label: metric2 + (unit2 ? " (" + unit2 + ")" : ""),
          });
        }

        ctx.innerHTML = "";
        sensorChart = new uPlot({
          width: ctx.clientWidth || 600,
          height: ctx.clientHeight || 320,
          scales: { x: { time: true }, y: {}, y2: {} },
          series: series,
          axes: axes,
          legend: { show: true },
          cursor: { sync: { key: "sensors" } },
        }, seriesData, ctx);
      }

      if (typeof uPlot === "undefined") {
        dbLoadUPlot(render);
      } else {
        render();
      }
    })
    .catch(function (e) {
      if (msg) msg.textContent = spT("errorPrefix", "Error: {e}", { e: e });
    });
}

// Update metric selectors when sensor changes (primary + overlay)
document.addEventListener("DOMContentLoaded", function () {
  function bindSensorMetricSync(sensorId, metricId) {
    var sensorSel = document.getElementById(sensorId);
    if (!sensorSel) return;
    sensorSel.addEventListener("change", function () {
      var sid = this.value;
      var metricSel = document.getElementById(metricId);
      if (!metricSel) return;
      if (!sid) {
        metricSel.innerHTML = '<option value="">' + esc(spT("optMetric", "— metric —")) + '</option>';
        return;
      }
      getSensors()
        .then(function (d) {
          var s = (d.sensors || []).find(function (s) { return s.id === sid; });
          if (s && s.metrics) {
            metricSel.innerHTML = s.metrics
              .map(function (m) { return '<option value="' + esc(m) + '">' + esc(m) + "</option>"; })
              .join("");
          }
        })
        .catch(function () {});
    });
  }
  bindSensorMetricSync("sc-sensor", "sc-metric");
  bindSensorMetricSync("sc-sensor2", "sc-metric2");

  // If the sensor selector already has a value (e.g. after a soft-nav back to this
  // page), fire a synthetic change so the metric list populates immediately.
  (function () {
    var sel = document.getElementById("sc-sensor");
    if (sel && sel.value) sel.dispatchEvent(new Event("change", { bubbles: true }));
  }());
});

// ============================================================================
// CORE LOGIC PAGE  (platform_config.json editor)
// ============================================================================
var CL_SENSOR_TYPES = [
  { value: "bme280", label: "BME280 (temp/humidity/pressure)", iface: "i2c" },
  { value: "bme688", label: "BME688 (T/H/P + gas + IAQ)", iface: "i2c" },
  { value: "bme680", label: "BME680 (T/H/P + gas + IAQ)", iface: "i2c" },
  { value: "sds011", label: "SDS011 (PM2.5/PM10)", iface: "uart" },
  { value: "pms5003", label: "PMS5003 (PM1/2.5/10)", iface: "uart" },
  { value: "sps30", label: "Sensirion SPS30 (PM1/2.5/4/10)", iface: "i2c" },
  { value: "yfs201", label: "YF-S201/YF-S403 (water flow)", iface: "pulse" },
  { value: "ens160", label: "ENS160 (TVOC/eCO2)", iface: "i2c" },
  { value: "sgp30", label: "SGP30 (TVOC/eCO2)", iface: "i2c" },
  { value: "rain", label: "Rain gauge (tipping bucket)", iface: "pulse" },
  { value: "wind", label: "Wind speed (anemometer)", iface: "pulse" },
  // Not wired to this board at all: the values arrive by POST /api/ingest from
  // a satellite node. It belongs in this list because the list is what names a
  // sensor in the UI, and without an entry a remote node showed up as the raw
  // string "remote" with no interface line.
  { value: "remote", label: "Remote node (HTTP ingest)", iface: "http" },
];

// Pins are typed, checked and drawn by www/js/pins.js — see clPinField /
// clWirePinWarn below. The board profile comes from /api/board-profiles
// through Pins.load(), shared with the Hardware page and the add-sensor
// wizard, instead of a fetch and a fallback table of its own here.

// Single source of truth for sleep-config defaults (mirrors Logger.ino initial values).
var CL_SLEEP_DEFAULTS = {
  cont_idle_timeout_ms: 300000,
  cont_idle_cpu_mhz: 80,
  cont_modem_sleep: true,
  hyb_idle_before_sleep_ms: 120000,
  hyb_sleep_duration_ms: 60000,
  hyb_active_window_ms: 30000,
};

function clLoad() {
  var msg = document.getElementById("cl-msg");
  if (msg) {
    msg.textContent = "";
    msg.className = "";
  }
  pcfgLoad(function (cfg) {
    // Mode — hidden <select> drives the form, .mode-card grid is the UI.
    var modeEl = document.getElementById("cl-mode");
    var mode = cfg.mode || "legacy";
    if (modeEl) modeEl.value = mode;
    document.querySelectorAll(".mode-card").forEach(function (card) {
      var on = card.getAttribute("data-mode") === mode;
      card.classList.toggle("selected", on);
      var radio = card.querySelector("input[type=radio]");
      if (radio) radio.checked = on;
      if (!card._wired) {
        card._wired = true;
        card.addEventListener("click", function () {
          var v = card.getAttribute("data-mode");
          var sel = document.getElementById("cl-mode");
          if (sel) { sel.value = v; sel.dispatchEvent(new Event("change", { bubbles: true })); }
          document.querySelectorAll(".mode-card").forEach(function (c) {
            c.classList.toggle("selected", c === card);
            var r = c.querySelector("input[type=radio]");
            if (r) r.checked = c === card;
          });
        });
      }
    });

    // Aggregation defaults
    var agg = cfg.aggregation || {};
    var amEl = document.getElementById("cl-aggmode");
    if (amEl) amEl.value = agg.default_mode || "lttb";
    var abEl = document.getElementById("cl-aggbucket");
    if (abEl) abEl.value = String(agg.default_bucket_min || 5);
    var mpEl = document.getElementById("cl-maxpoints");
    if (mpEl) mpEl.value = agg.max_points || 500;
    var rtEl = document.getElementById("cl-retention");
    if (rtEl) rtEl.value = agg.raw_retention_days || 7;

    // Export quick-enables
    var exp = cfg.export || {};
    var mqttEl = document.getElementById("cl-exp-mqtt");
    if (mqttEl) mqttEl.checked = !!(exp.mqtt && exp.mqtt.enabled);
    var httpEl = document.getElementById("cl-exp-http");
    if (httpEl) httpEl.checked = !!(exp.http && exp.http.enabled);
    var scEl = document.getElementById("cl-exp-sc");
    if (scEl)
      scEl.checked = !!(exp.sensor_community && exp.sensor_community.enabled);
    var osmEl = document.getElementById("cl-exp-osm");
    if (osmEl) osmEl.checked = !!(exp.opensensemap && exp.opensensemap.enabled);

    // Sleep settings
    var sl = cfg.sleep || {};
    var cont = sl.continuous || {};
    var hyb = sl.hybrid || {};
    var ciEl = document.getElementById("cl-cont-idle");
    if (ciEl)
      ciEl.value =
        cont.idle_timeout_ms || CL_SLEEP_DEFAULTS.cont_idle_timeout_ms;
    var ccEl = document.getElementById("cl-cont-cpu");
    if (ccEl)
      ccEl.value = String(
        cont.idle_cpu_mhz || CL_SLEEP_DEFAULTS.cont_idle_cpu_mhz,
      );
    var cmEl = document.getElementById("cl-cont-modem");
    if (cmEl) cmEl.checked = cont.modem_sleep !== false;
    var hiEl = document.getElementById("cl-hyb-idle");
    if (hiEl)
      hiEl.value =
        hyb.idle_before_sleep_ms || CL_SLEEP_DEFAULTS.hyb_idle_before_sleep_ms;
    var hsEl = document.getElementById("cl-hyb-sleep");
    if (hsEl)
      hsEl.value =
        hyb.sleep_duration_ms || CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms;
    var haEl = document.getElementById("cl-hyb-active");
    if (haEl)
      haEl.value =
        hyb.active_window_ms || CL_SLEEP_DEFAULTS.hyb_active_window_ms;

    // Show/hide sleep panel according to selected mode
    clUpdateSleepPanel();

    // Sensor list
    clRenderSensors(cfg.sensors || []);
  });
}

function clRenderSensors(sensors) {
  // The Hardware page draws the board with the sensors' pins marked; it
  // wires before this list loads, so tell it the list is here now.
  if (typeof window.hwPinsRepaint === "function") window.hwPinsRepaint();
  var list = document.getElementById("cl-sensors-list");
  if (!list) return;
  if (!sensors || sensors.length === 0) {
    list.innerHTML = "";
    list.appendChild(emptyState({
      icon: "gauge",
      title: spT("noSensorsConfigured", "No sensors configured"),
      msg: spT("noSensorsConfiguredMsg", "Click + Add Sensor to register your first sensor.")
    }));
    return;
  }
  list.innerHTML = sensors
    .map(function (s, i) {
      var typeLabel =
        (
          CL_SENSOR_TYPES.find(function (t) {
            return t.value === s.type;
          }) || {}
        ).label || s.type;
      // `|| "?"` on a pin number is wrong for exactly one value, and it is a
      // value people use: GPIO 0. It is a perfectly good I2C pin on the
      // ESP32-C3 — SCL=0 is a working, shipped configuration — and the row
      // printed "SCL:?" for it, which reads as "not configured".
      var pinTxt = function (v) {
        return (v === undefined || v === null || v === "") ? "?" : String(v);
      };
      var pinInfo =
        s.interface === "http" || s.type === "remote"
          ? spT("pinInfoNode", "Node:") + (s.node || s.id || "?")
          : s.interface === "i2c"
            ? "SDA:" + pinTxt(s.sda) + " SCL:" + pinTxt(s.scl) +
              (s.bus ? " " + spT("pinInfoBus", "Bus:") + s.bus : "")
            : s.interface === "uart"
              ? "RX:" + pinTxt(s.uart_rx)
              : s.interface === "pulse"
                ? spT("pinInfoPin", "Pin:") + pinTxt(s.pin)
                : "";
      return (
        '<div class="sensor-list-row" data-sensor-idx="' + i + '" style="display:flex;align-items:center;gap:8px;padding:10px 16px;border-bottom:1px solid var(--border)">' +
        '<label style="display:flex;align-items:center;gap:6px;cursor:pointer;flex:0 0 auto">' +
        '<input type="checkbox" data-change="clToggleSensor" data-args="[' +
        i +
        ']"' +
        (s.enabled ? " checked" : "") +
        ">" +
        '<span style="font-size:.8rem;color:var(--text-muted)">' +
        (s.enabled ? spT("on", "ON") : spT("off", "OFF")) +
        "</span>" +
        "</label>" +
        '<div style="flex:1;min-width:0">' +
        '<div style="font-weight:600">' +
        esc(s.id || s.type) +
        "</div>" +
        '<div style="font-size:.8rem;color:var(--text-muted)">' +
        esc(typeLabel) +
        " · " +
        esc(pinInfo) +
        "</div>" +
        "</div>" +
        '<button type="button" class="btn" data-click="clEditSensor" data-args="[' +
        i +
        ']">✏️</button>' +
        '<button type="button" class="btn warn" data-click="clRemoveSensor" data-args="[' +
        i +
        ']">🗑</button>' +
        "</div>"
      );
    })
    .join("");
}

function clToggleSensor(idx, enabled) {
  if (!PCFG || !PCFG.sensors) return;
  if (typeof enabled !== "boolean") enabled = !!this.checked;
  PCFG.sensors[idx].enabled = enabled;
}

function clRemoveSensor(idx) {
  if (!PCFG || !PCFG.sensors) return;
  var sensor = PCFG.sensors[idx];
  if (!sensor) return;
  var name = sensor.id || sensor.type || "sensor";
  // Optimistic remove with undo.  Pull the sensor out of PCFG and re-render
  // immediately; on commit the next clSave call will persist; on undo,
  // splice it back in at the original index.
  var removed = PCFG.sensors.splice(idx, 1)[0];
  clRenderSensors(PCFG.sensors);

  if (typeof showUndoToast === "function") {
    showUndoToast(
      spT("removedSensor", "Removed {name}", { name: name }),
      spT("removedSensorUndoHint", "Press Undo to restore (save on the page to persist)"),
      function () {
        // Re-insert at original index — clamp in case the list shrank.
        var insertAt = Math.min(idx, PCFG.sensors.length);
        PCFG.sensors.splice(insertAt, 0, removed);
        clRenderSensors(PCFG.sensors);
      }
    );
  } else if (window.showToast) {
    showToast(spT("removedSensor", "Removed {name}", { name: name }), "ok");
  }
}

// ── Corrections (calibration) ───────────────────────────────────────────────
// Every driver applies  value × scale + offset  per metric (CalibrationAxis,
// src/sensors/ISensor.h), read from an object keyed by metric name. The
// water-flow driver keeps a scalar "calibration" (its Multiplier field) and
// reads the per-metric object from "cal" instead. A remote sensor applies it
// on the collector to whatever metrics its node sends (RemoteNodeSensor).
// The lists mirror each driver's _cal*.load() calls.
var CL_CAL_METRICS = {
  bme280: ["temperature", "humidity", "pressure"],
  bmp280: ["temperature", "pressure"],
  bme688: ["temperature", "humidity", "pressure", "gas_resistance"],
  bme680: ["temperature", "humidity", "pressure", "gas_resistance"],
  ds18b20: ["temperature"],
  scd4x: ["co2", "temperature", "humidity"],
  sds011: ["pm25", "pm10"],
  pms5003: ["pm1", "pm25", "pm10"],
  sps30: ["pm1", "pm25", "pm4", "pm10"],
  ens160: ["tvoc", "eco2"],
  sgp30: ["tvoc", "eco2"],
  bh1750: ["lux"],
  veml7700: ["lux", "white"],
  veml6075: ["uva", "uvb", "uv_index"],
  hcsr04: ["distance"],
  rain: ["rain_rate", "rain_total"],
  wind: ["wind_speed"],
  soil_moisture: ["moisture"],
  zmct103c: ["current_arms"],
  zmpt101b: ["voltage_vrms"],
  yfs201: ["flow_rate", "volume"],
  yfs403: ["flow_rate", "volume"],
  water_flow: ["flow_rate", "volume"],
  remote: []
};
var CL_CAL_UNITS = {
  temperature: "°C", humidity: "%", pressure: "hPa", gas_resistance: "Ω",
  co2: "ppm", eco2: "ppm", tvoc: "ppb", pm1: "µg/m³", pm25: "µg/m³",
  pm4: "µg/m³", pm10: "µg/m³", lux: "lx", white: "lx", distance: "cm",
  rain_rate: "mm/h", rain_total: "mm", wind_speed: "m/s", moisture: "%",
  current_arms: "A", voltage_vrms: "V", flow_rate: "L/min", volume: "L"
};

function clCalKey(s) {
  return (s.type === "yfs201" || s.type === "yfs403" || s.type === "water_flow") ? "cal" : "calibration";
}

function clCalRowHtml(metric, entry, unit) {
  entry = entry || {};
  var off = (entry.offset !== undefined && entry.offset !== 0) ? entry.offset : "";
  var sc = (entry.scale !== undefined && entry.scale !== 1) ? entry.scale : "";
  var u = unit || CL_CAL_UNITS[metric] || "";
  return '<div class="form-grid cal-row" data-metric="' + esc(metric) + '" style="margin-top:6px">' +
    '<div class="field"><label class="field-label"><span class="mono">' + esc(metric) + '</span> · ' +
      esc(spT("fieldCalOffset", "Offset")) + (u ? " (" + esc(u) + ")" : "") + '</label>' +
      '<input type="number" step="any" name="cal_off_' + esc(metric) + '" class="input" placeholder="0" value="' + esc(String(off)) + '"></div>' +
    '<div class="field"><label class="field-label">' + esc(spT("fieldCalScale", "Multiplier")) + '</label>' +
      '<input type="number" step="any" name="cal_sc_' + esc(metric) + '" class="input" placeholder="1" value="' + esc(String(sc)) + '"></div>' +
    '</div>';
}

// The section's rows: the driver's metrics, plus any already configured.
function clCalSectionHtml(s) {
  var key = clCalKey(s);
  var cal = (s[key] && typeof s[key] === "object") ? s[key] : {};
  var list = (CL_CAL_METRICS[s.type] || []).slice();
  Object.keys(cal).forEach(function (m) { if (list.indexOf(m) === -1) list.push(m); });
  var html = '<div class="field" style="margin-top:1rem"><label class="field-label">' +
             esc(spT("fieldCorrections", "Corrections")) + '</label>' +
             '<p class="hint">' + esc(spT("correctionsHint", "Corrected value = reading × multiplier + offset. Empty means no correction. Applies to new readings only.")) + '</p>' +
             '<div id="sensor-cal">';
  list.forEach(function (m) { html += clCalRowHtml(m, cal[m]); });
  html += '</div>';
  if (s.type === "remote") {
    html += '<p class="hint" id="sensor-cal-wait"' + (list.length ? ' style="display:none"' : '') + '>' +
            esc(spT("correctionsRemoteWait", "The node's metrics appear here once it has reported.")) + '</p>';
  }
  return html + '</div>';
}

// A remote node's metrics are whatever it sends, so they come from the
// ingest mailbox; a local sensor's live list can name one the table lacks.
function clWireCal(s) {
  var box = document.getElementById("sensor-cal");
  if (!box) return;
  function add(metric, unit) {
    // Compared as strings, not through a selector: a node names its own
    // metrics, and a quote in one would make querySelector throw.
    var rows = box.querySelectorAll(".cal-row");
    for (var i = 0; i < rows.length; i++) if (rows[i].getAttribute("data-metric") === metric) return;
    if (!metric) return;
    box.insertAdjacentHTML("beforeend", clCalRowHtml(metric, null, unit));
    var w = document.getElementById("sensor-cal-wait");
    if (w) w.style.display = "none";
  }
  if (s.type === "remote") {
    // The mailbox and RemoteNodeSensor both keep 16 characters of a node id.
    var node = String(s.node || s.id || "").slice(0, 16);
    fetchWithTimeout("/api/remote/status", {}, 15000)
      .then(function (r) { return r.ok ? r.json() : {}; })
      .then(function (d) {
        (d.nodes || []).forEach(function (n) {
          if (n.id !== node) return;
          (n.metrics || []).forEach(function (m) { add(m.metric, m.unit); });
        });
      })
      .catch(function () {});
  } else if (typeof getSensors === "function") {
    getSensors().then(function (d) {
      (d.sensors || []).forEach(function (x) {
        if (x.id === s.id && x.type === s.type) (x.metrics || []).forEach(function (m) {
          if ((CL_CAL_METRICS[s.type] || []).indexOf(m) !== -1) add(m);
        });
      });
    }).catch(function () {});
  }
}

// Read the rows back. Only non-identity entries are stored, and an empty
// object is dropped so an untouched sensor keeps a tidy config.
function clReadCal(form, s) {
  var key = clCalKey(s);
  var rows = form.querySelectorAll(".cal-row");
  if (!rows.length) return;
  var cal = {};
  rows.forEach(function (row) {
    var m = row.getAttribute("data-metric");
    var inputs = row.querySelectorAll("input");   // offset, then multiplier
    var offEl = inputs[0], scEl = inputs[1];
    var off = parseFloat(String(offEl ? offEl.value : "").replace(",", "."));
    var sc = parseFloat(String(scEl ? scEl.value : "").replace(",", "."));
    if (!isFinite(off)) off = 0;
    if (!isFinite(sc) || sc === 0) sc = 1;
    if (off !== 0 || sc !== 1) cal[m] = { offset: off, scale: sc };
  });
  if (Object.keys(cal).length) s[key] = cal;
  else if (s[key] && typeof s[key] === "object") delete s[key];
}

window.clCurrentEditingSensor = -1;

// Build the inner HTML for the sensor-edit form.  Called by both the popup
// path (mobile / fallback) and the inline expander (desktop) so the two
// surfaces stay in lockstep.
function _clBuildEditFormHtml(s) {
  var html = '<form id="sensorEditForm" data-submit="clSaveEditedSensor">';
  
  // ID
  html += '<div class="field"><label class="field-label">' + esc(spT("fieldSensorId", "Sensor ID")) + '</label>' +
          '<input type="text" name="id" class="input" value="' + esc(s.id || '') + '"></div>';

  // Enabled
  html += '<div class="field"><label style="display:flex;align-items:center;gap:6px;cursor:pointer"><input type="checkbox" name="enabled"' + (s.enabled ? ' checked' : '') + '> ' + esc(spT("fieldEnabled", "Enabled")) + '</label></div>';

  // Read Interval
  html += '<div class="field"><label class="field-label">' + esc(spT("fieldReadInterval", "Read Interval (ms)")) + '</label>' +
          '<input type="number" step="100" name="read_interval_ms" class="input" value="' + (s.read_interval_ms || 10000) + '"></div>';

  if (s.interface === "http" || s.type === "remote") {
    // The whole configuration of a remote sensor. The collector never
    // contacts the node — the node POSTs to /api/ingest — so there is no
    // address here to get wrong: the pairing is this string, compared
    // exactly (strcmp) against the "node" field of the arriving payload.
    html += '<div class="field"><label class="field-label">' + esc(spT("fieldRemoteNodeId", "Remote node id")) + '</label>' +
            '<input type="text" name="node" class="input mono" maxlength="16" value="' +
            esc(s.node !== undefined ? s.node : "") + '" placeholder="' + esc(s.id || "") + '">' +
            '<p class="hint">' + spT(
              "remoteNodeHint",
              "Must match the <b>Node id</b> in the satellite\u2019s setup portal, exactly, up to 16 characters. Left empty, the sensor id above is used instead."
            ) + '</p></div>';
  } else if (s.interface === "i2c") {
    var busVal = (s.bus !== undefined ? s.bus : 0);
    html += '<div class="form-grid">' +
            clPinField("sda", spT("fieldSdaPin", "SDA Pin"), s.sda !== undefined ? s.sda : 6) +
            clPinField("scl", spT("fieldSclPin", "SCL Pin"), s.scl !== undefined ? s.scl : 7) +
            '</div>' +
            '<div class="field"><label class="field-label">' + esc(spT("fieldI2cBus", "I2C Bus")) + '</label>' +
            '<select name="bus" class="input">' +
              '<option value="0"' + (busVal === 0 ? " selected" : "") + '>' + esc(spT("busDefault", "Bus 0 (default)")) + '</option>' +
              '<option value="1"' + (busVal === 1 ? " selected" : "") + '>' + esc(spT("busSecond", "Bus 1 (second controller)")) + '</option>' +
            '</select>' +
            '<div class="hint">' + spT(
              "i2cBusHint",
              "Put devices with the same fixed address on different buses — e.g. VEML6075 and VEML7700 are both 0x10 and cannot share one. Bus 1 needs a chip with two I2C controllers (ESP32-S3, ESP32); the ESP32-C3 has only bus 0. Each bus needs its own SDA/SCL pins and its own pull-ups."
            ) + '</div></div>';
  } else if (s.interface === "uart") {
    html += '<div class="form-grid">' +
            clPinField("uart_rx", spT("fieldRxPin", "RX Pin"), s.uart_rx !== undefined ? s.uart_rx : 20) +
            clPinField("uart_tx", spT("fieldTxPin", "TX Pin"), s.uart_tx !== undefined ? s.uart_tx : -1) +
            '</div>';
    html += '<div class="field"><label class="field-label">' + esc(spT("fieldBaudRate", "Baud Rate")) + '</label><select name="baud" class="input">' +
            '<option value="9600"' + (s.baud == 9600 ? ' selected' : '') + '>9600</option>' +
            '<option value="19200"' + (s.baud == 19200 ? ' selected' : '') + '>19200</option>' +
            '<option value="38400"' + (s.baud == 38400 ? ' selected' : '') + '>38400</option>' +
            '<option value="115200"' + (s.baud == 115200 ? ' selected' : '') + '>115200</option>' +
            '</select></div>';
    if (s.type === "sds011") {
      html += '<div class="field"><label class="field-label">' + esc(spT("fieldWorkPeriod", "Working Period (minutes)")) + '</label>' +
              '<input type="number" min="0" max="30" name="work_period_min" class="input" value="' + (s.work_period_min !== undefined ? s.work_period_min : 1) + '">' +
              '<p class="hint">' + esc(spT("workPeriodHint", "0 = Continuous. 1-30 = Sensor sleeps and wakes automatically.")) + '</p></div>';
      html += '<div class="field" style="margin-top:10px"><label style="display:flex;align-items:center;gap:6px;cursor:pointer">' +
              '<input type="checkbox" name="humidityCorrectionEnabled"' + (s.humidityCorrectionEnabled ? ' checked' : '') + '> ' + esc(spT("fieldHumidityCorrection", "Enable Humidity Correction")) + '</label>' +
              '<p class="hint">' + esc(spT("humidityCorrectionHint", "Requires a humidity sensor in the stream.")) + '</p></div>';
      html += '<div class="field"><label class="field-label">' + spT("fieldCorrectionKappa", "Correction &kappa; (Köhler)") + '</label>' +
              '<input type="number" step="0.05" min="0" max="2" name="humidityCorrectionKappa" class="input" value="' + (s.humidityCorrectionKappa !== undefined ? s.humidityCorrectionKappa : 0.35) + '"></div>';
    }
  } else if (s.interface === "pulse") {
    html += clPinField("pin", spT("fieldPin", "Pin"), s.pin !== undefined ? s.pin : 9);
    if (s.type === "yfs201") {
      html += '<div class="form-grid">' +
              '<div class="field"><label class="field-label">' + esc(spT("fieldPulsesPerLiter", "Pulses/Liter")) + '</label><input type="number" step="0.1" name="pulses_per_liter" class="input" value="' + (s.pulses_per_liter !== undefined ? s.pulses_per_liter : 450) + '"></div>' +
              '<div class="field"><label class="field-label">' + esc(spT("fieldMultiplier", "Multiplier")) + '</label><input type="number" step="0.1" name="calibration" class="input" value="' + (s.calibration !== undefined ? s.calibration : 1.0) + '"></div>' +
              '</div>';
    }
  }

  html += clCalSectionHtml(s);

  // Support for custom JSON fields (advanced)
  // A yellow pin (strap, console, no pad) is allowed, as on the node's page:
  // the sensor is saved with allow_unsafe_pins so the firmware accepts it at
  // init, and this line says so. Shown by clWirePinWarn.
  html += '<p id="sensor-pinwarn" class="hint" style="display:none;color:var(--warn)"></p>';

  var stdKeys = ["id", "type", "enabled", "interface", "read_interval_ms", "sda", "scl", "bus", "uart_rx", "uart_tx", "baud", "pin", "node", "work_period_min", "pulses_per_liter", "calibration", "humidityCorrectionEnabled", "humidityCorrectionKappa", "allow_unsafe_pins"];
  // The per-metric object has its own section now; a scalar "calibration"
  // (water flow) is the Multiplier field, so both stay out of the overlay.
  stdKeys.push(clCalKey(s));
  var advObj = {};
  for (var k in s) {
    if (stdKeys.indexOf(k) === -1) advObj[k] = s[k];
  }
  var advStr = Object.keys(advObj).length > 0 ? JSON.stringify(advObj) : "{}";
  html += '<div class="field" style="margin-top:1rem"><label class="field-label">' + esc(spT("fieldAdvanced", "Advanced (JSON overlay)")) + '</label>' +
          '<input type="text" name="advanced" class="input" value="' + esc(advStr) + '">' +
          '<p class="hint">' + esc(spT("advancedHint", "Additional parameters applied directly to this sensor. Keep as {} if unsure.")) + '</p></div>';

  html += '</form>';
  return html;
}

// Inline-edit mount point for desktop (≥780 px).  Expands a panel below
// the row, replacing the modal popup for less context loss.  Falls back
// to the popup on mobile and when the row can't be located.
// ── Pin fields (www/js/pins.js) ─────────────────────────────────────────────
// The board context and the Hardware page's pins, loaded once per page load.
// Until they arrive a field still takes a GPIO number; labels, hints and the
// duplicate check light up when they do.
var CL_PINS = { ctx: null, hw: null, ready: null };
function clPinsReady() {
  if (CL_PINS.ready) return CL_PINS.ready;
  var hw = (typeof CFG !== "undefined" && CFG && CFG.hardware)
    ? Promise.resolve(CFG.hardware)
    : fetchWithTimeout("/export_settings", {}, 15000)
        .then(function (r) { return r.ok ? r.json() : {}; })
        .then(function (d) { return d.hardware || null; })
        .catch(function () { return null; });
  CL_PINS.ready = Promise.all([window.Pins ? Pins.load() : Promise.resolve(null), hw])
    .then(function (r) {
      CL_PINS.ctx = r[0] ? Pins.ctx(r[0], r[0].active) : null;
      CL_PINS.hw = r[1];
      // No profile list (a failed fetch): ask again next time, like Pins.load().
      if (!r[0] || !(r[0].profiles || []).length) CL_PINS.ready = null;
      return CL_PINS;
    });
  return CL_PINS.ready;
}
if (typeof window !== "undefined" && window.Pins) clPinsReady();

// One pin field. The text box shows the board's label; the hidden input
// under `name` carries the GPIO, so FormData reads a number as before.
function clPinField(name, label, value) {
  var v = parseInt(value, 10);
  return Pins.field("s-" + name, label, isNaN(v) ? -1 : v, CL_PINS.ctx, { target: name });
}

// The pins this form uses (with the bus share for I2C), plus the Hardware
// page's and every other enabled sensor's — so a sensor put on the WiFi
// button's GPIO, or on another UART sensor's RX, is red before it is saved.
function clFormUses(form, idx) {
  var ctx = CL_PINS.ctx, out = [], cur = (PCFG && PCFG.sensors && PCFG.sensors[idx]) || {};
  var busEl = form.querySelector('select[name="bus"]');
  var bus = busEl ? busEl.value : (cur.bus || 0);
  form.querySelectorAll("input[data-pin]").forEach(function (inp) {
    var r = Pins.parse(ctx, inp.value), k = inp.getAttribute("data-pin-target");
    if (r.gpio == null) return;
    out.push({ key: inp.getAttribute("data-pin"), g: r.gpio,
               who: (cur.id || cur.type || "") + " " + k.toUpperCase(),
               share: (k === "sda" || k === "scl") ? "i2c" + bus + ":" + k : "" });
  });
  return out.concat(Pins.hardwareUses(CL_PINS.hw), Pins.sensorUses(PCFG && PCFG.sensors, idx));
}

function clFormGpios(form) {
  var g = [];
  form.querySelectorAll("input[data-pin]").forEach(function (inp) {
    g.push(Pins.parse(CL_PINS.ctx, inp.value).gpio);
  });
  return g;
}

// Live hints under each pin of the sensor edit form (inline and popup).
// Returns nothing; the repaint function is kept on the form for the save.
function clWirePinWarn() {
  var form = document.getElementById("sensorEditForm");
  if (!form || !window.Pins) return;
  var idx = window.clCurrentEditingSensor;
  clPinsReady().then(function () {
    if (!form.isConnected) return;
    // Built before the context arrived: show the board's labels now.
    form.querySelectorAll("input[data-pin]").forEach(function (inp) {
      var hid = form.querySelector('input[name="' + inp.getAttribute("data-pin-target") + '"]');
      if (hid && document.activeElement !== inp) inp.value = Pins.text(CL_PINS.ctx, parseInt(hid.value, 10));
    });
    form._pinsRepaint = Pins.wire(form, CL_PINS.ctx || { profile: null, board: null },
      function () { return clFormUses(form, idx); },
      function () {
        var warn = document.getElementById("sensor-pinwarn");
        if (!warn) return;
        var on = Pins.needsUnsafe(CL_PINS.ctx, clFormGpios(form));
        warn.style.display = on ? "" : "none";
        warn.textContent = on ? I18n.t("pins.unsafeAuto") : "";
      });
  });
}

function _clEditInline(idx, s) {
  var row = document.querySelector('.sensor-list-row[data-sensor-idx="' + idx + '"]');
  if (!row) return false;
  // Close any open expander first
  document.querySelectorAll(".sensor-inline-edit").forEach(function (n) { n.remove(); });

  var panel = document.createElement("div");
  panel.className = "sensor-inline-edit";
  panel.setAttribute("data-sensor-idx", idx);
  var closeAria = window.I18n ? I18n.t("chrome.closeAria") : "Close";
  panel.innerHTML =
    '<div class="sensor-inline-head">' +
      '<div class="sensor-inline-title">' + esc(spT("editPrefix", "Edit · ")) + '<span class="mono">' + esc(s.id || s.type) + '</span></div>' +
      '<button type="button" class="btn-mini" data-role="close" aria-label="' + esc(closeAria) + '"><span data-icon="x"></span></button>' +
    '</div>' +
    '<div class="sensor-inline-body">' + _clBuildEditFormHtml(s) + '</div>' +
    '<div class="sensor-inline-foot">' +
      '<button type="button" class="btn" data-role="cancel">' + esc(window.I18n ? I18n.t("common.cancel") : "Cancel") + '</button>' +
      '<button type="button" class="btn primary" data-role="save"><span data-icon="save"></span> ' + esc(window.I18n ? I18n.t("common.save") : "Save") + '</button>' +
    '</div>';

  // Mount immediately after the row so the expander shows in flow
  row.parentNode.insertBefore(panel, row.nextSibling);
  if (window.Icons && Icons.swap) Icons.swap(panel);
  clWirePinWarn();
  clWireCal(s);

  function dismiss() { panel.remove(); window.clCurrentEditingSensor = -1; }
  panel.querySelector('[data-role="close"]').addEventListener("click", dismiss);
  panel.querySelector('[data-role="cancel"]').addEventListener("click", dismiss);
  panel.querySelector('[data-role="save"]').addEventListener("click", function () {
    if (clSaveEditedSensor() !== false) dismiss();
  });
  return true;
}

function clEditSensor(idx) {
  if (!PCFG || !PCFG.sensors) return;
  window.clCurrentEditingSensor = idx;
  var s = PCFG.sensors[idx];

  // Inline on desktop (≥ 780 px); modal popup on mobile or when the row
  // can't be located (e.g. when invoked from the command palette before
  // the Core Logic page has rendered yet).
  var canInline = window.innerWidth >= 780 &&
    document.querySelector('.sensor-list-row[data-sensor-idx="' + idx + '"]');
  if (canInline && _clEditInline(idx, s)) return;

  var b = document.getElementById("sensorPopupBody");
  var t = document.getElementById("sensorPopupTitle");
  var f = document.getElementById("sensorPopupFooter");
  var btn = document.getElementById("sensorPopupSaveBtn");
  t.textContent = spT("editSensorTitle", "Edit Sensor: {name}", { name: s.id || s.type });
  b.innerHTML = _clBuildEditFormHtml(s);
  f.style.display = "flex";
  btn.onclick = clSaveEditedSensor;
  document.getElementById("sensorPopup").style.display = "flex";
  clWirePinWarn();
  clWireCal(s);
}

function clSaveEditedSensor() {
  var idx = window.clCurrentEditingSensor;
  if (idx < 0 || !PCFG || !PCFG.sensors) return;
  var s = PCFG.sensors[idx];
  var form = document.getElementById("sensorEditForm");
  if (!form) return;
  // A red pin (flash bus, not on the chip, taken by something else) stops
  // the save here, with the reason already under the field.
  if (form._pinsRepaint && !form._pinsRepaint()) {
    showToast(I18n.t("pins.fixPins"), "err");
    return false;
  }
  var fd = new FormData(form);
  
  s.id = fd.get("id");
  s.enabled = fd.get("enabled") === "on";
  s.read_interval_ms = parseInt(fd.get("read_interval_ms") || 10000, 10);
  
  if (s.interface === "http" || s.type === "remote") {
    var nodeVal = (fd.get("node") || "").trim();
    // Empty means "use the sensor id", which is what RemoteNodeSensor::init
    // does with a missing field — so store nothing rather than an empty
    // string that would look like a deliberate, unmatchable node id.
    if (nodeVal) s.node = nodeVal; else delete s.node;
  } else if (s.interface === "i2c") {
    s.sda = parseInt(fd.get("sda") || 6, 10);
    s.scl = parseInt(fd.get("scl") || 7, 10);
    s.bus = parseInt(fd.get("bus") || 0, 10);
    if (!(s.bus === 0 || s.bus === 1)) s.bus = 0;
  } else if (s.interface === "uart") {
    s.uart_rx = parseInt(fd.get("uart_rx") || 20, 10);
    s.uart_tx = parseInt(fd.get("uart_tx") || -1, 10);
    s.baud = parseInt(fd.get("baud") || 9600, 10);
    if (s.type === "sds011") {
      s.work_period_min = parseInt(fd.get("work_period_min") || 1, 10);
      s.humidityCorrectionEnabled = fd.get("humidityCorrectionEnabled") === "on";
      s.humidityCorrectionKappa = parseFloat(fd.get("humidityCorrectionKappa") || 0.35);
    }
  } else if (s.interface === "pulse") {
    s.pin = parseInt(fd.get("pin") || 9, 10);
    if (s.type === "yfs201") {
      s.pulses_per_liter = parseFloat(fd.get("pulses_per_liter") || 450.0);
      s.calibration = parseFloat(fd.get("calibration") || 1.0);
    }
  }

  clReadCal(form, s);

  var adv = fd.get("advanced");
  if (adv && adv !== "{}") {
    try {
      var advObj = JSON.parse(adv);
      for (var k in advObj) s[k] = advObj[k];
    } catch(e) {
      showToast(spT("invalidAdvancedJson", "Invalid Advanced JSON. Saving standard fields only."), "error");
    }
  }

  // Set exactly when a yellow pin is in use: the firmware refuses a strap,
  // console or no-pad pin at init without it, and the page has already said
  // why the pin is risky. Kept tidy (absent) otherwise. Judged on every pin
  // the sensor stores once the advanced JSON is applied, not only the ones
  // the form shows: a gpio/adc pin or an HC-SR04 trig_pin/echo_pin lives
  // only in advanced JSON, and dropping the flag for it made the firmware
  // refuse the sensor at the next boot.
  if (window.Pins && CL_PINS.ctx && CL_PINS.ctx.profile) {
    if (Pins.needsUnsafe(CL_PINS.ctx, Pins.sensorGpios(s).concat(clFormGpios(form)))) s.allow_unsafe_pins = true;
    else delete s.allow_unsafe_pins;
  }

  clRenderSensors(PCFG.sensors);
  document.getElementById("sensorPopup").style.display = "none";
}

function clSave() {
  // Message element lives on whichever page hosts the sensor list
  // (corelogic legacy or the unified Sensors page).
  var msg = document.getElementById("cl-msg") || document.getElementById("ss-msg");
  if (!PCFG) {
    if (msg) {
      msg.textContent = spT("noConfigLoaded", "✗ No config loaded");
      msg.className = "alert alert-danger";
    }
    return;
  }

  // Read form values back into PCFG
  var modeEl = document.getElementById("cl-mode");
  if (modeEl) PCFG.mode = modeEl.value;

  if (!PCFG.aggregation) PCFG.aggregation = {};
  var amEl = document.getElementById("cl-aggmode");
  if (amEl) PCFG.aggregation.default_mode = amEl.value;
  var abEl = document.getElementById("cl-aggbucket");
  if (abEl) PCFG.aggregation.default_bucket_min = parseInt(abEl.value, 10);
  var mpEl = document.getElementById("cl-maxpoints");
  if (mpEl) PCFG.aggregation.max_points = parseInt(mpEl.value, 10);
  var rtEl = document.getElementById("cl-retention");
  if (rtEl) PCFG.aggregation.raw_retention_days = parseInt(rtEl.value, 10);

  if (!PCFG.export) PCFG.export = {};
  if (!PCFG.export.mqtt) PCFG.export.mqtt = {};
  if (!PCFG.export.http) PCFG.export.http = {};
  if (!PCFG.export.sensor_community) PCFG.export.sensor_community = {};
  if (!PCFG.export.opensensemap) PCFG.export.opensensemap = {};

  var mqttEl = document.getElementById("cl-exp-mqtt");
  if (mqttEl) PCFG.export.mqtt.enabled = mqttEl.checked;
  var httpEl = document.getElementById("cl-exp-http");
  if (httpEl) PCFG.export.http.enabled = httpEl.checked;
  var scEl = document.getElementById("cl-exp-sc");
  if (scEl) PCFG.export.sensor_community.enabled = scEl.checked;
  var osmEl = document.getElementById("cl-exp-osm");
  if (osmEl) PCFG.export.opensensemap.enabled = osmEl.checked;

  // Sleep settings
  if (!PCFG.sleep) PCFG.sleep = {};
  if (!PCFG.sleep.continuous) PCFG.sleep.continuous = {};
  if (!PCFG.sleep.hybrid) PCFG.sleep.hybrid = {};
  var ciEl = document.getElementById("cl-cont-idle");
  if (ciEl)
    PCFG.sleep.continuous.idle_timeout_ms =
      parseInt(ciEl.value, 10) || CL_SLEEP_DEFAULTS.cont_idle_timeout_ms;
  var ccEl = document.getElementById("cl-cont-cpu");
  if (ccEl)
    PCFG.sleep.continuous.idle_cpu_mhz =
      parseInt(ccEl.value, 10) || CL_SLEEP_DEFAULTS.cont_idle_cpu_mhz;
  var cmEl = document.getElementById("cl-cont-modem");
  if (cmEl) PCFG.sleep.continuous.modem_sleep = cmEl.checked;
  var hiEl = document.getElementById("cl-hyb-idle");
  if (hiEl)
    PCFG.sleep.hybrid.idle_before_sleep_ms =
      parseInt(hiEl.value, 10) || CL_SLEEP_DEFAULTS.hyb_idle_before_sleep_ms;
  var hsEl = document.getElementById("cl-hyb-sleep");
  if (hsEl)
    PCFG.sleep.hybrid.sleep_duration_ms =
      parseInt(hsEl.value, 10) || CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms;
  var haEl = document.getElementById("cl-hyb-active");
  if (haEl)
    PCFG.sleep.hybrid.active_window_ms =
      parseInt(haEl.value, 10) || CL_SLEEP_DEFAULTS.hyb_active_window_ms;

  if (msg) {
    msg.textContent = window.I18n ? I18n.t("common.saving") : "Saving…";
    msg.className = "";
  }

  pcfgSave(PCFG, function (ok, err) {
    if (ok) {
      if (msg) {
        msg.textContent = spT("savedRestarting", "✓ Saved! Restarting device…");
        msg.className = "";
      }
      // Trigger restart so new mode takes effect
      setTimeout(function () {
        postWithCsrf("/api/platform_reload", { method: "POST" }, 30000).catch(function () {});
      }, 500);
    } else {
      if (msg) {
        msg.textContent = spT("saveFailed", "✗ Save failed: {err}", { err: err });
        msg.className = "";
      }
    }
  });
}

// Show/hide the Power & Sleep card and its sub-panels based on selected mode.
function clUpdateSleepPanel() {
  var modeEl = document.getElementById("cl-mode");
  var mode = modeEl ? modeEl.value : "legacy";
  var card = document.getElementById("cl-sleep-card");
  var contDiv = document.getElementById("cl-sleep-cont");
  var hybDiv = document.getElementById("cl-sleep-hyb");
  if (card)
    card.style.display =
      mode === "continuous" || mode === "hybrid" ? "" : "none";
  if (contDiv) contDiv.style.display = mode === "continuous" ? "" : "none";
  if (hybDiv) hybDiv.style.display = mode === "hybrid" ? "" : "none";
  clUpdateHybCycle();
}

// Update the hybrid cycle summary label (sleep + active = total).
function clUpdateHybCycle() {
  var lbl = document.getElementById("cl-hyb-cycle-label");
  if (!lbl) return;
  var hsEl = document.getElementById("cl-hyb-sleep");
  var haEl = document.getElementById("cl-hyb-active");
  var sleepMs =
    parseInt(hsEl ? hsEl.value : CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms, 10) ||
    CL_SLEEP_DEFAULTS.hyb_sleep_duration_ms;
  var activeMs =
    parseInt(haEl ? haEl.value : CL_SLEEP_DEFAULTS.hyb_active_window_ms, 10) ||
    CL_SLEEP_DEFAULTS.hyb_active_window_ms;
  var totalMs = sleepMs + activeMs;
  lbl.textContent = spT("hybCycleSummary", "{sleep}s sleep + {active}s active = {total}s per cycle", {
    sleep: (sleepMs / 1000).toFixed(0),
    active: (activeMs / 1000).toFixed(0),
    total: (totalMs / 1000).toFixed(0),
  });
}

// ============================================================================
// EXPORT PAGE
// ============================================================================
function expLoad() {
  var caps = (ST && ST.caps && ST.caps.exporters) || ["mqtt", "http", "sensor_community", "opensensemap"];
  if (document.getElementById("card-mqtt")) document.getElementById("card-mqtt").style.display = caps.indexOf("mqtt") >= 0 ? "" : "none";
  if (document.getElementById("card-http")) document.getElementById("card-http").style.display = caps.indexOf("http") >= 0 ? "" : "none";
  if (document.getElementById("card-sc")) document.getElementById("card-sc").style.display = caps.indexOf("sensor_community") >= 0 ? "" : "none";
  if (document.getElementById("card-osm")) document.getElementById("card-osm").style.display = caps.indexOf("opensensemap") >= 0 ? "" : "none";
  pcfgLoad(function (cfg) {
    var exp = cfg.export || {};

    // MQTT
    var m = exp.mqtt || {};
    _setVal("exp-mqtt-en", m.enabled || false, true);
    _setVal("exp-mqtt-host", m.broker || "");
    _setVal("exp-mqtt-port", m.port || 1883);
    _setVal("exp-mqtt-prefix", m.topic_prefix || "waterlogger");
    _setVal("exp-mqtt-clientid", m.client_id || "");
    _setVal("exp-mqtt-user", m.username || "");
    _setVal("exp-mqtt-pass", m.password || "");
    _setVal("exp-mqtt-retain", m.retain || false, true);
    var tlsEl = document.getElementById("exp-mqtt-tls");
    if (tlsEl) {
      tlsEl.value = m.use_tls ? "tls" : "plain";
      tlsEl.onchange = function () {
        var portEl = document.getElementById("exp-mqtt-port");
        if (portEl) portEl.value = this.value === "tls" ? "8883" : "1883";
      };
    }

    // HTTP
    var h = exp.http || {};
    _setVal("exp-http-en", h.enabled || false, true);
    _setVal("exp-http-url", h.url || "");
    _setVal("exp-http-auth", (h.headers && h.headers.Authorization) || "");

    // Sensor.Community
    var sc = exp.sensor_community || {};
    _setVal("exp-sc-en", sc.enabled || false, true);

    // openSenseMap
    var osm = exp.opensensemap || {};
    _setVal("exp-osm-en", osm.enabled || false, true);
    _setVal("exp-osm-boxid", osm.box_id || "");
    _setVal("exp-osm-token", osm.access_token || "");

    // OSM sensor IDs grid. Drawn at once from the usual metrics plus every
    // metric already mapped, so the mapping is on the page (and saved intact)
    // whatever /api/sensors does; then widened to the metrics the configured
    // sensors actually report, keeping anything typed in the meantime.
    var osmIds = osm.sensor_ids || {};
    var osmSet = {};
    [
      "temperature", "humidity", "pressure", "pm25", "pm10",
      "tvoc", "tvoc_est", "eco2", "iaq", "gas_resistance",
      "dew_point", "flow_rate", "rain_total", "wind_speed",
    ].concat(Object.keys(osmIds)).forEach(function (m) { osmSet[m] = true; });
    _expOsmGrid(osmSet, osmIds);
    if (typeof getSensors === "function") {
      getSensors({ maxAgeMs: 0 }).then(function (d) {
        var added = false;
        ((d && d.sensors) || []).forEach(function (s) {
          (s.metrics || []).forEach(function (m) {
            // /api/sensors lists metric names as plain strings.
            var id = typeof m === "string" ? m : m && m.id;
            if (id && !osmSet[id]) { osmSet[id] = true; added = true; }
          });
        });
        if (added) _expOsmGrid(osmSet, _expOsmRead());
      }).catch(function () { /* the static grid is already there */ });
    }

    // Common schedule + sensor selection, and each exporter's override.
    var sensorIds = _expSensorIds(cfg);
    var d = exp.defaults || {};
    _setVal("exp-def-interval", Math.round((d.interval_ms || 60000) / 1000));
    var defBox = document.getElementById("exp-def-sensors");
    if (defBox) defBox.innerHTML = _expSensorChecks(sensorIds, d.sensors || []);
    _expRenderScope("mqtt", exp.mqtt || {}, sensorIds);
    _expRenderScope("http", exp.http || {}, sensorIds);
    _expRenderScope("sc", exp.sensor_community || {}, sensorIds);
    _expRenderScope("osm", exp.opensensemap || {}, sensorIds);
  });
}

// openSenseMap: one text field per metric, the box's sensor id for it.
function _expOsmGrid(set, ids) {
  var osmDiv = document.getElementById("exp-osm-ids");
  if (!osmDiv) return;
  var ph = esc(spT("osmSensorIdPh", "sensor ID…"));
  osmDiv.innerHTML =
    '<div class="form-grid" style="flex-wrap:wrap">' +
    Object.keys(set)
      .sort()
      .map(function (m) {
        return (
          '<div class="field" style="min-width:180px">' +
          '<label class="field-label">' + esc(m) + "</label>" +
          '<input type="text" class="input exp-osm-metric-input" data-metric="' + esc(m) +
          '" value="' + esc(ids[m] || "") + '" placeholder="' + ph + '">' +
          "</div>"
        );
      })
      .join("") +
    "</div>";
}

function _expOsmRead() {
  var ids = {};
  document.querySelectorAll("#exp-osm-ids .exp-osm-metric-input").forEach(function (el) {
    var v = (el.value || "").trim();
    var m = el.getAttribute("data-metric");
    if (v && m) ids[m] = v;
  });
  return ids;
}

// ---------------------------------------------------------------------------
// Export scope = WHEN (interval) + WHAT (sensors). Stored as
//   export.defaults.{interval_ms, sensors}     — common
//   export.<name>.{interval_ms, sensors}       — override; key absent = common
// sensors is a list of sensor ids; an empty list means all sensors.
// See src/export/ExportManager.h for how the firmware resolves it.
// ---------------------------------------------------------------------------
function _expSensorIds(cfg) {
  var out = [];
  (cfg.sensors || []).forEach(function (s) {
    if (s && s.id && out.indexOf(s.id) < 0) out.push(s.id);
  });
  return out;
}

function _expSensorChecks(ids, selected) {
  // Keep selected ids that are no longer configured visible (and ticked), so
  // saving doesn't silently drop them from the selection.
  var all = ids.slice();
  (selected || []).forEach(function (id) {
    if (all.indexOf(id) < 0) all.push(id);
  });
  if (!all.length) {
    return '<p class="hint">' + esc(spT("expNoSensors", "No sensors configured.")) + "</p>";
  }
  return (
    '<div style="display:flex;flex-wrap:wrap;gap:.25rem 1rem">' +
    all
      .map(function (id) {
        var on = (selected || []).indexOf(id) >= 0;
        var missing = ids.indexOf(id) < 0;
        return (
          '<label class="check"><input type="checkbox" class="exp-sid" value="' +
          esc(id) + '"' + (on ? " checked" : "") + "><span>" + esc(id) +
          (missing ? " " + esc(spT("expSensorMissing", "(not configured)")) : "") +
          "</span></label>"
        );
      })
      .join("") +
    "</div>"
  );
}

function _expReadChecks(containerId) {
  var box = document.getElementById(containerId);
  if (!box) return [];
  var out = [];
  box.querySelectorAll("input.exp-sid").forEach(function (cb) {
    if (cb.checked) out.push(cb.value);
  });
  return out;
}

function _expRenderScope(key, ecfg, ids) {
  var box = document.getElementById("exp-" + key + "-scope");
  if (!box) return;
  var minSec = parseInt(box.getAttribute("data-min-sec") || "5", 10);
  var ownIv = (ecfg.interval_ms || 0) > 0;
  var ownSel = Array.isArray(ecfg.sensors);
  var p = "exp-" + key;
  box.innerHTML =
    '<div class="field" style="margin-top:.75rem">' +
    '<label class="field-label" for="' + p + '-iv">' + esc(spT("expIntervalLabel", "Send interval (seconds)")) + "</label>" +
    '<label class="check"><input type="checkbox" id="' + p + '-iv-common"' + (ownIv ? "" : " checked") + "><span>" +
    esc(spT("expUseCommonInterval", "Use the common interval")) + "</span></label>" +
    '<input type="number" id="' + p + '-iv" class="input" min="' + minSec + '" step="1" value="' +
    (ownIv ? Math.round(ecfg.interval_ms / 1000) : "") + '"' + (ownIv ? "" : ' style="display:none"') + ">" +
    (minSec > 5
      ? '<p class="hint">' + esc(spT("expIntervalMin", "Minimum {n} s — a shorter value (also the common one) is raised to it.", { n: minSec })) + "</p>"
      : "") +
    "</div>" +
    '<div class="field">' +
    '<label class="field-label">' + esc(spT("expSensorsLabel", "Sensors to send")) + "</label>" +
    '<label class="check"><input type="checkbox" id="' + p + '-sel-common"' + (ownSel ? "" : " checked") + "><span>" +
    esc(spT("expUseCommonSensors", "Use the common sensor selection")) + "</span></label>" +
    '<div id="' + p + '-sel"' + (ownSel ? "" : ' style="display:none"') + ">" +
    _expSensorChecks(ids, ownSel ? ecfg.sensors : []) +
    '<p class="hint">' + esc(spT("expSensorsAllHint", "None ticked = all sensors.")) + "</p>" +
    "</div></div>";

  var ivCommon = document.getElementById(p + "-iv-common");
  var ivInput = document.getElementById(p + "-iv");
  ivCommon.addEventListener("change", function () {
    ivInput.style.display = ivCommon.checked ? "none" : "";
    if (!ivCommon.checked && !ivInput.value) {
      var def = parseInt((document.getElementById("exp-def-interval") || {}).value, 10) || 60;
      ivInput.value = Math.max(def, minSec);
    }
  });
  var selCommon = document.getElementById(p + "-sel-common");
  var selBox = document.getElementById(p + "-sel");
  selCommon.addEventListener("change", function () {
    selBox.style.display = selCommon.checked ? "none" : "";
  });
}

// Write the scope block of exporter `key` into its config object `target`.
function _expReadScope(key, target) {
  var p = "exp-" + key;
  var box = document.getElementById(p + "-scope");
  var ivCommon = document.getElementById(p + "-iv-common");
  var selCommon = document.getElementById(p + "-sel-common");
  if (!box || !ivCommon || !selCommon) return target;   // page not rendered: leave as is
  var minSec = parseInt(box.getAttribute("data-min-sec") || "5", 10);

  delete target.interval_ms;                 // absent = common interval
  if (!ivCommon.checked) {
    var sec = parseInt((document.getElementById(p + "-iv") || {}).value, 10);
    if (sec > 0) target.interval_ms = Math.max(sec, minSec) * 1000;
  }
  delete target.sensors;                     // absent = common selection
  if (!selCommon.checked) target.sensors = _expReadChecks(p + "-sel");
  return target;
}

function _setVal(id, val, isCheck) {
  var el = document.getElementById(id);
  if (!el) return;
  if (isCheck) el.checked = !!val;
  else el.value = val;
}

function expSave() {
  var msg = document.getElementById("exp-msg");
  if (!PCFG) PCFG = {};
  if (!PCFG.export) PCFG.export = {};

  // Common schedule + sensor selection
  var defSec = parseInt((document.getElementById("exp-def-interval") || {}).value, 10) || 60;
  PCFG.export.defaults = Object.assign({}, PCFG.export.defaults, {
    interval_ms: Math.max(defSec, 5) * 1000,
    sensors: _expReadChecks("exp-def-sensors"),
  });

  // Each exporter object is merged over what is already there, so keys this
  // page has no field for (mqtt.ha_discovery, http.method, …) survive a save.
  function merged(key, fields) {
    return Object.assign({}, PCFG.export[key] || {}, fields);
  }

  // MQTT
  PCFG.export.mqtt = _expReadScope("mqtt", merged("mqtt", {
    enabled: !!(document.getElementById("exp-mqtt-en") || {}).checked,
    broker: (document.getElementById("exp-mqtt-host") || {}).value || "",
    port: parseInt(
      (document.getElementById("exp-mqtt-port") || {}).value || "1883",
      10,
    ),
    topic_prefix:
      (document.getElementById("exp-mqtt-prefix") || {}).value || "waterlogger",
    client_id: (document.getElementById("exp-mqtt-clientid") || {}).value || "",
    username: (document.getElementById("exp-mqtt-user") || {}).value || "",
    password: (document.getElementById("exp-mqtt-pass") || {}).value || "",
    retain: !!(document.getElementById("exp-mqtt-retain") || {}).checked,
    use_tls: (document.getElementById("exp-mqtt-tls") || {}).value === "tls",
    qos: 0,
  }));

  // HTTP
  var authVal = (document.getElementById("exp-http-auth") || {}).value || "";
  PCFG.export.http = _expReadScope("http", merged("http", {
    enabled: !!(document.getElementById("exp-http-en") || {}).checked,
    url: (document.getElementById("exp-http-url") || {}).value || "",
    method: (PCFG.export.http && PCFG.export.http.method) || "POST",
    headers: authVal ? { Authorization: authVal } : {},
  }));

  // Sensor.Community
  PCFG.export.sensor_community = _expReadScope("sc", merged("sensor_community", {
    enabled: !!(document.getElementById("exp-sc-en") || {}).checked,
  }));

  // openSenseMap. If the grid never rendered, keep the saved mapping rather
  // than overwrite it with an empty one.
  var osmGridUp = !!document.querySelector("#exp-osm-ids .exp-osm-metric-input");
  var ids = osmGridUp ? _expOsmRead() : ((PCFG.export.opensensemap || {}).sensor_ids || {});
  PCFG.export.opensensemap = _expReadScope("osm", merged("opensensemap", {
    enabled: !!(document.getElementById("exp-osm-en") || {}).checked,
    box_id: (document.getElementById("exp-osm-boxid") || {}).value || "",
    access_token: (document.getElementById("exp-osm-token") || {}).value || "",
    sensor_ids: ids,
  }));

  if (msg) {
    msg.textContent = window.I18n ? I18n.t("common.saving") : "Saving…";
    msg.className = "";
  }
  pcfgSave(PCFG, function (ok, err) {
    if (ok) {
      if (msg) {
        msg.textContent = spT("savedRestartingExp", "✓ Saved! Restarting…");
        msg.className = "";
      }
      setTimeout(function () {
        postWithCsrf("/api/platform_reload", { method: "POST" }, 30000).catch(function () {});
      }, 500);
    } else {
      if (msg) {
        msg.textContent = spT("saveFailedShort", "✗ {err}", { err: err });
        msg.className = "";
      }
    }
  });
}

// Enrol markup-reachable handlers.  See core.js::Handlers for why the
// whitelist exists.
function sensorsPrint() { window.print(); }

registerHandlers({
  sensorsLoad: sensorsLoad,
  sensorsFilter: sensorsFilter,
  sensorsPrint: sensorsPrint,
  sensorChartLoad: sensorChartLoad,
  clToggleSensor: clToggleSensor,
  clRemoveSensor: clRemoveSensor,
  clEditSensor: clEditSensor,
  clSaveEditedSensor: clSaveEditedSensor,
  clSave: clSave,
  clLoad: clLoad,
  clUpdateSleepPanel: clUpdateSleepPanel,
  clUpdateHybCycle: clUpdateHybCycle,
  expLoad: expLoad,
  expSave: expSave,
});
