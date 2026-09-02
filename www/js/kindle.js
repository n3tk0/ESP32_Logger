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


// ============================================================================
// The slot editor
// ============================================================================
// The dashboard's readings, as an ordered list. The page holds the working
// copy; nothing is sent until Save, so reordering four rows is one request
// rather than four.
//
// The sensor and metric dropdowns come from GET /api/sensors, which already
// reports the metrics each sensor publishes. That is what makes the editor
// honest about hardware: a BMP280 offers temperature and pressure and no
// humidity, because that is what it measures, and a BME688 offers AQI because
// it has one.
var kdSlots   = [];     // the working copy
var kdSizes   = [];     // [{id, name, units}] from the firmware's own enum
var kdSensors = [];     // [{id, name, metrics: []}]
var kdCap     = 12;
var kdFlags   = { bold: 1, unit: 2, age: 4, trend: 8 };
var kdAutoDec = 255;

function kdSlotMsg(text, kind) {
  showMsg("kd-slot-msg",
          "<div class='alert alert-" + (kind === "ok" ? "success" : "error") + "'>" +
          esc(text) + "</div>", true);
}

function kdMetricsFor(sensorId) {
  for (var i = 0; i < kdSensors.length; i++) {
    if (kdSensors[i].id === sensorId) return kdSensors[i].metrics || [];
  }
  return [];
}

function kdRenderSlots() {
  var box = document.getElementById("kd-slots");
  var count = document.getElementById("kd-slot-count");
  if (!box) return;

  if (count) {
    count.textContent = kdSlots.length + " / " + kdCap;
    count.className = "badge " + (kdSlots.length >= kdCap ? "warn" : "dim");
  }

  if (!kdSlots.length) {
    box.innerHTML = "<p class='hint'>No readings configured — the e-ink page " +
                    "will show the clock and the chart only.</p>";
    return;
  }

  var html = "";
  for (var i = 0; i < kdSlots.length; i++) {
    var s = kdSlots[i];

    // The sensor this slot names may not be among the configured ones — a node
    // that has been removed, or a list restored from another device. Kept as an
    // option rather than silently reassigned: dropping it would rewrite the
    // reader's layout on their behalf just because a sensor was offline.
    var sensorOpts = "";
    var known = false;
    for (var j = 0; j < kdSensors.length; j++) {
      var sel = kdSensors[j].id === s.sensor;
      if (sel) known = true;
      sensorOpts += "<option value='" + esc(kdSensors[j].id) + "'" +
                    (sel ? " selected" : "") + ">" + esc(kdSensors[j].id) + "</option>";
    }
    if (!known && s.sensor) {
      sensorOpts = "<option value='" + esc(s.sensor) + "' selected>" +
                   esc(s.sensor) + " (not configured)</option>" + sensorOpts;
    }

    var metrics = kdMetricsFor(s.sensor);
    var metricOpts = "";
    var mKnown = false;
    for (var m = 0; m < metrics.length; m++) {
      var msel = metrics[m] === s.metric;
      if (msel) mKnown = true;
      metricOpts += "<option value='" + esc(metrics[m]) + "'" +
                    (msel ? " selected" : "") + ">" + esc(metrics[m]) + "</option>";
    }
    if (!mKnown && s.metric) {
      metricOpts = "<option value='" + esc(s.metric) + "' selected>" +
                   esc(s.metric) + " (not reported)</option>" + metricOpts;
    }

    var sizeOpts = "";
    for (var z = 0; z < kdSizes.length; z++) {
      sizeOpts += "<option value='" + kdSizes[z].id + "'" +
                  (kdSizes[z].id === s.size ? " selected" : "") + ">" +
                  esc(kdSizes[z].name) + "</option>";
    }

    html +=
      "<div class='card' style='margin-bottom:8px'><div class='card-body'>" +
        "<div style='display:flex;gap:8px;flex-wrap:wrap;align-items:flex-end'>" +
          "<div style='flex:1;min-width:130px'>" +
            "<label class='field-label'>Sensor</label>" +
            "<select class='input' data-change='kindleSlotEdit' " +
              "data-args='[" + i + ",\"sensor\"]'>" + sensorOpts + "</select></div>" +
          "<div style='flex:1;min-width:130px'>" +
            "<label class='field-label'>Reading</label>" +
            "<select class='input' data-change='kindleSlotEdit' " +
              "data-args='[" + i + ",\"metric\"]'>" + metricOpts + "</select></div>" +
          "<div style='flex:0 0 110px'>" +
            "<label class='field-label'>Size</label>" +
            "<select class='input' data-change='kindleSlotEdit' " +
              "data-args='[" + i + ",\"size\"]'>" + sizeOpts + "</select></div>" +
          "<div style='flex:1;min-width:110px'>" +
            "<label class='field-label'>Label</label>" +
            "<input class='input' maxlength='16' placeholder='" + esc(s.shown || "") + "' " +
              "value='" + esc(s.label || "") + "' data-change='kindleSlotEdit' " +
              "data-args='[" + i + ",\"label\"]'></div>" +
        "</div>" +
        "<div style='display:flex;gap:12px;flex-wrap:wrap;margin-top:8px;align-items:center'>" +
          kdFlagBox(i, kdFlags.unit,  "unit",  "Show the unit") +
          kdFlagBox(i, kdFlags.bold,  "bold",  "Bold") +
          kdFlagBox(i, kdFlags.age,   "age",   "Show the age when stale") +
          "<span style='flex:1'></span>" +
          "<button class='btn' data-click='kindleSlotMove' data-args='[" + i + ",-1]'>&uarr;</button>" +
          "<button class='btn' data-click='kindleSlotMove' data-args='[" + i + ",1]'>&darr;</button>" +
          "<button class='btn warn' data-click='kindleSlotRemove' data-args='[" + i + "]'>" +
            "<span data-icon='trash'></span></button>" +
        "</div>" +
      "</div></div>";
  }
  box.innerHTML = html;
  if (window.Icons && Icons.swap) Icons.swap(box);
}

