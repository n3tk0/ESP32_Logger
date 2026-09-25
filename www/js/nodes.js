// ============================================================================
// nodes.js — the unified Nodes settings page (#settings_nodes)
//
// Merges what were two separate pages/endpoints into one list:
//   ESP-NOW battery nodes  — GET/POST /api/espnow/*  (transport "espnow")
//   WiFi remote nodes      — GET      /api/remote/status (transport "wifi")
// Either endpoint 404s on a build without its FEATURE_* flag; that is read
// as "not in this build" for that transport specifically, not an error.
//
// A node's own settings — sensors, pins, interval, its network — are edited
// in the row's drawer for BOTH transports, through /api/nodes/config
// (docs/NODE_CONFIG.md §7, which is the contract: read it before changing
// anything below). The collector only holds a DESIRED config; the node picks
// it up the next time it reports, so every save is "pending" until the node
// says it applied it, and a node can refuse one. The row badge says which.
// A WiFi node that has never reported its settings has nothing to edit yet:
// the collector does not know its board, its pins or even its firmware.
//
// While the collector is moving to another WiFi network (§4), a banner on
// this page and on the Network page follows the nodes across.
//
// TWO VALUES ARE NULL RATHER THAN ZERO on ESP-NOW nodes, carried over from
// the page this replaces:
//   rssi   unavailable on Arduino core 2.x — IDF 4.4 gives the receive
//          callback no signal information at all.
//   skew_s null until the node has reported with both clocks set. Zero is a
//          real and good answer (a perfectly synchronised node), so the two
//          must not be conflated. Both render as an em dash.
// ============================================================================

var ndEspnowData = null;      // last successful /api/espnow/status payload, or null
var ndRemoteData = null;      // last successful /api/remote/status payload, or null
var ndEspnowAvailable = null; // true | false (404) | null (unknown/error)
var ndRemoteAvailable = null;
var ndList = [];              // merged rows, rebuilt by ndMerge()
var ndFilterState = "all";
var ndSearchQuery = "";
var ndOpenKey = null;         // currently expanded row's key, or null
var ndPairTimer = null;

function ndT(key, vars) { return window.I18n ? I18n.t(key, vars) : key; }

function ndMsg(text, kind) {
  showMsg("nd-msg",
    "<div class='alert alert-" + (kind === "ok" ? "success" : "error") + "'>" +
    esc(text) + "</div>", true);
}

// Seconds-based age formatter — shared by both transports. Remote reports
// age in ms; callers convert before calling this.
function ndFmtAge(s) {
  if (s < 60) return s + "s";
  var m = Math.round(s / 60);
  if (m < 60) return m + "m";
  var h = Math.round(m / 60);
  if (h < 24) return h + "h";
  return Math.round(h / 24) + "d";
}

// How far an ESP-NOW node's clock is from the collector's, as a badge.
// Threshold matches the firmware's ESPNOW_SKEW_WARN_S.
function ndSkew(n) {
  if (n.skew_s == null) {
    return { text: "—", cls: "dim", title: ndT("nodes.clockUnmeasured") };
  }
  var s = n.skew_s;
  var mag = Math.abs(s);
  var txt = mag < 60 ? mag + "s" : ndFmtAge(mag);
  if (s !== 0) txt = (s > 0 ? "-" : "+") + txt; // node behind us reads as negative
  return {
    text: txt,
    cls: mag >= 60 ? "warn" : "ok",
    title: s === 0 ? ndT("nodes.clockOk")
      : ndT("nodes.clockSkewed", { mag: mag, dir: s > 0 ? ndT("nodes.behind") : ndT("nodes.ahead") }),
  };
}

// Matches the firmware's own rule so the page and the device never disagree
// about what counts as a warning: the collector sends `warn`, this only
// chooses how to colour it.
function ndBattClass(n) {
  if (n.percent == null) return "dim";
  if (n.warn) return "err";
  if (n.percent <= 25) return "warn";
  return "ok";
}

function ndIsProblem(n) {
  if (n.transport === "espnow") {
    return !n.online || (n.skew_s != null && Math.abs(n.skew_s) >= 60) || !!n.raw.warn;
  }
  return !n.online;
}

// ── Fetch + merge ───────────────────────────────────────────────────────────

function ndFetchEspnow() {
  return fetchWithTimeout("/api/espnow/status", {}, 15000)
    .then(function (r) {
      if (r.status === 404) { ndEspnowAvailable = false; return null; }
      if (!r.ok) throw new Error("HTTP " + r.status);
      ndEspnowAvailable = true;
      return r.json();
    })
    .catch(function () { ndEspnowAvailable = ndEspnowAvailable === false ? false : null; return null; });
}

function ndFetchRemote() {
  return fetchWithTimeout("/api/remote/status", {}, 15000)
    .then(function (r) {
      if (r.status === 404) { ndRemoteAvailable = false; return null; }
      if (!r.ok) throw new Error("HTTP " + r.status);
      ndRemoteAvailable = true;
      return r.json();
    })
    .catch(function () { ndRemoteAvailable = ndRemoteAvailable === false ? false : null; return null; });
}

function ndMerge() {
  ndList = [];
  // `key` is the row's own key (what ndOpenKey and the driver use); `cfgKey`
  // is the collector's name for the node's config file, "e:<id>"/"w:<name>"
  // — taken from the status payload's `cfg` when it has one, so the two can
  // never disagree about which node a save is for.
  var enNodes = (ndEspnowData && ndEspnowData.nodes) || [];
  for (var i = 0; i < enNodes.length; i++) {
    var n = enNodes[i];
    ndList.push({
      transport: "espnow",
      key: "en:" + n.node_id,
      cfgKey: (n.cfg && n.cfg.key) || "e:" + n.node_id,
      cfg: n.cfg || null,
      name: n.id,
      online: !n.offline,
      raw: n,
    });
  }
  var rnNodes = (ndRemoteData && ndRemoteData.nodes) || [];
  for (var j = 0; j < rnNodes.length; j++) {
    var m = rnNodes[j];
    ndList.push({
      transport: "wifi",
      key: "rn:" + m.id,
      cfgKey: (m.cfg && m.cfg.key) || "w:" + m.id,
      cfg: m.cfg || null,
      name: m.id,
      online: !!m.online,
      raw: m,
    });
  }
}

function ndRowByCfgKey(cfgKey) {
  for (var i = 0; i < ndList.length; i++) if (ndList[i].cfgKey === cfgKey) return ndList[i];
  return null;
}

// ── KPIs ─────────────────────────────────────────────────────────────────

function ndRenderKpis() {
  var total = ndList.length;
  var online = ndList.filter(function (n) { return n.online; }).length;
  var enCount = ndList.filter(function (n) { return n.transport === "espnow"; }).length;
  var rnCount = total - enCount;
  setEl("nd-kpi-total", total);
  document.getElementById("nd-kpi-total-d").textContent =
    enCount || rnCount ? enCount + " ESP-NOW · " + rnCount + " WiFi" : "";

  setEl("nd-kpi-online", online);
  var onlineUnitEl = document.getElementById("nd-kpi-online-unit");
  if (onlineUnitEl) onlineUnitEl.textContent = total ? "/ " + total : "";
  var worstOffline = ndList.filter(function (n) { return !n.online; })
    .sort(function (a, b) {
      var as = a.transport === "espnow" ? (a.raw.age_s || 0) : (a.raw.age_ms || 0) / 1000;
      var bs = b.transport === "espnow" ? (b.raw.age_s || 0) : (b.raw.age_ms || 0) / 1000;
      return bs - as;
    })[0];
  document.getElementById("nd-kpi-online-d").textContent = worstOffline
    ? worstOffline.name + ": " + ndT("nodes.ago", {
        t: ndFmtAge(worstOffline.transport === "espnow" ? (worstOffline.raw.age_s || 0) : Math.round((worstOffline.raw.age_ms || 0) / 1000)),
      })
    : "";

  var battNodes = ndList.filter(function (n) { return n.transport === "espnow" && n.raw.percent != null; });
  if (battNodes.length) {
    var worst = battNodes.sort(function (a, b) { return a.raw.percent - b.raw.percent; })[0];
    setEl("nd-kpi-batt", worst.raw.percent);
    document.getElementById("nd-kpi-batt-d").textContent = worst.name;
    document.querySelector("#nd-kpis .kpi:nth-child(3)").style.display = "";
  } else {
    document.querySelector("#nd-kpis .kpi:nth-child(3)").style.display = "none";
  }

  if (ndEspnowData && ndEspnowData.stats && ndEspnowData.stats.frames != null) {
    setEl("nd-kpi-frames", ndEspnowData.stats.frames);
    var unknown = ndEspnowData.stats.unknown_node || 0;
    document.getElementById("nd-kpi-frames-d").textContent =
      unknown ? unknown + " " + ndT("nodes.diagUnknown") : "";
    document.querySelector("#nd-kpis .kpi:nth-child(4)").style.display = "";
  } else {
    document.querySelector("#nd-kpis .kpi:nth-child(4)").style.display = "none";
  }
}

// ── Row rendering ────────────────────────────────────────────────────────

function ndRowMatches(n) {
  if (ndFilterState === "espnow" && n.transport !== "espnow") return false;
  if (ndFilterState === "wifi" && n.transport !== "wifi") return false;
  if (ndFilterState === "problem" && !ndIsProblem(n)) return false;
  if (ndSearchQuery && n.name.toLowerCase().indexOf(ndSearchQuery) === -1) return false;
  return true;
}

