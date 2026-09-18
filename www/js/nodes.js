// ============================================================================
// nodes.js — the unified Nodes settings page (#settings_nodes)
//
// Merges what were two separate pages/endpoints into one list:
//   ESP-NOW battery nodes  — GET/POST /api/espnow/*  (transport "espnow")
//   WiFi remote nodes      — GET      /api/remote/status (transport "wifi",
//                             read-only: they're configured on their own
//                             captive portal, not from here)
// Either endpoint 404s on a build without its FEATURE_* flag; that is read
// as "not in this build" for that transport specifically, not an error.
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
var ndBase = "{}";            // dirty-tracking snapshot baseline
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
  var enNodes = (ndEspnowData && ndEspnowData.nodes) || [];
  for (var i = 0; i < enNodes.length; i++) {
    var n = enNodes[i];
    ndList.push({
      transport: "espnow",
      key: "en:" + n.node_id,
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
      name: m.id,
      online: !!m.online,
      raw: m,
    });
  }
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
    battCell = r.percent == null ? '<span class="mono" style="font-size:12px;color:var(--text-4)">—</span>'
      : '<span class="badge ' + ndBattClass(r) + ' mono">' + r.percent + "%" +
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
        '<div style="display:flex;align-items:center;gap:8px"><strong style="font-size:13px">' + esc(n.name) + "</strong>" + statusBadge + "</div>" +
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

function ndDetailHtml(n) {
  if (n.transport === "wifi") {
    var metrics = (n.raw.metrics || []).map(function (m) {
      var val = typeof m.value === "number" ? m.value.toFixed(1) : m.value;
      return '<span class="badge dim mono">' + esc(m.metric) + ": <strong>" + esc(val) + "</strong> " + esc(m.unit || "") + "</span>";
    }).join(" ");
    return '<div class="node-row-detail"><div style="display:flex;gap:6px;flex-wrap:wrap">' +
      (metrics || '<span class="hint">' + esc(ndT("nodes.noNodesReported")) + "</span>") + "</div></div>";
  }

  var r = n.raw;
  var alertHtml = "";
  if (ndIsProblem(n)) {
    var skew = ndSkew(r);
    alertHtml = '<div class="alert alert-error" style="margin:0 0 12px">' +
      "<strong>" + esc(ndT("nodes.clockAlert")) + "</strong>" +
      '<span class="hint" style="display:block;margin-top:3px">' +
      esc(ndT("nodes.clockAlertSub", { skew: skew.text, dropped: r.dropped || 0 })) + "</span></div>";
  }

  return '<div class="node-row-detail">' +
    alertHtml +
    '<div class="kd-fields" style="margin-bottom:12px">' +
      '<div class="field">' +
        '<label class="field-label" for="nd-label-' + r.node_id + '">' + esc(ndT("nodes.nameLabel")) + "</label>" +
        '<input class="input" id="nd-label-' + r.node_id + '" data-nd-node="' + r.node_id + '" data-nd-field="label" maxlength="16" value="' + esc(n.name) + '" data-change="nodesFieldInput" data-input="nodesFieldInput">' +
        '<p class="hint">' + esc(ndT("nodes.renameHint")) + "</p>" +
      "</div>" +
      '<div class="field">' +
        '<label class="field-label" for="nd-iv-' + r.node_id + '">' + esc(ndT("nodes.intervalLabel")) + "</label>" +
        '<input class="input" type="number" id="nd-iv-' + r.node_id + '" data-nd-node="' + r.node_id + '" data-nd-field="interval" min="10" max="65535" value="' + r.interval + '" data-change="nodesFieldInput" data-input="nodesFieldInput">' +
      "</div>" +
    "</div>" +
    '<div style="display:flex;gap:8px;flex-wrap:wrap">' +
      '<button class="btn" data-click="nodesPair"><span data-icon="link"></span> ' + esc(ndT("nodes.rePair")) + "</button>" +
      '<span style="flex:1"></span>' +
      '<button class="btn warn" data-click="nodesForget" data-args="' + esc(JSON.stringify([r.node_id, n.name])) + '">' +
        '<span data-icon="trash"></span> ' + esc(ndT("nodes.forget")) + "</button>" +
    "</div>" +
  "</div>";
}

function ndRenderRows() {
  var box = document.getElementById("nd-rows");
  if (!box) return;
  var visible = ndList.filter(ndRowMatches);

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
  badge.textContent = problems ? problems + " " + (problems === 1 ? "issue" : "issues") : "ok";
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
        if (ndPairTimer) clearInterval(ndPairTimer);
        ndPairTimer = setInterval(function () {
          ndFetchEspnow().then(function (data) {
            ndEspnowData = data;
            ndRenderPairState();
            if (!data || !data.pairing) { clearInterval(ndPairTimer); ndPairTimer = null; }
          });
        }, 5000);
      } else {
        ndMsg(ndT("nodes.pairFailed"), "err");
      }
    })
    .catch(function () { ndMsg(ndT("nodes.pairFailed"), "err"); });
}