function kdFlagBox(i, bit, name, label) {
  var on = (kdSlots[i].flags & bit) !== 0;
  return "<label style='display:flex;gap:5px;align-items:center;font-size:.85rem'>" +
           "<input type='checkbox' style='width:auto'" + (on ? " checked" : "") +
           " data-change='kindleSlotFlag' data-args='[" + i + "," + bit + "]'>" +
           esc(label) + "</label>";
}

function kindleSlotEdit(i, field, ev) {
  if (!kdSlots[i]) return;
  var v = ev && ev.target ? ev.target.value : "";
  if (field === "size") kdSlots[i].size = parseInt(v, 10) || 0;
  else                  kdSlots[i][field] = v;

  // Changing the sensor can invalidate the metric — a BME688's "aqi" means
  // nothing on a BMP280. Reset to the new sensor's first reading rather than
  // leaving a pairing that will never resolve.
  if (field === "sensor") {
    var ms = kdMetricsFor(v);
    if (ms.indexOf(kdSlots[i].metric) < 0) kdSlots[i].metric = ms.length ? ms[0] : "";
  }
  kdRenderSlots();
}

function kindleSlotFlag(i, bit, ev) {
  if (!kdSlots[i]) return;
  var on = ev && ev.target ? ev.target.checked : false;
  kdSlots[i].flags = on ? (kdSlots[i].flags | bit) : (kdSlots[i].flags & ~bit);
}

function kindleSlotMove(i, dir) {
  var j = i + dir;
  if (j < 0 || j >= kdSlots.length) return;
  var t = kdSlots[i]; kdSlots[i] = kdSlots[j]; kdSlots[j] = t;
  kdRenderSlots();
}

function kindleSlotRemove(i) {
  kdSlots.splice(i, 1);
  kdRenderSlots();
}

function kindleSlotAdd() {
  if (kdSlots.length >= kdCap) { kdSlotMsg("That is the most the page holds.", "err"); return; }
  var first = kdSensors.length ? kdSensors[0] : null;
  kdSlots.push({
    sensor: first ? first.id : "",
    metric: first && first.metrics && first.metrics.length ? first.metrics[0] : "",
    label: "", size: 3, flags: kdFlags.unit, decimals: kdAutoDec
  });
  kdRenderSlots();
}

