/**
 * /www/js/iot-extensions.js  —  IoT-first UI extensions
 *
 * Adds platform-mode awareness, Overview / Alerts / Health pages,
 * sensor zone grouping, compare-mode chips, and the 4-step Add-Sensor wizard.
 *
 * Depends on: core.js (ST, CFG, navigateTo, showToast, registerHandlers,
 *             Icons.swap, emptyState), icons.js, sensors.js (sensorsLoad).
 * Load order: after all other scripts in index.html.
 */

"use strict";

(function () {

  // ─── helpers ──────────────────────────────────────────────────────────────
  var root = document.documentElement;

  /** Small icon span understood by icons.js Icons.swap(). */
  function icon(name, cls) {
    var s = document.createElement("span");
    s.setAttribute("data-icon", name);
    if (cls) s.className = cls;
    return s;
  }

  /** Re-run icon swap on a subtree. */
  function reIcons(el) {
    if (window.Icons && Icons.swap) Icons.swap(el || document.body);
  }

  // ─── Platform-mode ────────────────────────────────────────────────────────
  // Read from CFG (loaded asynchronously by core.js).  Fall back to
  // localStorage cache, then 'continuous'.
  var MODES = ["legacy", "continuous", "hybrid"];

  function readMode() {
    // Real config from firmware
    if (window.CFG && CFG.platform && CFG.platform.mode &&
        MODES.indexOf(CFG.platform.mode) !== -1) {
      return CFG.platform.mode;
    }
    // Fall back to cached value
    try {
      var cached = localStorage.getItem("platformMode");
      if (cached && MODES.indexOf(cached) !== -1) return cached;
    } catch (e) {}
    return "continuous";
  }

  function applyMode(m) {
    root.dataset.mode = m;
    try { localStorage.setItem("platformMode", m); } catch (e) {}
    updateModePill(m);
    // Route to default page if the currently active one is now hidden
    routeToModeDefault(m);
  }

  // ─── Mode pill (sidebar header) ───────────────────────────────────────────
  var modePillEl = document.getElementById("modePill");

  function updateModePill(m) {
    if (!modePillEl) return;
    modePillEl.innerHTML = "";
    var ic = icon(m === "legacy" ? "moon" : m === "hybrid" ? "layers" : "activity");
    var txt = document.createElement("span");
    txt.textContent = m;
    modePillEl.appendChild(ic);
    modePillEl.appendChild(txt);
    reIcons(modePillEl);
  }

  // ─── Bootstrap: wait for CFG then apply mode ──────────────────────────────
  // core.js populates CFG asynchronously; poll briefly then settle.
  var _modeApplied = false;
  function tryApplyMode() {
    if (_modeApplied) return;
    var m = readMode();
    _modeApplied = true;
    applyMode(m);
    // Build dynamic pages now that we know the mode
    buildPages(m);
    // Post-load: re-group sensor grid if already rendered
    rebuildSensorZones();
    // Inject compare-mode strip into sensor chart card
    injectCompareChips();
  }

  // Check every 80 ms until CFG is populated (max ~3 s)
  var _modeTimer = null;
  var _modeAttempts = 0;
  _modeTimer = setInterval(function () {
    _modeAttempts++;
    if ((window.CFG && Object.keys(CFG).length > 0) || _modeAttempts > 38) {
      clearInterval(_modeTimer);
      tryApplyMode();
    }
  }, 80);

  // ─── Mode-aware default page routing ──────────────────────────────────────
  function routeToModeDefault(m) {
    var active = document.querySelector(".page.active");
    if (!active) return;
    var allowed = (active.getAttribute("data-mode-show") || "").split(" ").filter(Boolean);
    if (allowed.length && allowed.indexOf(m) === -1) {
      var def = m === "legacy" ? "dashboard" : "overview";
      if (typeof navigateTo === "function") navigateTo(def);
    }
  }

  // ─── LAZY_PAGES: tell core.js these pages are NOT fetched from /pages/ ────
  // They are injected by buildPages() below. We mark them with the special
  // sentinel value 2 so loadPagePartial() skips the fetch but still resolves.
  if (typeof LAZY_PAGES !== "undefined") {
    LAZY_PAGES.overview = 2;
    LAZY_PAGES.alerts   = 2;
    LAZY_PAGES.health   = 2;
  }

  // Patch loadPagePartial to skip our sentinel pages
  if (typeof loadPagePartial === "function") {
    var _origLoad = loadPagePartial;
    window.loadPagePartial = function (page) {
      if (LAZY_PAGES[page] === 2) return Promise.resolve();
      return _origLoad(page);
    };
  }

  // ─── Build new pages ──────────────────────────────────────────────────────
  function buildPages(mode) {
    buildOverviewPage();
    buildAlertsPage();
    buildHealthPage();
    buildWizardModal();

    // Set default active page based on mode if no hash
    var hash = location.hash.replace("#", "");
    if (!hash || hash === "dashboard") {
      if (mode !== "legacy") {
        if (typeof navigateTo === "function") navigateTo("overview");
      }
    }
  }

  // ── Overview page ──────────────────────────────────────────────────────────
  function buildOverviewPage() {
    if (document.getElementById("page-overview")) return; // already built

    var page = document.createElement("main");
    page.className = "main-content page";
    page.id = "page-overview";
    page.setAttribute("data-mode-show", "continuous hybrid");
    page.setAttribute("role", "main");

    page.innerHTML = [
      '<div class="page-head">',
        '<div>',
          '<h1 class="page-title"><span data-icon="layout-grid"></span> Overview</h1>',
          '<div class="page-sub" id="ov-sub">IoT sensor dashboard · loading…</div>',
        '</div>',
        '<div class="page-actions">',
          '<div class="seg" role="group" aria-label="Time range">',
            '<button class="active" aria-pressed="true" onclick="ovSetRange(3600)">1h</button>',
            '<button aria-pressed="false" onclick="ovSetRange(86400)">24h</button>',
            '<button aria-pressed="false" onclick="ovSetRange(604800)">7d</button>',
          '</div>',
          '<button class="btn" id="ovAddSensorBtn"><span data-icon="plus"></span> Add sensor</button>',
        '</div>',
      '</div>',

      // AQI + Environment row
      '<div class="grid grid-12">',
        '<div class="card span-6">',
          '<div class="card-head">',
            '<div class="card-title"><span data-icon="wind"></span> Air Quality Index</div>',
            '<span class="badge ok" id="aqi-badge">GOOD</span>',
          '</div>',
          '<div class="card-body" style="padding:0">',
            '<div class="aqi-card">',
              '<div class="aqi-gauge">',
                '<svg viewBox="0 0 120 120">',
                  '<circle class="aqi-track" cx="60" cy="60" r="50"/>',
                  '<circle class="aqi-fill" id="aqi-arc" cx="60" cy="60" r="50" stroke-dasharray="314" stroke-dashoffset="220"/>',
                '</svg>',
                '<div class="aqi-center">',
                  '<div class="aqi-label">AQI</div>',
                  '<div class="aqi-score" id="aqi-score">—</div>',
                  '<div class="aqi-quality" id="aqi-quality">—</div>',
                '</div>',
              '</div>',
              '<div class="aqi-breakdown" id="aqi-breakdown">',
                '<div class="aqi-bar"><div class="aqi-bar-name">PM2.5</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-pm25" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-pm25v">—</div></div>',
                '<div class="aqi-bar"><div class="aqi-bar-name">PM10</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-pm10" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-pm10v">—</div></div>',
                '<div class="aqi-bar"><div class="aqi-bar-name">TVOC</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-tvoc" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-tvocv">—</div></div>',
                '<div class="aqi-bar"><div class="aqi-bar-name">eCO₂</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-eco2" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-eco2v">—</div></div>',
              '</div>',
            '</div>',
          '</div>',
        '</div>',

        '<div class="card span-6">',
          '<div class="card-head">',
            '<div class="card-title"><span data-icon="thermometer"></span> Environment</div>',
            '<span class="mono" style="font-size:11px;color:var(--text-3)" id="ov-env-src">—</span>',
          '</div>',
          '<div class="card-body">',
            '<div class="grid grid-3" style="gap:10px" id="ov-env-kpis">',
              '<div class="kpi" style="padding:14px"><div class="kpi-l"><span data-icon="thermometer"></span> Temp</div><div class="kpi-v"><span class="num" id="ov-temp">—</span><span class="unit">°C</span></div><div class="kpi-d" id="ov-temp-d">—</div></div>',
              '<div class="kpi" style="padding:14px"><div class="kpi-l"><span data-icon="droplet"></span> Humidity</div><div class="kpi-v"><span class="num" id="ov-hum">—</span><span class="unit">%</span></div><div class="kpi-d" id="ov-hum-d">—</div></div>',
              '<div class="kpi" style="padding:14px"><div class="kpi-l"><span data-icon="gauge"></span> Pressure</div><div class="kpi-v"><span class="num" id="ov-pres">—</span><span class="unit">hPa</span></div><div class="kpi-d" id="ov-pres-d">—</div></div>',
            '</div>',
          '</div>',
        '</div>',
      '</div>',

      // Energy + Water/Outdoor row
      '<div class="grid grid-12" style="margin-top:var(--gap)">',
        '<div class="card span-8">',
          '<div class="card-head">',
            '<div class="card-title"><span data-icon="zap"></span> Energy</div>',
            '<span class="mono" style="font-size:11px;color:var(--text-3)" id="ov-energy-src">—</span>',
          '</div>',
          '<div class="card-body">',
            '<div class="energy-grid" id="ov-energy-grid">',
              '<div class="energy-tile live"><div class="energy-tile-l">Voltage</div><div class="energy-tile-v"><span id="ov-volt">—</span><span class="u">V</span></div><div class="energy-tile-trend" id="ov-volt-t">—</div></div>',
              '<div class="energy-tile live"><div class="energy-tile-l">Current</div><div class="energy-tile-v"><span id="ov-amp">—</span><span class="u">A</span></div><div class="energy-tile-trend" id="ov-amp-t">—</div></div>',
              '<div class="energy-tile live"><div class="energy-tile-l">Power</div><div class="energy-tile-v"><span id="ov-power">—</span><span class="u">W</span></div><div class="energy-tile-trend" id="ov-pf-t">—</div></div>',
              '<div class="energy-tile"><div class="energy-tile-l">Today</div><div class="energy-tile-v"><span id="ov-kwh-day">—</span><span class="u">kWh</span></div><div class="energy-tile-trend" id="ov-kwh-day-t">—</div></div>',
              '<div class="energy-tile"><div class="energy-tile-l">This week</div><div class="energy-tile-v"><span id="ov-kwh-week">—</span><span class="u">kWh</span></div><div class="energy-tile-trend" id="ov-kwh-week-t">—</div></div>',
              '<div class="energy-tile"><div class="energy-tile-l">This month</div><div class="energy-tile-v"><span id="ov-kwh-month">—</span><span class="u">kWh</span></div><div class="energy-tile-trend" id="ov-kwh-month-t">—</div></div>',
            '</div>',
          '</div>',
        '</div>',

        '<div class="card span-4" data-mode-show="hybrid">',
          '<div class="card-head"><div class="card-title"><span data-icon="droplets"></span> Water</div></div>',
          '<div class="card-body" style="display:flex;flex-direction:column;gap:14px">',
            '<div><div style="color:var(--text-3);font-size:11px;text-transform:uppercase;letter-spacing:.05em">Today</div>',
            '<div class="mono" style="font-size:28px;font-weight:700"><span id="ov-water-today">—</span><span style="font-size:13px;color:var(--text-3);margin-left:4px">L</span></div></div>',
            '<div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Events</span><span class="mono" id="ov-water-events">—</span></div>',
          '</div>',
        '</div>',

        '<div class="card span-4" data-mode-show="continuous">',
          '<div class="card-head"><div class="card-title"><span data-icon="cloud-rain"></span> Outdoor</div></div>',
          '<div class="card-body" style="display:flex;flex-direction:column;gap:14px">',
            '<div><div style="color:var(--text-3);font-size:11px;text-transform:uppercase;letter-spacing:.05em">Rain today</div>',
            '<div class="mono" style="font-size:28px;font-weight:700"><span id="ov-rain">—</span><span style="font-size:13px;color:var(--text-3);margin-left:4px">mm</span></div></div>',
            '<div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Wind</span><span class="mono" id="ov-wind">—</span></div>',
          '</div>',
        '</div>',
      '</div>',

      // Alerts feed + Sensor snapshot
      '<div class="grid grid-12" style="margin-top:var(--gap)">',
        '<div class="card span-6">',
          '<div class="card-head">',
            '<div class="card-title"><span data-icon="bell"></span> Recent alerts</div>',
            '<a class="mono" style="font-size:11px;color:var(--accent);cursor:pointer" data-click="navPage" data-page="alerts">View all →</a>',
          '</div>',
          '<div class="card-body" style="padding:0" id="ov-alert-feed">',
            '<div class="empty" style="padding:20px"><span class="empty-title">No alerts</span></div>',
          '</div>',
        '</div>',
        '<div class="card span-6">',
          '<div class="card-head">',
            '<div class="card-title"><span data-icon="thermometer"></span> Active sensors</div>',
            '<a class="mono" style="font-size:11px;color:var(--accent);cursor:pointer" data-click="navPage" data-page="sensors">All sensors →</a>',
          '</div>',
          '<div class="card-body" id="ov-sensors-list" style="display:flex;flex-direction:column;gap:6px">',
            '<div class="empty" style="padding:20px"><span class="empty-title">Loading…</span></div>',
          '</div>',
        '</div>',
      '</div>',
    ].join("");

    document.body.insertBefore(page, document.getElementById("toastContainer") || null);
    reIcons(page);

    // Bind Add sensor button
    var addBtn = document.getElementById("ovAddSensorBtn");
    if (addBtn) {
      addBtn.addEventListener("click", function () { openWizard(); });
    }

    // Populate with real data once ST/CFG available
    populateOverview();
  }

  /** Populate Overview with real /api/latest data. */
  function populateOverview() {
    // Sub-heading from ST
    var sub = document.getElementById("ov-sub");
    if (sub && window.ST) {
      var sc = (window.ST.sensorCount !== undefined ? window.ST.sensorCount : "?");
      sub.textContent = sc + " sensors · live readings";
    }

    // Fetch latest sensor readings
    fetch("/api/latest")
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data) return;
        ovFillEnvironment(data);
        ovFillEnergy(data);
        ovFillAQI(data);
        ovFillWater(data);
        ovFillSensorList(data);
      })
      .catch(function () {});

    // Populate alert feed from the live log if available
    ovFillAlertFeed();
  }

  function ovFillEnvironment(data) {
    // Look for first bme280/bme688 sensor in data
    var envSensor = null;
    var envId = "";
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (!envSensor && s.readings &&
            (s.id.indexOf("bme") !== -1 || s.id.indexOf("env") !== -1 ||
             s.type === "bme280" || s.type === "bme688")) {
          envSensor = s; envId = s.id;
        }
      });
    }
    var src = document.getElementById("ov-env-src");
    if (src) src.textContent = envId || "No env sensor";
    if (!envSensor) return;
    var r = envSensor.readings || {};
    var t = document.getElementById("ov-temp");
    var h = document.getElementById("ov-hum");
    var p = document.getElementById("ov-pres");
    if (t && r.temperature !== undefined) t.textContent = r.temperature.toFixed(1);
    if (h && r.humidity !== undefined) h.textContent = Math.round(r.humidity);
    if (p && r.pressure !== undefined) p.textContent = Math.round(r.pressure);
  }

  function ovFillEnergy(data) {
    var energySensor = null;
    var energyId = "";
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (!energySensor && s.readings &&
            (s.type === "zmct103c" || s.type === "zmpt101b" ||
             s.id.indexOf("power") !== -1 || s.id.indexOf("energy") !== -1)) {
          energySensor = s; energyId = s.id;
        }
      });
    }
    var src = document.getElementById("ov-energy-src");
    if (src) src.textContent = energyId || "No energy sensor";
    if (!energySensor) return;
    var r = energySensor.readings || {};
    var setE = function (id, v, suffix) {
      var el = document.getElementById(id);
      if (el && v !== undefined) el.textContent = (typeof v === "number" ? v.toFixed(1) : v) + (suffix || "");
    };
    setE("ov-volt",  r.voltage,  "");
    setE("ov-amp",   r.current,  "");
    setE("ov-power", r.power,    "");
  }

  function ovFillAQI(data) {
    // Try to get PM / VOC / CO2 data
    var pm = null, voc = null, co2 = null;
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (!pm  && s.readings && (s.type === "sds011" || s.type === "pms5003")) pm  = s.readings;
        if (!voc && s.readings && (s.type === "sgp30"  || s.type === "ens160"))  voc = s.readings;
        if (!co2 && s.readings && (s.type === "scd4x"  || s.type === "scd30"))   co2 = s.readings;
      });
    }
    // Compute a simple AQI from PM2.5 (EPA linear interpolation simplified)
    var pm25 = pm && (pm.pm25 || pm.pm2_5);
    var aqi = pm25 ? Math.min(500, Math.round((pm25 / 35.4) * 100)) : null;
    var score = document.getElementById("aqi-score");
    var quality = document.getElementById("aqi-quality");
    var badge = document.getElementById("aqi-badge");
    var arc = document.getElementById("aqi-arc");
    if (aqi !== null && score) {
      score.textContent = aqi;
      var label = aqi < 50 ? "Good" : aqi < 100 ? "Moderate" : "Poor";
      var cls   = aqi < 50 ? "aqi-good" : aqi < 100 ? "aqi-mod" : "aqi-poor";
      if (quality) { quality.textContent = label; quality.className = "aqi-quality " + cls; }
      if (badge)   { badge.textContent = label.toUpperCase(); badge.className = "badge " + (aqi < 50 ? "ok" : aqi < 100 ? "warn" : "err"); }
      if (arc) {
        var pct = Math.min(1, aqi / 300);
        arc.setAttribute("stroke-dashoffset", (314 * (1 - pct)).toFixed(1));
        arc.style.stroke = aqi < 50 ? "var(--ok)" : aqi < 100 ? "var(--warn)" : "var(--err)";
      }
    }
    var fill = function (barId, valId, raw, max, unit) {
      var b = document.getElementById(barId); var v = document.getElementById(valId);
      if (b && raw !== undefined) b.style.width = Math.min(100, (raw / max) * 100).toFixed(0) + "%";
      if (v && raw !== undefined) v.textContent = raw.toFixed(1) + " " + unit;
    };
    if (pm) {
      fill("aqi-pm25", "aqi-pm25v", pm.pm25 || pm.pm2_5, 75, "µg");
      fill("aqi-pm10", "aqi-pm10v", pm.pm10, 150, "µg");
    }
    if (voc) {
      fill("aqi-tvoc", "aqi-tvocv", voc.tvoc || voc.TVOC, 500, "ppb");
      fill("aqi-eco2", "aqi-eco2v", co2 ? (co2.co2 || co2.eCO2) : (voc.eco2 || voc.eCO2), 2000, "ppm");
    }
  }

  function ovFillWater(data) {
    // Water stats come from the live endpoint for legacy/hybrid
    fetch("/api/status")
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (d) {
        if (!d) return;
        var today = document.getElementById("ov-water-today");
        var events = document.getElementById("ov-water-events");
        if (today && d.todayVol !== undefined) today.textContent = d.todayVol.toFixed(2);
        if (events && d.todayEvents !== undefined) events.textContent = d.todayEvents + " events";
      })
      .catch(function () {});
  }

  function ovFillSensorList(data) {
    var list = document.getElementById("ov-sensors-list");
    if (!list) return;
    if (!Array.isArray(data) || data.length === 0) {
      list.innerHTML = "";
      list.appendChild(emptyState({ icon: "thermometer", title: "No sensors", msg: "Add a sensor to get started." }));
      return;
    }
    list.innerHTML = data.slice(0, 8).map(function (s) {
      var r = s.readings || {};
      var keys = Object.keys(r);
      var firstVal = keys.length ? r[keys[0]] : null;
      var valStr = firstVal !== null && firstVal !== undefined
        ? (typeof firstVal === "number" ? firstVal.toFixed(2) : String(firstVal)) + " " + (s.units && s.units[keys[0]] || "")
        : "—";
      var st = s.error ? "err" : s.stale ? "warn" : "ok";
      return '<div style="display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid var(--border);font-size:12px">' +
               '<span class="mono" style="color:var(--text-2)">' + esc(s.id) + '</span>' +
               '<span style="display:flex;align-items:center;gap:8px">' +
                 '<span class="mono" style="font-weight:600">' + esc(valStr) + '</span>' +
                 '<span class="badge ' + st + '">' + st.toUpperCase() + '</span>' +
               '</span>' +
             '</div>';
    }).join("");
  }

  function ovFillAlertFeed() {
    // This would ideally fetch from /api/alerts; use empty state for now
    // since the alerts backend is not yet implemented on the firmware side.
    var feed = document.getElementById("ov-alert-feed");
    if (feed) {
      feed.innerHTML = '<div class="empty" style="padding:20px"><span class="empty-title">No recent alerts</span></div>';
    }
  }

  // ── Alerts page ────────────────────────────────────────────────────────────
  function buildAlertsPage() {
    if (document.getElementById("page-alerts")) return;

    var page = document.createElement("main");
    page.className = "main-content page";
    page.id = "page-alerts";
    page.setAttribute("data-mode-show", "continuous hybrid");
    page.setAttribute("role", "main");

    page.innerHTML = [
      '<div class="page-head">',
        '<div>',
          '<h1 class="page-title"><span data-icon="bell-ring"></span> Alerts</h1>',
          '<div class="page-sub">Threshold rules across all sensors</div>',
        '</div>',
        '<div class="page-actions">',
          '<button class="btn primary"><span data-icon="plus"></span> New rule</button>',
        '</div>',
      '</div>',

      '<div class="grid grid-4">',
        '<div class="kpi"><div class="kpi-l"><span data-icon="list-checks"></span> Rules</div><div class="kpi-v"><span class="num" id="al-total">—</span></div><div class="kpi-d" id="al-rule-d">—</div></div>',
        '<div class="kpi"><div class="kpi-l"><span data-icon="alert-triangle"></span> Firing</div><div class="kpi-v" style="color:var(--err)"><span class="num" id="al-firing">0</span></div><div class="kpi-d">right now</div></div>',
        '<div class="kpi"><div class="kpi-l"><span data-icon="clock"></span> Last 24 h</div><div class="kpi-v"><span class="num" id="al-day-trips">0</span></div><div class="kpi-d">trips</div></div>',
        '<div class="kpi"><div class="kpi-l"><span data-icon="bell-off"></span> Snoozed</div><div class="kpi-v"><span class="num" id="al-snoozed">0</span></div><div class="kpi-d">—</div></div>',
      '</div>',

      '<div class="card" style="margin-top:var(--gap)">',
        '<div class="card-head">',
          '<div class="card-title"><span data-icon="list-checks"></span> Rules</div>',
          '<input class="input" placeholder="Filter rules…" id="al-filter" style="width:200px;height:28px"/>',
        '</div>',
        '<div class="card-body" style="padding:0" id="al-rules">',
          '<div class="empty" style="padding:24px"><span class="empty-title">Loading…</span></div>',
        '</div>',
      '</div>',

      '<div class="card" style="margin-top:var(--gap)">',
        '<div class="card-head"><div class="card-title"><span data-icon="history"></span> Trigger history</div></div>',
        '<div class="card-body" style="padding:0" id="al-history">',
          '<div class="empty" style="padding:24px"><span class="empty-title">Loading…</span></div>',
        '</div>',
      '</div>',
    ].join("");

    document.body.insertBefore(page, document.getElementById("toastContainer") || null);
    reIcons(page);

    // Filter input — wired up after API data populates the rules container
    var fi = document.getElementById("al-filter");
    if (fi) fi.addEventListener("input", function () {
      var q = fi.value.toLowerCase();
      page.querySelectorAll(".alert-rule").forEach(function (r) {
        r.style.display = r.textContent.toLowerCase().indexOf(q) !== -1 ? "" : "none";
      });
    });

    // Fetch live data from /api/alerts immediately after page is in DOM
    _loadAlertsData();
  }

  // Fetch GET /api/alerts and render rules + history from real firmware data.
  function _loadAlertsData() {
    fetch("/api/alerts")
      .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
      .then(function (data) {
        _renderAlertRules(data.rules  || []);
        _renderAlertHistory(data.history || []);
      })
      .catch(function (err) {
        var msg = '<div class="empty" style="padding:24px"><span class="empty-title">Failed to load alerts (' + err + ')</span></div>';
        var rc = document.getElementById("al-rules");
        var hc = document.getElementById("al-history");
        if (rc) rc.innerHTML = msg;
        if (hc) hc.innerHTML = msg;
      });
  }

  // Render the rules list from API data and update KPI counters.
  function _renderAlertRules(rules) {
    var rc = document.getElementById("al-rules");
    if (!rc) return;

    if (!rules.length) {
      rc.innerHTML = '<div class="empty" style="padding:24px">' +
        '<span class="empty-title">No rules configured</span>' +
        '<div class="empty-sub">Click "New rule" to create your first alert.</div></div>';
      setEl("al-total",   0);
      setEl("al-firing",  0);
      setEl("al-snoozed", 0);
      setEl("al-rule-d",  "across sensors");
      return;
    }

    var nowTs    = Math.floor(Date.now() / 1000);
    var firing   = 0;
    var snoozed  = 0;

    rc.innerHTML = rules.map(function (r) {
      var isSnoozed = r.snooze_until && r.snooze_until > nowTs;
      var isFiring  = r.firing && !isSnoozed;
      var state     = isSnoozed ? "snoozed" : (isFiring ? "firing" : "armed");
      if (isFiring)  firing++;
      if (isSnoozed) snoozed++;

      var expr = esc(r.expr ? r.expr.sensor + "." + r.expr.metric + " " + r.expr.op + " " + r.expr.value
                             + (r.expr.duration_s ? " for " + r.expr.duration_s + "s" : "") : "");

      var lastFired = r.last_fired
        ? _relTime(r.last_fired)
        : (r.lastFiredTs ? _relTime(r.lastFiredTs) : "—");

      var acts = (r.actions || []).map(function (a) {
        return '<span class="badge dim" title="' + esc(a) + '">' + esc(a) + '</span>';
      }).join("");

      return '<div class="alert-rule ' + state + '" data-id="' + esc(r.id || "") + '">' +
        '<span class="ar-state" aria-label="' + state + '"></span>' +
        '<div><div class="ar-name">' + esc(r.name || r.id || "Rule") + '</div>' +
          '<div class="ar-expr">' + expr + '</div></div>' +
        '<div class="ar-meta">' +
          '<span class="ar-last">' + lastFired + '</span>' +
        '</div>' +
        '<div class="ar-actions">' + acts +
          '<button class="btn-mini" aria-label="Edit rule"><span data-icon="pencil"></span></button>' +
          '<label class="switch" style="margin-left:4px" title="' + (r.enabled ? "Disable" : "Enable") + ' rule">' +
            '<input type="checkbox"' + (r.enabled ? " checked" : "") + ' data-rule-id="' + esc(r.id || "") + '"/>' +
            '<span></span></label>' +
        '</div>' +
      '</div>';
    }).join("");

    // Wire toggle checkboxes to POST /api/alerts with updated rule
    rc.querySelectorAll('input[data-rule-id]').forEach(function (cb) {
      cb.addEventListener("change", function () {
        var ruleId = cb.dataset.ruleId;
        // Re-fetch, flip enabled, save back
        fetch("/api/alerts")
          .then(function (r) { return r.json(); })
          .then(function (data) {
            (data.rules || []).forEach(function (rule) {
              if (rule.id === ruleId) rule.enabled = cb.checked;
            });
            return fetch("/api/alerts", {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify(data)
            });
          })
          .catch(function () { cb.checked = !cb.checked; }); // revert on error
      });
    });

    reIcons(rc);

    setEl("al-total",   rules.length);
    setEl("al-firing",  firing);
    setEl("al-snoozed", snoozed);
    setEl("al-rule-d",  "across sensors");

    var badge = document.getElementById("alerts-badge");
    if (badge) {
      badge.textContent = firing || "";
      badge.style.display = firing ? "" : "none";
    }
  }

  // Render the history feed from API data.
  function _renderAlertHistory(history) {
    var hc = document.getElementById("al-history");
    if (!hc) return;

    if (!history.length) {
      hc.innerHTML = '<div class="empty" style="padding:24px">' +
        '<span class="empty-title">No alert history</span></div>';
      return;
    }

    // Show newest-first (API returns oldest-first)
    hc.innerHTML = history.slice().reverse().map(function (h) {
      return '<div class="alert-feed-row">' +
        '<span class="af-time">' + esc(_relTime(h.ts)) + '</span>' +
        '<div><div class="af-name">' + esc(h.rule_id || "") + '</div>' +
          '<div class="af-val">' + (h.value !== undefined ? h.value : "") + '</div></div>' +
        '<span class="badge ' + esc(h.outcome || "ok") + '">' + esc((h.outcome || "ok").toUpperCase()) + '</span>' +
      '</div>';
    }).join("");
  }

  // Format a unix timestamp as a relative time string ("3 min ago", "2 h ago").
  function _relTime(ts) {
    if (!ts) return "—";
    var diff = Math.floor(Date.now() / 1000) - ts;
    if (diff < 0)    return "just now";
    if (diff < 60)   return diff + "s ago";
    if (diff < 3600) return Math.floor(diff / 60) + " min ago";
    if (diff < 86400) return Math.floor(diff / 3600) + " h ago";
    return Math.floor(diff / 86400) + " d ago";
  }

  // ── Health page ────────────────────────────────────────────────────────────
  function buildHealthPage() {
    if (document.getElementById("page-health")) return;

    var page = document.createElement("main");
    page.className = "main-content page";
    page.id = "page-health";
    page.setAttribute("data-mode-show", "continuous hybrid");
    page.setAttribute("role", "main");

    page.innerHTML = [
      '<div class="page-head">',
        '<div>',
          '<h1 class="page-title"><span data-icon="heart-pulse"></span> Sensor health</h1>',
          '<div class="page-sub">Diagnostics, retry counts, uptime per sensor</div>',
        '</div>',
        '<div class="page-actions">',
          '<button class="btn"><span data-icon="refresh-cw"></span> Refresh</button>',
        '</div>',
      '</div>',

      '<div class="grid grid-4">',
        '<div class="kpi"><div class="kpi-l"><span data-icon="activity"></span> Up</div><div class="kpi-v" style="color:var(--ok)"><span class="num" id="hl-up">—</span></div><div class="kpi-d up" id="hl-up-d">—</div></div>',
        '<div class="kpi"><div class="kpi-l"><span data-icon="clock"></span> Stale</div><div class="kpi-v" style="color:var(--warn)"><span class="num" id="hl-stale">0</span></div><div class="kpi-d" id="hl-stale-d">—</div></div>',
        '<div class="kpi"><div class="kpi-l"><span data-icon="x-circle"></span> Errored</div><div class="kpi-v" style="color:var(--err)"><span class="num" id="hl-err">0</span></div><div class="kpi-d" id="hl-err-d">—</div></div>',
        '<div class="kpi"><div class="kpi-l"><span data-icon="refresh-cw"></span> Retries (24h)</div><div class="kpi-v"><span class="num" id="hl-retries">0</span></div><div class="kpi-d">total</div></div>',
      '</div>',

      '<div class="health-grid" style="margin-top:var(--gap)" id="health-grid">',
        '<div class="empty" style="padding:32px"><span class="empty-title">Loading sensor health…</span></div>',
      '</div>',
    ].join("");

    document.body.insertBefore(page, document.getElementById("toastContainer") || null);
    reIcons(page);

    // Refresh button
    page.querySelector(".btn").addEventListener("click", function () {
      populateHealthPage();
    });

    populateHealthPage();
  }

  function populateHealthPage() {
    fetch("/api/sensors")
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        var sensors = (data && data.sensors) || [];
        if (!sensors.length) return;
        renderHealthGrid(sensors);
      })
      .catch(function () {
        var grid = document.getElementById("health-grid");
        if (grid) {
          grid.innerHTML = "";
          grid.appendChild(emptyState({ icon: "heart-pulse", title: "Unable to load", msg: "Could not reach /api/sensors." }));
        }
      });
  }

  function uptimeBar(uptime, state) {
    var slots = 24;
    var html = "";
    for (var i = 0; i < slots; i++) {
      var threshold = slots * (1 - uptime / 100);
      var cls = state === "err" ? "err"
              : state === "disabled" ? "unknown"
              : state === "warn" && i >= slots - Math.ceil(threshold) ? "warn"
              : i < threshold ? "err"
              : "ok";
      html += "<span class='" + cls + "'></span>";
    }
    return html;
  }

  function renderHealthGrid(sensors) {
    var grid = document.getElementById("health-grid");
    if (!grid) return;
    if (!sensors.length) {
      grid.innerHTML = "";
      grid.appendChild(emptyState({ icon: "heart-pulse", title: "No sensors", msg: "Add sensors via Core Logic." }));
      return;
    }

    var upCount = 0, staleCount = 0, errCount = 0, totalRetries = 0;
    var staleNames = [], errNames = [];

    grid.innerHTML = sensors.map(function (s) {
      var state = s.error ? "err" : s.stale ? "warn" : s.disabled ? "disabled" : "ok";
      var uptime = s.uptime_pct !== undefined ? s.uptime_pct : (state === "err" ? 0 : state === "warn" ? 80 : 99);
      var reads   = s.reads   || 0;
      var errors  = s.errors  || 0;
      var retries = s.retries || 0;
      var avgLat  = s.avg_latency_ms !== undefined ? s.avg_latency_ms.toFixed(1) + " ms" : "—";
      var lastSeen = s.age_s !== undefined ? s.age_s + "s ago" : "—";

      if (state === "ok")   upCount++;
      if (state === "warn") { staleCount++; staleNames.push(s.id); }
      if (state === "err")  { errCount++;   errNames.push(s.id); }
      totalRetries += retries;

      return '<div class="health-tile' + (state === "err" ? " err" : state === "warn" ? " warn" : "") + '">' +
        '<div class="health-tile-head">' +
          '<div class="health-name"><span data-icon="cpu"></span>' + esc(s.type || s.id) + '</div>' +
          '<span class="badge ' + (state === "ok" ? "ok" : state === "err" ? "err" : state === "warn" ? "warn" : "dim") + '">' + state.toUpperCase() + '</span>' +
        '</div>' +
        '<div class="mono" style="font-size:11px;color:var(--text-3)">' + esc(s.id) + ' · ' + esc(s.interface || "—") + '</div>' +
        '<div>' +
          '<div class="mono" style="font-size:10px;color:var(--text-3);margin-bottom:4px">24h uptime · ' + uptime.toFixed(1) + '%</div>' +
          '<div class="health-uptime-bar" aria-label="24-hour uptime">' + uptimeBar(uptime, state) + '</div>' +
        '</div>' +
        '<div class="health-stats">' +
          '<div class="health-stat"><span class="health-stat-l">Reads</span><span class="health-stat-v">' + reads.toLocaleString() + '</span></div>' +
          '<div class="health-stat"><span class="health-stat-l">Errors</span><span class="health-stat-v' + (errors > 10 ? " err" : errors > 0 ? " warn" : "") + '">' + errors + '</span></div>' +
          '<div class="health-stat"><span class="health-stat-l">Retries</span><span class="health-stat-v' + (retries > 10 ? " warn" : "") + '">' + retries + '</span></div>' +
          '<div class="health-stat"><span class="health-stat-l">Avg latency</span><span class="health-stat-v">' + avgLat + '</span></div>' +
        '</div>' +
        '<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--text-3);font-family:var(--mono)">' +
          '<span>Last: ' + esc(lastSeen) + '</span>' +
        '</div>' +
      '</div>';
    }).join("");

    reIcons(grid);
    setEl("hl-up",      upCount);
    setEl("hl-up-d",    upCount + " of " + sensors.length);
    setEl("hl-stale",   staleCount);
    setEl("hl-stale-d", staleNames.slice(0, 2).join(", ") || "—");
    setEl("hl-err",     errCount);
    setEl("hl-err-d",   errNames.slice(0, 2).join(", ") || "—");
    setEl("hl-retries", totalRetries);
  }

  // ─── Sensor zone grouping ─────────────────────────────────────────────────
  // Called after sensors have been rendered into #sensors-grid.
  function rebuildSensorZones() {
    // Hook into sensorsLoad completion: watch for mutations on #sensors-grid
    var grid = document.getElementById("sensors-grid");
    if (!grid) return;

    var mo = new MutationObserver(function () {
      mo.disconnect();
      doZoneGrouping();
      // Re-observe in case sensorsLoad fires again
      setTimeout(function () { mo.observe(grid, { childList: true }); }, 500);
    });
    mo.observe(grid, { childList: true });
    // Also group if already populated
    if (grid.children.length > 0) doZoneGrouping();
  }

  function doZoneGrouping() {
    var grid = document.getElementById("sensors-grid");
    if (!grid || !window.CFG) return;

    // Build zone map from platform_config sensors
    var sensors = (CFG.platform && CFG.platform.sensors) || [];
    var zoneMap = {};
    sensors.forEach(function (s) {
      var z = s.zone || "other";
      if (!zoneMap[z]) zoneMap[z] = [];
      zoneMap[z].push(s.id);
    });

    // Collect existing sensor cards by data-sid attribute
    var existing = {};
    grid.querySelectorAll("[data-sid]").forEach(function (c) {
      existing[c.dataset.sid] = c;
    });

    if (!Object.keys(existing).length) return;

    // Rebuild
    grid.innerHTML = "";
    grid.style.display = "block";

    var ZONE_ICONS = { indoor:"home", outdoor:"sun", utility:"wrench", other:"grid" };

    Object.keys(zoneMap).forEach(function (zone) {
      var cards = zoneMap[zone].map(function (id) { return existing[id]; }).filter(Boolean);
      if (!cards.length) return;

      var sec = document.createElement("div");
      sec.className = "zone-section";
      sec.innerHTML =
        '<div class="zone-head">' +
          '<div class="zone-title">' +
            '<span data-icon="' + (ZONE_ICONS[zone] || "grid") + '"></span>' +
            zone.charAt(0).toUpperCase() + zone.slice(1) +
          '</div>' +
          '<div class="zone-meta">' + cards.length + ' sensor' + (cards.length !== 1 ? 's' : '') + '</div>' +
        '</div>' +
        '<div class="sensors-grid zone-cards"></div>';

      var cardGrid = sec.querySelector(".zone-cards");
      cards.forEach(function (c) { cardGrid.appendChild(c); });

      // Sensors without a zone entry go to "other" implicitly
      grid.appendChild(sec);
    });

    // Any cards not assigned to a zone
    var unzoned = Object.keys(existing).filter(function (id) {
      return !sensors.find(function (s) { return s.id === id; });
    }).map(function (id) { return existing[id]; }).filter(Boolean);
    if (unzoned.length) {
      var sec2 = document.createElement("div");
      sec2.className = "zone-section";
      sec2.innerHTML =
        '<div class="zone-head">' +
          '<div class="zone-title"><span data-icon="grid"></span> Other</div>' +
          '<div class="zone-meta">' + unzoned.length + ' sensor' + (unzoned.length !== 1 ? 's' : '') + '</div>' +
        '</div>' +
        '<div class="sensors-grid zone-cards"></div>';
      var cg2 = sec2.querySelector(".zone-cards");
      unzoned.forEach(function (c) { cg2.appendChild(c); });
      grid.appendChild(sec2);
    }

    reIcons(grid);
  }

  // ─── Compare-mode chips in sensor chart card ──────────────────────────────
  function injectCompareChips() {
    // Find the sensor chart card in page-sensors (class card after the grid)
    var sensorsPage = document.getElementById("page-sensors");
    if (!sensorsPage) return;

    // Look for the card that contains the sensor chart
    var chartCard = sensorsPage.querySelector(".card:last-of-type");
    if (!chartCard || chartCard.querySelector(".compare-chips")) return;

    // Replace the old two-selector overlay row with chip-based compare
    var oldOverlay = chartCard.querySelector('div[style*="border-top"]');
    if (oldOverlay) oldOverlay.remove();

    var chipBar = document.createElement("div");
    chipBar.className = "compare-chips";
    chipBar.innerHTML =
      '<span style="color:var(--text-3);align-self:center;margin-right:4px;font-size:11px">Compare:</span>' +
      '<button class="cmp-add"><span data-icon="plus"></span> Add series</button>';

    // Insert after card-head
    var cardHead = chartCard.querySelector(".card-head");
    if (cardHead) cardHead.insertAdjacentElement("afterend", chipBar);

    reIcons(chipBar);

    // Add series button → show a simple dropdown from current sensor selects
    chipBar.querySelector(".cmp-add").addEventListener("click", function () {
      var s1 = document.getElementById("sc-sensor");
      var m1 = document.getElementById("sc-metric");
      if (!s1 || !s1.value || !m1 || !m1.value) {
        showToast("Select a sensor first", "Choose a sensor and metric above to compare", "warn");
        return;
      }
      var existing = chipBar.querySelectorAll(".cmp-chip");
      var colors = ["var(--warn)", "var(--ok)", "var(--err)", "var(--accent-2)"];
      var color = colors[existing.length % colors.length];

      var chip = document.createElement("span");
      chip.className = "cmp-chip";
      chip.innerHTML =
        '<span class="cmp-dot" style="background:' + color + '"></span>' +
        esc(s1.options[s1.selectedIndex].text) + " · " + esc(m1.value) + " " +
        '<button class="cmp-chip-rm" aria-label="Remove series">×</button>';
      chip.querySelector(".cmp-chip-rm").addEventListener("click", function () {
        chip.remove();
      });
      chipBar.insertBefore(chip, chipBar.querySelector(".cmp-add"));
    });
  }

  // ─── 4-step Add Sensor wizard ─────────────────────────────────────────────
  var _wizardEl = null;
  var _wizStep = 1;
  var _wizLabels = ["Type", "ID & Zone", "Interface", "Review"];

  function buildWizardModal() {
    if (document.getElementById("sensorWizard")) return;

    var wiz = document.createElement("div");
    wiz.className = "modal-backdrop";
    wiz.id = "sensorWizard";
    wiz.setAttribute("role", "dialog");
    wiz.setAttribute("aria-modal", "true");
    wiz.setAttribute("aria-label", "Add sensor");

    var typeCards = [
      ["bme280",  "thermometer",     "BME280",    "I2C · T/H/P"],
      ["bme688",  "thermometer-sun", "BME688",    "I2C · 4-in-1"],
      ["sds011",  "cloud-fog",       "SDS011",    "UART · PM2.5/10"],
      ["pms5003", "cloud-fog",       "PMS5003",   "UART · PM1/2.5/10"],
      ["sgp30",   "wind",            "SGP30",     "I2C · TVOC/eCO₂"],
      ["ens160",  "wind",            "ENS160",    "I2C · AQI/TVOC"],
      ["scd4x",   "leaf",            "SCD4x",     "I2C · CO₂"],
      ["bh1750",  "sun",             "BH1750",    "I2C · Lux"],
      ["ds18b20", "thermometer",     "DS18B20",   "1-Wire · Temp"],
      ["yfs201",  "droplets",        "YF-S201",   "Pulse · Flow"],
      ["rain",    "cloud-rain",      "Rain",      "Pulse"],
      ["wind",    "wind",            "Wind",      "Pulse"],
    ];

    var typeGridHTML = typeCards.map(function (c) {
      return '<div class="wiz-type-card" data-type="' + c[0] + '">' +
        '<span data-icon="' + c[1] + '"></span>' +
        '<div class="wiz-type-name">' + esc(c[2]) + '</div>' +
        '<div class="wiz-type-meta">' + esc(c[3]) + '</div>' +
      '</div>';
    }).join("");

    wiz.innerHTML =
      '<div class="modal">' +
        '<div class="modal-head">' +
          '<div class="modal-title"><span data-icon="plus-circle"></span> Add sensor</div>' +
          '<button class="btn-mini" id="wizClose" aria-label="Close"><span data-icon="x"></span></button>' +
        '</div>' +
        '<div class="modal-body">' +
          '<div class="wiz-steps">' +
            '<div class="wiz-dot active"></div><div class="wiz-dot"></div><div class="wiz-dot"></div><div class="wiz-dot"></div>' +
          '</div>' +
          // Step 1
          '<div class="wiz-step active" data-step="1">' +
            '<div class="label" style="margin-bottom:8px;font-size:12px;color:var(--text-3);font-weight:600">Sensor type</div>' +
            '<div class="wiz-type-grid">' + typeGridHTML + '</div>' +
          '</div>' +
          // Step 2
          '<div class="wiz-step" data-step="2">' +
            '<div class="form-grid">' +
              '<div class="field"><label for="wiz-id">Sensor ID</label><input id="wiz-id" class="input mono" value="new_sensor" placeholder="e.g. bme280_kitchen"/><p class="hint">Used in API responses and MQTT topics</p></div>' +
              '<div class="field"><label for="wiz-zone">Zone</label><select id="wiz-zone" class="input"><option>indoor</option><option>outdoor</option><option>utility</option></select></div>' +
              '<div class="field" style="grid-column:span 2"><label for="wiz-name">Display name</label><input id="wiz-name" class="input" value="New sensor"/></div>' +
            '</div>' +
          '</div>' +
          // Step 3
          '<div class="wiz-step" data-step="3">' +
            '<div class="form-grid">' +
              '<div class="field"><label for="wiz-iface">Interface</label><select id="wiz-iface" class="input"><option>I2C</option><option>UART</option><option>GPIO</option><option>ADC</option><option>1-Wire</option></select></div>' +
              '<div class="field"><label for="wiz-addr">I2C Address</label><input id="wiz-addr" class="input mono" value="0x76"/></div>' +
              '<div class="field"><label for="wiz-sda">SDA pin</label><input id="wiz-sda" class="input mono" type="number" value="6"/></div>' +
              '<div class="field"><label for="wiz-scl">SCL pin</label><input id="wiz-scl" class="input mono" type="number" value="7"/></div>' +
              '<div class="field"><label for="wiz-int">Read interval (ms)</label><input id="wiz-int" class="input mono" type="number" value="10000" min="500"/></div>' +
            '</div>' +
          '</div>' +
          // Step 4
          '<div class="wiz-step" data-step="4">' +
            '<div class="label" style="margin-bottom:8px;font-size:12px;color:var(--text-3);font-weight:600">Review configuration</div>' +
            '<pre id="wiz-json" style="background:var(--panel-2);border:1px solid var(--border);border-radius:6px;padding:14px;font-family:var(--mono);font-size:11.5px;line-height:1.7;overflow:auto;white-space:pre-wrap"></pre>' +
            '<p style="font-size:12px;color:var(--text-3);margin-top:10px">Saving will reload the platform pipeline.</p>' +
          '</div>' +
        '</div>' +
        '<div class="modal-foot">' +
          '<button class="btn" id="wizPrev" style="visibility:hidden"><span data-icon="arrow-left"></span> Back</button>' +
          '<span class="mono" style="color:var(--text-3);font-size:11px" id="wizStepLabel">Step 1 of 4 · Type</span>' +
          '<button class="btn primary" id="wizNext">Next <span data-icon="arrow-right"></span></button>' +
        '</div>' +
      '</div>';

    document.body.appendChild(wiz);
    reIcons(wiz);
    _wizardEl = wiz;

    // Type card selection
    wiz.querySelectorAll(".wiz-type-card").forEach(function (c) {
      c.addEventListener("click", function () {
        wiz.querySelectorAll(".wiz-type-card").forEach(function (x) { x.classList.remove("selected"); });
        c.classList.add("selected");
        var id = document.getElementById("wiz-id");
        if (id && id.value === "new_sensor") id.value = c.dataset.type + "_1";
        var nm = document.getElementById("wiz-name");
        if (nm) nm.value = c.querySelector(".wiz-type-name").textContent + " sensor";
      });
    });
    // Select first by default
    var first = wiz.querySelector(".wiz-type-card");
    if (first) first.classList.add("selected");

    wiz.querySelector("#wizClose").addEventListener("click", closeWizard);
    wiz.addEventListener("click", function (e) { if (e.target === wiz) closeWizard(); });

    wiz.querySelector("#wizPrev").addEventListener("click", function () {
      if (_wizStep > 1) { _wizStep--; updateWizard(); }
    });
    wiz.querySelector("#wizNext").addEventListener("click", function () {
      if (_wizStep < 4) {
        if (_wizStep === 3) buildWizReview();
        _wizStep++;
        updateWizard();
      } else {
        wizardSave();
      }
    });
  }

  function updateWizard() {
    if (!_wizardEl) return;
    _wizardEl.querySelectorAll(".wiz-step").forEach(function (s) {
      s.classList.toggle("active", +s.dataset.step === _wizStep);
    });
    _wizardEl.querySelectorAll(".wiz-dot").forEach(function (d, i) {
      d.classList.toggle("done",   i + 1 < _wizStep);
      d.classList.toggle("active", i + 1 === _wizStep);
    });
    setEl("wizStepLabel", "Step " + _wizStep + " of 4 · " + _wizLabels[_wizStep - 1]);
    var prev = document.getElementById("wizPrev");
    if (prev) prev.style.visibility = _wizStep === 1 ? "hidden" : "visible";
    var next = document.getElementById("wizNext");
    if (next) {
      next.innerHTML = _wizStep === 4
        ? '<span data-icon="check"></span> Save &amp; reload'
        : 'Next <span data-icon="arrow-right"></span>';
      reIcons(next);
    }
  }

  function buildWizReview() {
    var typeCard = _wizardEl.querySelector(".wiz-type-card.selected");
    var typeVal  = typeCard ? typeCard.dataset.type : "unknown";
    var idVal    = (document.getElementById("wiz-id")    || {}).value || "new_sensor";
    var zoneVal  = (document.getElementById("wiz-zone")  || {}).value || "indoor";
    var nameVal  = (document.getElementById("wiz-name")  || {}).value || "";
    var ifaceVal = (document.getElementById("wiz-iface") || {}).value || "I2C";
    var addrVal  = (document.getElementById("wiz-addr")  || {}).value || "0x76";
    var sdaVal   = (document.getElementById("wiz-sda")   || {}).value || "6";
    var sclVal   = (document.getElementById("wiz-scl")   || {}).value || "7";
    var intVal   = (document.getElementById("wiz-int")   || {}).value || "10000";
    var addrNum  = parseInt(addrVal, 16) || parseInt(addrVal, 10) || 0;

    var obj = {
      id:                 idVal,
      type:               typeVal,
      zone:               zoneVal,
      name:               nameVal,
      enabled:            true,
      interface:          ifaceVal.toLowerCase(),
      sda:                parseInt(sdaVal, 10),
      scl:                parseInt(sclVal, 10),
      address:            addrNum,
      read_interval_ms:   parseInt(intVal, 10),
    };
    var pre = document.getElementById("wiz-json");
    if (pre) pre.textContent = JSON.stringify(obj, null, 2);
  }

  function wizardSave() {
    // Build the sensor object from the review step
    var pre = document.getElementById("wiz-json");
    var obj;
    try { obj = pre ? JSON.parse(pre.textContent) : null; } catch (e) { obj = null; }
    if (!obj) { showToast("Parse error", "Could not read sensor config", "err"); return; }

    // POST to /save_corelogic — same endpoint as Core Logic page uses
    getCsrfToken().then(function (token) {
      var fd = new FormData();
      fd.append("add_sensor", JSON.stringify(obj));
      if (token) fd.append("csrf", token);
      fetch("/save_corelogic", { method: "POST", body: fd })
        .then(function (r) { return r.ok ? r.json() : null; })
        .then(function (res) {
          if (res && res.ok) {
            closeWizard();
            showToast("Sensor added", obj.id + " · pipeline reloading", "ok");
            setTimeout(function () { if (typeof sensorsLoad === "function") sensorsLoad(); }, 2000);
          } else {
            showToast("Save failed", (res && res.error) || "Check firmware logs", "err");
          }
        })
        .catch(function () { showToast("Network error", "Could not reach /save_corelogic", "err"); });
    });
  }

  function openWizard() {
    if (!_wizardEl) buildWizardModal();
    _wizStep = 1;
    updateWizard();
    _wizardEl.classList.add("visible");
    var firstCard = _wizardEl.querySelector(".wiz-type-card");
    if (firstCard && !_wizardEl.querySelector(".wiz-type-card.selected")) firstCard.classList.add("selected");
  }

  function closeWizard() {
    if (_wizardEl) _wizardEl.classList.remove("visible");
  }

  // Register openWizard / closeWizard in the handler registry
  registerHandlers({ openSensorWizard: openWizard });

  // Wire the existing "Add sensor" button on page-sensors and clAddSensor
  document.addEventListener("DOMContentLoaded", function () {
    // clAddSensor is registered in sensors.js; we override it with our wizard
    registerHandlers({ clAddSensor: openWizard });
  });
  // Try immediately too (script loads at end of body, so DOM is ready)
  registerHandlers({ clAddSensor: openWizard });

  // ─── Keyboard shortcuts for new pages (G O / G A / G H) ──────────────────
  // core.js already handles G+D/L/S/F/C/U. We hook the same keydown so the
  // two-key sequences share the same timing context.
  (function () {
    var gWait = false, gTimer = null;
    var extraMap = { o: "overview", a: "alerts", h: "health" };

    document.addEventListener("keydown", function (ev) {
      if (ev.ctrlKey || ev.metaKey || ev.altKey) return;
      var tag = (ev.target.tagName || "").toLowerCase();
      if (tag === "input" || tag === "textarea" || tag === "select" || ev.target.isContentEditable) return;

      var key = (ev.key || "").toLowerCase();

      if (key === "g" && !gWait) {
        gWait = true;
        clearTimeout(gTimer);
        gTimer = setTimeout(function () { gWait = false; }, 1200);
        return;
      }
      if (gWait && extraMap[key]) {
        gWait = false;
        clearTimeout(gTimer);
        ev.preventDefault();
        if (typeof navigateTo === "function") navigateTo(extraMap[key]);
      }
    }, true); // capture so we run before core.js handler resets the G flag
  })();

  // ─── KB shortcut hints for new nav items ──────────────────────────────────
  function addKbdHints() {
    var hints = { overview: ["G","O"], alerts: ["G","A"], health: ["G","H"] };
    Object.keys(hints).forEach(function (page) {
      var item = document.querySelector('.nav-item[data-page="' + page + '"]');
      if (!item || item.querySelector(".kbd")) return;
      var kbd = document.createElement("span");
      kbd.className = "kbd";
      kbd.setAttribute("aria-hidden", "true");
      hints[page].forEach(function (k) {
        var s = document.createElement("span");
        s.className = "key";
        s.textContent = k;
        kbd.appendChild(s);
      });
      item.appendChild(kbd);
    });
  }
  addKbdHints();

})();