function ndRenderPairState() {
  var el = document.getElementById("nd-pair-state");
  if (!el) return;
  var open = !!(ndEspnowData && ndEspnowData.pairing);
  el.textContent = open ? "open" : "closed";
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

// ── Savebar: label/interval per ESP-NOW node + the offline-threshold field ─

function ndSnapshot() {
  var nodes = {};
  document.querySelectorAll("[data-nd-node]").forEach(function (el) {
    var id = el.getAttribute("data-nd-node");
    nodes[id] = nodes[id] || {};
    nodes[id][el.getAttribute("data-nd-field")] = el.value;
  });
  var oiv = document.getElementById("nd-offline-iv");
  return JSON.stringify({ nodes: nodes, offlineIv: oiv ? oiv.value : null });
}

function nodesFieldInput() {
  ndDirtyRefresh();
}

function ndDirtyRefresh() {
  var bar = document.getElementById("nd-savebar");
  var out = document.getElementById("nd-dirty");
  if (!bar || !out) return;
  var now = ndSnapshot();
  if (now === ndBase) { bar.hidden = true; return; }

  var a = JSON.parse(ndBase), b = JSON.parse(now);
  var dirtyNodes = 0, dirtySettings = 0;
  for (var id in b.nodes) {
    var av = a.nodes[id] || {}, bv = b.nodes[id];
    var changed = false;
    for (var f in bv) { if (String(av[f]) !== String(bv[f])) changed = true; }
    if (changed) dirtyNodes++;
  }
  if (String(a.offlineIv) !== String(b.offlineIv)) dirtySettings++;

  var parts = [];
  if (dirtyNodes) parts.push(ndT(dirtyNodes === 1 ? "nodes.unsavedNode" : "nodes.unsavedNodeP", { n: dirtyNodes }));
  if (dirtySettings) parts.push(ndT(dirtySettings === 1 ? "nodes.unsavedSetting" : "nodes.unsavedSettingP", { n: dirtySettings }));
  out.innerHTML = "<strong>" + esc(ndT("common.unsaved")) + "</strong> " + esc(parts.join(", "));
  bar.hidden = false;
}

function nodesSave() {
  var a = JSON.parse(ndBase), b = JSON.parse(ndSnapshot());
  var calls = [];

  for (var id in b.nodes) {
    var av = a.nodes[id] || {}, bv = b.nodes[id];
    var changed = Object.keys(bv).some(function (f) { return String(av[f]) !== String(bv[f]); });
    if (!changed) continue;
    var body = new URLSearchParams();
    body.set("node_id", id);
    if (bv.label !== undefined) body.set("label", bv.label);
    if (bv.interval !== undefined) body.set("interval", bv.interval);
    calls.push(postWithCsrf("/api/espnow/node", { body: body, headers: { "Content-Type": "application/x-www-form-urlencoded" } })
      .then(function (r) { return r.json(); }));
  }
  if (String(a.offlineIv) !== String(b.offlineIv) && b.offlineIv !== null) {
    var cbody = new URLSearchParams();
    cbody.set("offline_intervals", b.offlineIv);
    calls.push(postWithCsrf("/api/espnow/config", { body: cbody, headers: { "Content-Type": "application/x-www-form-urlencoded" } })
      .then(function (r) { return r.json(); }));
  }

  if (!calls.length) return;
  Promise.all(calls).then(function (results) {
    var failed = results.filter(function (r) { return !r || !r.ok; });
    if (failed.length) {
      ndMsg((failed[0] && failed[0].error) || ndT("nodes.saveFailed"), "err");
    } else {
      ndMsg(ndT("nodes.savedNextReport"), "ok");
    }
    nodesRefresh();
  }).catch(function () { ndMsg(ndT("nodes.saveFailed"), "err"); });
}

function nodesDiscard() {
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

// ── Boot / refresh ───────────────────────────────────────────────────────

function nodesRefresh() {
  return Promise.all([ndFetchEspnow(), ndFetchRemote()]).then(function (res) {
    ndEspnowData = res[0];
    ndRemoteData = res[1];
    ndMerge();
    ndRenderKpis();
    ndRenderRows();
    ndRenderPairState();
    ndRenderDiag();

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

    var oiv = document.getElementById("nd-offline-iv");
    if (oiv && ndEspnowData && ndEspnowData.offline_intervals) oiv.value = ndEspnowData.offline_intervals;

    ndBase = ndSnapshot();
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
  ndOpenKey = null;
  ndFilterState = "all";
  ndSearchQuery = "";
  var search = document.getElementById("nd-search");
  if (search) search.value = "";
  nodesRefresh();
}

registerHandlers({
  nodesRefresh: nodesRefresh,
  nodesPair: nodesPair,
  nodesAddManual: nodesAddManual,
  nodesForget: nodesForget,
  nodesFilter: nodesFilter,
  nodesSearch: nodesSearch,
  nodesToggleRow: nodesToggleRow,
  nodesFieldInput: nodesFieldInput,
  nodesSave: nodesSave,
  nodesDiscard: nodesDiscard,
});