function ndRowHtml(n) {
  var isOpen = ndOpenKey === n.key;
  var statusBadge = '<span class="badge ' + (n.online ? "ok" : "err") + '">' +
    esc(n.online ? ndT("nodes.online") : ndT("nodes.offline")) + "</span>";
  var transportBadge = n.transport === "espnow"
    ? '<span class="badge dim"><span data-icon="radio"></span> ESP-NOW</span>'
    : '<span class="badge dim"><span data-icon="wifi"></span> WiFi</span>';

  var battCell, seenCell, rssiCell, clockCell;
  if (n.transport === "espnow") {
    var r = n.raw;
    // r.days is null whenever the battery model refuses to estimate (too
    // little history, a flat trace, a slope inside the noise) — that's not
    // zero, so it only ever appears as a title, never printed as "0 d".
    var daysTitle = r.days == null ? "" : ' title="' + esc(ndT("nodes.daysLeft", { d: r.days >= 365 ? "365+" : r.days })) + '"';
    battCell = r.percent == null ? '<span class="mono" style="font-size:12px;color:var(--text-4)">—</span>'
      : '<span class="badge ' + ndBattClass(r) + ' mono"' + daysTitle + '>' + r.percent + "%" +
        (r.mv != null ? " · " + (r.mv / 1000).toFixed(2) + "V" : "") + "</span>";
    seenCell = '<span class="mono" style="font-size:12px">' + (r.seen ? ndFmtAge(r.age_s) : ndT("nodes.never")) + "</span>";
    rssiCell = '<span class="mono" style="font-size:12px' + (r.rssi == null ? ";color:var(--text-4)" : "") + '">' +
      (r.rssi == null ? "—" : r.rssi + " dBm") + "</span>";
    var skew = ndSkew(r);
    clockCell = '<span class="badge ' + skew.cls + ' mono" title="' + esc(skew.title) + '">' + esc(skew.text) + "</span>";
  } else {
    var age = n.raw.age_ms == null ? null : Math.round(n.raw.age_ms / 1000);
    battCell = '<span class="mono" style="font-size:12px;color:var(--text-4)">—</span>';
    seenCell = '<span class="mono" style="font-size:12px">' + (age == null ? ndT("nodes.never") : ndFmtAge(age)) + "</span>";
    rssiCell = '<span class="mono" style="font-size:12px;color:var(--text-4)">—</span>';
    clockCell = '<span class="badge dim mono">—</span>';
  }

  var chevIcon = isOpen ? '<span data-icon="chevron-right" style="display:grid;transform:rotate(90deg)"></span>'
                         : '<span data-icon="chevron-right"></span>';

  var html =
    '<div class="node-row' + (isOpen ? " node-row-open" : "") + '" data-click="nodesToggleRow" data-args=\'["' + n.key + '"]\'>' +
      '<div class="node-row-name">' +
        '<div style="display:flex;align-items:center;gap:8px;flex-wrap:wrap"><strong style="font-size:13px">' + esc(n.name) + "</strong>" + statusBadge + ndCfgBadge(n) + "</div>" +
        '<span class="mono" style="font-size:11px;color:var(--text-3)">' +
          (n.transport === "espnow" ? "node " + n.raw.node_id + " · " + esc(n.raw.mac) : esc(n.raw.metrics && n.raw.metrics.length ? n.raw.metrics.map(function (m) { return m.metric; }).join(", ") : "")) +
        "</span>" +
      "</div>" +
      '<span class="node-row-col">' + transportBadge + "</span>" +
      '<span class="node-row-col">' + battCell + "</span>" +
      '<span class="node-row-col">' + seenCell + "</span>" +
      '<span class="node-row-col">' + rssiCell + "</span>" +
      '<span class="node-row-col">' + clockCell + "</span>" +
      '<div class="node-row-mobile-status">' + transportBadge + battCell + seenCell + "</div>" +
      '<button class="btn-mini node-row-chev" aria-label="' + esc(ndT("common.open")) + '">' + chevIcon + "</button>" +
    "</div>";

  if (isOpen) html += ndDetailHtml(n);
  return html;
}

// The config-state badge beside a row's name. It comes from the `cfg` the
// status payloads carry per node (§7), so the list can say which nodes are
// behind without one config read per row. A node the collector holds no
// config file for has no `cfg` at all and gets no badge — not "applied".
function ndCfgBadge(n) {
  var c = n.cfg;
  if (!c || !c.status) {
    var loaded = ndCfg[n.cfgKey];
    if (n.transport === "wifi" && loaded && loaded.data && !loaded.data.reported) {
      return '<span class="badge dim">' + esc(ndT("nodes.cfgNotReported")) + "</span>";
    }
    return "";
  }
  if (c.status === "applied") {
    return '<span class="badge ok" title="rev ' + esc(c.rev) + '">' + esc(ndT("nodes.cfgApplied")) + "</span>";
  }
  if (c.status === "pending") {
    return '<span class="badge warn">' + esc(ndT("nodes.cfgPending", { from: c.applied_rev || 0, to: c.rev })) + "</span>";
  }
  if (c.status === "rejected") {
    // The reason rides along in the status payload when the firmware sends
    // it; if not, the panel's own read has it once the row has been opened.
    var err = c.error || (ndCfg[n.cfgKey] && ndCfg[n.cfgKey].data && ndCfg[n.cfgKey].data.error);
    var why = err && err.reason;
    var text = why ? ndT("nodes.cfgRejectedWhy", { reason: why }) : ndT("nodes.cfgRejected");
    return '<span class="badge err nd-cfg-badge" title="' + esc(text) + '"><span>' + esc(text) + "</span></span>";
  }
  return "";
}

function ndDetailHtml(n) {
  var top = "";
  if (n.transport === "wifi") {
    var metrics = (n.raw.metrics || []).map(function (m) {
      var val = typeof m.value === "number" ? m.value.toFixed(1) : m.value;
      return '<span class="badge dim mono">' + esc(m.metric) + ": <strong>" + esc(val) + "</strong> " + esc(m.unit || "") + "</span>";
    }).join(" ");
    top = '<div style="display:flex;gap:6px;flex-wrap:wrap;margin-bottom:12px">' +
      (metrics || '<span class="hint">' + esc(ndT("nodes.noNodesReported")) + "</span>") + "</div>";
    return '<div class="node-row-detail">' + top + ndCfgPanelHtml(n) + "</div>";
  }

  var r = n.raw;
  if (ndIsProblem(n)) {
    var skew = ndSkew(r);
    top = '<div class="alert alert-error" style="margin:0 0 12px">' +
      "<strong>" + esc(ndT("nodes.clockAlert")) + "</strong>" +
      '<span class="hint" style="display:block;margin-top:3px">' +
      esc(ndT("nodes.clockAlertSub", { skew: skew.text, dropped: r.dropped || 0 })) + "</span></div>";
  }

  return '<div class="node-row-detail">' +
    top +
    ndCfgPanelHtml(n) +
    '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:12px">' +
      '<button class="btn" data-click="nodesPair"><span data-icon="link"></span> ' + esc(ndT("nodes.rePair")) + "</button>" +
      '<span style="flex:1"></span>' +
      '<button class="btn warn" data-click="nodesForget" data-args="' + esc(JSON.stringify([r.node_id, n.name])) + '">' +
        '<span data-icon="trash"></span> ' + esc(ndT("nodes.forget")) + "</button>" +
    "</div>" +
  "</div>";
}

// ── The node settings panel (docs/NODE_CONFIG.md §1, §7) ───────────────────
//
// Loaded lazily: GET /api/nodes/config?key=… the first time a row is opened,
// kept in ndCfg until the next Refresh or Save. Everything the panel shows
// is drawn from ndDoc(key) — the unsaved working copy when there is one,
// otherwise the collector's desired config — for the same reason the drafts
// comment further down gives: the inputs are rebuilt on every re-render and
// must never be where an edit lives.

var ndCfg = {};      // cfgKey → {loading, err, data: GET /api/nodes/config payload}
var ndSaveErr = {};  // cfgKey → {field, reason} from the last refused save

// Metrics each sensor type costs against the node's per-report budget (§1.1).
// ds18b20 costs its probe count. pressure_sea is counted even when altitude
// is 0, because the firmware counts it.
var ND_TYPE_METRICS = { bmx280: 4, bme688: 5, bh1750: 1, sds011: 2, pulse: 2 };
// A sleeping ESP-NOW node is off between wakes, so nothing can count pulses
// or run a fan for 30 s — the validator refuses these with sleep on.
var ND_SLEEP_UNSAFE = { sds011: 1, pulse: 1 };
var ND_I2C_TYPES = { bmx280: 1, bme688: 1, bh1750: 1 };
var ND_ALL_TYPES = ["bmx280", "bme688", "ds18b20", "bh1750", "sds011", "pulse"];
// Write-only (§0.5): a GET returns "", and a POST of "" keeps the stored one.
// So an empty box is never an edit, and a typed one always is.
var ND_SECRETS = { "net.pass": 1, "net.token": 1, "net.basic_pass": 1 };
// Reported by the node, never sent back to it.
var ND_READONLY = { rev: 1, local: 1, transport: 1, hw: 1, fw: 1, "net.next": 1, "link.next_ssid": 1 };

function ndClone(o) { return o == null ? o : JSON.parse(JSON.stringify(o)); }
function ndSlug(s) { return String(s).replace(/[^A-Za-z0-9_-]/g, "_"); }

