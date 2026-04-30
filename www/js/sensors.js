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

function pcfgLoad(cb) {
  fetch("/api/platform_config")
    .then(function (r) {
      return r.ok ? r.json() : null;
    })
    .then(function (d) {
      PCFG = d || {
        version: 1,
        mode: "legacy",
        sensors: [],
        aggregation: {},
        export: {},
        storage: {},
      };
      if (cb) cb(PCFG);
    })
    .catch(function () {
      PCFG = {
        version: 1,
        mode: "legacy",
        sensors: [],
        aggregation: {},
        export: {},
        storage: {},
      };
      if (cb) cb(PCFG);
    });
}

function pcfgSave(obj, cb) {
  var body = JSON.stringify(obj, null, 2);
  fetch("/save_platform", {
    method: "POST",
    headers: { "Content-Type": "application/octet-stream" },
    body: body,
  })
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      if (cb) cb(d.ok, d.error || "");
    })
    .catch(function (e) {
      if (cb) cb(false, String(e));
    });
}

// ============================================================================
// AGGREGATION SETTINGS  (in Datalog page)
// ============================================================================
// Load stored aggregation prefs from localStorage (client-side only)
(function aggSettingsInit() {
  window.addEventListener("DOMContentLoaded", function () {
    var b = localStorage.getItem("agg_bucket") || "5m";
    var m = localStorage.getItem("agg_mode") || "lttb";
    var l = parseInt(localStorage.getItem("agg_limit") || "500", 10);
    var eb = document.getElementById("agg-bucket");
    var em = document.getElementById("agg-mode");
    var el = document.getElementById("agg-limit");
    if (eb) eb.value = b;
    if (em) em.value = m;
    if (el) el.value = l;
  });
})();

function aggSettingsSave() {
  var b = (document.getElementById("agg-bucket") || {}).value || "5m";
  var m = (document.getElementById("agg-mode") || {}).value || "lttb";
  var l = parseInt(
    (document.getElementById("agg-limit") || {}).value || "500",
    10,
  );
  localStorage.setItem("agg_bucket", b);
  localStorage.setItem("agg_mode", m);
  localStorage.setItem("agg_limit", String(l));
  var msg = document.getElementById("agg-msg");
  if (msg) {
    msg.textContent = "✅ Saved (used when viewing sensor charts)";
    msg.style.color = "green";
  }
}

