// ============================================================================
// kindle.js — the E-ink dashboard settings page (#settings_kindle)
//
// Reads and writes GET/POST /api/kindle/config. The page it configures is at
// /kindle and has no settings of its own: it is served to a reader with no
// JavaScript and a five-way pad, so a form there would be a worse version of
// this one.
//
// The two bitmask groups are built here rather than written out in the HTML
// partial, because the bit VALUES have to match src/core/Config.h and a list
// that names each bit next to its label is the only form in which that is
// checkable by eye. Getting one wrong would toggle the neighbouring setting,
// which is the kind of bug that looks like a rendering fault.
// ============================================================================

// Must match the KBOLD_* constants in src/core/Config.h.
var KD_BOLD = [
  [0x0001, "Outside temperature"],
  [0x0002, "Outside humidity"],
  [0x0004, "Pressure"],
  [0x0008, "Clock"],
  [0x0010, "Inside temperature"],
  [0x0020, "Inside humidity"],
  [0x0040, "Forecast"],
  [0x0080, "Week strip"],
  [0x0100, "Section labels"]
];

// Must match the KSHOW_* constants in src/core/Config.h.
var KD_SHOW = [
  [0x0001, "Outside humidity"],
  [0x0002, "Pressure"],
  [0x0004, "Pressure tendency"],
  [0x0008, "24-hour range"],
  [0x0010, "Inside block"],
  [0x0020, "Trend chart"],
  [0x0040, "Week strip"],
  [0x0080, "Low-battery badge"]
];

function kdMsg(text, kind) {
  showMsg("kd-msg",
          "<div class='alert alert-" + (kind === "ok" ? "success" : "error") + "'>" +
          kdEsc(text) + "</div>", true);
}

function kdEsc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
  });
}

function kdBoxes(boxId, defs, prefix, mask) {
  var box = document.getElementById(boxId);
  if (!box) return;
  var html = "";
  for (var i = 0; i < defs.length; i++) {
    html +=
      '<label class="check"><input type="checkbox" id="' + prefix + defs[i][0] + '"' +
      ((mask & defs[i][0]) ? " checked" : "") + '><span>' + kdEsc(defs[i][1]) +
      "</span></label>";
  }
  box.innerHTML = html;
}

function kdMaskOf(defs, prefix) {
  var mask = 0;
  for (var i = 0; i < defs.length; i++) {
    var el = document.getElementById(prefix + defs[i][0]);
    if (el && el.checked) mask |= defs[i][0];
  }
  return mask;
}

function kdSet(id, value) {
  var el = document.getElementById(id);
  if (el) el.value = String(value);
}

function kdVal(id, fallback) {
  var el = document.getElementById(id);
  return el ? el.value : fallback;
}

// The custom field is only meaningful for the last option, and the date format
// only for the dated clock. Both are hidden rather than disabled: a control
// that is visible but does nothing is a question the page is asking and then
// ignoring the answer to.
function kindleFaceChanged() {
  var row = document.getElementById("kd-face-custom-row");
  if (row) row.style.display = (kdVal("kd-face", "0") === "6") ? "" : "none";
}

function kindleClockChanged() {
  var hint = document.getElementById("kd-date-hint");
  var dated = kdVal("kd-clock", "0") === "3";
  if (hint) hint.style.display = dated ? "none" : "";
}

function kindleRender(d) {
  kdSet("kd-face",  d.face);
  kdSet("kd-clock", d.clock_style);
  kdSet("kd-time",  d.time_format);
  kdSet("kd-date",  d.date_format);
  kdSet("kd-press", d.pressure_unit);
  kdSet("kd-dec",   d.decimals);

  var fc = document.getElementById("kd-face-custom");
  if (fc) fc.value = d.face_custom || "";

  kdBoxes("kd-bold", KD_BOLD, "kd-b-", d.bold | 0);
  kdBoxes("kd-show", KD_SHOW, "kd-s-", d.show | 0);

  kdSet("kd-refresh",  d.refresh_sec);
  kdSet("kd-follow",   d.follow_data);
  kdSet("kd-clockpin", d.clock_pin_refresh);
  kdSet("kd-fbink-res", d.fbink_res_w || 0);
  kdSet("kd-outdoor-sensor", d.outdoor_sensor || "");
  kdSet("kd-indoor-sensor",  d.indoor_sensor || "");

  // Stated, not settable. The width is a build-time constant because every
  // size in the page's stylesheet is derived from it, so a reader on which the
  // layout looks wrong needs to know where the number came from rather than
  // hunting for the control that would change it.
  var pw = document.getElementById("kd-pagew");
  if (pw && d.page_w) {
    pw.innerHTML = " The layout is " + (d.page_w | 0) +
      " px wide, fixed when the firmware was built; <code>/kindle/probe</code>" +
      " on the reader itself says what it should be.";
  }

  kindleFaceChanged();
  kindleClockChanged();
}

