// ============================================================================
// remote.js — the Remote WiFi nodes settings page (#settings_remote)
//
// Reads GET /api/remote/status
// ============================================================================

function rnMsg(text, kind) {
  showMsg("rn-msg",
          "<div class='alert alert-" + (kind === "ok" ? "success" : "error") + "'>" +
          rnEsc(text) + "</div>", true);
}

function rnEsc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
  });
}

// null means the node has never reported — the mailbox holds an entry for it
// but no timestamp — and that is NOT an age of zero. Math.round(null/1000) is
// 0, so the page said "seen: 0s ago" about a node it had never heard from,
// which is the most reassuring possible way to display the opposite.
function rnFmtAge(ms) {
  if (ms === null || ms === undefined) return null;
  var s = Math.round(ms / 1000);
  if (s < 60) return s + "s";
  var m = Math.round(s / 60);
  if (m < 60) return m + "m";
  var h = Math.round(m / 60);
  if (h < 24) return h + "h";
  return Math.round(h / 24) + "d";
}

function rnRenderNodes(d) {
  var box = document.getElementById("rn-nodes");
  var count = document.getElementById("rn-node-count");
  if (!box) return;

  var nodes = (d && d.nodes) || [];
  if (count) {
    count.textContent = nodes.length ? (nodes.length + (nodes.length === 1 ? " node" : " nodes")) : "none";
    count.className = "badge dim";
  }

  if (!nodes.length) {
    box.innerHTML = '<p class="hint">No nodes reported yet.</p>';
    return;
  }

  var html = "";
  for (var i = 0; i < nodes.length; i++) {
    var n = nodes[i];
    var age = rnFmtAge(n.age_ms);
    var seenTxt = age === null ? "never" : age + " ago";

    var metricsHtml = "";
    if (n.metrics && n.metrics.length) {
      for (var j = 0; j < n.metrics.length; j++) {
        var m = n.metrics[j];
        var val = (typeof m.value === "number") ? m.value.toFixed(1) : m.value;
        metricsHtml += '<span class="badge dim" style="margin-right:6px; font-family:monospace">' + 
                       rnEsc(m.metric) + ': <strong>' + rnEsc(val) + '</strong> <small>' + rnEsc(m.unit) + '</small></span>';
      }
    }

    html +=
      '<div class="card" style="margin-bottom:10px">' +
        '<div class="card-head">' +
          '<div class="card-title"><span data-icon="cpu"></span> ' + rnEsc(n.id) + '</div>' +
          '<span class="badge ' + (n.online ? "ok" : "err") + '">' +
            (n.online ? "online" : "offline") +
          "</span>" +
        "</div>" +
        '<div class="card-body">' +
          '<div style="margin-bottom:8px">' +
            '<span class="badge dim">seen: ' + seenTxt + '</span>' +
          '</div>' +
          '<div style="display:flex; flex-wrap:wrap; gap:4px">' +
            metricsHtml +
          '</div>' +
        "</div>" +
      "</div>";
  }
  box.innerHTML = html;
  if (window.Icons && Icons.swap) Icons.swap(box);
}

function remoteRefresh() {
  return fetchWithTimeout("/api/remote/status", {}, 15000)
    .then(function (r) {
      if (r.status === 404) throw new Error("not-in-build");
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      rnRenderNodes(d);
      return d;
    })
    .catch(function (e) {
      var box = document.getElementById("rn-nodes");
      if (!box) return;
      if (e && e.message === "not-in-build") {
        box.innerHTML =
          '<p class="hint">This firmware was not built with <code>FEATURE_REMOTE_NODES</code>. ' +
          "Enable it in the deploy tool's build features and reflash.</p>";
      } else {
        box.innerHTML = '<p class="hint">Could not read the node status.</p>';
      }
    });
}

function remoteInit() {
  remoteRefresh();
}

registerHandlers({
  remoteRefresh: remoteRefresh
});
