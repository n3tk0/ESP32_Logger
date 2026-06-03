/**
 * /www/js/iot-extensions.js  —  IoT-first UI extensions
 *
 * Adds platform-mode awareness, Overview + Alerts pages (built as editable
 * card decks), sensor-diagnostics card (formerly the Health page), sensor
 * zone grouping, compare-mode chips, the 4-step Add-Sensor wizard, and a
 * live-cycle section injected into the Sensors page when a flow-meter is
 * wired (legacy / hybrid modes).
 *
 * Depends on: core.js (ST, CFG, navigateTo, showToast, registerHandlers,
 *             Icons.swap, emptyState), icons.js, sensors.js (sensorsLoad),
 *             editable-deck.js (EditableDeck.mount).
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
    // May be invoked early (synchronously from the loadPagePartial wrapper on a
    // direct #overview/#alerts load) before the poll interval fires — cancel it
    // so it doesn't run a redundant second pass.
    if (_modeTimer) { clearInterval(_modeTimer); _modeTimer = null; }
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

  // ─── LAZY_PAGES sentinels ────────────────────────────────────────────────
  // core.js treats LAZY_PAGES[page] = 1 as "fetch /pages/<page>.html and
  // inject", and skips the fetch entirely when the value is the constant
  // INJECTED below.  Overview + Alerts are built in JS by buildPages(),
  // not stored on flash as partials, so we use the INJECTED marker.
  var INJECTED = 2;
  if (typeof LAZY_PAGES !== "undefined") {
    LAZY_PAGES.overview = INJECTED;
    LAZY_PAGES.alerts   = INJECTED;
    // Health page removed: diagnostics merged into Sensors as a deck card.
  }
  // Wrap loadPagePartial so the INJECTED sentinel short-circuits the fetch.
  if (typeof loadPagePartial === "function") {
    var _origLoad = loadPagePartial;
    window.loadPagePartial = function (page) {
      if (LAZY_PAGES[page] === INJECTED) {
        // overview/alerts are built by buildPages() inside tryApplyMode(),
        // which is normally deferred to the CFG-poll interval. On a direct load
        // or reload of #overview/#alerts, core.js calls navigateTo()
        // synchronously right after populating CFG — before that interval fires
        // — so the page element wouldn't exist yet and the router would fall
        // back to the settings hub. Build them now, synchronously, so the
        // element is present before navigateTo's promise resolves.
        if (!_modeApplied) tryApplyMode();
        return Promise.resolve();
      }
      return _origLoad(page);
    };
  }

  // ─── Build new pages ──────────────────────────────────────────────────────
  function buildPages(mode) {
    buildOverviewPage();
    buildAlertsPage();
    buildWizardModal();
    injectSensorsLiveCycle(mode);

    // Set default active page based on mode if no hash
    var hash = location.hash.replace("#", "");
    if (!hash || hash === "dashboard") {
      if (mode !== "legacy") {
        if (typeof navigateTo === "function") navigateTo("overview");
      }
    }
  }

  // ── Overview page ──────────────────────────────────────────────────────────
  // Built as an editable card deck (drag-reorder, resize, hide/library tray,
  // localStorage-persisted layout).  Each card type is registered with a
  // `render()` that returns the card's HTML; data is populated by ID-targeted
  // helpers (ovFillEnvironment etc.) AFTER every deck render.
  var _overviewDeck = null;
  var _overviewLastData = null;     // cached /api/latest payload
  var _overviewStatusData = null;   // cached /api/status payload
  var _overviewSensorList = null;   // cached /api/sensors list

  // ── Sensor-to-card binding (persisted in localStorage) ──────────────────────
  var OV_BIND_KEY = "esp32logger.overview.bindings";
  function _loadBindings() { try { return JSON.parse(localStorage.getItem(OV_BIND_KEY)) || {}; } catch (e) { return {}; } }
  function _saveBinding(cardId, sensorId) {
    var b = _loadBindings();
    if (sensorId) b[cardId] = sensorId; else delete b[cardId];
    try { localStorage.setItem(OV_BIND_KEY, JSON.stringify(b)); } catch (e) {}
  }
  function _getBinding(cardId) { return _loadBindings()[cardId] || ""; }

  // Registry: id → { title, icon, render(card) → HTML string }
  var OVERVIEW_REGISTRY = {
    aqi: {
      title: "Air Quality Index", icon: "wind",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="wind"></span> Air Quality Index</div>' +
            '<div style="display:flex;align-items:center;gap:6px">' +
              '<select class="ov-sensor-pick" id="ov-aqi-pick" data-card-bind="aqi" title="Assign PM sensor"><option value="">Auto</option></select>' +
              '<span class="badge ok" id="aqi-badge">GOOD</span>' +
            '</div>' +
          '</div>' +
          '<div class="card-body" style="padding:0">' +
            '<div class="aqi-card">' +
              '<div class="aqi-gauge">' +
                '<svg viewBox="0 0 120 120">' +
                  '<circle class="aqi-track" cx="60" cy="60" r="50"/>' +
                  '<circle class="aqi-fill" id="aqi-arc" cx="60" cy="60" r="50" stroke-dasharray="314" stroke-dashoffset="220"/>' +
                '</svg>' +
                '<div class="aqi-center">' +
                  '<div class="aqi-label">AQI</div>' +
                  '<div class="aqi-score" id="aqi-score">—</div>' +
                  '<div class="aqi-quality" id="aqi-quality">—</div>' +
                '</div>' +
              '</div>' +
              '<div class="aqi-breakdown" id="aqi-breakdown">' +
                '<div class="aqi-bar"><div class="aqi-bar-name">PM2.5</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-pm25" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-pm25v">—</div></div>' +
                '<div class="aqi-bar"><div class="aqi-bar-name">PM10</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-pm10" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-pm10v">—</div></div>' +
                '<div class="aqi-bar"><div class="aqi-bar-name">TVOC</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-tvoc" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-tvocv">—</div></div>' +
                '<div class="aqi-bar"><div class="aqi-bar-name">eCO₂</div><div class="aqi-bar-track"><div class="aqi-bar-fill" id="aqi-eco2" style="width:0%;background:var(--ok)"></div></div><div class="aqi-bar-val" id="aqi-eco2v">—</div></div>' +
              '</div>' +
            '</div>' +
          '</div>' +
        '</div>';
      },
    },
    environment: {
      title: "Environment", icon: "thermometer",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="thermometer"></span> Environment</div>' +
            '<select class="ov-sensor-pick" id="ov-env-pick" data-card-bind="environment" title="Assign sensor"><option value="">Auto</option></select>' +
          '</div>' +
          '<div class="card-body">' +
            '<div class="grid grid-3" style="gap:10px" id="ov-env-kpis">' +
              '<div class="kpi" style="padding:14px"><div class="kpi-l"><span data-icon="thermometer"></span> Temp</div><div class="kpi-v"><span class="num" id="ov-temp">—</span><span class="unit">°C</span></div><div class="kpi-d" id="ov-temp-d">—</div><svg class="metric-spark-bg" id="ov-temp-spark" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true"></svg></div>' +
              '<div class="kpi" style="padding:14px"><div class="kpi-l"><span data-icon="droplet"></span> Humidity</div><div class="kpi-v"><span class="num" id="ov-hum">—</span><span class="unit">%</span></div><div class="kpi-d" id="ov-hum-d">—</div><svg class="metric-spark-bg" id="ov-hum-spark" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true"></svg></div>' +
              '<div class="kpi" style="padding:14px"><div class="kpi-l"><span data-icon="gauge"></span> Pressure</div><div class="kpi-v"><span class="num" id="ov-pres">—</span><span class="unit">hPa</span></div><div class="kpi-d" id="ov-pres-d">—</div><svg class="metric-spark-bg" id="ov-pres-spark" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true"></svg></div>' +
            '</div>' +
          '</div>' +
        '</div>';
      },
    },
    energy: {
      title: "Energy", icon: "zap",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="zap"></span> Energy</div>' +
            '<select class="ov-sensor-pick" id="ov-energy-pick" data-card-bind="energy" title="Assign sensor"><option value="">Auto</option></select>' +
          '</div>' +
          '<div class="card-body">' +
            '<div class="energy-grid" id="ov-energy-grid">' +
              '<div class="energy-tile live"><div class="energy-tile-l">Voltage</div><div class="energy-tile-v"><span id="ov-volt">—</span><span class="u">V</span></div><div class="energy-tile-trend" id="ov-volt-t">—</div></div>' +
              '<div class="energy-tile live"><div class="energy-tile-l">Current</div><div class="energy-tile-v"><span id="ov-amp">—</span><span class="u">A</span></div><div class="energy-tile-trend" id="ov-amp-t">—</div></div>' +
              '<div class="energy-tile live"><div class="energy-tile-l">Power</div><div class="energy-tile-v"><span id="ov-power">—</span><span class="u">W</span></div><div class="energy-tile-trend" id="ov-pf-t">—</div></div>' +
              '<div class="energy-tile"><div class="energy-tile-l">Today</div><div class="energy-tile-v"><span id="ov-kwh-day">—</span><span class="u">kWh</span></div><div class="energy-tile-trend" id="ov-kwh-day-t">—</div></div>' +
              '<div class="energy-tile"><div class="energy-tile-l">This week</div><div class="energy-tile-v"><span id="ov-kwh-week">—</span><span class="u">kWh</span></div><div class="energy-tile-trend" id="ov-kwh-week-t">—</div></div>' +
              '<div class="energy-tile"><div class="energy-tile-l">This month</div><div class="energy-tile-v"><span id="ov-kwh-month">—</span><span class="u">kWh</span></div><div class="energy-tile-trend" id="ov-kwh-month-t">—</div></div>' +
            '</div>' +
          '</div>' +
        '</div>';
      },
    },
    water: {
      title: "Water (live)", icon: "droplets",
      render: function () {
        return '<div class="card" data-mode-show="legacy hybrid">' +
          '<div class="card-head"><div class="card-title"><span data-icon="droplets"></span> Water</div>' +
            '<select class="ov-sensor-pick" id="ov-water-pick" data-card-bind="water" title="Assign sensor"><option value="">Auto</option></select></div>' +
          '<div class="card-body" style="display:flex;flex-direction:column;gap:14px">' +
            '<div><div class="form-label">Current cycle</div>' +
            '<div class="mono" style="font-size:28px;font-weight:700"><span id="ov-water-today">—</span><span style="font-size:13px;color:var(--text-3);margin-left:4px">L</span></div></div>' +
            '<div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Total pulses</span><span class="mono" id="ov-water-events">—</span></div>' +
          '</div>' +
        '</div>';
      },
    },
    outdoor: {
      title: "Outdoor", icon: "cloud-rain",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head"><div class="card-title"><span data-icon="cloud-rain"></span> Outdoor</div>' +
            '<select class="ov-sensor-pick" id="ov-outdoor-pick" data-card-bind="outdoor" title="Assign sensor"><option value="">Auto</option></select></div>' +
          '<div class="card-body" style="display:flex;flex-direction:column;gap:14px">' +
            '<div><div style="color:var(--text-3);font-size:11px;text-transform:uppercase;letter-spacing:.05em">Rain today</div>' +
            '<div class="mono" style="font-size:28px;font-weight:700"><span id="ov-rain">—</span><span style="font-size:13px;color:var(--text-3);margin-left:4px">mm</span></div></div>' +
            '<div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Wind</span><span class="mono" id="ov-wind">—</span></div>' +
          '</div>' +
        '</div>';
      },
    },
    alertFeed: {
      title: "Recent alerts", icon: "bell",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="bell"></span> Recent alerts</div>' +
            '<a class="mono" style="font-size:11px;color:var(--accent);cursor:pointer" data-click="navPage" data-page="alerts">View all →</a>' +
          '</div>' +
          '<div class="card-body" style="padding:0" id="ov-alert-feed">' +
            '<div class="empty" style="padding:20px"><span class="empty-title">No alerts</span></div>' +
          '</div>' +
        '</div>';
      },
    },
    sensorsList: {
      title: "Active sensors", icon: "thermometer",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="thermometer"></span> Active sensors</div>' +
            '<a class="mono" style="font-size:11px;color:var(--accent);cursor:pointer" data-click="navPage" data-page="sensors">All sensors →</a>' +
          '</div>' +
          '<div class="card-body" id="ov-sensors-list" style="display:flex;flex-direction:column;gap:6px">' +
            '<div class="empty" style="padding:20px"><span class="empty-title">Loading…</span></div>' +
          '</div>' +
        '</div>';
      },
    },
    diagnostics: {
      title: "Sensor diagnostics", icon: "heart-pulse",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="heart-pulse"></span> Sensor diagnostics</div>' +
            '<span class="mono" style="font-size:11px;color:var(--text-3)" id="hl-summary">—</span>' +
          '</div>' +
          '<div class="card-body" id="ov-diagnostics-body">' +
            '<div class="health-grid" id="health-grid">' +
              '<div class="empty" style="padding:20px"><span class="empty-title">Loading sensor health…</span></div>' +
            '</div>' +
          '</div>' +
        '</div>';
      },
    },
  };

  var OVERVIEW_DEFAULTS = [
    { id: "aqi",         span: 6 },
    { id: "environment", span: 6 },
    { id: "energy",      span: 8 },
    { id: "outdoor",     span: 4 },
    { id: "alertFeed",   span: 6 },
    { id: "sensorsList", span: 6 },
  ];

  function buildOverviewPage() {
    if (document.getElementById("page-overview")) return;

    var page = document.createElement("section");
    page.className = "page";
    page.id = "page-overview";
    page.setAttribute("data-mode-show", "continuous hybrid");

    page.innerHTML =
      '<div class="page-head">' +
        '<div>' +
          '<h1 class="page-title"><span data-icon="layout-grid"></span> Overview</h1>' +
          '<div class="page-sub" id="ov-sub">IoT sensor dashboard · loading…</div>' +
        '</div>' +
        '<div class="page-actions">' +
          '<div class="page-actions-deck" data-role="deck-toolbar"></div>' +
          '<button class="btn" id="ovAddSensorBtn"><span data-icon="plus"></span> Add sensor</button>' +
        '</div>' +
      '</div>' +
      '<div class="deck" id="overview-deck"></div>';

    // Mount inside <main id="main-content"> (the grid's "main" area), not
    // <body> — otherwise the page renders outside the .app grid, below the
    // sidebar. Matches where the inlined .page sections live.
    (document.getElementById("main-content") || document.body).appendChild(page);
    reIcons(page);

    var addBtn = document.getElementById("ovAddSensorBtn");
    if (addBtn) addBtn.addEventListener("click", function () { openWizard(); });

    _overviewDeck = window.EditableDeck.mount({
      pageId:   "overview",
      container: document.getElementById("overview-deck"),
      registry:  OVERVIEW_REGISTRY,
      defaults:  OVERVIEW_DEFAULTS,
      toolbar:   page.querySelector('[data-role="deck-toolbar"]'),
      onEdit:    function () { /* re-apply cached data after render */
        if (_overviewLastData)   _applyOverviewData(_overviewLastData);
        if (_overviewStatusData) ovFillWaterCached(_overviewStatusData);
        ovFillAlertFeed();
        if (_overviewHealthData) renderHealthGrid(_overviewHealthData);
        if (_overviewSensorList) _populateSensorPickers(_overviewSensorList);
      },
    });

    populateOverview();
  }

  function _normalizeLatest(data) {
    if (Array.isArray(data)) return data;
    var items = (data && data.items) || [];
    var map = {};
    items.forEach(function (it) {
      if (!it) return;
      if (!map[it.id]) map[it.id] = { id: it.id, type: it.type || "", readings: {}, units: {} };
      map[it.id].readings[it.metric] = it.value;
      map[it.id].units[it.metric] = it.unit;
    });
    return Object.keys(map).map(function (k) { return map[k]; });
  }

  function _applyOverviewData(data) {
    var normalized = _normalizeLatest(data);
    ovFillEnvironment(normalized);
    ovFillEnergy(normalized);
    ovFillAQI(normalized);
    ovFillOutdoor(normalized);
    ovFillSensorList(normalized);
    ovFillSensorCards(data);
  }

  /** Populate Overview with real /api/latest data. */
  function populateOverview() {
    // Subtitle is updated below once getSensors() resolves — /api/status does
    // not include a sensorCount field so we cannot read it here up-front.

    fetchWithTimeout("/api/latest", {}, 15000)
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data) return;
        _overviewLastData = data;
        _applyOverviewData(data);
      })
      .catch(function () {});

    ovFillWater();
    ovFillAlertFeed();

    // Fetch sensor list — populates pickers, dynamic cards, AND diagnostics
    getSensors()
      .then(function (data) {
        var sensors = (data && data.sensors) || [];
        _overviewSensorList = sensors;
        _overviewHealthData = sensors;
        _populateSensorPickers(sensors);
        _registerDynamicSensorCards(sensors);
        renderHealthGrid(sensors);
        // Update subtitle now that we know the real sensor count
        var sub = document.getElementById("ov-sub");
        if (sub) sub.textContent = sensors.length + " sensor" + (sensors.length !== 1 ? "s" : "") + " · live readings";
      })
      .catch(function () {
        var grid = document.getElementById("health-grid");
        if (grid) {
          grid.innerHTML = "";
          grid.appendChild(emptyState({ icon: "heart-pulse", title: "Unable to load", msg: "Could not reach /api/sensors." }));
        }
      });
  }

  // ── Background sparkline drawer for Overview metric tiles ──────────────────
  // Throttled worker pool — the ESP's connection pool is tiny, so we keep at
  // most OV_SPARK_MAX /api/data requests in flight. Each placeholder is drawn
  // once per (sensor::metric); the dataset guard avoids refetching on every poll.
  var _ovSparkQueue = [];
  var _ovSparkActive = 0;
  var OV_SPARK_MAX = 2;

  function _ovQueueSpark(svg, sensorId, metric) {
    if (!svg || !sensorId || !metric) return;
    var key = sensorId + "::" + metric;
    if (svg.dataset.drawnFor === key) return;  // already drawn for this binding
    svg.dataset.drawnFor = key;
    _ovSparkQueue.push({ svg: svg, sensorId: sensorId, metric: metric });
    _ovSparkPump();
  }

  function _ovSparkPump() {
    while (_ovSparkActive < OV_SPARK_MAX && _ovSparkQueue.length) {
      var job = _ovSparkQueue.shift();
      _ovSparkActive++;
      _ovFetchSpark(job.svg, job.sensorId, job.metric)
        .then(function () { _ovSparkActive--; _ovSparkPump(); });
    }
  }

  function _ovFetchSpark(svg, sensorId, metric) {
    var now = Math.floor(Date.now() / 1000), from = now - 3600;
    var url = "/api/data?sensor=" + encodeURIComponent(sensorId) +
              "&metric=" + encodeURIComponent(metric) +
              "&from=" + from + "&to=" + now + "&agg=raw&mode=lttb&limit=40";
    return fetchWithTimeout(url, {}, 6000)
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (res) {
        if (!res || !res.data || res.data.length < 2) return;
        var min = Infinity, max = -Infinity, ys = [];
        res.data.forEach(function (pt) {
          if (pt && pt.v !== undefined) {
            var v = Number(pt.v);
            if (!isNaN(v)) { if (v < min) min = v; if (v > max) max = v; ys.push(v); }
          }
        });
        if (ys.length < 2) return;
        var range = max - min; if (range < 1e-9) range = 1;
        var stepX = 100 / (ys.length - 1), line = "", area = "M 0,36";
        for (var j = 0; j < ys.length; j++) {
          var x = (j * stepX).toFixed(1);
          var y = (34 - ((ys[j] - min) / range) * 30).toFixed(1);
          line += (j ? " " : "") + x + "," + y;
          area += " L " + x + "," + y;
        }
        area += " L 100,36 Z";
        svg.innerHTML =
          '<path d="' + area + '" fill="currentColor" opacity="0.15"></path>' +
          '<polyline points="' + line + '" fill="none" stroke="currentColor" stroke-width="1.5" opacity="0.55"></polyline>';
      })
      .catch(function () { svg.dataset.drawnFor = ""; });  // allow retry on failure
  }

  // Scan the overview deck for dynamic-card spark placeholders and draw them.
  function ovDrawCardSparks() {
    var nodes = document.querySelectorAll(".ov-card-spark[data-sensor]");
    [].forEach.call(nodes, function (svg) {
      _ovQueueSpark(svg, svg.getAttribute("data-sensor"), svg.getAttribute("data-metric"));
    });
  }

  function ovFillEnvironment(data) {
    var binding = _getBinding("environment");
    var envSensor = null;
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (binding && s.id === binding) { envSensor = s; return; }
        if (!binding && !envSensor && s.readings &&
            (s.id.indexOf("bme") !== -1 || s.id.indexOf("env") !== -1 ||
             s.type === "bme280" || s.type === "bme688")) {
          envSensor = s;
        }
      });
    }
    if (!envSensor) return;
    var r = envSensor.readings || {};
    var t = document.getElementById("ov-temp");
    var h = document.getElementById("ov-hum");
    var p = document.getElementById("ov-pres");
    if (t && r.temperature !== undefined) t.textContent = r.temperature.toFixed(1);
    if (h && r.humidity !== undefined) h.textContent = Math.round(r.humidity);
    if (p && r.pressure !== undefined) p.textContent = Math.round(r.pressure);

    // Draw background sparklines now that the bound sensor id is known
    if (envSensor.id) {
      _ovQueueSpark(document.getElementById("ov-temp-spark"), envSensor.id, "temperature");
      _ovQueueSpark(document.getElementById("ov-hum-spark"),  envSensor.id, "humidity");
      _ovQueueSpark(document.getElementById("ov-pres-spark"), envSensor.id, "pressure");
    }
  }

  function ovFillEnergy(data) {
    var binding = _getBinding("energy");
    var energySensor = null;
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (binding && s.id === binding) { energySensor = s; return; }
        if (!binding && !energySensor && s.readings &&
            (s.type === "zmct103c" || s.type === "zmpt101b" ||
             s.id.indexOf("power") !== -1 || s.id.indexOf("energy") !== -1)) {
          energySensor = s;
        }
      });
    }
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
    var binding = _getBinding("aqi");
    var pm = null, voc = null, co2 = null;
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (!s) return;   // guard against null/undefined entries in the payload
        if (binding && s.id === binding && s.readings) { pm = s.readings; return; }
        if (!binding && !pm  && s.readings && (s.type === "sds011" || s.type === "pms5003" || s.type === "sps30")) pm  = s.readings;
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

  function ovFillOutdoor(data) {
    var binding = _getBinding("outdoor");
    var outdoorSensor = null;
    if (Array.isArray(data)) {
      data.forEach(function (s) {
        if (binding && s.id === binding) { outdoorSensor = s; return; }
        if (!binding && !outdoorSensor && s.readings &&
            (s.id.indexOf("out") !== -1 || s.id.indexOf("weather") !== -1 || s.id.indexOf("rain") !== -1)) {
          outdoorSensor = s;
        }
      });
    }
    if (!outdoorSensor) return;
    var r = outdoorSensor.readings || {};
    var rainEl = document.getElementById("ov-rain");
    var windEl = document.getElementById("ov-wind");
    if (rainEl && r.rain !== undefined) rainEl.textContent = typeof r.rain === "number" ? r.rain.toFixed(1) : r.rain;
    if (windEl && r.wind !== undefined) windEl.textContent = typeof r.wind === "number" ? r.wind.toFixed(1) : r.wind;
  }

  function ovFillWater() {
    // Pull from /api/live — that endpoint has `liters` (current cycle) and
    // `totalPulses` (cumulative this boot).  The previous code read
    // `todayVol`/`todayEvents` from /api/status, but the firmware never
    // emits those fields, so the card always showed "—".
    fetchWithTimeout("/api/live", {}, 15000)
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (d) {
        if (!d) return;
        _overviewStatusData = d;
        ovFillWaterCached(d);
      })
      .catch(function () {});
  }
  function ovFillWaterCached(d) {
    var today  = document.getElementById("ov-water-today");
    var pulses = document.getElementById("ov-water-events");
    if (today  && typeof d.liters      === "number") today.textContent  = d.liters.toFixed(2);
    if (pulses && d.totalPulses !== undefined)       pulses.textContent = d.totalPulses;
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
    var feed = document.getElementById("ov-alert-feed");
    if (!feed) return;

    // Use already-loaded cache if available; otherwise fetch from /api/alerts
    var history = _alertsData && _alertsData.history;
    if (history) {
      _renderOverviewAlertFeed(feed, history);
    } else {
      fetchWithTimeout("/api/alerts", {}, 10000)
        .then(function (r) { return r.ok ? r.json() : null; })
        .then(function (data) {
          if (data) {
            _alertsData = data;
            _renderOverviewAlertFeed(feed, data.history || []);
          }
        })
        .catch(function () {});
    }
  }

  function _renderOverviewAlertFeed(feed, history) {
    if (!history || !history.length) {
      feed.innerHTML = '<div class="empty" style="padding:20px"><span class="empty-title">No recent alerts</span></div>';
      return;
    }
    // Newest first, capped at 5 rows
    feed.innerHTML = history.slice().reverse().slice(0, 5).map(function (h) {
      return '<div class="alert-feed-row">' +
        '<span class="af-time">' + esc(_relTime(h.ts)) + '</span>' +
        '<div><div class="af-name">' + esc(h.rule_id || "") + '</div>' +
          '<div class="af-val">' + esc(h.value !== undefined ? String(h.value) : "") + '</div></div>' +
        '<span class="badge ' + esc(h.outcome || "ok") + '">' +
          esc((h.outcome || "ok").toUpperCase()) + '</span>' +
      '</div>';
    }).join("");
  }

  // ── Sensor picker & dynamic card helpers ────────────────────────────────────

  function _metricIcon(metric) {
    var m = (metric || "").toLowerCase();
    if (m === "temperature" || m === "temp") return "thermometer";
    if (m === "humidity" || m === "hum") return "droplet";
    if (m === "pressure" || m === "pres") return "gauge";
    if (m === "voltage" || m === "volt") return "zap";
    if (m === "current" || m === "amp") return "zap";
    if (m === "power" || m === "watt") return "zap";
    if (m.indexOf("pm") !== -1 || m === "dust") return "wind";
    if (m === "co2" || m === "eco2") return "cloud";
    if (m === "tvoc" || m === "voc") return "wind";
    if (m === "rain" || m === "rainfall") return "cloud-rain";
    if (m === "wind" || m === "windspeed") return "wind";
    if (m === "light" || m === "lux") return "sun";
    if (m === "soil" || m === "moisture") return "droplets";
    return "activity";
  }

  function _populateSensorPickers(sensors) {
    var picks = document.querySelectorAll(".ov-sensor-pick");
    if (!picks.length) return;
    var bindings = _loadBindings();
    for (var i = 0; i < picks.length; i++) {
      var sel = picks[i];
      var cardId = sel.dataset.cardBind;
      var saved = bindings[cardId] || "";
      sel.innerHTML = '<option value="">Auto</option>';
      sensors.forEach(function (s) {
        var opt = document.createElement("option");
        opt.value = s.id;
        opt.textContent = (s.name || s.id) + " (" + (s.type || "?") + ")";
        if (s.id === saved) opt.selected = true;
        sel.appendChild(opt);
      });
      if (!sel.dataset.wired) {
        sel.dataset.wired = "1";
        sel.addEventListener("change", function () {
          _saveBinding(this.dataset.cardBind, this.value);
          if (_overviewLastData) _applyOverviewData(_overviewLastData);
        });
      }
    }
  }

  function _registerDynamicSensorCards(sensors) {
    // Build the set of valid dynamic cards for the CURRENT sensor list.
    var valid = {};
    sensors.forEach(function (s) {
      if (!s || s.status === "disabled") return;
      (s.metrics || []).forEach(function (m) {
        valid["sensor__" + s.id + "__" + m] = { name: s.name || s.id, metric: m, id: s.id };
      });
    });

    var changed = 0;

    // Prune dynamic cards whose sensor/metric no longer exists (removed,
    // disabled, or renamed) so the Add-Card tray and saved layouts don't keep
    // stale tiles. Only touches the dynamic "sensor__" namespace.
    Object.keys(OVERVIEW_REGISTRY).forEach(function (k) {
      if (k.indexOf("sensor__") === 0 && !valid[k]) {
        delete OVERVIEW_REGISTRY[k];
        changed++;
      }
    });

    // Register newly-seen sensor metrics.
    Object.keys(valid).forEach(function (cardId) {
      if (OVERVIEW_REGISTRY[cardId]) return;
      changed++;
      var sName = valid[cardId].name, m = valid[cardId].metric, sId = valid[cardId].id;
      OVERVIEW_REGISTRY[cardId] = {
        title: sName + " · " + m,
        icon: _metricIcon(m),
        render: function () {
          return '<div class="card ov-metric-card">' +
            '<div class="card-head">' +
              '<div class="card-title"><span data-icon="' + esc(_metricIcon(m)) + '"></span> ' + esc(m) + '</div>' +
              '<span class="mono" style="font-size:11px;color:var(--text-3)">' + esc(sName) + '</span>' +
            '</div>' +
            '<div class="card-body ov-metric-body">' +
              '<div>' +
                '<div class="ov-metric-value" id="ov-sm-' + esc(cardId) + '-v">—</div>' +
                '<div class="ov-metric-unit" id="ov-sm-' + esc(cardId) + '-u"></div>' +
              '</div>' +
            '</div>' +
            '<svg class="metric-spark-bg ov-card-spark" data-sensor="' + esc(sId) + '" data-metric="' + esc(m) + '" viewBox="0 0 100 36" preserveAspectRatio="none" aria-hidden="true"></svg>' +
          '</div>';
        },
      };
    });

    // Reload deck so previously-saved dynamic cards appear / pruned ones drop,
    // and the Add Card library tray reflects the current sensor set.
    if (changed > 0 && _overviewDeck && _overviewDeck.reloadCards) {
      _overviewDeck.reloadCards();
      if (_overviewLastData)   _applyOverviewData(_overviewLastData);
      if (_overviewStatusData) ovFillWaterCached(_overviewStatusData);
      ovFillAlertFeed();
      if (_overviewHealthData) renderHealthGrid(_overviewHealthData);
      if (_overviewSensorList) _populateSensorPickers(_overviewSensorList);
    }
  }

  function ovFillSensorCards(data) {
    var items = (data && data.items) || [];
    if (!items.length && Array.isArray(data)) {
      data.forEach(function (s) {
        if (!s) return;
        var r = s.readings || {};
        Object.keys(r).forEach(function (m) {
          items.push({ id: s.id, metric: m, value: r[m], unit: (s.units && s.units[m]) || "" });
        });
      });
    }
    items.forEach(function (it) {
      if (!it) return;
      var cardId = "sensor__" + it.id + "__" + it.metric;
      var vEl = document.getElementById("ov-sm-" + cardId + "-v");
      var uEl = document.getElementById("ov-sm-" + cardId + "-u");
      if (vEl && it.value !== undefined && it.value !== null) {
        var val = Number(it.value);
        if (!isNaN(val)) {
          var s = val.toFixed(2);
          if (s.indexOf('.') !== -1) {
            s = s.replace(/\.?0+$/, '');
          }
          vEl.textContent = s;
        } else {
          vEl.textContent = String(it.value);
        }
      }
      if (uEl && it.unit) uEl.textContent = it.unit;
    });

    // Draw background sparklines for any dynamic sensor-metric cards on the deck
    ovDrawCardSparks();
  }

  // ── Alerts page ────────────────────────────────────────────────────────────
  // Same editable-deck treatment as Overview: each card type registered with
  // a render(), populated by ID after every deck render.
  var _alertsDeck = null;
  var _alertsData = null;   // cached /api/alerts payload

  var ALERTS_REGISTRY = {
    kpiRules: {
      title: "Rules KPI", icon: "list-checks",
      render: function () {
        return '<div class="kpi"><div class="kpi-l"><span data-icon="list-checks"></span> Rules</div><div class="kpi-v"><span class="num" id="al-total">—</span></div><div class="kpi-d" id="al-rule-d">—</div></div>';
      },
    },
    kpiFiring: {
      title: "Firing KPI", icon: "alert-triangle",
      render: function () {
        return '<div class="kpi"><div class="kpi-l"><span data-icon="alert-triangle"></span> Firing</div><div class="kpi-v" style="color:var(--err)"><span class="num" id="al-firing">0</span></div><div class="kpi-d">right now</div></div>';
      },
    },
    kpiToday: {
      title: "Last 24 h KPI", icon: "clock",
      render: function () {
        return '<div class="kpi"><div class="kpi-l"><span data-icon="clock"></span> Last 24 h</div><div class="kpi-v"><span class="num" id="al-day-trips">0</span></div><div class="kpi-d">trips</div></div>';
      },
    },
    kpiSnoozed: {
      title: "Snoozed KPI", icon: "bell-off",
      render: function () {
        return '<div class="kpi"><div class="kpi-l"><span data-icon="bell-off"></span> Snoozed</div><div class="kpi-v"><span class="num" id="al-snoozed">0</span></div><div class="kpi-d">—</div></div>';
      },
    },
    rules: {
      title: "Rules list", icon: "list-checks",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="list-checks"></span> Rules</div>' +
            '<input class="input" placeholder="Filter rules…" id="al-filter" style="width:200px;height:28px"/>' +
          '</div>' +
          '<div class="card-body" style="padding:0" id="al-rules">' +
            '<div class="empty" style="padding:24px"><span class="empty-title">Loading…</span></div>' +
          '</div>' +
        '</div>';
      },
    },
    history: {
      title: "Trigger history", icon: "history",
      render: function () {
        return '<div class="card">' +
          '<div class="card-head"><div class="card-title"><span data-icon="history"></span> Trigger history</div></div>' +
          '<div class="card-body" style="padding:0" id="al-history">' +
            '<div class="empty" style="padding:24px"><span class="empty-title">Loading…</span></div>' +
          '</div>' +
        '</div>';
      },
    },
  };

  var ALERTS_DEFAULTS = [
    { id: "kpiRules",   span: 3 },
    { id: "kpiFiring",  span: 3 },
    { id: "kpiToday",   span: 3 },
    { id: "kpiSnoozed", span: 3 },
    { id: "rules",      span: 12 },
    { id: "history",    span: 12 },
  ];

  function buildAlertsPage() {
    if (document.getElementById("page-alerts")) return;

    var page = document.createElement("section");
    page.className = "page";
    page.id = "page-alerts";
    page.setAttribute("data-mode-show", "continuous hybrid");

    page.innerHTML =
      '<div class="page-head">' +
        '<div>' +
          '<h1 class="page-title"><span data-icon="bell-ring"></span> Alerts</h1>' +
          '<div class="page-sub">Threshold rules across all sensors</div>' +
        '</div>' +
        '<div class="page-actions">' +
          '<div class="page-actions-deck" data-role="deck-toolbar"></div>' +
          '<button class="btn primary"><span data-icon="plus"></span> New rule</button>' +
        '</div>' +
      '</div>' +
      '<div class="deck" id="alerts-deck"></div>';

    // Mount inside <main id="main-content"> (the grid's "main" area), not
    // <body> — otherwise the page renders outside the .app grid, below the
    // sidebar. Matches where the inlined .page sections live.
    (document.getElementById("main-content") || document.body).appendChild(page);
    reIcons(page);

    _alertsDeck = window.EditableDeck.mount({
      pageId:   "alerts",
      container: document.getElementById("alerts-deck"),
      registry:  ALERTS_REGISTRY,
      defaults:  ALERTS_DEFAULTS,
      toolbar:   page.querySelector('[data-role="deck-toolbar"]'),
      onEdit:    function () {
        // Re-bind the filter input + re-apply cached data after every render.
        _bindAlertsFilter();
        if (_alertsData) {
          _renderAlertRules(_alertsData.rules || []);
          _renderAlertHistory(_alertsData.history || []);
        }
      },
    });

    _bindAlertsFilter();
    _loadAlertsData();
  }

  function _bindAlertsFilter() {
    var fi = document.getElementById("al-filter");
    if (!fi || fi.dataset.bound === "1") return;
    fi.dataset.bound = "1";
    fi.addEventListener("input", function () {
      var q = fi.value.toLowerCase();
      var rules = document.querySelectorAll(".alert-rule");
      for (var i = 0; i < rules.length; i++) {
        rules[i].style.display = rules[i].textContent.toLowerCase().indexOf(q) !== -1 ? "" : "none";
      }
    });
  }

  // Fetch GET /api/alerts and render rules + history from real firmware data.
  function _loadAlertsData() {
    fetchWithTimeout("/api/alerts", {}, 15000)
      .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
      .then(function (data) {
        _alertsData = data;
        _renderAlertRules(data.rules  || []);
        _renderAlertHistory(data.history || []);
      })
      .catch(function (err) {
        var msg = '<div class="empty" style="padding:24px"><span class="empty-title">Failed to load alerts (' + esc(String(err)) + ')</span></div>';
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

      // role="group" + aria-label gives screen readers a single
      // announcement per row ("Rule X · firing") instead of reading the
      // state dot, the title, the expression, and the meta column
      // separately as disconnected fragments.
      var ariaLabel = (r.name || r.id || "Rule") + " · " + state;
      return '<div class="alert-rule ' + state + '" role="group" ' +
              'aria-label="' + esc(ariaLabel) + '" data-id="' + esc(r.id || "") + '">' +
        '<span class="ar-state" aria-hidden="true"></span>' +
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

    // Wire toggle checkboxes to POST /api/alerts with the updated rule.
    // Uses the cached _alertsData (filled by _loadAlertsData) instead of
    // re-fetching the whole config before posting it back — halves the
    // round-trips on every toggle.
    rc.querySelectorAll('input[data-rule-id]').forEach(function (cb) {
      cb.addEventListener("change", function () {
        var ruleId = cb.dataset.ruleId;
        if (!_alertsData) { cb.checked = !cb.checked; return; }
        // Mutate the cache so subsequent toggles + the next re-render see
        // the new state without another GET.
        (_alertsData.rules || []).forEach(function (rule) {
          if (rule.id === ruleId) rule.enabled = cb.checked;
        });
        getCsrfToken().then(function (token) {
          var url = "/api/alerts" + (token ? "?csrf=" + encodeURIComponent(token) : "");
          // Send only the rules array — the backend's fromJson() reads
          // doc["rules"] and ignores everything else, so we save bandwidth
          // and memory on the ESP by stripping the (potentially large)
          // history feed (gemini review PR #108).
          return fetchWithTimeout(url, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ rules: _alertsData.rules }),
          }, 30000);
        }).then(function (r) {
          if (r && r.status === 403) {
            window.__csrfToken = null;
            throw new Error("CSRF token rejected — refresh page");
          }
        }).catch(function () {
          // Revert the optimistic mutation on failure
          (_alertsData.rules || []).forEach(function (rule) {
            if (rule.id === ruleId) rule.enabled = !cb.checked;
          });
          cb.checked = !cb.checked;
        });
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
    // Keep the topbar bell badge in sync
    var tbadge = document.getElementById("topbar-alert-badge");
    if (tbadge) {
      tbadge.textContent = firing || "0";
      tbadge.style.display = firing ? "" : "none";
    }
  }

  // Render the history feed from API data and update the "Last 24 h" KPI.
  function _renderAlertHistory(history) {
    var hc = document.getElementById("al-history");
    if (!hc) return;

    // Count trips in the last 24 h regardless of list length
    var cutoff = Math.floor(Date.now() / 1000) - 86400;
    var trips24 = (history || []).filter(function (h) { return h.ts && h.ts >= cutoff; }).length;
    setEl("al-day-trips", trips24);

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
          '<div class="af-val">' + esc(h.value !== undefined ? h.value : "") + '</div></div>' +
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

  // ── Sensor diagnostics (formerly the Health page) ──────────────────────────
  // The standalone /health page was removed; the same content is now offered
  // as a deck card type (id "diagnostics") on the Overview page.  Logic for
  // fetching, rendering, and summarising lives here so the card can call it.
  var _overviewHealthData = null;

  function populateDiagnostics() {
    getSensors()
      .then(function (data) {
        var sensors = (data && data.sensors) || [];
        _overviewHealthData = sensors;
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
      // /api/sensors nests health metrics inside s.health.*  with different
      // field names than the legacy flat layout the renderer originally expected.
      var h = s.health || {};
      var state = s.status === "err"    ? "err"
                : s.status === "stale"  ? "warn"
                : s.enabled === false   ? "disabled"
                : "ok";
      var uptime  = h.uptime_pct_24h  !== undefined ? h.uptime_pct_24h
                  : (state === "err" ? 0 : state === "warn" ? 80 : 99);
      var reads   = h.reads_24h   || 0;
      var errors  = h.errors_24h  || 0;
      // API has no explicit retry counter; surface lifetime error_count instead.
      var retries = s.error_count  || 0;
      // avg_latency_us is in microseconds — convert to ms for display.
      var avgLat  = h.avg_latency_us !== undefined
                  ? (h.avg_latency_us / 1000).toFixed(2) + " ms" : "—";
      var lastSeen = h.last_read_ms_ago !== undefined
                  ? Math.round(h.last_read_ms_ago / 1000) + "s ago" : "—";

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
    setEl("hl-summary",
      sensors.length + " sensors · " +
      upCount + " up · " +
      staleCount + " stale · " +
      errCount + " errored");
  }

  // ─── Sensors page: live-cycle section (merged from removed Live page) ────
  // The standalone Live page was removed; its current-cycle + state-machine
  // cards now mount at the top of the Sensors page in legacy / hybrid modes
  // (i.e. whenever a flow-meter is wired).  In continuous mode the section
  // is skipped — there's nothing to display.
  function injectSensorsLiveCycle(mode) {
    if (mode === "continuous") return;
    var sensorsPage = document.getElementById("page-sensors");
    if (!sensorsPage) return;
    if (document.getElementById("sensors-live-cycle")) return;

    var section = document.createElement("section");
    section.id = "sensors-live-cycle";
    section.className = "sensors-live-cycle";
    section.style.marginBottom = "var(--gap)";
    // Use `slc-` (sensors-live-cycle) prefix on every ID so the injected
    // Sensors card doesn't collide with the matching nodes in #page-live
    // (which carry the same semantic IDs `live-liters`, `live-pulses`, …).
    // getElementById returns first-match in document order, and the legacy
    // Live section parses before this card injects — without unique IDs
    // the Sensors mirror would stay stale while the hidden Live page nodes
    // got the updates (codex review PR #108).
    section.innerHTML =
      '<div class="grid grid-12">' +
        '<div class="card span-8">' +
          '<div class="card-head">' +
            '<div class="card-title"><span data-icon="droplets"></span> Current cycle</div>' +
            '<span class="badge dim mono" id="slc-state">IDLE</span>' +
          '</div>' +
          '<div class="card-body">' +
            '<div style="display:flex;align-items:center;gap:24px;flex-wrap:wrap;justify-content:space-between">' +
              '<div class="bigstat"><div class="bigstat-l">Volume</div><div class="bigstat-v mono"><span id="slc-liters">0.00</span><span>L</span></div></div>' +
              '<div class="bigstat"><div class="bigstat-l">Pulses</div><div class="bigstat-v mono" id="slc-pulses">0</div></div>' +
              '<div class="bigstat"><div class="bigstat-l">Duration</div><div class="bigstat-v mono"><span id="slc-cycleTime">0</span><span>s</span></div></div>' +
              '<div class="bigstat"><div class="bigstat-l">Trigger</div><div class="bigstat-v" style="color:var(--accent)" id="slc-trigger">–</div></div>' +
            '</div>' +
          '</div>' +
        '</div>' +
        '<div class="card span-4">' +
          '<div class="card-head"><div class="card-title"><span data-icon="git-branch"></span> State machine</div></div>' +
          '<div class="card-body" style="display:flex;flex-direction:column;gap:12px">' +
            '<div id="sensors-live-state-wrap">' +
              '<div id="slc-state-mirror" class="badge dim">–</div>' +
              '<div class="mono" style="font-size:11px;color:var(--text-3);margin-top:8px">Live feed from the legacy flow-meter pipeline. Open <a href="#live" data-click="navPage" data-page="live" data-args="[]">the full Live page</a> for the timer + log.</div>' +
            '</div>' +
          '</div>' +
        '</div>' +
      '</div>';

    // Mount it as the first child of the Sensors page, after .page-head.
    var head = sensorsPage.querySelector(".page-head");
    if (head && head.nextSibling) sensorsPage.insertBefore(section, head.nextSibling);
    else sensorsPage.appendChild(section);
    reIcons(section);

    // Hook the existing /api/live stream so values update.  We piggy-back on
    // the same SSE that the Live page uses (core.js sets up liveES); if it's
    // not active we fall back to a 2 s poll of /api/status.
    _wireLiveCycleMirror();
  }

  // SSE-first: the firmware pushes a `live` event on /api/events at 1 Hz with
  // the full snapshot (pulses, liters, cycleTime, trigger, state, …).  Falls
  // back to /api/live polling when EventSource is unavailable or fails.
  // Sleeps entirely when the Sensors page isn't visible (document.hidden or
  // the page is not .active) to save CPU + bandwidth on the AP.
  var _liveCycleES    = null;
  var _liveCycleTimer = null;

  function _liveCycleApply(d) {
    if (!d) return;
    setEl("slc-liters",    typeof d.liters   === "number" ? d.liters.toFixed(2) : "0.00");
    setEl("slc-pulses",    d.pulses    !== undefined ? d.pulses    : 0);
    setEl("slc-cycleTime", d.cycleTime !== undefined ? d.cycleTime : 0);
    setEl("slc-trigger",   d.trigger   || "–");
    if (d.state) {
      var s = String(d.state).toUpperCase();
      setEl("slc-state",        s);
      setEl("slc-state-mirror", s);
    }
  }

  function _liveCyclePageVisible() {
    if (document.hidden) return false;
    var sensorsPage = document.getElementById("page-sensors");
    return sensorsPage && sensorsPage.classList.contains("active");
  }

  function _liveCycleStop() {
    if (_liveCycleES)    { try { _liveCycleES.close(); } catch (e) {} _liveCycleES = null; }
    if (_liveCycleTimer) { clearInterval(_liveCycleTimer); _liveCycleTimer = null; }
  }

  function _liveCycleStart() {
    if (!document.getElementById("sensors-live-cycle")) return;
    if (_liveCycleES || _liveCycleTimer) return; // already running

    // Prefer SSE — single TCP connection, 1 Hz push.  Track a small error
    // counter: if the browser fails to reconnect 3 times in a row (server
    // 404, CSP block, captive-portal redirect, …) we close SSE and degrade
    // to polling so the card doesn't sit frozen (gemini review PR #108).
    if (typeof EventSource !== "undefined") {
      try {
        _liveCycleES = new EventSource("/api/events");
        var errCount = 0;
        _liveCycleES.addEventListener("live", function (ev) {
          errCount = 0;
          try { _liveCycleApply(JSON.parse(ev.data)); } catch (e) {}
        });
        _liveCycleES.onerror = function () {
          errCount++;
          if (errCount < 3) return;  // let the browser keep auto-retrying
          try { _liveCycleES.close(); } catch (e) {}
          _liveCycleES = null;
          _startLiveCyclePolling();
        };
      } catch (e) { _liveCycleES = null; }
    }
    // Polling fallback — used when SSE is unavailable up-front and when
    // SSE persistently errors (see onerror handler above).  4 s instead of
    // the old 2 s; paired with SSE on fresh devices it's a safety net,
    // not the primary transport.
    if (!_liveCycleES) _startLiveCyclePolling();
  }

  function _startLiveCyclePolling() {
    if (_liveCycleTimer) return;
    _liveCycleTimer = setInterval(function () {
      if (!_liveCyclePageVisible()) return;
      fetchWithTimeout("/api/live", {}, 8000)
        .then(function (r) { return r.ok ? r.json() : null; })
        .then(_liveCycleApply)
        .catch(function () {});
    }, 4000);
  }

  function _wireLiveCycleMirror() {
    _liveCycleStart();
    // Pause / resume on tab visibility — keeps an idle laptop or backgrounded
    // phone from streaming when nothing is watching.
    document.addEventListener("visibilitychange", function () {
      if (document.hidden) _liveCycleStop();
      else if (_liveCyclePageVisible()) _liveCycleStart();
    });
    // Also re-evaluate when the SPA swaps pages — cheap MutationObserver.
    var main = document.getElementById("main-content") || document.querySelector(".main");
    if (main) {
      var mo = new MutationObserver(function () {
        if (_liveCyclePageVisible()) _liveCycleStart();
        else _liveCycleStop();
      });
      mo.observe(main, { attributes: true, subtree: true, attributeFilter: ["class"] });
    }
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

    // Collect existing sensor cards by data-sid attribute.
    // Multiple cards can share the same data-sid (one per metric), so keep an array.
    var existing = {};
    grid.querySelectorAll("[data-sid]").forEach(function (c) {
      var sid = c.dataset.sid;
      if (!existing[sid]) existing[sid] = [];
      existing[sid].push(c);
    });

    if (!Object.keys(existing).length) return;

    // Rebuild
    grid.innerHTML = "";
    grid.style.display = "block";

    var ZONE_ICONS = { indoor:"home", outdoor:"sun", utility:"wrench", other:"grid" };

    // Helper: flatten arrays of cards for a list of sensor ids
    function _cardsFor(ids) {
      return ids.reduce(function (acc, id) {
        return existing[id] ? acc.concat(existing[id]) : acc;
      }, []);
    }

    Object.keys(zoneMap).forEach(function (zone) {
      var cards = _cardsFor(zoneMap[zone]);
      if (!cards.length) return;

      // Sensor count = unique data-sid values in this zone
      var uniqueIds = zoneMap[zone].filter(function (id) { return existing[id]; });

      var sec = document.createElement("div");
      sec.className = "zone-section";
      sec.innerHTML =
        '<div class="zone-head">' +
          '<div class="zone-title">' +
            '<span data-icon="' + (ZONE_ICONS[zone] || "grid") + '"></span>' +
            zone.charAt(0).toUpperCase() + zone.slice(1) +
          '</div>' +
          '<div class="zone-meta">' + uniqueIds.length + ' sensor' + (uniqueIds.length !== 1 ? 's' : '') + '</div>' +
        '</div>' +
        '<div class="sensors-grid zone-cards"></div>';

      var cardGrid = sec.querySelector(".zone-cards");
      cards.forEach(function (c) { cardGrid.appendChild(c); });

      grid.appendChild(sec);
    });

    // Any cards not assigned to any zone
    var unzonedIds = Object.keys(existing).filter(function (id) {
      return !sensors.find(function (s) { return s.id === id; });
    });
    var unzoned = _cardsFor(unzonedIds);
    if (unzoned.length) {
      var sec2 = document.createElement("div");
      sec2.className = "zone-section";
      sec2.innerHTML =
        '<div class="zone-head">' +
          '<div class="zone-title"><span data-icon="grid"></span> Other</div>' +
          '<div class="zone-meta">' + unzonedIds.length + ' sensor' + (unzonedIds.length !== 1 ? 's' : '') + '</div>' +
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
      ["sps30",   "cloud-fog",       "SPS30",     "I2C · PM1/2.5/4/10"],
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
              '<div class="field"><label for="wiz-iface">Interface</label><select id="wiz-iface" class="input"><option>I2C</option><option>UART</option><option>Pulse</option><option>GPIO</option><option>ADC</option><option>1-Wire</option></select></div>' +
              // I2C fields
              '<div class="field wiz-if" data-if="i2c"><label for="wiz-addr">I2C Address</label><input id="wiz-addr" class="input mono" value="0x76"/></div>' +
              '<div class="field wiz-if" data-if="i2c"><label for="wiz-sda">SDA pin</label><input id="wiz-sda" class="input mono" type="number" value="6"/></div>' +
              '<div class="field wiz-if" data-if="i2c"><label for="wiz-scl">SCL pin</label><input id="wiz-scl" class="input mono" type="number" value="7"/></div>' +
              // UART fields
              '<div class="field wiz-if" data-if="uart"><label for="wiz-rx">RX pin</label><input id="wiz-rx" class="input mono" type="number" value="4"/></div>' +
              '<div class="field wiz-if" data-if="uart"><label for="wiz-tx">TX pin <span style="color:var(--text-3)">(optional)</span></label><input id="wiz-tx" class="input mono" type="number" placeholder="—"/></div>' +
              '<div class="field wiz-if" data-if="uart"><label for="wiz-baud">Baud</label><input id="wiz-baud" class="input mono" type="number" value="9600"/></div>' +
              // GPIO / ADC / 1-Wire: single data pin
              '<div class="field wiz-if" data-if="pulse gpio adc 1-wire"><label for="wiz-pin">Data pin</label><input id="wiz-pin" class="input mono" type="number" value="4"/></div>' +
              // Always shown
              '<div class="field"><label for="wiz-int">Read interval (ms)</label><input id="wiz-int" class="input mono" type="number" value="10000" min="500"/></div>' +
            '</div>' +
            // Restricted-pin warning + per-sensor override (populated by wizUpdatePinWarn)
            '<div id="wiz-pinwarn" style="display:none;margin-top:10px;padding:8px 10px;border-radius:6px;font-size:12px"></div>' +
            '<label id="wiz-unsafe-wrap" style="display:none;align-items:center;gap:6px;cursor:pointer;margin-top:8px;font-size:12px">' +
              '<input type="checkbox" id="wiz-unsafe"> Use this pin anyway (I\'ve added proper pull-ups)' +
            '</label>' +
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
        // Pre-select the interface from the card's meta ("I2C · …",
        // "UART · …", "Pulse") so the right pin fields show on the next step.
        var metaEl = c.querySelector(".wiz-type-meta");
        var meta = ((metaEl && metaEl.textContent) || "").split("·")[0].trim().toUpperCase();
        var ifMap = { "I2C": "I2C", "UART": "UART", "PULSE": "Pulse", "ADC": "ADC", "1-WIRE": "1-Wire" };
        var ifaceEl = document.getElementById("wiz-iface");
        if (ifaceEl && ifMap[meta]) { ifaceEl.value = ifMap[meta]; wizUpdateIfaceFields(); }
      });
    });
    // Select first by default
    var first = wiz.querySelector(".wiz-type-card");
    if (first) first.classList.add("selected");

    // Interface → show only the relevant pin fields (and refresh once now).
    var ifaceSel = wiz.querySelector("#wiz-iface");
    if (ifaceSel) ifaceSel.addEventListener("change", wizUpdateIfaceFields);
    wizUpdateIfaceFields();
    // Live restricted-pin warning as the user edits any pin field.
    ["wiz-sda", "wiz-scl", "wiz-rx", "wiz-tx", "wiz-pin"].forEach(function (id) {
      var el = wiz.querySelector("#" + id);
      if (el) el.addEventListener("input", wizUpdatePinWarn);
    });

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

  // Show only the pin/config fields that belong to the selected interface.
  function wizUpdateIfaceFields() {
    if (!_wizardEl) return;
    var ifaceEl = _wizardEl.querySelector("#wiz-iface");
    if (!ifaceEl) return;
    var iface = (ifaceEl.value || "I2C").toLowerCase();   // i2c|uart|gpio|adc|1-wire
    _wizardEl.querySelectorAll(".wiz-if").forEach(function (el) {
      var list = (el.getAttribute("data-if") || "").split(" ");
      el.style.display = (list.indexOf(iface) >= 0) ? "" : "none";
    });
    wizUpdatePinWarn();
  }

  // Warn when a chosen GPIO is a strapping/reserved/flash pin for this board,
  // and reveal the per-sensor override for the (soft) risky-but-usable cases.
  function wizUpdatePinWarn() {
    if (!_wizardEl || typeof getBoardPins !== "function") return;
    var warn = document.getElementById("wiz-pinwarn");
    var wrap = document.getElementById("wiz-unsafe-wrap");
    if (!warn) return;
    var iface = ((document.getElementById("wiz-iface") || {}).value || "I2C").toLowerCase();
    var ids = iface === "i2c"  ? ["wiz-sda", "wiz-scl"]
            : iface === "uart" ? ["wiz-rx", "wiz-tx"]
            : ["wiz-pin"];
    getBoardPins().then(function (pins) {
      var msgs = [], hard = false, soft = false;
      ids.forEach(function (id) {
        var el = document.getElementById(id);
        if (!el || el.value === "") return;
        var risk = pinRisk(pins, el.value);
        if (risk) {
          msgs.push("GPIO" + parseInt(el.value, 10) + " — " + risk.reason);
          if (risk.hard) hard = true; else soft = true;
        }
      });
      if (!msgs.length) { warn.style.display = "none"; if (wrap) wrap.style.display = "none"; return; }
      warn.style.display = "";
      warn.style.background = hard ? "rgba(220,38,38,.12)" : "rgba(217,119,6,.14)";
      warn.style.color      = hard ? "var(--err)" : "var(--warn)";
      warn.innerHTML = "⚠ " + msgs.join(" · ") +
        (hard ? " — this pin can't be used (hardware-reserved); pick another."
              : " — usable only with proper pull-ups; the device may fail to boot if held LOW at reset.");
      // Override applies only to soft risks with no hard blocker present.
      if (wrap) wrap.style.display = (soft && !hard) ? "flex" : "none";
    });
  }

  function buildWizReview() {
    var typeCard = _wizardEl.querySelector(".wiz-type-card.selected");
    var typeVal  = typeCard ? typeCard.dataset.type : "unknown";
    var idVal    = (document.getElementById("wiz-id")    || {}).value || "new_sensor";
    var zoneVal  = (document.getElementById("wiz-zone")  || {}).value || "indoor";
    var nameVal  = (document.getElementById("wiz-name")  || {}).value || "";
    var ifaceVal = (document.getElementById("wiz-iface") || {}).value || "I2C";
    var intVal   = (document.getElementById("wiz-int")   || {}).value || "10000";
    var iface    = ifaceVal.toLowerCase();

    // Read an integer field by id; return `def` when blank/missing/non-numeric.
    function pinVal(id, def) {
      var v = (document.getElementById(id) || {}).value;
      var n = parseInt(v, 10);
      return isNaN(n) ? def : n;
    }

    var obj = {
      id:               idVal,
      type:             typeVal,
      zone:             zoneVal,
      name:             nameVal,
      enabled:          true,
      interface:        iface,
      read_interval_ms: parseInt(intVal, 10),
    };
    if ((document.getElementById("wiz-unsafe") || {}).checked) {
      obj.allow_unsafe_pins = true;   // user opted into a strapping/reserved pin
    }

    // Interface-specific pins/keys — must match the SensorManager plugin schema.
    if (iface === "i2c") {
      var addrVal = (document.getElementById("wiz-addr") || {}).value || "0x76";
      obj.address = parseInt(addrVal, 16) || parseInt(addrVal, 10) || 0;
      obj.sda     = pinVal("wiz-sda", -1);
      obj.scl     = pinVal("wiz-scl", -1);
    } else if (iface === "uart") {
      obj.uart_rx = pinVal("wiz-rx", -1);   // required by SDS011/PMS5003 plugins
      obj.uart_tx = pinVal("wiz-tx", -1);   // optional (-1 = RX-only)
      obj.baud    = pinVal("wiz-baud", 9600);
    } else {
      // gpio (pulse) / adc / 1-wire — single data pin
      obj.pin     = pinVal("wiz-pin", -1);
    }

    var pre = document.getElementById("wiz-json");
    if (pre) pre.textContent = JSON.stringify(obj, null, 2);
  }

  function wizardSave() {
    // Build the sensor object from the review step
    var pre = document.getElementById("wiz-json");
    var obj;
    try { obj = pre ? JSON.parse(pre.textContent) : null; } catch (e) { obj = null; }
    if (!obj) { showToast("Parse error", "Could not read sensor config", "err"); return; }

    // Sensors live in platform_config.json, not the binary config, and there is
    // no single-sensor endpoint (the old /save_corelogic + add_sensor never
    // existed → 404). Mirror the Core Logic page: fetch the current platform
    // config, append/replace the sensor in its `sensors` array, and POST the
    // whole document to /save_platform via postWithCsrf (handles the csrf token
    // + 403 retry, same as pcfgSave()).
    fetchWithTimeout("/api/platform_config", {}, 15000)
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (cfg) {
        if (!cfg || typeof cfg !== "object") throw new Error("platform config unavailable");
        if (!Array.isArray(cfg.sensors)) cfg.sensors = [];
        // Replace a sensor with the same id if present, else append.
        var idx = -1;
        for (var i = 0; i < cfg.sensors.length; i++) {
          if (cfg.sensors[i] && cfg.sensors[i].id === obj.id) { idx = i; break; }
        }
        if (idx >= 0) cfg.sensors[idx] = obj; else cfg.sensors.push(obj);
        return postWithCsrf("/save_platform", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(cfg),
        }, 30000);
      })
      // Parse the body even on a non-2xx response so the server's own error
      // (e.g. {ok:false,error:"busy"} on 503) surfaces instead of a generic
      // fallback message.
      .then(function (r) { return r ? r.json() : null; })
      .then(function (res) {
        if (res && res.ok) {
          closeWizard();
          showToast("Sensor added", obj.id + " · device restarting to apply", "ok");
          // /save_platform only writes platform_config.json — the running
          // pipeline keeps the old config until reloaded. Mirror the Core Logic
          // page: signal a reload (sets shouldRestart server-side; not CSRF-
          // gated). Refresh the list once the device is back up.
          fetchWithTimeout("/api/platform_reload", { method: "POST" }, 30000).catch(function () {});
          setTimeout(function () { if (typeof sensorsLoad === "function") sensorsLoad(); }, 6000);
        } else {
          showToast("Save failed", (res && res.error) || "Check firmware logs", "err");
        }
      })
      .catch(function (e) {
        showToast("Save failed", (e && e.message) ? e.message : "Could not reach device", "err");
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
  registerHandlers({ openSensorWizard: openWizard, clAddSensor: openWizard });

  // ─── Keyboard shortcuts for new pages (G O / G A) ─────────────────────────
  // core.js already handles G+D/L/S/F/C/U. We hook the same keydown so the
  // two-key sequences share the same timing context.
  (function () {
    var gWait = false, gTimer = null;
    var extraMap = { o: "overview", a: "alerts" };

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
    var hints = { overview: ["G","O"], alerts: ["G","A"] };
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