// "sensors[1].pin" → ["sensors", "1", "pin"]
function ndPathTokens(path) { return path.replace(/\[(\d+)\]/g, ".$1").split("."); }
function ndGet(o, path) {
  var t = ndPathTokens(path);
  for (var i = 0; i < t.length; i++) { if (o == null) return undefined; o = o[t[i]]; }
  return o;
}
function ndSet(o, path, v) {
  var t = ndPathTokens(path);
  for (var i = 0; i < t.length - 1; i++) {
    if (o[t[i]] == null || typeof o[t[i]] !== "object") o[t[i]] = {};
    o = o[t[i]];
  }
  o[t[t.length - 1]] = v;
}

function ndLoadCfg(cfgKey) {
  if (ndCfg[cfgKey] && (ndCfg[cfgKey].loading || ndCfg[cfgKey].data)) return;
  ndCfg[cfgKey] = { loading: true };
  fetchWithTimeout("/api/nodes/config?key=" + encodeURIComponent(cfgKey), {}, 15000)
    .then(function (r) {
      if (!r.ok) throw r.status;
      return r.json();
    })
    .then(function (d) { ndCfg[cfgKey] = { data: d }; })
    .catch(function (e) { ndCfg[cfgKey] = { err: typeof e === "number" ? e : "net" }; })
    .then(function () { ndRenderRows(); ndDirtyRefresh(); });
}

function nodesCfgRetry(cfgKey) {
  delete ndCfg[cfgKey];
  ndRenderRows();
}

// The config the panel renders: the unsaved working copy if the reader has
// touched this node, otherwise what the collector wants the node to run.
function ndDoc(cfgKey) {
  if (ndDrafts[cfgKey]) return ndDrafts[cfgKey].doc;
  var c = ndCfg[cfgKey];
  return c && c.data ? c.data.desired : null;
}

function ndDraftFor(cfgKey) {
  if (!ndDrafts[cfgKey]) {
    ndDrafts[cfgKey] = { doc: ndClone(ndCfg[cfgKey].data.desired) || {}, raw: {}, bad: {} };
  }
  return ndDrafts[cfgKey];
}

function ndCaps(cfgKey) {
  var c = ndCfg[cfgKey];
  return (c && c.data && c.data.caps) || {};
}

function ndBoard(cfgKey) {
  var boards = ndCaps(cfgKey).boards || [];
  var doc = ndDoc(cfgKey) || {};
  for (var i = 0; i < boards.length; i++) if (boards[i].id === doc.board) return boards[i];
  return null;
}

// A pin box takes the label printed on the board ("D6") or the GPIO number
// ("12", "GPIO12"); the config document only ever holds the GPIO. Parsing and
// naming are www/js/pins.js's, the same as on the Collector's own pin pages;
// the risk lists stay the node's own (`caps`), below.
function ndParsePin(cfgKey, text) {
  var r = Pins.parse({ board: ndBoard(cfgKey) }, text);
  if (r.empty || r.bad) return { gpio: null, bad: true };
  return { gpio: r.gpio, bad: false };
}

// How a GPIO is shown back: the board's own label when it has one for it.
function ndPinLabel(cfgKey, gpio) {
  return Pins.text({ board: ndBoard(cfgKey) }, gpio);
}

// The line under a pin box: what it resolved to, and whether the chip can
// actually use it — from the same lists the validator uses, sent in `caps`.
function ndPinHint(cfgKey, path) {
  var d = ndDrafts[cfgKey];
  var caps = ndCaps(cfgKey);
  if (d && d.bad[path]) {
    var empty = String(d.raw[path] == null ? "" : d.raw[path]).trim() === "";
    return { cls: "err", text: ndT(empty ? "nodes.cfgPinRequired" : "nodes.cfgPinUnknown") };
  }
  var g = ndGet(ndDoc(cfgKey), path);
  if (g == null) return { cls: "err", text: ndT("nodes.cfgPinRequired") };
  if ((caps.forbidden_pins || []).indexOf(g) !== -1) {
    return { cls: "err", text: ndT("nodes.cfgPinForbidden", { g: g }) };
  }
  var warn = caps.warn_pins && caps.warn_pins[String(g)];
  if (warn) return { cls: "warn", text: "GPIO" + g + " — " + warn };
  return { cls: "", text: "GPIO" + g };
}

function ndMetricCount(sensors) {
  var n = 0;
  (sensors || []).forEach(function (s) {
    if (s.type === "ds18b20") n += Math.max(1, parseInt(s.count, 10) || 1);
    else n += ND_TYPE_METRICS[s.type] || 0;
  });
  return n;
}

function ndSleeping(doc) { return !!doc && doc.transport === "espnow" && doc.sleep !== false; }

// The error to show beside a field: this page's own refused save first (it
// is the newer news), then the node's rejection of the rev it was sent.
function ndFieldErr(cfgKey, path) {
  var s = ndSaveErr[cfgKey];
  if (s && s.field === path) return s.reason;
  var c = ndCfg[cfgKey];
  var e = c && c.data && c.data.status === "rejected" && c.data.error;
  if (e && e.field === path) return ndT("nodes.cfgNodeRejected", { reason: e.reason });
  return "";
}

// One field. `ctx` carries the node being drawn and collects every path it
// rendered, so an error naming a field the panel does not show still gets
// shown (at the top) rather than nowhere.
function ndField(ctx, path, label, kind, opt) {
  opt = opt || {};
  ctx.paths[path] = 1;
  var key = ctx.key;
  var id = "ndc-" + ndSlug(key) + "-" + ndSlug(path);
  var d = ndDrafts[key];
  var v = ndGet(ctx.doc, path);
  var raw = d && d.raw[path] !== undefined ? d.raw[path] : null;
  var err = ndFieldErr(key, path);
  var common = ' id="' + id + '" data-nd-key="' + esc(key) + '" data-nd-path="' + esc(path) + '" data-nd-kind="' + kind + '"' +
    (opt.rerender ? ' data-nd-rerender="1"' : "");
  var errHtml = err ? '<p class="nd-ferr" data-nd-err="' + esc(path) + '">' + esc(err) + "</p>" : "";
  var ctl;

  if (kind === "bool") {
    return '<div class="nd-bool"><label class="toggle-row" for="' + id + '"><span>' + esc(label) + "</span>" +
      '<span class="switch"><input type="checkbox"' + common + (v ? " checked" : "") + ' data-change="nodesCfgInput"><span></span></span></label>' +
      (opt.hint ? '<p class="hint" style="margin:0">' + esc(opt.hint) + "</p>" : "") + errHtml + "</div>";
  }
  if (kind === "select" || kind === "numsel" || kind === "stype") {
    ctl = '<select class="input' + (err ? " nd-bad" : "") + '"' + common + ' data-change="nodesCfgInput">' +
      opt.options.map(function (o) {
        return '<option value="' + esc(o[0]) + '"' + (String(o[0]) === String(v) ? " selected" : "") +
          (o[2] ? " disabled" : "") + ">" + esc(o[1]) + "</option>";
      }).join("") + "</select>";
  } else {
    var shown = raw !== null ? raw : kind === "pin" ? ndPinLabel(key, v) : kind === "secret" ? "" : (v == null ? "" : v);
    var type = kind === "secret" ? "password" : (kind === "int" || kind === "float") ? "number" : "text";
    ctl = '<input class="input mono' + (err || (d && d.bad[path]) ? " nd-bad" : "") + '" type="' + type + '"' + common +
      ' value="' + esc(shown) + '"' +
      (opt.min != null ? ' min="' + opt.min + '"' : "") + (opt.max != null ? ' max="' + opt.max + '"' : "") +
      (kind === "float" ? ' step="any"' : "") + (opt.maxlength ? ' maxlength="' + opt.maxlength + '"' : "") +
      (opt.placeholder ? ' placeholder="' + esc(opt.placeholder) + '"' : "") +
      (kind === "pin" ? ' autocapitalize="characters" spellcheck="false"' : "") +
      (kind === "secret" ? ' autocomplete="new-password"' : "") +
      ' data-input="nodesCfgInput" data-change="nodesCfgInput">';
  }

  var hint = "";
  if (kind === "pin") {
    var h = ndPinHint(key, path);
    hint = '<span class="nd-pinhint ' + h.cls + '" data-nd-hint="' + esc(path) + '">' + esc(h.text) + "</span>";
  }
  return '<div class="field">' +
    '<label class="field-label" for="' + id + '">' + esc(label) + "</label>" + ctl + hint +
    (opt.hint ? '<p class="hint" style="margin:0">' + esc(opt.hint) + "</p>" : "") + errHtml +
    "</div>";
}

function ndSection(icon, title, body, extraHead, wide) {
  return '<div class="nd-sec' + (wide ? " nd-sec-wide" : "") + '"><div class="nd-sec-h"><span data-icon="' + icon + '"></span> <span>' + esc(title) + "</span>" +
    (extraHead || "") + '</div><div class="nd-sec-b">' + body + "</div></div>";
}

function ndAddrOptions(type) {
  if (type === "bh1750") return [[0x23, "0x23"], [0x5C, "0x5C"]];
  return [[0, ndT("nodes.cfgAddrProbe")], [0x76, "0x76"], [0x77, "0x77"]];
}