function aggGetPrefs() {
  return {
    bucket: localStorage.getItem("agg_bucket") || "5m",
    mode: localStorage.getItem("agg_mode") || "lttb",
    limit: parseInt(localStorage.getItem("agg_limit") || "500", 10),
  };
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

  fetch("/api/sensors")
    .then(function (r) {
      return r.ok ? r.json() : null;
    })
    .then(function (d) {
      if (!d || !d.sensors || d.sensors.length === 0) {
        if (msg)
          msg.textContent =
            "No sensors registered. Set mode to Continuous in Core Logic settings and configure sensors.";
        return;
      }
      if (msg) msg.textContent = "";
      if (grid) {
        var nowMs = Date.now();
        grid.innerHTML = d.sensors
          .map(function (s) {
            var statusClass =
              s.status === "ok"
                ? "badge-ok"
                : s.status === "disabled"
                  ? "badge-dis"
                  : "badge-err";
            var ts = s.last_read_ts
              ? new Date(s.last_read_ts * 1000).toLocaleTimeString()
              : "—";

            // Staleness — flag the card amber if no reading within 2×
            // the configured read_interval_ms; flag red if errored.  Cards
            // with no last_read_ts yet (just-booted) get neither flag.
            var ageStr = "";
            var staleClass = "";
            if (s.last_read_ts && s.read_interval_ms) {
              var ageMs = nowMs - (s.last_read_ts * 1000);
              if (ageMs > s.read_interval_ms * 2) staleClass = " sensor-stale";
              ageStr = " · " + _sensorFmtAge(ageMs) + " ago";
            }
            if (s.status === "error") staleClass = " sensor-error";

            return (
              '<div class="sensor-card' + staleClass + '">' +
              '<div class="sensor-card-header">' +
              '<span class="sensor-name">' + esc(s.name) + "</span>" +
              '<span class="badge ' + statusClass + '">' + esc(s.status) + "</span>" +
              "</div>" +
              '<div class="sensor-meta">' +
              "<span>ID: <code>" + esc(s.id) + "</code></span>" +
              "<span>Type: <code>" + esc(s.type) + "</code></span>" +
              "<span>Last: " + ts + ageStr + "</span>" +
              "</div>" +
              '<div class="sensor-metrics">' +
              (s.metrics || [])
                .map(function (m) {
                  // last_values[m] can be either a {v,u,ts} object (current
                  // firmware) or a bare string (older firmware) — handle both.
                  var lv = s.last_values && s.last_values[m];
                  var val = "", unit = "", metricStale = false;
                  if (lv !== undefined && lv !== null) {
                    if (typeof lv === "object") {
                      val  = lv.v !== undefined ? String(lv.v) : "";
                      unit = lv.u || "";
                      if (lv.ts && s.read_interval_ms) {
                        var mAge = nowMs - lv.ts * 1000;
                        if (mAge > s.read_interval_ms * 2) metricStale = true;
                      }
                    } else {
                      val = String(lv);
                    }
                  }
                  var hasVal = val !== "";
                  return '<span class="metric-tag' +
                         (hasVal ? ' has-value' : '') +
                         (metricStale ? ' stale' : '') + '">' +
                         (hasVal
                           ? '<strong class="metric-val">' + esc(val) + '</strong>' +
                             (unit ? '<span class="metric-unit">' + esc(unit) + '</span>' : '')
                           : '') +
                         '<span class="metric-name">' + esc(m) + '</span>' +
                         '</span>';
                })
                .join("") +
              "</div>" +
              "</div>"
            );
          })
          .join("");
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
  var fetches = [fetch(url1).then(function (r) { return r.ok ? r.json() : null; })];
  if (url2) fetches.push(fetch(url2).then(function (r) { return r.ok ? r.json() : null; }));

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

      var infoStr = d1.count + " pts";
      if (hasDual) infoStr += " + " + d2.count + " pts (overlay)";
      infoStr += " · " + d1.agg + " · " + d1.mode;
      if (msg) msg.textContent = infoStr;

      var ctx = document.getElementById("sensorChart");
      if (!ctx) return;

      if (sensorChart) sensorChart.destroy();

      var datasets = [
        {
          label: sid + " / " + metric + (unit1 ? " (" + unit1 + ")" : ""),
          data: values1,
          borderColor: "#275673",
          backgroundColor: "rgba(39,86,115,0.08)",
          borderWidth: 2,
          pointRadius: d1.data.length > 100 ? 0 : 3,
          tension: 0.3,
          fill: true,
          yAxisID: "y",
        },
      ];

      if (hasDual) {
        datasets.push({
          label: sid2 + " / " + metric2 + (unit2 ? " (" + unit2 + ")" : ""),
          data: values2,
          borderColor: "#e67e22",
          backgroundColor: "rgba(230,126,34,0.08)",
          borderWidth: 2,
          pointRadius: d2.data.length > 100 ? 0 : 3,
          tension: 0.3,
          fill: false,
          borderDash: [5, 3],
          yAxisID: "y2",
        });
      }

      var scales = {
        x: { ticks: { maxTicksLimit: 10, maxRotation: 45 } },
        y: {
          beginAtZero: false,
          position: "left",
          title: { display: true, text: metric + (unit1 ? " (" + unit1 + ")" : "") },
        },
      };

      if (hasDual) {
        scales.y2 = {
          beginAtZero: false,
          position: "right",
          grid: { drawOnChartArea: false },
          title: { display: true, text: metric2 + (unit2 ? " (" + unit2 + ")" : "") },
        };
      }

      sensorChart = new Chart(ctx, {
        type: "line",
        data: { labels: labels, datasets: datasets },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          plugins: {
            legend: { display: true },
            tooltip: { mode: "index", intersect: false },
          },
          scales: scales,
        },
      });
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
      fetch("/api/sensors")
        .then(function (r) { return r.json(); })
        .then(function (d) {
          var s = (d.sensors || []).find(function (s) { return s.id === sid; });
          if (s && s.metrics) {
            metricSel.innerHTML = s.metrics
              .map(function (m) { return '<option value="' + m + '">' + m + "</option>"; })
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
  { value: "sds011", label: "SDS011 (PM2.5/PM10)", iface: "uart" },
  { value: "pms5003", label: "PMS5003 (PM1/2.5/10)", iface: "uart" },
  { value: "yfs201", label: "YF-S201/YF-S403 (water flow)", iface: "pulse" },
  { value: "ens160", label: "ENS160 (TVOC/eCO2)", iface: "i2c" },
  { value: "sgp30", label: "SGP30 (TVOC/eCO2)", iface: "i2c" },
  { value: "rain", label: "Rain gauge (tipping bucket)", iface: "pulse" },
  { value: "wind", label: "Wind speed (anemometer)", iface: "pulse" },
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
    // Mode
    var modeEl = document.getElementById("cl-mode");
    if (modeEl) modeEl.value = cfg.mode || "legacy";

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
        '<div class="sensor-list-row" style="display:flex;align-items:center;gap:8px;padding:10px 16px;border-bottom:1px solid var(--border)">' +
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
        (s.id || s.type) +
        "</div>" +
        '<div style="font-size:.8rem;color:var(--text-muted)">' +
        typeLabel +
        " · " +
        pinInfo +
        "</div>" +
        "</div>" +
        '<button type="button" class="btn btn-sm btn-secondary" data-click="clEditSensor" data-args="[' +
        i +
        ']">✏️</button>' +
        '<button type="button" class="btn btn-sm btn-danger"   data-click="clRemoveSensor" data-args="[' +
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
  if (
    !confirm(
      'Remove sensor "' +
        (PCFG.sensors[idx].id || PCFG.sensors[idx].type) +
        '"?',
    )
  )
    return;
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
  var b = document.getElementById("sensorPopupBody");
  var t = document.getElementById("sensorPopupTitle");
  var f = document.getElementById("sensorPopupFooter");
  
  t.textContent = "Select Sensor Type";
  var html = '<div style="display:grid;grid-template-columns:1fr 1fr;gap:10px">';
  CL_SENSOR_TYPES.forEach(function(st) {
    html += '<button class="btn btn-secondary" style="text-align:left;height:auto;padding:10px;display:flex;flex-direction:column;gap:4px" data-click="clDoAddSensor" data-args="' + esc(JSON.stringify([st.value])) + '">'
          + '<strong style="color:var(--text)">' + esc(st.value) + '</strong><span style="font-size:0.8rem;color:var(--text-muted)">' + esc(st.label) + '</span></button>';
  });
  html += '</div>';
  b.innerHTML = html;
  f.style.display = "none";
  document.getElementById("sensorPopup").style.display = "flex";
}

function clDoAddSensor(type) {
  var info = CL_SENSOR_TYPES.find(function (t) { return t.value === type; });
  if (!info) return;
  if (!PCFG) PCFG = { sensors: [] };
  if (!PCFG.sensors) PCFG.sensors = [];
  var newS = {
    id: type + "_" + (PCFG.sensors.length + 1),
    type: type,
    enabled: true,
    interface: info.iface,
  };
  if (info.iface === "i2c") {
    newS.sda = 6;
    newS.scl = 7;
    newS.read_interval_ms = 10000;
  }
  if (info.iface === "uart") {
    newS.uart_rx = 20;
    newS.uart_tx = -1;
    newS.baud = 9600;
  }
  if (info.iface === "pulse") {
    newS.pin = 9;
    newS.read_interval_ms = 5000;
  }
  PCFG.sensors.push(newS);
  
  window.clCurrentEditingSensor = PCFG.sensors.length - 1;
  clEditSensor(window.clCurrentEditingSensor);
  clRenderSensors(PCFG.sensors);
}

window.clCurrentEditingSensor = -1;

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
  if (!PCFG || !PCFG.sensors) return;
  window.clCurrentEditingSensor = idx;
  var s = PCFG.sensors[idx];
  
  var b = document.getElementById("sensorPopupBody");
  var t = document.getElementById("sensorPopupTitle");
  var f = document.getElementById("sensorPopupFooter");
  var btn = document.getElementById("sensorPopupSaveBtn");
  
  t.textContent = "Edit Sensor: " + (s.id || s.type);
  
  var html = '<form id="sensorEditForm" data-submit="clSaveEditedSensor">';
  
  // ID
  html += '<div class="form-group"><label class="form-label">Sensor ID</label>' +
          '<input type="text" name="id" class="form-input" value="' + esc(s.id || '') + '"></div>';
          
  // Enabled
  html += '<div class="form-group"><label style="display:flex;align-items:center;gap:6px;cursor:pointer"><input type="checkbox" name="enabled"' + (s.enabled ? ' checked' : '') + '> Enabled</label></div>';
  
  // Read Interval
  html += '<div class="form-group"><label class="form-label">Read Interval (ms)</label>' +
          '<input type="number" step="100" name="read_interval_ms" class="form-input" value="' + (s.read_interval_ms || 10000) + '"></div>';

  if (s.interface === "i2c") {
    html += '<div class="form-row">' +
            '<div class="form-group"><label class="form-label">SDA Pin</label><input type="number" name="sda" class="form-input" value="' + (s.sda !== undefined ? s.sda : 6) + '"></div>' +
            '<div class="form-group"><label class="form-label">SCL Pin</label><input type="number" name="scl" class="form-input" value="' + (s.scl !== undefined ? s.scl : 7) + '"></div>' +
            '</div>';
  } else if (s.interface === "uart") {
    html += '<div class="form-row">' +
            '<div class="form-group"><label class="form-label">RX Pin</label><input type="number" name="uart_rx" class="form-input" value="' + (s.uart_rx !== undefined ? s.uart_rx : 20) + '"></div>' +
            '<div class="form-group"><label class="form-label">TX Pin</label><input type="number" name="uart_tx" class="form-input" value="' + (s.uart_tx !== undefined ? s.uart_tx : -1) + '"></div>' +
            '</div>';
    html += '<div class="form-group"><label class="form-label">Baud Rate</label><select name="baud" class="form-input form-select">' +
            '<option value="9600"' + (s.baud == 9600 ? ' selected' : '') + '>9600</option>' +
            '<option value="19200"' + (s.baud == 19200 ? ' selected' : '') + '>19200</option>' +
            '<option value="38400"' + (s.baud == 38400 ? ' selected' : '') + '>38400</option>' +
            '<option value="115200"' + (s.baud == 115200 ? ' selected' : '') + '>115200</option>' +
            '</select></div>';
    if (s.type === "sds011") {
      html += '<div class="form-group"><label class="form-label">Working Period (minutes)</label>' +
              '<input type="number" min="0" max="30" name="work_period_min" class="form-input" value="' + (s.work_period_min !== undefined ? s.work_period_min : 1) + '">' +
              '<p class="form-hint">0 = Continuous. 1-30 = Sensor sleeps and wakes automatically.</p></div>';
    }
  } else if (s.interface === "pulse") {
    html += '<div class="form-group"><label class="form-label">Pin</label><input type="number" name="pin" class="form-input" value="' + (s.pin !== undefined ? s.pin : 9) + '"></div>';
  }

  // Support for custom JSON fields (advanced)
  var stdKeys = ["id", "type", "enabled", "interface", "read_interval_ms", "sda", "scl", "uart_rx", "uart_tx", "baud", "pin", "work_period_min"];
  var advObj = {};
  for (var k in s) {
    if (stdKeys.indexOf(k) === -1) advObj[k] = s[k];
  }
  var advStr = Object.keys(advObj).length > 0 ? JSON.stringify(advObj) : "{}";
  html += '<div class="form-group" style="margin-top:1rem"><label class="form-label">Advanced (JSON overlay)</label>' +
          '<input type="text" name="advanced" class="form-input" value=\'' + advStr.replace(/'/g, "&apos;") + '\'>' +
          '<p class="form-hint">Additional parameters applied directly to this sensor. Keep as {} if unsure.</p></div>';

  html += '</form>';
  
  b.innerHTML = html;
  f.style.display = "flex";
  
  btn.onclick = clSaveEditedSensor;
  document.getElementById("sensorPopup").style.display = "flex";
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
    }
  } else if (s.interface === "pulse") {
    s.pin = parseInt(fd.get("pin") || 9, 10);
  }

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
        fetch("/api/platform_reload", { method: "POST" }).catch(function () {});
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
        '<div class="form-row" style="flex-wrap:wrap">' +
        metrics
          .map(function (m) {
            return (
              '<div class="form-group" style="min-width:180px">' +
              '<label class="form-label">' +
              m +
              "</label>" +
              '<input type="text" id="osm-id-' +
              m +
              '" class="form-input" value="' +
              (ids[m] || "") +
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
        fetch("/api/platform_reload", { method: "POST" }).catch(function () {});
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
registerHandlers({
  aggSettingsSave: aggSettingsSave,
  sensorsLoad: sensorsLoad,
  sensorChartLoad: sensorChartLoad,
  clToggleSensor: clToggleSensor,
  clRemoveSensor: clRemoveSensor,
  clAddSensor: clAddSensor,
  clDoAddSensor: clDoAddSensor,
  clEditSensor: clEditSensor,
  clSaveEditedSensor: clSaveEditedSensor,
  clSave: clSave,
  clLoad: clLoad,
  clUpdateSleepPanel: clUpdateSleepPanel,
  clUpdateHybCycle: clUpdateHybCycle,
  expLoad: expLoad,
  expSave: expSave,
});

