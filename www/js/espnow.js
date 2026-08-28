// ============================================================================
// espnow.js — the Battery nodes settings page (#settings_espnow)
//
// Reads GET /api/espnow/status and drives the four mutating routes. The page
// is only reachable on a build with FEATURE_ESPNOW_INGEST compiled in; on any
// other build the routes are not registered and the fetch 404s, which is what
// the "not in this build" message below is for. That is deliberate: a settings
// card that silently shows nothing is indistinguishable from a broken one.
//
// TWO VALUES ARE NULL RATHER THAN ZERO, and the page has to keep them apart.
//   rssi   unavailable on Arduino core 2.x — IDF 4.4 gives the receive
//          callback no signal information at all.
//   days   null whenever the battery model refuses to answer: too little
//          history, a flat trace, or a slope inside the noise.
// Both render as an em dash. Printing 0 dBm or 0 days would be a number
// somebody believes.
// ============================================================================

var _enTimer = null;

// The page's own message line, in the markup every other settings page uses.
// showMsg() and .alert-* are what core.js already has; a second convention
// here would be a second thing to keep in step with the stylesheet.
function enMsg(text, kind) {
  showMsg("en-msg",
          "<div class='alert alert-" + (kind === "ok" ? "success" : "error") + "'>" +
          enEsc(text) + "</div>", true);
}

function enFmtAge(s) {
  if (s < 60) return s + "s";
  var m = Math.round(s / 60);
  if (m < 60) return m + "m";
  var h = Math.round(m / 60);
  if (h < 24) return h + "h";
  return Math.round(h / 24) + "d";
}

function enEsc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
  });
}

// Battery badge class. Matches the firmware's own rule so the page and the
// device never disagree about what counts as a warning: the collector sends
// `warn`, and this only chooses how to colour it.
function enBattClass(node) {
  if (node.percent == null) return "dim";
  if (node.warn) return "err";
  if (node.percent <= 25) return "warn";
  return "ok";
}

function enRenderNodes(d) {
  var box = document.getElementById("en-nodes");
  var count = document.getElementById("en-node-count");
  if (!box) return;

  var nodes = (d && d.nodes) || [];
  if (count) {
    count.textContent = nodes.length ? (nodes.length + (nodes.length === 1 ? " node" : " nodes")) : "none";
    count.className = "badge " + (d && d.offline ? "warn" : "dim");
  }

  if (!nodes.length) {
    box.innerHTML =
      '<p class="hint">No nodes paired yet. Open the pairing window above, then power the node.</p>';
    return;
  }

  var html = "";
  for (var i = 0; i < nodes.length; i++) {
    var n = nodes[i];
    var battTxt = n.percent == null ? "—" : n.percent + "%";
    var daysTxt = n.days == null ? "—" : (n.days >= 365 ? "365+ d" : n.days + " d");
    var mvTxt = n.mv == null ? "" : " · " + (n.mv / 1000).toFixed(2) + " V";
    var rssiTxt = n.rssi == null ? "—" : n.rssi + " dBm";
    var seenTxt = n.seen ? enFmtAge(n.age_s) + " ago" : "never";

    html +=
      '<div class="card" style="margin-bottom:10px">' +
        '<div class="card-head">' +
          '<div class="card-title">' + enEsc(n.id) + "</div>" +
          '<span class="badge ' + (n.offline ? "err" : "ok") + '">' +
            (n.offline ? "offline" : "online") +
          "</span>" +
        "</div>" +
        '<div class="card-body">' +
          '<div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px">' +
            '<span class="badge ' + enBattClass(n) + '">' +
              '<span data-icon="battery"></span> ' + battTxt + enEsc(mvTxt) +
            "</span>" +
            '<span class="badge dim">left: ' + daysTxt + "</span>" +
            '<span class="badge dim">seen: ' + seenTxt + "</span>" +
            '<span class="badge dim">rssi: ' + rssiTxt + "</span>" +
            '<span class="badge dim">' + n.frames + " frames</span>" +
            (n.dropped ? '<span class="badge warn">' + n.dropped + " dropped</span>" : "") +
          "</div>" +
          '<p class="hint" style="margin-top:0">node ' + n.node_id + " · " + enEsc(n.mac) + "</p>" +
          '<div style="display:flex;gap:8px;flex-wrap:wrap;align-items:flex-end">' +
            '<div style="flex:1;min-width:150px">' +
              '<label class="field-label" for="en-label-' + n.node_id + '">Name</label>' +
              '<input class="input" id="en-label-' + n.node_id + '" maxlength="16" value="' +
                enEsc(n.id) + '">' +
            "</div>" +
            '<div style="flex:0 0 130px">' +
              '<label class="field-label" for="en-iv-' + n.node_id + '">Interval (s)</label>' +
              '<input class="input" id="en-iv-' + n.node_id + '" type="number" min="10" max="65535" value="' +
                n.interval + '">' +
            "</div>" +
            '<button class="btn" data-click="espnowSaveNode" data-args=\'[' + n.node_id + ']\'>' +
              '<span data-icon="save"></span> Save</button>' +
            '<button class="btn warn" data-click="espnowForget" data-args=\'[' + n.node_id + ',"' +
              enEsc(n.id) + '"]\'>' +
              '<span data-icon="trash"></span> Forget</button>' +
          "</div>" +
          // Renaming is not free and the page says so where the field is,
          // not in a manual: the label becomes SensorReading::sensorId, so
          // readings already stored keep the old name.
          '<p class="hint">The name is the sensor id these readings arrive under. ' +
            "Renaming affects new readings only — history stays under the old name.</p>" +
        "</div>" +
      "</div>";
  }
  box.innerHTML = html;
  if (window.Icons && Icons.swap) Icons.swap(box);
}