function ndSensorHtml(ctx, s, i) {
  var p = "sensors[" + i + "]";
  var sleeping = ndSleeping(ctx.doc);
  var types = (ctx.caps.sensor_types || ND_ALL_TYPES).map(function (t) {
    return [t, ndT("nodes.cfgType_" + t), sleeping && ND_SLEEP_UNSAFE[t] && t !== s.type];
  });
  var head = '<div class="nd-sensor-h">' +
    ndField(ctx, p + ".type", ndT("nodes.cfgSensorType"), "stype", { options: types, rerender: true }) +
    '<button type="button" class="btn-mini" data-click="nodesCfgRemoveSensor" data-args="' + esc(JSON.stringify([ctx.key, i])) + '"' +
      ' title="' + esc(ndT("nodes.cfgRemoveSensor")) + '" aria-label="' + esc(ndT("nodes.cfgRemoveSensor")) + '"><span data-icon="trash"></span></button>' +
    "</div>";
  var f = "";
  switch (s.type) {
    case "bmx280":
    case "bme688":
    case "bh1750":
      f = ndField(ctx, p + ".addr", ndT("nodes.cfgAddr"), "numsel", { options: ndAddrOptions(s.type) });
      break;
    case "ds18b20":
      f = ndField(ctx, p + ".pin", ndT("nodes.cfgPin"), "pin") +
        ndField(ctx, p + ".count", ndT("nodes.cfgProbeCount"), "int", { min: 1, max: 8 }) +
        ndField(ctx, p + ".metric", ndT("nodes.cfgProbeMetric"), "str", { maxlength: 10, placeholder: "probe_temp" });
      break;
    case "sds011":
      f = ndField(ctx, p + ".rx", ndT("nodes.cfgRx"), "pin") + ndField(ctx, p + ".tx", ndT("nodes.cfgTx"), "pin");
      break;
    case "pulse":
      f = ndField(ctx, p + ".pin", ndT("nodes.cfgPin"), "pin") +
        ndField(ctx, p + ".mode", ndT("nodes.cfgPulseMode"), "select", {
          options: [["rain", ndT("nodes.cfgPulseRain")], ["flow", ndT("nodes.cfgPulseFlow")]], rerender: true }) +
        ndField(ctx, p + ".per_pulse", ndT(s.mode === "flow" ? "nodes.cfgPerPulseFlow" : "nodes.cfgPerPulseRain"), "float") +
        ndField(ctx, p + ".debounce_us", ndT("nodes.cfgDebounce"), "int", { min: 0 });
      break;
  }
  var unsafe = sleeping && ND_SLEEP_UNSAFE[s.type]
    ? '<p class="nd-ferr">' + esc(ndT("nodes.cfgSleepUnsafe")) + "</p>" : "";
  return '<div class="nd-sensor">' + head + (f ? '<div class="kd-fields">' + f + "</div>" : "") + unsafe + "</div>";
}

function ndBudgetHtml(cfgKey) {
  var doc = ndDoc(cfgKey) || {};
  var max = ndCaps(cfgKey).max_metrics || 8;
  var used = ndMetricCount(doc.sensors);
  return '<span class="badge ' + (used > max ? "err" : "dim") + ' mono" data-nd-budget="' + esc(cfgKey) + '" title="' +
    esc(ndT("nodes.cfgBudgetTitle")) + '">' + esc(ndT("nodes.cfgBudget", { n: used, max: max })) + "</span>";
}

function ndCfgPanelHtml(n) {
  var key = n.cfgKey;
  var c = ndCfg[key];
  if (!c || c.loading) {
    return '<p class="hint">' + esc(ndT("nodes.cfgLoading")) + "</p>";
  }
  if (c.err === 404) {
    return '<p class="hint">' + esc(ndT("nodes.cfgUnsupported")) + "</p>";
  }
  if (c.err) {
    return '<div class="alert alert-error" style="margin:0">' + esc(ndT("nodes.cfgLoadFailed")) +
      ' <button type="button" class="btn" style="margin-left:8px" data-click="nodesCfgRetry" data-args="' + esc(JSON.stringify([key])) + '">' +
      '<span data-icon="refresh-cw"></span> ' + esc(ndT("nodes.cfgRetry")) + "</button></div>";
  }
  var data = c.data;
  var doc = ndDoc(key);
  // §7: a WiFi node the collector has never had a report from has no config
  // to edit — not an empty one. Its board, pins and firmware are unknown.
  if (!doc || (n.transport === "wifi" && !data.reported)) {
    return '<div class="alert alert-warning nd-waiting" style="margin:0"><strong>' + esc(ndT("nodes.cfgWaitingTitle")) + "</strong>" +
      '<span class="hint" style="display:block;margin-top:3px">' + esc(ndT("nodes.cfgWaitingSub")) + "</span></div>";
  }

  var ctx = { key: key, doc: doc, caps: data.caps || {}, paths: {} };
  var isEn = n.transport === "espnow";
  var sensors = doc.sensors || [];
  var sleeping = ndSleeping(doc);

  var facts = [doc.hw, doc.fw ? "fw " + doc.fw : "",
    "rev " + (data.applied_rev || 0) + (doc.rev && doc.rev !== data.applied_rev ? " → " + doc.rev : "")]
    .filter(Boolean).join(" · ");

  var ident = '<div class="kd-fields">' +
    ndField(ctx, "name", ndT("nodes.nameLabel"), "str", { maxlength: 16,
      hint: ndT(isEn ? "nodes.cfgNameHintEn" : "nodes.cfgNameHintWifi") }) +
    "</div>" + '<span class="nd-ro mono">' + esc(facts) + "</span>";

  var timing = '<div class="kd-fields">' +
    ndField(ctx, "interval_s", ndT("nodes.intervalLabel"), "int", { min: 10, max: 65535 }) +
    "</div>" +
    (isEn ? ndField(ctx, "sleep", ndT("nodes.cfgSleep"), "bool", { rerender: true, hint: ndT("nodes.cfgSleepHint") }) : "");

  var boards = (ctx.caps.boards || []).map(function (b) { return [b.id, b.name]; });
  var anyI2c = sensors.some(function (s) { return ND_I2C_TYPES[s.type]; });
  var board = '<div class="kd-fields">' +
    (boards.length ? ndField(ctx, "board", ndT("nodes.cfgBoard"), "numsel", { options: boards, rerender: true }) : "") +
    ndField(ctx, "i2c.sda", "I²C SDA", "pin") +
    ndField(ctx, "i2c.scl", "I²C SCL", "pin") + "</div>" +
    '<p class="hint" style="margin:0">' + esc(ndT(anyI2c ? "nodes.cfgI2cHint" : "nodes.cfgI2cUnused")) + "</p>";

  // Add-a-sensor: the types the validator would refuse are listed but
  // disabled, with the reason under them, rather than silently missing.
  var max = ctx.caps.max_sensors || 8;
  var used = {};
  sensors.forEach(function (s) { used[s.type] = 1; });
  var addOpts = (ctx.caps.sensor_types || ND_ALL_TYPES).map(function (t) {
    var off = (sleeping && ND_SLEEP_UNSAFE[t]) || (used[t] && t !== "ds18b20") ||
      (t === "bmx280" && used.bme688) || (t === "bme688" && used.bmx280);
    return '<option value="' + esc(t) + '"' + (off ? " disabled" : "") + ">" + esc(ndT("nodes.cfgType_" + t)) + "</option>";
  }).join("");
  var full = sensors.length >= max;
  var sens = '<div class="kd-fields" style="max-width:340px">' +
    ndField(ctx, "altitude_m", ndT("nodes.cfgAltitude"), "float", { hint: ndT("nodes.cfgAltitudeHint") }) + "</div>" +
    (sensors.length ? sensors.map(function (s, i) { return ndSensorHtml(ctx, s, i); }).join("")
      : '<p class="hint" style="margin:0">' + esc(ndT("nodes.cfgNoSensors")) + "</p>") +
    '<div class="nd-add">' +
      '<select class="input" id="ndc-' + ndSlug(key) + '-addtype" aria-label="' + esc(ndT("nodes.cfgSensorType")) + '"' + (full ? " disabled" : "") + ">" + addOpts + "</select>" +
      '<button type="button" class="btn" data-click="nodesCfgAddSensor" data-args="' + esc(JSON.stringify([key])) + '"' + (full ? " disabled" : "") + ">" +
        '<span data-icon="plus"></span> ' + esc(ndT("nodes.cfgAddSensor")) + "</button>" +
    "</div>" +
    (sleeping ? '<p class="hint" style="margin:0">' + esc(ndT("nodes.cfgSleepUnsafeHint")) + "</p>" : "");

  var html = ndSection("tag", ndT("nodes.cfgIdentity"), ident) +
    ndSection("timer", ndT("nodes.cfgTiming"), timing) +
    ndSection("cpu", ndT("nodes.cfgBoardI2c"), board);

  if (!isEn) {
    var net = doc.net || {};
    var next = net.next && net.next.ssid
      ? '<p class="nd-ro" style="margin:0">' + esc(ndT("nodes.cfgNetNext", { ssid: net.next.ssid })) + "</p>" : "";
    html += ndSection("wifi", ndT("nodes.cfgNetwork"),
      '<div class="kd-fields">' +
        ndField(ctx, "net.ssid", "SSID", "str", { maxlength: 32 }) +
        ndField(ctx, "net.pass", ndT("nodes.cfgPass"), "secret", { placeholder: ndT(net.pass_set ? "nodes.cfgSecretKeep" : "nodes.cfgSecretNone") }) +
        ndField(ctx, "net.host", ndT("nodes.cfgHost"), "str", { maxlength: 64 }) +
        ndField(ctx, "net.port", ndT("nodes.cfgPort"), "int", { min: 1, max: 65535 }) +
        ndField(ctx, "net.token", ndT("nodes.cfgToken"), "secret", { placeholder: ndT(net.token_set ? "nodes.cfgSecretKeep" : "nodes.cfgSecretNone") }) +
        ndField(ctx, "net.basic_user", ndT("nodes.cfgBasicUser"), "str", { maxlength: 32 }) +
        ndField(ctx, "net.basic_pass", ndT("nodes.cfgBasicPass"), "secret", { placeholder: ndT(net.basic_pass_set ? "nodes.cfgSecretKeep" : "nodes.cfgSecretNone") }) +
      "</div>" + next +
      '<p class="hint" style="margin:0">' + esc(ndT("nodes.cfgNetHint")) + "</p>");
  } else {
    var link = doc.link || {};
    html += ndSection("radio", ndT("nodes.cfgLinkBatt"),
      '<div class="kd-fields">' +
        ndField(ctx, "link.ack_window_ms", ndT("nodes.cfgAckWindow"), "int", { min: 1, max: 1000 }) +
        ndField(ctx, "link.rescan_fails", ndT("nodes.cfgRescanFails"), "int", { min: 1, max: 255 }) +
        ndField(ctx, "link.rescan_min_s", ndT("nodes.cfgRescanMin"), "int", { min: 300, max: 604800 }) +
      "</div>" +
      (link.next_ssid ? '<p class="nd-ro" style="margin:0">' + esc(ndT("nodes.cfgNetNext", { ssid: link.next_ssid })) + "</p>" : "") +
      '<div class="kd-fields">' +
        ndField(ctx, "batt.pin", ndT("nodes.cfgBattPin"), "pin") +
        ndField(ctx, "batt.divider", ndT("nodes.cfgBattDivider"), "float") +
        ndField(ctx, "batt.trim", ndT("nodes.cfgBattTrim"), "float") +
      "</div>" +
      '<p class="hint" style="margin:0">' + esc(ndT("nodes.cfgBattHint")) + "</p>");
  }
  // Last, and full width: on a wide screen the four small sections above
  // pair up two by two, and the sensor list gets the room its rows need.
  html += ndSection("thermometer", ndT("nodes.cfgSensors"), sens, ndBudgetHtml(key), true);

  // Errors that name no field drawn above still have to be seen somewhere.
  var alerts = "";
  var se = ndSaveErr[key];
  if (se && !ctx.paths[se.field]) {
    alerts += '<div class="alert alert-error" style="margin:0">' + esc((se.field ? se.field + ": " : "") + se.reason) + "</div>";
  }
  if (data.status === "rejected" && data.error) {
    alerts += '<div class="alert alert-error" style="margin:0"><strong>' +
      esc(ndT("nodes.cfgRejectedTitle", { rev: (data.desired && data.desired.rev) || "" })) + "</strong>" +
      '<span class="hint" style="display:block;margin-top:3px">' +
      esc((data.error.field ? data.error.field + ": " : "") + data.error.reason) + "</span></div>";
  } else if (data.status === "pending") {
    alerts += '<p class="hint" style="margin:0">' + esc(ndT(isEn ? "nodes.cfgPendingHintEn" : "nodes.cfgPendingHintWifi")) + "</p>";
  }
  if (isEn && !data.reported) {
    alerts += '<p class="hint" style="margin:0">' + esc(ndT("nodes.cfgEnNotReported")) + "</p>";
  }
  return '<div class="nd-panel" data-nd-key="' + esc(key) + '">' + alerts + html + "</div>";
}

