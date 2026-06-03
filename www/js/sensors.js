/**
 * /www/js/sensors.js — sensors live grid, Core Logic editor,
 * platform_config.json IO, settings import/export.
 * Loaded after settings.js. Depends on core globals + CFG.
 */
"use strict";

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
  if (msg) msg.textContent = "Loading…";
  if (grid) grid.innerHTML = "";

  // Hydrate the inline "Sensor CSV logging" card if it's mounted on this
  // page. Defined in settings.js; safe to call even if the card isn't
  // present (the fetch just populates IDs that aren't there).
  if (typeof slInit === "function") slInit();

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
          msg.textContent =
            "No sensors registered. Set mode to Continuous in Core Logic settings and configure sensors.";
        var sub = document.getElementById("sensors-sub");
        if (sub) sub.textContent = "0 sensors";
        return;
      }
      if (msg) msg.textContent = "";

      // Update page subtitle with live counts
      var nowMs = Date.now();
      var errCount = d.sensors.filter(function(s) { return s.status === "error"; }).length;
      var okCount  = d.sensors.filter(function(s) { return s.status === "ok"; }).length;
      var sub = document.getElementById("sensors-sub");
      if (sub) {
        var parts = [okCount + " active"];
        if (errCount) parts.push(errCount + " errored");
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
              ageStr = _sensorFmtAge(ageMs) + " ago";
              if (ageMs > freshMs * 2) {
                stateClass = " stale";
              } else if (s.periodic && ageMs > s.read_interval_ms) {
                sleeping = true;
                ageStr = "sleeping · " + ageStr;
              }
            }
            if (s.status === "error")    stateClass = " err";
            if (s.status === "disabled") stateClass = " dis";

            var badgeClass =
              s.status === "ok" ? "ok" :
              s.status === "disabled" ? "dim" : "err";
            var badgeText =
              s.status === "ok" ? "OK" :
              s.status === "disabled" ? "OFF" : "ERR";

            var transport = s.transport || s.type || "";
            var errChip = "";
            if (s.status === "error" && s.status_detail) {
              errChip = '<div class="s-metrics"><span class="badge err" style="font-size:10px">' +
                        esc(s.status_detail) + '</span></div>';
            }

            var ageRefMs = s.last_read_ms || 0;
            var ageIcon = "", ageColor = "inherit";
            if (stateClass === " err")        { ageIcon = "⊘"; ageColor = "var(--err)"; }
            else if (stateClass === " stale") { ageIcon = "⚠"; ageColor = "var(--warn)"; }
            else if (sleeping)                { ageIcon = "💤"; ageColor = "var(--text-3)"; }
            else if (ageRefMs && stateClass !== " dis") { ageIcon = "✓"; ageColor = "var(--ok)"; }

            var cardName = esc(s.name) + (m && metrics.length > 1 ? " (" + esc(m) + ")" : "");
            
            html.push(
              '<div class="sensor' + stateClass + '" data-sensor-name="' + esc((cardName + ' ' + (s.id || '')).toLowerCase().trim()) + '">' +
                '<div class="s-head">' +
                  '<div>' +
                    '<div class="s-name">' + cardName + '</div>' +
                    '<div class="s-id">' + esc(s.id) +
                      (transport ? ' · ' + esc(transport) : '') + '</div>' +
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
                  '<span>' + esc(transport) + '</span>' +
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
        sel.innerHTML = '<option value="">— select sensor —</option>' + sensorOpts;
      }
      var sel2 = document.getElementById("sc-sensor2");
      if (sel2) {
        sel2.innerHTML = '<option value="">— none —</option>' + sensorOpts;
      }
    })
    .catch(function (e) {
      if (msg) msg.textContent = "Failed to load sensors: " + e;
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

function sensorChartLoad() {
  var sid = (document.getElementById("sc-sensor") || {}).value;
  var metric = (document.getElementById("sc-metric") || {}).value;
  var agg = (document.getElementById("sc-agg") || {}).value || "5m";
  var mode = (document.getElementById("sc-mode") || {}).value || "lttb";
  var range = parseInt(
    (document.getElementById("sc-range") || {}).value || "86400",
    10,
  );
  var msg = document.getElementById("sc-msg");

  if (!sid) return;

  // Update metric dropdown when sensor changes
  var metricSel = document.getElementById("sc-metric");
  if (metricSel && !metric) {
    if (msg) msg.textContent = "Select a metric…";
    return;
  }

  var now = Math.floor(Date.now() / 1000);
  var from = now - range;

  // Build primary URL
  var url1 =
    "/api/data?sensor=" +
    encodeURIComponent(sid) +
    "&metric=" +
    encodeURIComponent(metric) +
    "&from=" + from + "&to=" + now +
    "&agg=" + agg + "&mode=" + mode + "&limit=250";

  // Secondary overlay sensor
  var sid2 = (document.getElementById("sc-sensor2") || {}).value;
  var metric2 = (document.getElementById("sc-metric2") || {}).value;
  var url2 = null;
  if (sid2 && metric2) {
    url2 =
      "/api/data?sensor=" +
      encodeURIComponent(sid2) +
      "&metric=" +
      encodeURIComponent(metric2) +
      "&from=" + from + "&to=" + now +
      "&agg=" + agg + "&mode=" + mode + "&limit=250";
  }

  if (msg) msg.textContent = "Loading…";

  // Fetch primary (and optionally secondary) data
  var fetches = [fetchWithTimeout(url1, {}, 15000).then(function (r) { return r.ok ? r.json() : null; })];
  if (url2) fetches.push(fetchWithTimeout(url2, {}, 15000).then(function (r) { return r.ok ? r.json() : null; }));

  Promise.all(fetches)
    .then(function (results) {
      var d1 = results[0];
      var d2 = results.length > 1 ? results[1] : null;

      if (!d1 || !d1.data || d1.data.length === 0) {
        if (msg) msg.textContent = "No data for selected period.";
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
      if (elPts) elPts.textContent = d1.count;

      var infoStr = d1.agg + " · " + d1.mode;
      if (hasDual) infoStr += " + overlay";
      if (msg) msg.textContent = infoStr;

      var ctx = document.getElementById("sensorChart");
      if (!ctx) return;

      function render() {
        if (sensorChart) { sensorChart.destroy(); sensorChart = null; }

        // uPlot wants epoch-seconds for time scale; convert from API ts.
        var xs = d1.data.map(function (pt) { return pt.ts; });

        var series = [
          { label: "Time" },
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
      if (msg) msg.textContent = "Error: " + e;
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
        metricSel.innerHTML = '<option value="">— metric —</option>';
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
  { value: "yfs201", label: "YF-S201/YF-S403 (water flow)", iface: "pulse" },
  { value: "ens160", label: "ENS160 (TVOC/eCO2)", iface: "i2c" },
  { value: "sgp30", label: "SGP30 (TVOC/eCO2)", iface: "i2c" },
  { value: "rain", label: "Rain gauge (tipping bucket)", iface: "pulse" },
  { value: "wind", label: "Wind speed (anemometer)", iface: "pulse" },
];

// R11: GPIO pin list + per-pin warnings are derived from the active
// board profile (fetched from /api/board-profiles on page load). The
// hardcoded XIAO C3 fallback below is used only if the API call fails
// or the response is malformed — keeps the sensors page functional
// during partial-init or pre-R11 builds.
var CL_GPIO_PINS = [
    { gpio: 0,  label: 'GPIO0',  adc: true  },
    { gpio: 1,  label: 'GPIO1',  adc: true  },
    { gpio: 2,  label: 'GPIO2',  adc: true  },
    { gpio: 3,  label: 'GPIO3',  adc: true  },
    { gpio: 4,  label: 'GPIO4',  adc: true  },
    { gpio: 5,  label: 'GPIO5',  adc: true  },
    { gpio: 6,  label: 'GPIO6',  adc: false },
    { gpio: 7,  label: 'GPIO7',  adc: false },
    { gpio: 8,  label: 'GPIO8',  adc: false },
    { gpio: 9,  label: 'GPIO9',  adc: false },
    { gpio: 10, label: 'GPIO10', adc: false },
    { gpio: 20, label: 'GPIO20', adc: false },
    { gpio: 21, label: 'GPIO21', adc: false }
];

// Populated from the active profile by clLoadBoardProfile().  Keys are
// GPIO numbers; values are a short reason string ("strap", "USB", etc).
var CL_SYSTEM_PINS = {};

// Active board profile descriptor (from /api/board-profiles → "active").
// null until the fetch resolves; treated as "no validation" until then.
window._r11Profile = null;

// One-shot fetch of the active board profile. Refreshes CL_GPIO_PINS so
// the pin selector knows the right GPIO range, and CL_SYSTEM_PINS so it
// can show profile-aware warnings. Safe to call multiple times.
function clLoadBoardProfile() {
    return fetchWithTimeout('/api/board-profiles', { credentials: 'same-origin' }, 15000)
        .then(function (r) { return r.json(); })
        .then(function (data) {
            var activeId = (data.active && data.active.id) || '';
            var profile  = (data.profiles || []).find(function (p) { return p.id === activeId; });
            if (!profile) return;
            window._r11Profile = profile;

            // Rebuild GPIO list within the profile's range.  ADC range
            // on C3/S3 is GPIO 0-5 / 1-10 respectively; keep the simple
            // "<= 10 is potentially ADC" heuristic — sensor plugins
            // perform their own analogRead validity check at runtime.
            var pins = [];
            for (var g = 0; g <= profile.maxGpio; g++) {
                pins.push({ gpio: g, label: 'GPIO' + g, adc: (g <= 10) });
            }
            CL_GPIO_PINS = pins;

            // Build warnings map from restriction lists.
            CL_SYSTEM_PINS = {};
            (profile.strapPins    || []).forEach(function (p) { CL_SYSTEM_PINS[p] = 'strap pin (boot risk)'; });
            (profile.usbPins      || []).forEach(function (p) { CL_SYSTEM_PINS[p] = 'USB CDC'; });
            (profile.flashPins    || []).forEach(function (p) { CL_SYSTEM_PINS[p] = 'SPI flash bus'; });
            (profile.reservedPins || []).forEach(function (p) { CL_SYSTEM_PINS[p] = 'UART0 console'; });
        })
        .catch(function () {
            // Silent — fallback table above keeps the UI alive on legacy
            // builds or transient API failures.
        });
}
// Fire the fetch as soon as the script loads.  No await — the selector
// re-renders on every popup open and will pick up the populated data.
if (typeof window !== 'undefined') clLoadBoardProfile();

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
  var list = document.getElementById("cl-sensors-list");
  if (!list) return;
  if (!sensors || sensors.length === 0) {
    list.innerHTML = "";
    list.appendChild(emptyState({
      icon: "gauge",
      title: "No sensors configured",
      msg: "Click + Add Sensor to register your first sensor."
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
      var pinInfo =
        s.interface === "i2c"
          ? "SDA:" + (s.sda || "?") + " SCL:" + (s.scl || "?")
          : s.interface === "uart"
            ? "RX:" + (s.uart_rx || "?")
            : s.interface === "pulse"
              ? "Pin:" + (s.pin || "?")
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
        (s.enabled ? "ON" : "OFF") +
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
      "Removed " + name,
      "Press Undo to restore (save on the page to persist)",
      function () {
        // Re-insert at original index — clamp in case the list shrank.
        var insertAt = Math.min(idx, PCFG.sensors.length);
        PCFG.sensors.splice(insertAt, 0, removed);
        clRenderSensors(PCFG.sensors);
      }
    );
  } else if (window.showToast) {
    showToast("Removed " + name, "ok");
  }
}

window.clCurrentEditingSensor = -1;

// Build the inner HTML for the sensor-edit form.  Called by both the popup
// path (mobile / fallback) and the inline expander (desktop) so the two
// surfaces stay in lockstep.
function _clBuildEditFormHtml(s) {
  var html = '<form id="sensorEditForm" data-submit="clSaveEditedSensor">';
  
  // ID
  html += '<div class="field"><label class="field-label">Sensor ID</label>' +
          '<input type="text" name="id" class="input" value="' + esc(s.id || '') + '"></div>';
          
  // Enabled
  html += '<div class="field"><label style="display:flex;align-items:center;gap:6px;cursor:pointer"><input type="checkbox" name="enabled"' + (s.enabled ? ' checked' : '') + '> Enabled</label></div>';
  
  // Read Interval
  html += '<div class="field"><label class="field-label">Read Interval (ms)</label>' +
          '<input type="number" step="100" name="read_interval_ms" class="input" value="' + (s.read_interval_ms || 10000) + '"></div>';

  if (s.interface === "i2c") {
    html += '<div class="form-grid">' +
            '<div class="field"><label class="field-label">SDA Pin</label><input type="number" name="sda" class="input" value="' + (s.sda !== undefined ? s.sda : 6) + '"></div>' +
            '<div class="field"><label class="field-label">SCL Pin</label><input type="number" name="scl" class="input" value="' + (s.scl !== undefined ? s.scl : 7) + '"></div>' +
            '</div>';
  } else if (s.interface === "uart") {
    html += '<div class="form-grid">' +
            '<div class="field"><label class="field-label">RX Pin</label><input type="number" name="uart_rx" class="input" value="' + (s.uart_rx !== undefined ? s.uart_rx : 20) + '"></div>' +
            '<div class="field"><label class="field-label">TX Pin</label><input type="number" name="uart_tx" class="input" value="' + (s.uart_tx !== undefined ? s.uart_tx : -1) + '"></div>' +
            '</div>';
    html += '<div class="field"><label class="field-label">Baud Rate</label><select name="baud" class="input">' +
            '<option value="9600"' + (s.baud == 9600 ? ' selected' : '') + '>9600</option>' +
            '<option value="19200"' + (s.baud == 19200 ? ' selected' : '') + '>19200</option>' +
            '<option value="38400"' + (s.baud == 38400 ? ' selected' : '') + '>38400</option>' +
            '<option value="115200"' + (s.baud == 115200 ? ' selected' : '') + '>115200</option>' +
            '</select></div>';
    if (s.type === "sds011") {
      html += '<div class="field"><label class="field-label">Working Period (minutes)</label>' +
              '<input type="number" min="0" max="30" name="work_period_min" class="input" value="' + (s.work_period_min !== undefined ? s.work_period_min : 1) + '">' +
              '<p class="hint">0 = Continuous. 1-30 = Sensor sleeps and wakes automatically.</p></div>';
      html += '<div class="field" style="margin-top:10px"><label style="display:flex;align-items:center;gap:6px;cursor:pointer">' +
              '<input type="checkbox" name="humidityCorrectionEnabled"' + (s.humidityCorrectionEnabled ? ' checked' : '') + '> Enable Humidity Correction</label>' +
              '<p class="hint">Requires a humidity sensor in the stream.</p></div>';
      html += '<div class="field"><label class="field-label">Correction &kappa; (Köhler)</label>' +
              '<input type="number" step="0.05" min="0" max="2" name="humidityCorrectionKappa" class="input" value="' + (s.humidityCorrectionKappa !== undefined ? s.humidityCorrectionKappa : 0.35) + '"></div>';
    }
  } else if (s.interface === "pulse") {
    html += '<div class="field"><label class="field-label">Pin</label><input type="number" name="pin" class="input" value="' + (s.pin !== undefined ? s.pin : 9) + '"></div>';
    if (s.type === "yfs201") {
      html += '<div class="form-grid">' +
              '<div class="field"><label class="field-label">Pulses/Liter</label><input type="number" step="0.1" name="pulses_per_liter" class="input" value="' + (s.pulses_per_liter !== undefined ? s.pulses_per_liter : 450) + '"></div>' +
              '<div class="field"><label class="field-label">Multiplier</label><input type="number" step="0.1" name="calibration" class="input" value="' + (s.calibration !== undefined ? s.calibration : 1.0) + '"></div>' +
              '</div>';
    }
  }

  // Support for custom JSON fields (advanced)
  // Restricted-pin warning + per-sensor override (populated by clWirePinWarn).
  html += '<div id="sensor-pinwarn" style="display:none;margin-top:1rem;padding:8px 10px;border-radius:6px;font-size:12px"></div>';
  html += '<label id="sensor-unsafe-wrap" style="display:' + (s.allow_unsafe_pins ? 'flex' : 'none') +
          ';align-items:center;gap:6px;cursor:pointer;margin-top:8px;font-size:12px">' +
          '<input type="checkbox" name="allow_unsafe_pins"' + (s.allow_unsafe_pins ? ' checked' : '') +
          '> Use restricted pin anyway (proper pull-ups added)</label>';

  var stdKeys = ["id", "type", "enabled", "interface", "read_interval_ms", "sda", "scl", "uart_rx", "uart_tx", "baud", "pin", "work_period_min", "pulses_per_liter", "calibration", "humidityCorrectionEnabled", "humidityCorrectionKappa", "allow_unsafe_pins"];
  var advObj = {};
  for (var k in s) {
    if (stdKeys.indexOf(k) === -1) advObj[k] = s[k];
  }
  var advStr = Object.keys(advObj).length > 0 ? JSON.stringify(advObj) : "{}";
  html += '<div class="field" style="margin-top:1rem"><label class="field-label">Advanced (JSON overlay)</label>' +
          '<input type="text" name="advanced" class="input" value="' + esc(advStr) + '">' +
          '<p class="hint">Additional parameters applied directly to this sensor. Keep as {} if unsure.</p></div>';

  html += '</form>';
  return html;
}

// Inline-edit mount point for desktop (≥780 px).  Expands a panel below
// the row, replacing the modal popup for less context loss.  Falls back
// to the popup on mobile and when the row can't be located.
// Live restricted-pin warning for the sensor edit form (both inline + popup
// mounts). Warns when a pin field holds a strapping/reserved/flash GPIO and
// reveals the allow_unsafe_pins checkbox for the soft (override-able) cases.
function clWirePinWarn() {
  if (typeof getBoardPins !== "function") return;
  var form = document.getElementById("sensorEditForm");
  if (!form) return;
  var sel = 'input[name="sda"],input[name="scl"],input[name="uart_rx"],input[name="uart_tx"],input[name="pin"]';
  function refresh() {
    var warn = document.getElementById("sensor-pinwarn");
    var wrap = document.getElementById("sensor-unsafe-wrap");
    var chk  = form.querySelector('input[name="allow_unsafe_pins"]');
    if (!warn) return;
    getBoardPins().then(function (pins) {
      var msgs = [], hard = false, soft = false;
      form.querySelectorAll(sel).forEach(function (el) {
        if (el.value === "") return;
        var risk = pinRisk(pins, el.value);
        if (risk) {
          msgs.push("GPIO" + parseInt(el.value, 10) + " — " + risk.reason);
          if (risk.hard) hard = true; else soft = true;
        }
      });
      if (!msgs.length) {
        warn.style.display = "none";
        if (wrap && !(chk && chk.checked)) wrap.style.display = "none";
        return;
      }
      warn.style.display = "";
      warn.style.background = hard ? "rgba(220,38,38,.12)" : "rgba(217,119,6,.14)";
      warn.style.color      = hard ? "var(--err)" : "var(--warn)";
      warn.innerHTML = "⚠ " + msgs.join(" · ") +
        (hard ? " — can't be used (hardware-reserved); pick another pin."
              : " — usable only with proper pull-ups; the device may fail to boot if held LOW at reset.");
      if (wrap) wrap.style.display = ((soft && !hard) || (chk && chk.checked)) ? "flex" : "none";
    });
  }
  form.querySelectorAll(sel).forEach(function (el) { el.addEventListener("input", refresh); });
  refresh();
}

function _clEditInline(idx, s) {
  var row = document.querySelector('.sensor-list-row[data-sensor-idx="' + idx + '"]');
  if (!row) return false;
  // Close any open expander first
  document.querySelectorAll(".sensor-inline-edit").forEach(function (n) { n.remove(); });

  var panel = document.createElement("div");
  panel.className = "sensor-inline-edit";
  panel.setAttribute("data-sensor-idx", idx);
  panel.innerHTML =
    '<div class="sensor-inline-head">' +
      '<div class="sensor-inline-title">Edit · <span class="mono">' + esc(s.id || s.type) + '</span></div>' +
      '<button type="button" class="btn-mini" data-role="close" aria-label="Close"><span data-icon="x"></span></button>' +
    '</div>' +
    '<div class="sensor-inline-body">' + _clBuildEditFormHtml(s) + '</div>' +
    '<div class="sensor-inline-foot">' +
      '<button type="button" class="btn" data-role="cancel">Cancel</button>' +
      '<button type="button" class="btn primary" data-role="save"><span data-icon="save"></span> Save</button>' +
    '</div>';

  // Mount immediately after the row so the expander shows in flow
  row.parentNode.insertBefore(panel, row.nextSibling);
  if (window.Icons && Icons.swap) Icons.swap(panel);
  clWirePinWarn();

  function dismiss() { panel.remove(); window.clCurrentEditingSensor = -1; }
  panel.querySelector('[data-role="close"]').addEventListener("click", dismiss);
  panel.querySelector('[data-role="cancel"]').addEventListener("click", dismiss);
  panel.querySelector('[data-role="save"]').addEventListener("click", function () {
    clSaveEditedSensor();
    dismiss();
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
  t.textContent = "Edit Sensor: " + (s.id || s.type);
  b.innerHTML = _clBuildEditFormHtml(s);
  f.style.display = "flex";
  btn.onclick = clSaveEditedSensor;
  document.getElementById("sensorPopup").style.display = "flex";
  clWirePinWarn();
}

function clSaveEditedSensor() {
  var idx = window.clCurrentEditingSensor;
  if (idx < 0 || !PCFG || !PCFG.sensors) return;
  var s = PCFG.sensors[idx];
  var form = document.getElementById("sensorEditForm");
  if (!form) return;
  var fd = new FormData(form);
  
  s.id = fd.get("id");
  s.enabled = fd.get("enabled") === "on";
  s.read_interval_ms = parseInt(fd.get("read_interval_ms") || 10000, 10);
  
  if (s.interface === "i2c") {
    s.sda = parseInt(fd.get("sda") || 6, 10);
    s.scl = parseInt(fd.get("scl") || 7, 10);
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

  s.allow_unsafe_pins = fd.get("allow_unsafe_pins") === "on";
  if (!s.allow_unsafe_pins) delete s.allow_unsafe_pins;   // keep config tidy

  var adv = fd.get("advanced");
  if (adv && adv !== "{}") {
    try {
      var advObj = JSON.parse(adv);
      for (var k in advObj) s[k] = advObj[k];
    } catch(e) {
      showToast("Invalid Advanced JSON. Saving standard fields only.", "error");
    }
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
      msg.textContent = "❌ No config loaded";
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
    msg.textContent = "Saving…";
    msg.className = "";
  }

  pcfgSave(PCFG, function (ok, err) {
    if (ok) {
      if (msg) {
        msg.textContent = "✅ Saved! Restarting device…";
        msg.className = "";
      }
      // Trigger restart so new mode takes effect
      setTimeout(function () {
        fetchWithTimeout("/api/platform_reload", { method: "POST" }, 30000).catch(function () {});
      }, 500);
    } else {
      if (msg) {
        msg.textContent = "❌ Save failed: " + err;
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
  lbl.textContent = `${(sleepMs / 1000).toFixed(0)}s sleep + ${(activeMs / 1000).toFixed(0)}s active = ${(totalMs / 1000).toFixed(0)}s per cycle`;
}

// ============================================================================
// EXPORT PAGE
// ============================================================================
function expLoad() {
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
    _setVal("exp-mqtt-interval", m.interval_ms || 60000);
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
    _setVal("exp-http-interval", h.interval_ms || 60000);

    // Sensor.Community
    var sc = exp.sensor_community || {};
    _setVal("exp-sc-en", sc.enabled || false, true);
    _setVal("exp-sc-interval", sc.interval_ms || 145000);

    // openSenseMap
    var osm = exp.opensensemap || {};
    _setVal("exp-osm-en", osm.enabled || false, true);
    _setVal("exp-osm-boxid", osm.box_id || "");
    _setVal("exp-osm-token", osm.access_token || "");

    // OSM sensor IDs grid
    var ids = osm.sensor_ids || {};
    var osmDiv = document.getElementById("exp-osm-ids");
    if (osmDiv) {
      var metrics = [
        "temperature",
        "humidity",
        "pressure",
        "pm25",
        "pm10",
        "tvoc",
        "eco2",
        "flow_rate",
        "rain_total",
        "wind_speed",
      ];
      osmDiv.innerHTML =
        '<div class="form-grid" style="flex-wrap:wrap">' +
        metrics
          .map(function (m) {
            return (
              '<div class="field" style="min-width:180px">' +
              '<label class="field-label">' +
              m +
              "</label>" +
              '<input type="text" id="osm-id-' +
              m +
              '" class="input" value="' +
              esc(ids[m] || "") +
              '" placeholder="sensor ID…">' +
              "</div>"
            );
          })
          .join("") +
        "</div>";
    }
  });
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

  // MQTT
  PCFG.export.mqtt = {
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
    interval_ms: parseInt(
      (document.getElementById("exp-mqtt-interval") || {}).value || "60000",
      10,
    ),
    retain: !!(document.getElementById("exp-mqtt-retain") || {}).checked,
    use_tls: (document.getElementById("exp-mqtt-tls") || {}).value === "tls",
    qos: 0,
  };

  // HTTP
  var authVal = (document.getElementById("exp-http-auth") || {}).value || "";
  PCFG.export.http = {
    enabled: !!(document.getElementById("exp-http-en") || {}).checked,
    url: (document.getElementById("exp-http-url") || {}).value || "",
    method: "POST",
    headers: authVal ? { Authorization: authVal } : {},
    interval_ms: parseInt(
      (document.getElementById("exp-http-interval") || {}).value || "60000",
      10,
    ),
  };

  // Sensor.Community
  PCFG.export.sensor_community = {
    enabled: !!(document.getElementById("exp-sc-en") || {}).checked,
    interval_ms: parseInt(
      (document.getElementById("exp-sc-interval") || {}).value || "145000",
      10,
    ),
  };

  // openSenseMap
  var ids = {};
  [
    "temperature",
    "humidity",
    "pressure",
    "pm25",
    "pm10",
    "tvoc",
    "eco2",
    "flow_rate",
    "rain_total",
    "wind_speed",
  ].forEach(function (m) {
    var v = ((document.getElementById("osm-id-" + m) || {}).value || "").trim();
    if (v) ids[m] = v;
  });
  PCFG.export.opensensemap = {
    enabled: !!(document.getElementById("exp-osm-en") || {}).checked,
    box_id: (document.getElementById("exp-osm-boxid") || {}).value || "",
    access_token: (document.getElementById("exp-osm-token") || {}).value || "",
    sensor_ids: ids,
  };

  if (msg) {
    msg.textContent = "Saving…";
    msg.className = "";
  }
  pcfgSave(PCFG, function (ok, err) {
    if (ok) {
      if (msg) {
        msg.textContent = "✅ Saved! Restarting…";
        msg.className = "";
      }
      setTimeout(function () {
        fetchWithTimeout("/api/platform_reload", { method: "POST" }, 30000).catch(function () {});
      }, 500);
    } else {
      if (msg) {
        msg.textContent = "❌ " + err;
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