function kindleSlotsLoad() {
  return fetchWithTimeout("/api/kindle/slots", {}, 10000)
    .then(function(r) { return r.ok ? r.json() : null; })
    .then(function(d) {
      if (!d) return;
      kdSlots = d.slots || [];
      kdSizes = d.sizes || [];
      kdCap   = d.cap || 12;
      kdFlags = { bold: d.flag_bold, unit: d.flag_unit, age: d.flag_age, trend: d.flag_trend };
      kdAutoDec = d.auto_decimals;
      kdRenderSlots();
    })
    .catch(function() {});
}

function kindleSlotsSave() {
  return postWithCsrf("/api/kindle/slots", {
    body: JSON.stringify({ slots: kdSlots }),
    headers: { "Content-Type": "application/json" }
  })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d && d.ok) {
        var m = "Saved " + d.count + " reading" + (d.count === 1 ? "" : "s") + ".";
        if (d.dropped) m += " " + d.dropped + " incomplete one(s) were dropped.";
        kdSlotMsg(m, "ok");
        kindleSlotsLoad();
      } else {
        kdSlotMsg((d && d.error) || "Save failed.", "err");
      }
    })
    .catch(function() { kdSlotMsg("Save failed.", "err"); });
}

function kindleSlotsReset() {
  // The built-in six, rebuilt from the two sensors the page is pointed at, so
  // "back to the built-in design" means the same thing here as on the button
  // above it.
  var out = (document.getElementById("kd-outdoor-sensor") || {}).value || "";
  var inn = (document.getElementById("kd-indoor-sensor") || {}).value || "";
  if (!out && kdSensors.length) out = kdSensors[0].id;
  if (!inn && kdSensors.length) inn = kdSensors[kdSensors.length > 1 ? 1 : 0].id;

  kdSlots = [
    { sensor: out, metric: "temperature", label: "", size: 0, flags: kdFlags.bold | kdFlags.unit | kdFlags.age, decimals: kdAutoDec },
    { sensor: out, metric: "humidity",    label: "", size: 2, flags: kdFlags.unit, decimals: kdAutoDec },
    { sensor: out, metric: "pressure",    label: "", size: 2, flags: kdFlags.unit | kdFlags.trend, decimals: kdAutoDec },
    { sensor: inn, metric: "temperature", label: "", size: 1, flags: kdFlags.unit | kdFlags.age, decimals: kdAutoDec },
    { sensor: inn, metric: "humidity",    label: "", size: 2, flags: kdFlags.unit, decimals: kdAutoDec },
    { sensor: inn, metric: "aqi",         label: "", size: 2, flags: kdFlags.unit, decimals: kdAutoDec }
  ];
  kdRenderSlots();
  kdSlotMsg("Reset — press Save readings to keep it.", "ok");
}

function kindleInit() {
  fetchWithTimeout("/api/sensors", {}, 10000)
    .then(function(r) { return r.ok ? r.json() : null; })
    .then(function(d) {
      var options = '<option value="">Default</option>';
      if (d && d.sensors) {
        d.sensors.forEach(function(s) {
          options += '<option value="' + esc(s.id) + '">' + esc(s.id) + (s.name ? " (" + esc(s.name) + ")" : "") + '</option>';
        });
      }
      kdSensors = (d && d.sensors) ? d.sensors : [];
      var outSel = document.getElementById("kd-outdoor-sensor");
      var inSel = document.getElementById("kd-indoor-sensor");
      if (outSel) outSel.innerHTML = options.replace("Default", "Default (outdoor)");
      if (inSel) inSel.innerHTML = options.replace("Default", "Default (indoor)");
    })
    .catch(function() {})
    .finally(function() {
      kindleRefresh();
      kindleSlotsLoad();
    });
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
  kindleSlotAdd: kindleSlotAdd,
  kindleSlotEdit: kindleSlotEdit,
  kindleSlotFlag: kindleSlotFlag,
  kindleSlotMove: kindleSlotMove,
  kindleSlotRemove: kindleSlotRemove,
  kindleSlotsSave: kindleSlotsSave,
  kindleSlotsReset: kindleSlotsReset,
});