// Refresh what depends on a value without rebuilding the panel (which would
// take the caret out of the box being typed in): pin lines, the budget.
function ndLive(cfgKey) {
  var panel = document.querySelector('.nd-panel[data-nd-key="' + cfgKey + '"]');
  if (!panel) return;
  panel.querySelectorAll("[data-nd-hint]").forEach(function (el) {
    var h = ndPinHint(cfgKey, el.getAttribute("data-nd-hint"));
    el.className = "nd-pinhint " + h.cls;
    el.textContent = h.text;
  });
  var b = panel.querySelector("[data-nd-budget]");
  if (b) b.outerHTML = ndBudgetHtml(cfgKey);
}

function nodesCfgInput() {
  var el = (this && this.nodeType === 1) ? this : null;
  if (!el) return;
  var key = el.getAttribute("data-nd-key");
  var path = el.getAttribute("data-nd-path");
  var kind = el.getAttribute("data-nd-kind");
  var c = ndCfg[key];
  if (!key || !path || !c || !c.data || !c.data.desired) return;
  var d = ndDraftFor(key);
  var v, bad = false;

  if (kind === "bool") {
    v = el.checked;
  } else if (kind === "int") {
    v = el.value.trim() === "" ? NaN : Number(el.value);
    bad = !isFinite(v) || Math.floor(v) !== v;
  } else if (kind === "float") {
    v = el.value.trim() === "" ? NaN : Number(el.value);
    bad = !isFinite(v);
  } else if (kind === "numsel") {
    v = Number(el.value);
  } else if (kind === "pin") {
    var r = ndParsePin(key, el.value);
    v = r.gpio; bad = r.bad;
  } else {
    v = el.value;
  }

  if (kind === "stype") {
    // A different type is a different sensor: its fields start over.
    var i = parseInt(ndPathTokens(path)[1], 10);
    d.doc.sensors[i] = ndSensorDefaults(v);
    ndForgetSensorRaw(d, i);
    ndMarkEmptyPins(d, i);
  } else {
    if (kind !== "bool" && kind !== "numsel" && kind !== "select") d.raw[path] = el.value;
    if (bad) d.bad[path] = true;
    else { delete d.bad[path]; ndSet(d.doc, path, v); }
  }
  // Another board prints other labels on the same GPIOs: the pins stay the
  // GPIOs they are, and are shown again in the new board's labels.
  if (path === "board") {
    for (var rp in d.raw) if (!d.bad[rp] && /(pin|rx|tx|sda|scl)$/.test(rp)) delete d.raw[rp];
  }
  // An edit is the answer to the error that was about this field.
  if (ndSaveErr[key] && ndSaveErr[key].field === path) {
    delete ndSaveErr[key];
    var f = el.closest(".field");
    var e = f && f.querySelector(".nd-ferr[data-nd-err]");
    if (e) e.remove();
    el.classList.remove("nd-bad");
  }
  if (el.hasAttribute("data-nd-rerender")) {
    ndRenderRows();
  } else {
    if (kind !== "bool" && kind !== "select" && kind !== "numsel") el.classList.toggle("nd-bad", !!d.bad[path]);
    ndLive(key);
  }
  ndDirtyRefresh();
}

// Typed-in text for a sensor's fields is keyed by its index; once the entry
// is replaced by one of another type, those boxes are about nothing.
function ndForgetSensorRaw(d, index) {
  [d.raw, d.bad].forEach(function (m) {
    for (var p in m) {
      var mm = /^sensors\[(\d+)\]/.exec(p);
      if (mm && parseInt(mm[1], 10) === index) delete m[p];
    }
  });
}

// A sensor's pin starts empty, and empty is not a pin: mark it, so Save says
// so here rather than the node refusing the whole config a report later.
function ndMarkEmptyPins(d, i) {
  ["pin", "rx", "tx"].forEach(function (f) {
    if (f in d.doc.sensors[i] && d.doc.sensors[i][f] == null) {
      d.raw["sensors[" + i + "]." + f] = "";
      d.bad["sensors[" + i + "]." + f] = true;
    }
  });
}

function ndSensorDefaults(type) {
  switch (type) {
    case "bmx280": case "bme688": return { type: type, addr: 0 };
    case "bh1750": return { type: type, addr: 0x23 };
    case "ds18b20": return { type: type, pin: null, count: 1, metric: "probe_temp" };
    case "sds011": return { type: type, rx: null, tx: null };
    case "pulse": return { type: type, pin: null, mode: "rain", per_pulse: 0.2794, debounce_us: 5000 };
  }
  return { type: type };
}

function nodesCfgAddSensor(cfgKey) {
  var sel = document.getElementById("ndc-" + ndSlug(cfgKey) + "-addtype");
  if (!sel || !sel.value || !ndCfg[cfgKey] || !ndCfg[cfgKey].data) return;
  var opt = sel.options[sel.selectedIndex];
  if (opt && opt.disabled) return;
  var d = ndDraftFor(cfgKey);
  d.doc.sensors = d.doc.sensors || [];
  d.doc.sensors.push(ndSensorDefaults(sel.value));
  ndMarkEmptyPins(d, d.doc.sensors.length - 1);
  delete ndSaveErr[cfgKey];
  ndRenderRows();
  ndDirtyRefresh();
}

function nodesCfgRemoveSensor(cfgKey, index) {
  if (!ndCfg[cfgKey] || !ndCfg[cfgKey].data) return;
  var d = ndDraftFor(cfgKey);
  if (!d.doc.sensors || index >= d.doc.sensors.length) return;
  d.doc.sensors.splice(index, 1);
  // Everything after the removed entry moved up one; its typed text did not.
  var shifted = { raw: {}, bad: {} };
  ["raw", "bad"].forEach(function (m) {
    for (var p in d[m]) {
      var mm = /^sensors\[(\d+)\](.*)$/.exec(p);
      if (!mm) { shifted[m][p] = d[m][p]; continue; }
      var i = parseInt(mm[1], 10);
      if (i < index) shifted[m][p] = d[m][p];
      else if (i > index) shifted[m]["sensors[" + (i - 1) + "]" + mm[2]] = d[m][p];
    }
  });
  d.raw = shifted.raw;
  d.bad = shifted.bad;
  delete ndSaveErr[cfgKey];
  ndRenderRows();
  ndDirtyRefresh();
}