function enRenderStats(d) {
  var box = document.getElementById("en-stats");
  if (!box) return;
  var s = (d && d.stats) || {};

  // Ordered by what a person hunting a silent node should read first.
  var rows = [
    ["frames",            "accepted"],
    ["unknown_node",      "from an unknown node"],
    ["malformed",         "malformed"],
    ["discover_bad_sig",  "bad pairing signature"],
    ["replayed",          "duplicate / stale"],
    ["ring_full",         "arrived faster than drained"],
    ["history_collapsed", "backfill dropped"],
    ["history_no_clock",  "backfill with no clock"],
    ["acks",              "acks sent"],
    ["discover_seen",     "pairing requests seen"],
    ["paired",            "nodes paired"]
  ];

  var html = "";
  for (var i = 0; i < rows.length; i++) {
    var k = rows[i][0], label = rows[i][1];
    var v = s[k] == null ? 0 : s[k];
    // Only the failure counters get coloured, and only when non-zero: a page
    // where everything is orange teaches people to stop reading it.
    var bad = v > 0 && (k === "unknown_node" || k === "malformed" ||
                        k === "discover_bad_sig" || k === "ring_full" ||
                        k === "history_collapsed" || k === "history_no_clock");
    html +=
      '<div class="set-card" style="cursor:default">' +
        '<div class="set-card-t"' + (bad ? ' style="color:var(--warn)"' : "") + ">" + v + "</div>" +
        '<div class="set-card-d">' + label + "</div>" +
      "</div>";
  }
  box.innerHTML = html;
}

function enRenderPairing(d) {
  var el = document.getElementById("en-pair-state");
  if (!el) return;
  var open = !!(d && d.pairing);
  el.textContent = open ? "open" : "closed";
  el.className = "badge " + (open ? "acc pulse" : "dim");
}

function espnowRefresh() {
  return fetchWithTimeout("/api/espnow/status", {}, 15000)
    .then(function (r) {
      if (r.status === 404) throw new Error("not-in-build");
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      enRenderPairing(d);
      enRenderNodes(d);
      enRenderStats(d);
      return d;
    })
    .catch(function (e) {
      var box = document.getElementById("en-nodes");
      if (!box) return;
      if (e && e.message === "not-in-build") {
        // Said plainly rather than shown as an empty list. A page that looks
        // like "no nodes" when the feature is not compiled in sends people
        // looking for a hardware fault that does not exist.
        box.innerHTML =
          '<p class="hint">This firmware was not built with <code>FEATURE_ESPNOW_INGEST</code>, ' +
          "so there is no ESP-NOW radio to report on. Enable it in the deploy tool's " +
          "build features and reflash.</p>";
      } else {
        box.innerHTML = '<p class="hint">Could not read the node status.</p>';
      }
    });
}

function espnowPair() {
  var sel = document.getElementById("en-pair-secs");
  var secs = sel ? sel.value : "120";
  var body = new URLSearchParams();
  body.set("seconds", secs);

  return postWithCsrf("/api/espnow/pair", {
    body: body,
    headers: { "Content-Type": "application/x-www-form-urlencoded" }
  })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) {
        enMsg("Pairing window open for " + d.seconds + " s — power the node now.", "ok");
        espnowRefresh();
        // Poll while the window is open so the badge closes itself rather
        // than sitting on "open" until somebody reloads the page.
        if (_enTimer) clearInterval(_enTimer);
        _enTimer = setInterval(function () {
          espnowRefresh().then(function (s) {
            if (!s || !s.pairing) { clearInterval(_enTimer); _enTimer = null; }
          });
        }, 5000);
      } else {
        enMsg("Could not open the pairing window.", "err");
      }
    })
    .catch(function () {
      enMsg("Could not open the pairing window.", "err");
    });
}

function espnowSaveNode(nodeId) {
  var label = document.getElementById("en-label-" + nodeId);
  var iv = document.getElementById("en-iv-" + nodeId);
  var body = new URLSearchParams();
  body.set("node_id", nodeId);
  if (label) body.set("label", label.value);
  if (iv) body.set("interval", iv.value);

  return postWithCsrf("/api/espnow/node", {
    body: body,
    headers: { "Content-Type": "application/x-www-form-urlencoded" }
  })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) {
        // The interval reaches the node on its NEXT report, in the ACK. Said
        // here because a node that is asleep will not have changed yet, and
        // "saved" on its own would imply it had.
        enMsg("Saved. The node picks up a new interval on its next report.", "ok");
        espnowRefresh();
      } else {
        enMsg((d && d.error) || "Save failed.", "err");
      }
    })
    .catch(function () { enMsg("Save failed.", "err"); });
}

function espnowForget(nodeId, label) {
  if (!confirm('Forget "' + label + '"?\n\nIts radio peer and slot are dropped. ' +
               "It will have to be paired again to report.")) return;

  var body = new URLSearchParams();
  body.set("node_id", nodeId);
  return postWithCsrf("/api/espnow/forget", {
    body: body,
    headers: { "Content-Type": "application/x-www-form-urlencoded" }
  })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) {
        enMsg("Node forgotten.", "ok");
        espnowRefresh();
      } else {
        enMsg((d && d.error) || "Could not forget the node.", "err");
      }
    })
    .catch(function () { enMsg("Could not forget the node.", "err"); });
}

function espnowInit() {
  espnowRefresh();
}

// Registered through the dispatcher's allowlist, not just hung on window.
// core.js routes every data-click through Handlers precisely so that injected
// markup cannot call an arbitrary global; putting these on window alone would
// leave the buttons dead.
registerHandlers({
  espnowRefresh: espnowRefresh,
  espnowPair: espnowPair,
  espnowSaveNode: espnowSaveNode,
  espnowForget: espnowForget,
});