function kindleRefresh() {
  return fetchWithTimeout("/api/kindle/config", {}, 15000)
    .then(function (r) {
      if (r.status === 404) throw new Error("not-in-build");
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(kindleRender)
    .catch(function (e) {
      var form = document.getElementById("kd-form");
      if (!form) return;
      if (e && e.message === "not-in-build") {
        // Named rather than shown as an empty form, for the same reason the
        // battery-nodes page does it: a page that looks merely blank sends
        // people looking for a fault that is not there.
        form.innerHTML =
          '<div class="card"><div class="card-body"><p class="hint">' +
          "This firmware was not built with <code>FEATURE_KINDLE_DASHBOARD</code>, " +
          "so there is no <code>/kindle</code> page to configure. Enable it in " +
          "the deploy tool's build features and reflash.</p></div></div>";
      } else {
        form.innerHTML =
          '<div class="card"><div class="card-body"><p class="hint">' +
          "Could not read the dashboard settings.</p></div></div>";
      }
    });
}

function kindlePost(body, okText) {
  return postWithCsrf("/api/kindle/config", {
    body: body,
    headers: { "Content-Type": "application/x-www-form-urlencoded" }
  })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok) {
        kdMsg(okText, "ok");
        kindleRefresh();
      } else {
        kdMsg((d && d.error) || "Save failed.", "err");
      }
    })
    .catch(function () { kdMsg("Save failed.", "err"); });
}

function kindleSave() {
  var body = new URLSearchParams();
  body.set("face",          kdVal("kd-face", "0"));
  body.set("face_custom",   (document.getElementById("kd-face-custom") || {}).value || "");
  body.set("clock_style",   kdVal("kd-clock", "0"));
  body.set("time_format",   kdVal("kd-time", "0"));
  body.set("date_format",   kdVal("kd-date", "0"));
  body.set("pressure_unit", kdVal("kd-press", "0"));
  body.set("decimals",      kdVal("kd-dec", "1"));
  body.set("bold",          kdMaskOf(KD_BOLD, "kd-b-"));
  body.set("show",          kdMaskOf(KD_SHOW, "kd-s-"));

  body.set("refresh_sec",      kdVal("kd-refresh", "300"));
  body.set("follow_data",      kdVal("kd-follow", "1"));
  body.set("clock_pin_refresh", kdVal("kd-clockpin", "1"));
  body.set("fbink_res_w",       kdVal("kd-fbink-res", "0"));
  body.set("outdoor_sensor",    (document.getElementById("kd-outdoor-sensor") || {}).value || "");
  body.set("indoor_sensor",     (document.getElementById("kd-indoor-sensor") || {}).value || "");

  // Said in terms of the panel, not of the server. "Saved" alone would leave
  // somebody standing in front of a reader that has not repainted yet
  // wondering whether it worked.
  return kindlePost(body, "Saved. The reader picks it up on its next repaint.");
}

function kindleDefaults() {
  if (!confirm("Put the e-ink page back to the built-in design?\n\n" +
               "Bookerly, nothing bold, every block shown, hPa and one decimal."))
    return;

  var body = new URLSearchParams();
  body.set("face", "0");
  body.set("face_custom", "");
  body.set("clock_style", "0");
  body.set("time_format", "0");
  body.set("date_format", "0");
  body.set("pressure_unit", "0");
  body.set("decimals", "1");
  body.set("bold", "0");
  body.set("show", "255");   // KSHOW_ALL
  
  body.set("refresh_sec", "0");      // 0 = use compile-time default
  body.set("follow_data", "255");     // 0xFF = use compile-time default  
  body.set("clock_pin_refresh", "255");
  body.set("fbink_res_w", "0");
  body.set("outdoor_sensor", "");
  body.set("indoor_sensor", "");

  return kindlePost(body, "Back to the built-in design.");
}

function kindleInit() {
  kindleRefresh();
}

// Through the dispatcher's allowlist, not on window: core.js routes every
// data-click and data-change through Handlers so injected markup cannot reach
// an arbitrary global, and a handler that is only global is a dead button.
registerHandlers({
  kindleRefresh: kindleRefresh,
  kindleSave: kindleSave,
  kindleDefaults: kindleDefaults,
  kindleFaceChanged: kindleFaceChanged,
  kindleClockChanged: kindleClockChanged,
});