function ndRenderRows() {
  var box = document.getElementById("nd-rows");
  if (!box) return;
  var visible = ndList.filter(ndRowMatches);

  // The open row's settings load the first time it is drawn open.
  var open = null;
  for (var i = 0; i < ndList.length; i++) if (ndList[i].key === ndOpenKey) open = ndList[i];
  if (open && !ndCfg[open.cfgKey]) ndLoadCfg(open.cfgKey);

  // The list is rebuilt wholesale — by a toggle, a filter, the pairing
  // poll's tick, a select that reshapes the panel. Whatever box the reader
  // was in gets focus (and its caret) back, so a tick mid-word is invisible.
  var act = document.activeElement;
  var focusId = act && act.id && box.contains(act) ? act.id : null;
  var selStart = null, selEnd = null;
  if (focusId) { try { selStart = act.selectionStart; selEnd = act.selectionEnd; } catch (e) { /* not a text box */ } }

  if (!ndList.length) {
    var bothAbsent = ndEspnowAvailable === false && ndRemoteAvailable === false;
    var msg = bothAbsent ? ndT("nodes.notInBuildBoth")
      : ndEspnowAvailable === false ? ndT("nodes.notInBuildEspnow")
      : ndRemoteAvailable === false ? ndT("nodes.notInBuildRemote")
      : ndT("nodes.noNodesPaired");
    box.innerHTML = '<p class="hint" style="margin:14px">' + esc(msg) + "</p>";
  } else if (!visible.length) {
    box.innerHTML = '<p class="hint" style="margin:14px">' + esc(ndT("nodes.noMatch")) + "</p>";
  } else {
    box.innerHTML = visible.map(ndRowHtml).join("");
  }
  if (window.Icons && Icons.swap) Icons.swap(box);
  if (focusId) {
    var back = document.getElementById(focusId);
    if (back) {
      back.focus();
      if (selStart != null) { try { back.setSelectionRange(selStart, selEnd); } catch (e) { /* number inputs */ } }
    }
  }
}

// ── Diagnostics (ESP-NOW only) ──────────────────────────────────────────

function ndRenderDiag() {
  var card = document.getElementById("nd-card-diag");
  if (!card) return;
  var s = ndEspnowData && ndEspnowData.stats;
  if (!s) { card.style.display = "none"; return; }
  card.style.display = "";

  var rows = [
    ["frames", "diagAccepted"], ["unknown_node", "diagUnknown"], ["malformed", "diagMalformed"],
    ["discover_bad_sig", "diagBadSig"], ["replayed", "diagReplayed"], ["ring_full", "diagRingFull"],
    ["history_collapsed", "diagHistCollapsed"], ["history_no_clock", "diagHistNoClock"],
    ["acks", "diagAcks"], ["discover_seen", "diagDiscoverSeen"], ["paired", "diagPaired"],
  ];
  var badKeys = { unknown_node: 1, malformed: 1, discover_bad_sig: 1, ring_full: 1, history_collapsed: 1, history_no_clock: 1 };
  var problems = 0;
  var html = rows.map(function (row) {
    var k = row[0], v = s[k] == null ? 0 : s[k];
    var bad = v > 0 && badKeys[k];
    if (bad) problems++;
    return '<div class="set-card" style="cursor:default"><div class="set-card-t"' +
      (bad ? ' style="color:var(--warn)"' : "") + ">" + v + '</div><div class="set-card-d">' +
      esc(ndT("nodes." + row[1])) + "</div></div>";
  }).join("");
  document.getElementById("nd-stats").innerHTML = html;
  var badge = document.getElementById("nd-diag-badge");
  badge.className = "badge " + (problems ? "warn" : "dim") + " mono";
  badge.textContent = problems
    ? ndT(problems === 1 ? "nodes.diagIssue" : "nodes.diagIssues", { n: problems })
    : ndT("nodes.diagOk");
}

// ── Pairing / add / forget (ESP-NOW mutating actions — immediate, not part
//    of the savebar; same real endpoints the old espnow.js page used) ──────

function nodesPair() {
  var sel = document.getElementById("nd-pair-secs");
  var secs = sel ? sel.value : "120";
  var body = new URLSearchParams();
  body.set("seconds", secs);
  return postWithCsrf("/api/espnow/pair", { body: body, headers: { "Content-Type": "application/x-www-form-urlencoded" } })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) {
        ndMsg(ndT("nodes.pairOpenedFor", { s: d.seconds }), "ok");
        nodesRefresh();
        ndStopPairPoll();
        // The whole point of the window is that a node joins DURING it, so
        // the tick has to redraw the LIST, not just the open/closed badge —
        // otherwise the node that was just adopted stays invisible until
        // something else refreshes. Not nodesRefresh() though: that clears
        // ndDrafts, which would throw away an unsaved edit every 5 s for as
        // long as the window is open.
        var deadline = Date.now() + ((parseInt(d.seconds, 10) || 120) + 15) * 1000;
        ndPairTimer = setInterval(function () {
          // Left the page, or the window outlived its own length (which a
          // run of failed ticks would otherwise poll through forever).
          if (currentPage !== "settings_nodes" || Date.now() > deadline) {
            ndStopPairPoll();
            return;
          }
          ndFetchEspnow().then(function (data) {
            // A failed tick is not "the device now has no nodes" — keep the
            // last good payload rather than blanking the list and the KPIs.
            if (!data) return;
            ndEspnowData = data;
            ndMerge();
            ndRenderKpis();
            ndRenderRows();
            ndRenderPairState();
            ndRenderDiag();
            ndDirtyRefresh();
            if (!data.pairing) ndStopPairPoll();
          });
        }, 5000);
      } else {
        ndMsg(ndT("nodes.pairFailed"), "err");
      }
    })
    .catch(function () { ndMsg(ndT("nodes.pairFailed"), "err"); });
}

function ndStopPairPoll() {
  if (ndPairTimer) { clearInterval(ndPairTimer); ndPairTimer = null; }
}

function ndRenderPairState() {
  var el = document.getElementById("nd-pair-state");
  if (!el) return;
  var open = !!(ndEspnowData && ndEspnowData.pairing);
  el.textContent = ndT(open ? "nodes.pairOpen" : "nodes.pairClosed");
  el.className = "badge " + (open ? "acc pulse" : "dim");
}

function nodesAddManual() {
  var mac = (document.getElementById("nd-add-mac") || {}).value || "";
  var nodeId = (document.getElementById("nd-add-id") || {}).value || "";
  var label = (document.getElementById("nd-add-label") || {}).value || "";
  var iv = (document.getElementById("nd-add-iv") || {}).value || "60";
  if (!mac || !nodeId) { ndMsg(ndT("nodes.addRequired"), "err"); return; }

  var body = new URLSearchParams();
  body.set("mac", mac); body.set("node_id", nodeId);
  if (label) body.set("label", label);
  body.set("interval", iv);
  return postWithCsrf("/api/espnow/add", { body: body, headers: { "Content-Type": "application/x-www-form-urlencoded" } })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) { ndMsg(ndT("nodes.addOk"), "ok"); nodesRefresh(); }
      else { ndMsg((d && d.error) || ndT("nodes.addFailed"), "err"); }
    })
    .catch(function () { ndMsg(ndT("nodes.addFailed"), "err"); });
}

function nodesForget(nodeId, label) {
  if (!confirm(ndT("nodes.forgetConfirm", { name: label }))) return;
  var body = new URLSearchParams();
  body.set("node_id", nodeId);
  return postWithCsrf("/api/espnow/forget", { body: body, headers: { "Content-Type": "application/x-www-form-urlencoded" } })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) { ndMsg(ndT("nodes.forgetOk"), "ok"); nodesRefresh(); }
      else { ndMsg((d && d.error) || ndT("nodes.forgetFailed"), "err"); }
    })
    .catch(function () { ndMsg(ndT("nodes.forgetFailed"), "err"); });
}

// ── Savebar: every node's settings + the offline-threshold field ──────────
//
// DRAFTS LIVE OUTSIDE THE ROW DOM. ndRenderRows() replaces #nd-rows wholesale
// on every filter, search and row toggle, and only the OPEN row carries input
// elements at all — so reading the unsaved state back out of the inputs (which
// is what this did first) lost it the moment the reader collapsed the row they
// had just typed into, or opened a second one, while the savebar stayed up
// claiming otherwise and Save then found nothing to send and wrote nothing.
// That is the same "reported success, stored nothing" failure the e-ink page's
// one-Save redesign exists to end, so it is kept in a model instead: the
// inputs render FROM ndDrafts, and never define it.
//
// A draft is a whole working copy of the node's desired config, taken on the
// first edit, plus what was literally typed into each box (`raw` — "D6" is
// what the reader wrote, 12 is what the document holds) and which boxes do
// not parse (`bad`). Save sends only what differs from the desired config: a
// partial document is a valid edit (§1), and it keeps a save of one field
// from racing a change the node reported in the meantime to every other.
var ndDrafts = {};          // cfgKey → {doc, raw, bad} — unsaved edits
var ndOfflineDraft = null;  // unsaved offline threshold, or null when untouched

// What differs between the desired config and the working copy, as the
// partial document to POST — or null when nothing does. Objects recurse (a
// changed i2c.sda sends {"i2c":{"sda":…}}); the sensor list is one value and
// goes whole, since "entry 2 changed" has no meaning once one was removed.
function ndPartial(base, doc, prefix) {
  var out = {}, any = false;
  for (var k in doc) {
    var path = prefix ? prefix + "." + k : k;
    if (ND_READONLY[path] || /_set$/.test(k)) continue;
    var v = doc[k], b = base ? base[k] : undefined;
    if (ND_SECRETS[path]) {
      if (v) { out[k] = v; any = true; }
    } else if (Array.isArray(v)) {
      if (JSON.stringify(v) !== JSON.stringify(b)) { out[k] = v; any = true; }
    } else if (v && typeof v === "object") {
      var sub = ndPartial(b, v, path);
      if (sub) { out[k] = sub; any = true; }
    } else if (v !== b) {
      out[k] = v; any = true;
    }
  }
  return any ? out : null;
}

function ndHasBad(d) {
  for (var p in d.bad) return true;
  return false;
}

// Every node whose draft actually differs from the collector's desired
// config, or holds a box that does not parse (which is unsaved work too).
function ndDirtyNodes() {
  var out = [];
  for (var key in ndDrafts) {
    var c = ndCfg[key];
    if (!c || !c.data || !ndRowByCfgKey(key)) continue;  // forgotten since the edit
    var d = ndDrafts[key];
    var partial = ndPartial(c.data.desired, d.doc, "");
    if (partial || ndHasBad(d)) out.push({ key: key, config: partial, bad: ndHasBad(d) });
  }
  return out;
}

function ndOfflineDirty() {
  if (ndOfflineDraft === null) return false;
  var base = ndEspnowData && ndEspnowData.offline_intervals;
  return base !== undefined && String(ndOfflineDraft) !== String(base);
}

// The offline threshold (the one page-level setting). Node fields go through
// nodesCfgInput, which knows their paths.
function nodesFieldInput() {
  var el = (this && this.nodeType === 1) ? this : null;
  if (el && el.id === "nd-offline-iv") ndOfflineDraft = el.value;
  ndDirtyRefresh();
}

function ndDirtyRefresh() {
  var bar = document.getElementById("nd-savebar");
  var out = document.getElementById("nd-dirty");
  if (!bar || !out) return;

  var dirtyNodes = ndDirtyNodes().length;
  var dirtySettings = ndOfflineDirty() ? 1 : 0;
  if (!dirtyNodes && !dirtySettings) { bar.hidden = true; return; }

  var parts = [];
  if (dirtyNodes) parts.push(ndT(dirtyNodes === 1 ? "nodes.unsavedNode" : "nodes.unsavedNodeP", { n: dirtyNodes }));
  if (dirtySettings) parts.push(ndT(dirtySettings === 1 ? "nodes.unsavedSetting" : "nodes.unsavedSettingP", { n: dirtySettings }));
  out.innerHTML = "<strong>" + esc(ndT("common.unsaved")) + "</strong> " + esc(parts.join(", "));
  bar.hidden = false;
}

function nodesSave() {
  var dirty = ndDirtyNodes();

  // A box that does not parse cannot be sent as anything — say which node
  // and open it, rather than sending the rest and leaving it looking saved.
  var blocked = dirty.filter(function (x) { return x.bad; })[0];
  if (blocked) {
    var br = ndRowByCfgKey(blocked.key);
    if (br) ndOpenKey = br.key;
    ndRenderRows();
    ndMsg(ndT("nodes.cfgFixFirst", { name: br ? br.name : blocked.key }), "err");
    return;
  }

  var calls = [];
  dirty.forEach(function (n) {
    calls.push(postWithCsrf("/api/nodes/config", {
      body: JSON.stringify({ key: n.key, config: n.config }),
      headers: { "Content-Type": "application/json" },
    }).then(function (r) {
      return r.json().catch(function () { return null; })
        .then(function (d) { return { key: n.key, status: r.status, body: d }; });
    }));
  });
  if (ndOfflineDirty()) {
    var cbody = new URLSearchParams();
    cbody.set("offline_intervals", ndOfflineDraft);
    calls.push(postWithCsrf("/api/espnow/config", { body: cbody, headers: { "Content-Type": "application/x-www-form-urlencoded" } })
      .then(function (r) { return r.json(); })
      .then(function (d) { return { key: null, body: d }; }));
  }

  // Nothing to send means the bar was lying about something being unsaved —
  // put it right rather than leaving a Save button that does nothing.
  if (!calls.length) { ndDirtyRefresh(); return; }

  Promise.all(calls).then(function (results) {
    var failed = [];
    results.forEach(function (res) {
      var ok = res.body && res.body.ok;
      if (res.key === null) { if (ok) ndOfflineDraft = null; else failed.push(res); return; }
      if (ok) {
        // Saved: that node's draft is now the desired config, so drop both
        // and let the row read the new rev back when it is next drawn open.
        delete ndDrafts[res.key];
        delete ndCfg[res.key];
        delete ndSaveErr[res.key];
      } else {
        // Refused (400 {field, reason}): keep the draft, put the reason
        // beside the field it names.
        ndSaveErr[res.key] = {
          field: (res.body && res.body.field) || "",
          reason: (res.body && (res.body.reason || res.body.error)) || ndT("nodes.saveFailed"),
        };
        failed.push(res);
      }
    });

    if (failed.length) {
      // Keep the drafts that failed: nodesRefresh() would clear them, so the
      // edit the reader was trying to save is exactly what would be thrown
      // away by the failure to save it. The savebar stays up with the work
      // still in it, which is what makes Save worth pressing again.
      var f = failed[0];
      var fr = f.key ? ndRowByCfgKey(f.key) : null;
      if (fr) ndOpenKey = fr.key;
      var why = f.key ? ndSaveErr[f.key].reason : ((f.body && f.body.error) || ndT("nodes.saveFailed"));
      ndMsg(fr ? fr.name + ": " + why : why, "err");
      ndReloadLists();
      return;
    }
    ndMsg(ndT("nodes.savedNextReport"), "ok");
    nodesRefresh();
  }).catch(function () { ndMsg(ndT("nodes.saveFailed"), "err"); });
}

function nodesDiscard() {
  ndDrafts = {};
  ndOfflineDraft = null;
  nodesRefresh();
}

// ── Filter / search / row toggle ────────────────────────────────────────

function nodesFilter(kind) {
  ndFilterState = kind;
  document.querySelectorAll("#nd-filter button").forEach(function (b) {
    b.classList.toggle("active", b.getAttribute("data-args") === '["' + kind + '"]');
  });
  ndRenderRows();
}

function nodesSearch() {
  ndSearchQuery = (this.value || "").trim().toLowerCase();
  ndRenderRows();
}

function nodesToggleRow(key) {
  ndOpenKey = ndOpenKey === key ? null : key;
  ndRenderRows();
}

// ── Network handover banner (docs/NODE_CONFIG.md §4) ───────────────────────
//
// When the collector's own WiFi moves to another network, it does not switch
// at once: it first hands every node the new network as `next`, and switches
// when every node that is online has confirmed it (or when told to). This is
// the banner that follows that — on this page and on the Network page, where
// the move is started (settings.js netSaveForm). Rendered into every
// [data-nd-ho] slot, so the page that is not showing holds a hidden copy.
//
// The collector's GET /api/nodes/handover names nodes by config key
// ("w:balcony", "e:3"); ndList turns those into the names on the rows.

var ndHo = null;          // last GET /api/nodes/handover payload, or null
var ndHoPhase = "";       // "" | "switching" — the collector is on its way out
var ndHoTimer = null;
var ndHoPollMs = 4000;

function ndHoName(k) {
  var r = ndRowByCfgKey(k);
  if (r) return r.name;
  var m = /^([ew]):(.*)$/.exec(k || "");
  return m ? (m[1] === "e" ? ndT("nodes.hoNodeN", { n: m[2] }) : m[2]) : String(k);
}

function ndHoMsg(text, kind) {
  var id = currentPage === "settings_network" ? "net-msg" : "nd-msg";
  showMsg(id, "<div class='alert alert-" + (kind === "ok" ? "success" : "error") + "'>" + esc(text) + "</div>", true);
}

function ndHoChips(list, cls) {
  if (!list.length) return '<span class="hint" style="margin:0">—</span>';
  return list.map(function (k) { return '<span class="badge ' + cls + '">' + esc(ndHoName(k)) + "</span>"; }).join(" ");
}

function ndHoHtml() {
  if (ndHoPhase === "switching") {
    return '<div class="card nd-ho"><div class="card-head"><div class="card-title"><span data-icon="wifi"></span> ' +
      esc(ndT("nodes.hoSwitchingTitle", { ssid: (ndHo && ndHo.ssid) || "" })) + "</div></div>" +
      '<div class="card-body"><p class="hint" style="margin:0">' + esc(ndT("nodes.hoSwitchingSub", { ssid: (ndHo && ndHo.ssid) || "" })) + "</p></div></div>";
  }
  if (!ndHo || !ndHo.active) return "";
  var ready = ndHo.ready || [], pending = ndHo.pending || [], offline = ndHo.offline || [];
  // Offline nodes do not hold the switch up (§4.4), so they are not in the
  // count the reader is watching fill.
  var total = ready.length + pending.length;
  var pct = total ? Math.round(ready.length * 100 / total) : 100;
  return '<div class="card nd-ho">' +
    '<div class="card-head" style="gap:10px;flex-wrap:wrap">' +
      '<div class="card-title"><span data-icon="wifi"></span> <span class="nd-ho-title">' +
        esc(ndT("nodes.hoTitle", { ssid: ndHo.ssid || "", n: ready.length, total: total })) + "</span></div>" +
      '<span class="badge acc pulse">' + esc(ndT("nodes.hoActive")) + "</span>" +
    "</div>" +
    '<div class="card-body" style="display:flex;flex-direction:column;gap:10px">' +
      '<div class="nd-ho-bar" role="progressbar" aria-valuemin="0" aria-valuemax="' + total + '" aria-valuenow="' + ready.length + '"><span style="width:' + pct + '%"></span></div>' +
      '<div class="nd-ho-list"><span class="nd-ho-l">' + esc(ndT("nodes.hoReady")) + "</span>" + ndHoChips(ready, "ok") + "</div>" +
      '<div class="nd-ho-list"><span class="nd-ho-l">' + esc(ndT("nodes.hoPending")) + "</span>" + ndHoChips(pending, "warn") + "</div>" +
      '<div class="nd-ho-list"><span class="nd-ho-l">' + esc(ndT("nodes.hoOffline")) + "</span>" + ndHoChips(offline, "dim") + "</div>" +
      '<p class="hint" style="margin:0">' + esc(ndT("nodes.hoHint")) + "</p>" +
      '<div style="display:flex;gap:8px;flex-wrap:wrap">' +
        '<button type="button" class="btn primary" data-click="nodesHoSwitch"><span data-icon="arrow-right"></span> ' + esc(ndT("nodes.hoSwitchNow")) + "</button>" +
        '<button type="button" class="btn" data-click="nodesHoCancel"><span data-icon="x"></span> ' + esc(ndT("nodes.hoCancel")) + "</button>" +
      "</div>" +
    "</div></div>";
}

function ndHoRender() {
  var html = ndHoHtml();
  document.querySelectorAll("[data-nd-ho]").forEach(function (slot) {
    slot.innerHTML = html;
    slot.hidden = !html;
    if (html && window.Icons && Icons.swap) Icons.swap(slot);
  });
}

function ndHoStop() {
  if (ndHoTimer) { clearTimeout(ndHoTimer); ndHoTimer = null; }
}

function ndHoSchedule() {
  ndHoStop();
  ndHoTimer = setTimeout(ndHoPoll, ndHoPollMs);
}

function ndHoPoll() {
  ndHoTimer = null;
  // Only the two pages that show the banner keep asking.
  if (currentPage !== "settings_nodes" && currentPage !== "settings_network") return;
  var before = ndHo;
  fetchWithTimeout("/api/nodes/handover", {}, 10000)
    .then(function (r) {
      if (r.status === 404) return null;   // a build without handover
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      if (d && d.active) {
        ndHo = d;
        ndHoPhase = "";
        ndHoSchedule();
      } else if (before && before.active && !(before.pending || []).length) {
        // Everyone was ready last time we asked, and now it is over: the
        // collector switched by itself, as §4.4 says it does.
        ndHoPhase = "switching";
      } else {
        ndHo = d;
      }
      ndHoRender();
    })
    .catch(function () {
      // The collector going away right after everyone was ready is it
      // switching, not a failure. Anything else: keep asking.
      if (before && before.active && !(before.pending || []).length) {
        ndHoPhase = "switching";
        ndHoRender();
      } else {
        ndHoSchedule();
      }
    });
}

// Called when a page that shows the banner opens: is a move under way?
function ndHoCheck() {
  ndHoStop();
  if (ndHoPhase === "switching") { ndHoRender(); return; }
  ndHoPoll();
}

function ndHoPost(body) {
  return postWithCsrf("/api/nodes/handover", {
    body: JSON.stringify(body),
    headers: { "Content-Type": "application/json" },
  }).then(function (r) {
    return r.json().catch(function () { return null; })
      .then(function (d) { return { status: r.status, body: d }; });
  });
}

// For settings.js: how many nodes would have to follow the collector.
// Resolves null when either list could not be fetched: "no nodes" and "could
// not find out" must not look alike, or a failed fetch reads as 0 nodes and
// the collector switches networks without handing them the new one.
function ndHoCountNodes() {
  return Promise.all([ndFetchEspnow(), ndFetchRemote()]).then(function (res) {
    ndEspnowData = res[0];
    ndRemoteData = res[1];
    ndMerge();
    if (ndEspnowAvailable === null || ndRemoteAvailable === null) return null;
    return ndList.length;
  });
}

// For settings.js: begin a handover. Resolves {status, body}; 404 means this
// firmware has no handover and the caller should save the old way.
function ndHoStart(ssid, pass, form) {
  var body = { action: "start", ssid: ssid, pass: pass };
  if (form) body.form = form;
  return ndHoPost(body).then(function (res) {
    if (res.body && res.body.ok !== false && res.status < 400) {
      ndHoPhase = "";
      ndHo = { active: true, ssid: ssid, ready: [], pending: [], offline: [] };
      ndHoRender();
      ndHoPoll();
    }
    return res;
  });
}

function nodesHoSwitch() {
  var pending = (ndHo && ndHo.pending) || [];
  if (pending.length && !confirm(ndT("nodes.hoSwitchConfirm", { n: pending.length }))) return;
  ndHoPost({ action: "switch" }).then(function (res) {
    if (res.status >= 400 || !res.body || res.body.ok === false) {
      ndHoMsg((res.body && (res.body.reason || res.body.error)) || ndT("nodes.hoFailed"), "err");
      return;
    }
    ndHoStop();
    ndHoPhase = "switching";
    ndHoRender();
  }).catch(function () { ndHoMsg(ndT("nodes.hoFailed"), "err"); });
}

function nodesHoCancel() {
  ndHoPost({ action: "cancel" }).then(function (res) {
    if (res.status >= 400 || !res.body || res.body.ok === false) {
      ndHoMsg((res.body && (res.body.reason || res.body.error)) || ndT("nodes.hoFailed"), "err");
      return;
    }
    ndHoStop();
    ndHo = null;
    ndHoPhase = "";
    ndHoRender();
    ndHoMsg(ndT("nodes.hoCancelled"), "ok");
  }).catch(function () { ndHoMsg(ndT("nodes.hoFailed"), "err"); });
}

// ── Boot / refresh ───────────────────────────────────────────────────────

function ndShowCards() {
  var pairingCard = document.getElementById("nd-card-pairing");
  var addManualCard = document.getElementById("nd-card-addmanual");
  var configCard = document.getElementById("nd-card-config");
  var wifiNoteCard = document.getElementById("nd-card-wifinote");
  var pairCta = document.getElementById("nd-pair-cta");
  if (pairingCard) pairingCard.style.display = ndEspnowAvailable ? "" : "none";
  if (addManualCard) addManualCard.style.display = ndEspnowAvailable ? "" : "none";
  if (configCard) configCard.style.display = ndEspnowAvailable ? "" : "none";
  if (wifiNoteCard) wifiNoteCard.style.display = ndRemoteAvailable ? "" : "none";
  if (pairCta) pairCta.style.display = ndEspnowAvailable ? "" : "none";
}

// Re-read both lists and redraw WITHOUT touching the drafts — after a save
// that the collector refused in part, where the refused edits must survive.
function ndReloadLists() {
  return Promise.all([ndFetchEspnow(), ndFetchRemote()]).then(function (res) {
    if (res[0]) ndEspnowData = res[0];
    if (res[1]) ndRemoteData = res[1];
    ndMerge();
    ndRenderKpis();
    ndRenderRows();
    ndRenderPairState();
    ndRenderDiag();
    ndDirtyRefresh();
  });
}

function nodesRefresh() {
  return Promise.all([ndFetchEspnow(), ndFetchRemote()]).then(function (res) {
    ndEspnowData = res[0];
    ndRemoteData = res[1];
    // A fresh read from the device is the new baseline, so nothing is
    // outstanding against it any more — including after a save, where these
    // drafts are exactly what was just written. The node settings are read
    // again too, lazily, the next time a row is drawn open.
    ndDrafts = {};
    ndOfflineDraft = null;
    ndCfg = {};
    ndSaveErr = {};
    ndMerge();
    ndRenderKpis();
    ndRenderRows();
    ndRenderPairState();
    ndRenderDiag();
    ndShowCards();

    var oiv = document.getElementById("nd-offline-iv");
    if (oiv && ndEspnowData && ndEspnowData.offline_intervals) oiv.value = ndEspnowData.offline_intervals;

    var bar = document.getElementById("nd-savebar");
    if (bar) bar.hidden = true;

    if (ndEspnowAvailable === false && ndRemoteAvailable === false) {
      ndMsg(ndT("nodes.notInBuildBoth"), "err");
    }
  }).catch(function () {
    ndMsg(ndT("nodes.couldNotRead"), "err");
  });
}

function nodesInit() {
  ndStopPairPoll();   // a poll left running from a previous visit
  ndOpenKey = null;
  ndFilterState = "all";
  ndSearchQuery = "";
  var search = document.getElementById("nd-search");
  if (search) search.value = "";
  nodesRefresh();
  ndHoCheck();
}

// Rows, KPIs and the diagnostics grid are built as strings with I18n.t()
// baked in at render time, so I18n.apply()'s data-i18n walk cannot reach
// them — without this a language switch left this page half-translated
// until it was navigated away from and back. Re-rendered from the cached
// payload rather than re-fetched, so unsaved drafts survive the switch.
document.addEventListener("i18n:change", function () {
  ndHoRender();
  if (!document.getElementById("nd-rows")) return;
  ndRenderKpis();
  ndRenderRows();
  ndRenderPairState();
  ndRenderDiag();
  ndDirtyRefresh();
});

registerHandlers({
  nodesRefresh: nodesRefresh,
  nodesPair: nodesPair,
  nodesAddManual: nodesAddManual,
  nodesForget: nodesForget,
  nodesFilter: nodesFilter,
  nodesSearch: nodesSearch,
  nodesToggleRow: nodesToggleRow,
  nodesFieldInput: nodesFieldInput,
  nodesCfgInput: nodesCfgInput,
  nodesCfgAddSensor: nodesCfgAddSensor,
  nodesCfgRemoveSensor: nodesCfgRemoveSensor,
  nodesCfgRetry: nodesCfgRetry,
  nodesSave: nodesSave,
  nodesDiscard: nodesDiscard,
  nodesHoSwitch: nodesHoSwitch,
  nodesHoCancel: nodesHoCancel,
});
