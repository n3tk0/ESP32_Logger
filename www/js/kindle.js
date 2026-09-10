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

// ── The page, zone by zone ──────────────────────────────────────────────────
//
// ONE ROW PER REGION OF THE PANEL, IN THE ORDER THE PANEL DRAWS THEM.
//
// This was three separate lists. A grid of nine checkboxes headed "Weight", a
// grid of eight headed "What is drawn", and a card of eleven place-editors —
// three different namings of the same regions, in three different orders, none
// of them saying where on the panel a region is or what fills it. "The grid"
// appeared in two of them and meant the same thing; "Beside the headline"
// appeared in two and meant the same thing; the forecast appeared in one of
// them and was not configurable from this page at all. To answer "where do I
// turn this on" the reader had to hold the whole layout in their head and
// guess which of the three lists owned it.
//
// So: one table, the panel's own reading order, and a fourth column that says
// where the content comes from — a jump to the control further down this page,
// a link to the page that actually owns it, or a plain statement that nothing
// configures it. Every KSHOW bit and every KBOLD bit appears exactly once,
// which is also what makes the mapping checkable by eye against
// src/core/Config.h.
//
//   show:  KSHOW_* bit; 0 = always drawn; -1 = a build-time module.
//   bold:  KBOLD_* bit; 0 = no weight of its own.
//   fill:  ["jump", elementId, label]  a control further down this page
//          ["link", href, label]       the page that owns it
//          ["text", sentence]          nothing configures it
var KD_ZONES = [
  { name: "Headline",
    where: "Top left. The largest number on the page.",
    show: 0, bold: 0x0001,
    fill: ["jump", "kd-zone-hero", "Pick the reading"] },

  { name: "Beside the headline",
    where: "Shares the headline's baseline, after a slash.",
    show: 0x0001, bold: 0x0002,
    fill: ["jump", "kd-zone-big", "Pick the reading"] },

  { name: "24-hour low-to-high",
    where: "The small line under the headline, with the reading's age.",
    show: 0x0008, bold: 0,
    fill: ["text", "Follows the headline's sensor."] },

  { name: "The grid",
    where: "Under that line \u2014 up to three across and two deep.",
    show: 0x0002, bold: 0x0004,
    fill: ["jump", "kd-zone-g1", "Pick the six readings"] },

  { name: "Pressure tendency arrow",
    where: "After a pressure reading, wherever one is placed.",
    show: 0x0004, bold: 0,
    fill: ["text", "Per place, under \u201cTendency arrow\u201d."] },

  { name: "Clock",
    where: "Top right, above the indoor row.",
    show: 0, bold: 0x0008,
    fill: ["jump", "kd-card-clock", "Style, time and date"] },

  { name: "Indoor row",
    where: "Under the clock \u2014 up to three readings on one baseline.",
    show: 0x0010, bold: 0x0010,
    fill: ["jump", "kd-zone-in1", "Pick the three readings"] },

  { name: "24-hour trend chart",
    where: "Full width, under the two columns.",
    show: 0x0020, bold: 0,
    fill: ["text", "Drawn from the stored history of the headline's sensor."] },

  { name: "Weather forecast",
    where: "Under the chart: the condition, and three outlook columns.",
    show: -1, bold: 0x0040,
    fill: ["link", "#settings_modules", "Provider, place and outlook"] },

  { name: "Week strip",
    where: "The seven days above the footer, today knocked out in black.",
    show: 0x0040, bold: 0x0080,
    fill: ["text", "Drawn from the device's own date."] },

  { name: "Low-battery badge",
    where: "Beside the outdoor heading, when a node is nearly flat.",
    show: 0x0080, bold: 0,
    fill: ["link", "#settings_espnow", "Which nodes report a battery"] },

  // The last two are not regions. They are the two things that appear inside
  // every region, and they own a weight bit each — which is why they were in
  // the "Weight" list alongside eight regions, reading as if they were places
  // on the panel.
  { name: "Captions", span: true,
    where: "The small grey heading above every value, and the two column headings.",
    show: 0, bold: 0x0100,
    fill: ["jump", "kd-slots", "Wording, per place"] },

  { name: "Units", span: true,
    where: "The suffix after a number \u2014 \u00b0, %, hPa.",
    show: 0, bold: 0x0020,
    fill: ["jump", "kd-card-units", "Which units"] }
];

// Kept for the round-trip: kdMaskOf() walks these to collect the checkboxes
// the table above rendered, and they are the list the firmware's constants are
// compared against. Derived from KD_ZONES rather than written twice, so a zone
// added to the table cannot be left out of the save.
function kdBitsOf(field) {
  var out = [];
  for (var i = 0; i < KD_ZONES.length; i++) {
    var bit = KD_ZONES[i][field];
    if (bit > 0) out.push([bit, KD_ZONES[i].name]);
  }
  return out;
}
var KD_BOLD = kdBitsOf("bold");
var KD_SHOW = kdBitsOf("show");

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

// One cell of the table: a checkbox bound to a bit, or the reason there is
// none. A blank cell would read as "off"; a word says which it is.
function kdBitCell(bit, prefix, mask, none) {
  if (bit > 0) {
    return '<input type="checkbox" style="width:auto" id="' + prefix + bit + '"' +
           ((mask & bit) ? " checked" : "") + ' aria-label="' + kdEsc(none.aria) + '">';
  }
  return '<span class="hint" style="margin:0">' + kdEsc(none.word) + "</span>";
}

function kdFillCell(fill) {
  if (!fill) return "";
  if (fill[0] === "jump") {
    return '<button class="btn sm" data-click="kindleJump" data-args=' +
           "'[\"" + fill[1] + "\"]'>" + kdEsc(fill[2]) +
           ' <span data-icon="arrow-down"></span></button>';
  }
  if (fill[0] === "link") {
    return '<a class="btn sm" href="' + kdEsc(fill[1]) + '">' + kdEsc(fill[2]) +
           ' <span data-icon="arrow-right"></span></a>';
  }
  return '<span class="hint" style="margin:0">' + kdEsc(fill[1]) + "</span>";
}

function kdRenderZones(show, bold) {
  var box = document.getElementById("kd-zones");
  if (!box) return;

  var rows = "", spanOpened = false;
  for (var i = 0; i < KD_ZONES.length; i++) {
    var z = KD_ZONES[i];
    // The two page-wide rows are separated by a heading rather than just sat
    // at the bottom: they are not places on the panel, and a table of regions
    // that ends with two non-regions is the confusion this replaced.
    if (z.span && !spanOpened) {
      spanOpened = true;
      rows += '<tr class="kd-zsplit"><th colspan="4">Everywhere on the page</th></tr>';
    }
    rows +=
      "<tr>" +
        "<td><strong>" + kdEsc(z.name) + "</strong>" +
          '<div class="hint" style="margin:2px 0 0">' + kdEsc(z.where) + "</div></td>" +
        '<td class="kd-zbit">' +
          kdBitCell(z.show, "kd-s-", show,
                    { aria: "Show " + z.name,
                      word: z.show === -1 ? "Module" : "Always" }) + "</td>" +
        '<td class="kd-zbit">' +
          kdBitCell(z.bold, "kd-b-", bold,
                    { aria: "Bold " + z.name, word: "\u2014" }) + "</td>" +
        '<td class="kd-zfill">' + kdFillCell(z.fill) + "</td>" +
      "</tr>";
  }

  box.innerHTML =
    '<table class="ftable kd-ztable"><thead><tr>' +
      "<th>Zone</th><th>Shown</th><th>Bold</th><th>What fills it</th>" +
    "</tr></thead><tbody>" + rows + "</tbody></table>";

  if (window.Icons && Icons.swap) Icons.swap(box);
}

// Take the reader to the control that fills a zone, and flash it, rather than
// naming it and leaving them to find it four cards down. The table is this
// page's index; an index whose entries are not links is just a list.
function kindleJump(id) {
  var el = document.getElementById(id);
  if (!el) return;
  try { el.scrollIntoView({ block: "center", behavior: "smooth" }); }
  catch (e) { el.scrollIntoView(); }
  el.classList.add("kd-flash");
  setTimeout(function () { el.classList.remove("kd-flash"); }, 1400);
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
  // "As built" names the language it actually resolves to. Without this the
  // option is a promise the page cannot keep: a reader looking at it has no
  // way to know whether leaving it there means English or Bulgarian, and the
  // collector already sends the answer.
  var asBuilt = document.querySelector("#kd-lang option[value='0']");
  if (asBuilt) {
    asBuilt.textContent = (d.lang_built === 2) ? "As built (Български)"
                        : (d.lang_built === 1) ? "As built (English)"
                        : "As built";
  }
  kdSet("kd-lang",  d.lang);
  kdSet("kd-face",  d.face);
  kdSet("kd-clock", d.clock_style);
  kdSet("kd-time",  d.time_format);
  kdSet("kd-date",  d.date_format);
  kdSet("kd-press", d.pressure_unit);
  kdSet("kd-dec",   d.decimals);

  var fc = document.getElementById("kd-face-custom");
  if (fc) fc.value = d.face_custom || "";

  kdRenderZones(d.show | 0, d.bold | 0);

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
  body.set("lang",          kdVal("kd-lang", "0"));
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
  // Back to "as built", which is where a device that has never been touched
  // sits — not to English, which would be a choice this button did not make.
  body.set("lang", "0");
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
// The layout editor
// ============================================================================
// The dashboard is nine named PLACES and the reader chooses what goes in each.
// This draws them in the shape the page draws them — the headline and its grid
// on the left, the indoor row on the right — rather than as a flat list, so
// that "where will this land" is answered by looking at the form.
//
// The page holds a working copy and nothing is sent until Save, so filling in
// four places is one request rather than four, and the device can never end up
// holding half an edit.
//
// The sensor and metric dropdowns come from GET /api/sensors, which already
// reports the metrics each sensor publishes. That is what makes the editor
// honest about hardware: a BMP280 offers temperature and pressure and no
// humidity, because that is what it measures, and a BME688 offers AQI because
// it has one.
var kdZones   = {};     // the working copy, keyed by place
var kdOrder   = [];     // [{key, group, role}] from the firmware's own enum
var kdSensors = [];     // [{id, name, metrics: []}]
var kdGroups  = { out: "", in: "" };        // the reader's headings, "" = built-in
var kdGroupPh = { out: "OUTSIDE", in: "INSIDE" };   // what "" renders as
var kdFlags   = { bold: 1, unit: 2, age: 4, trend: 8 };
var kdInks    = [];     // [{id, css}] from the firmware's own enum
var kdAutoDec = 255;

// The grey levels, in the firmware's order. Named here rather than derived from
// the hex, because "#777" is not a word anybody wants in a dropdown.
var KD_INK_NAME = ["Black", "Dark grey", "Mid grey", "Light grey"];

// What each place is FOR, in the reader's terms. The firmware sends the keys
// and the roles; the wording is the form's own business.
var KD_ZONE_TEXT = {
  hero: ["Headline", "The largest number on the page."],
  big:  ["Beside it", "Shares the headline's baseline, after a slash. Usually the humidity of the same air."],
  g1:   ["Grid, first", ""],
  g2:   ["Grid, second", ""],
  g3:   ["Grid, third", ""],
  g4:   ["Grid, fourth", ""],
  g5:   ["Grid, fifth", ""],
  g6:   ["Grid, sixth", ""],
  in1:  ["Indoor, first", "Set much larger than the other two, and drawn with no caption — the heading above it already names the room."],
  in2:  ["Indoor, second", ""],
  in3:  ["Indoor, third", "Leave empty for a row of two."]
};

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

function kdZone(key) {
  if (!kdZones[key]) {
    kdZones[key] = { sensor: "", metric: "", label: "", shown: "",
                     flags: kdFlags.unit, decimals: kdAutoDec, ink: 0 };
  }
  return kdZones[key];
}

// One place's card: which sensor, which reading, what to call it, and the
// switches that decide how it is drawn.
function kdZoneCard(key) {
  var s = kdZone(key);
  var text = KD_ZONE_TEXT[key] || [key, ""];

  // The sensor this place names may not be among the configured ones — a node
  // that has been removed, or a layout restored from another device. Kept as an
  // option rather than silently reassigned: dropping it would rewrite the
  // reader's page on their behalf just because a sensor was offline.
  var sensorOpts = "<option value=''" + (s.sensor ? "" : " selected") +
                   ">&mdash; empty &mdash;</option>";
  var known = false;
  for (var j = 0; j < kdSensors.length; j++) {
    var sel = kdSensors[j].id === s.sensor;
    if (sel) known = true;
    sensorOpts += "<option value='" + esc(kdSensors[j].id) + "'" +
                  (sel ? " selected" : "") + ">" + esc(kdSensors[j].id) + "</option>";
  }
  if (!known && s.sensor) {
    sensorOpts += "<option value='" + esc(s.sensor) + "' selected>" +
                  esc(s.sensor) + " (not configured)</option>";
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

  var empty = !s.sensor || !s.metric;

  // The id is what the zone table's "Pick the reading" button jumps to.
  return "<div class='card' id='kd-zone-" + key + "' style='margin-bottom:8px'>" +
    "<div class='card-body'>" +
    "<div style='display:flex;gap:6px;align-items:baseline;margin-bottom:6px'>" +
      "<strong style='font-size:.9rem'>" + esc(text[0]) + "</strong>" +
      (text[1] ? "<span class='hint' style='margin:0'>" + esc(text[1]) + "</span>" : "") +
      (empty ? "<span class='badge dim' style='margin-left:auto'>empty</span>" : "") +
    "</div>" +
    "<div style='display:flex;gap:8px;flex-wrap:wrap;align-items:flex-end'>" +
      "<div style='flex:1;min-width:130px'>" +
        "<label class='field-label'>Sensor</label>" +
        "<select class='input' data-change='kindleZoneEdit' " +
          "data-args='[\"" + key + "\",\"sensor\"]'>" + sensorOpts + "</select></div>" +
      "<div style='flex:1;min-width:130px'>" +
        "<label class='field-label'>Reading</label>" +
        "<select class='input' data-change='kindleZoneEdit' " +
          "data-args='[\"" + key + "\",\"metric\"]'>" + metricOpts + "</select></div>" +
      "<div style='flex:1;min-width:110px'>" +
        "<label class='field-label'>Caption</label>" +
        "<input class='input' maxlength='16' placeholder='" + esc(s.shown || "") + "' " +
          "value='" + esc(s.label || "") + "' data-change='kindleZoneEdit' " +
          "data-args='[\"" + key + "\",\"label\"]'></div>" +
      "<div style='flex:0 0 110px'>" +
        "<label class='field-label'>Decimals</label>" +
        kdDecimalSelect(key, s.decimals) + "</div>" +
      "<div style='flex:0 0 130px'>" +
        "<label class='field-label'>Ink</label>" +
        kdInkSelect(key, s.ink | 0) + "</div>" +
    "</div>" +
    "<div style='display:flex;gap:12px;flex-wrap:wrap;margin-top:8px;align-items:center'>" +
      kdFlagBox(key, kdFlags.unit,  "Show the unit") +
      kdFlagBox(key, kdFlags.bold,  "Bold") +
      kdFlagBox(key, kdFlags.age,   "Show the age when stale") +
      kdFlagBox(key, kdFlags.trend, "Tendency arrow (pressure)") +
      "<span style='flex:1'></span>" +
      "<button class='btn warn' data-click='kindleZoneClear' data-args='[\"" + key + "\"]'>" +
        "<span data-icon='trash'></span> Empty it</button>" +
    "</div>" +
  "</div></div>";
}

function kdDecimalSelect(key, dec) {
  var html = "<select class='input' data-change='kindleZoneEdit' " +
             "data-args='[\"" + key + "\",\"decimals\"]'>" +
             "<option value='" + kdAutoDec + "'" +
             (dec === kdAutoDec ? " selected" : "") + ">Automatic</option>";
  for (var d = 0; d <= 3; d++) {
    html += "<option value='" + d + "'" + (dec === d ? " selected" : "") + ">" +
            d + "</option>";
  }
  return html + "</select>";
}

// How dark the value is drawn. Four levels rather than a colour picker: the
// panel has sixteen real grey levels and the ones worth having are the ones far
// enough apart to render solid, which is what the page's palette already is.
function kdInkSelect(key, ink) {
  var html = "<select class='input' data-change='kindleZoneEdit' " +
             "data-args='[\"" + key + "\",\"ink\"]'>";
  var n = kdInks.length ? kdInks.length : KD_INK_NAME.length;
  for (var i = 0; i < n; i++) {
    html += "<option value='" + i + "'" + (ink === i ? " selected" : "") + ">" +
            esc(KD_INK_NAME[i] || ("Level " + i)) + "</option>";
  }
  return html + "</select>";
}

function kdFlagBox(key, bit, label) {
  var on = (kdZone(key).flags & bit) !== 0;
  return "<label style='display:flex;gap:5px;align-items:center;font-size:.85rem'>" +
           "<input type='checkbox' style='width:auto'" + (on ? " checked" : "") +
           " data-change='kindleZoneFlag' data-args='[\"" + key + "\"," + bit + "]'>" +
           esc(label) + "</label>";
}

// A heading, and the places under it. Two of these make the whole editor.
function kdGroupBlock(which, title, note, keys) {
  var html = "<div class='card' style='margin-bottom:10px'><div class='card-head'>" +
      "<div class='card-title'>" + esc(title) + "</div></div><div class='card-body'>" +
      "<p class='hint' style='margin-top:0'>" + esc(note) + "</p>" +
      "<div class='field' style='max-width:280px'>" +
        "<label class='field-label'>Heading on the page</label>" +
        "<input class='input' maxlength='16' placeholder='" + esc(kdGroupPh[which]) +
          "' value='" + esc(kdGroups[which] || "") + "' data-change='kindleGroupEdit' " +
          "data-args='[\"" + which + "\"]'>" +
        "<p class='hint'>Leave blank for the built-in wording.</p>" +
      "</div>";
  for (var i = 0; i < keys.length; i++) html += kdZoneCard(keys[i]);
  return html + "</div></div>";
}

function kdRenderSlots() {
  var box = document.getElementById("kd-slots");
  if (!box) return;

  // Grouped the way the page groups them, and in the firmware's own order, so
  // a place added to the enum one day appears here without a second edit.
  var outKeys = [], inKeys = [];
  for (var i = 0; i < kdOrder.length; i++) {
    (kdOrder[i].group === "indoor" ? inKeys : outKeys).push(kdOrder[i].key);
  }

  var filled = 0;
  for (var k in kdZones) {
    if (kdZones.hasOwnProperty(k) && kdZones[k].sensor && kdZones[k].metric) filled++;
  }
  var count = document.getElementById("kd-slot-count");
  if (count) {
    count.textContent = filled + " / " + kdOrder.length + " filled";
    count.className = "badge dim";
  }

  box.innerHTML =
    kdGroupBlock("out", "The left column",
                 "The headline, the value beside it, and the grid under the " +
                 "24-hour line — up to three across and two deep. An empty " +
                 "place is skipped and the grid closes up behind it; every " +
                 "row then divides its own width by however many cells it " +
                 "ended up with, so two is two halves rather than two thirds " +
                 "and a gap.", outKeys) +
    kdGroupBlock("in", "The right column, under the clock",
                 "One row of up to three. The first is set much larger and " +
                 "carries no caption; the other two sit on its bottom edge. " +
                 "Leave the third empty for a row of two.", inKeys);

  if (window.Icons && Icons.swap) Icons.swap(box);
}

// THE ELEMENT, NOT THE EVENT.
//
// core.js's dispatcher calls a handler as `fn.apply(el, args)` when the tag
// carries data-args, and as `fn.call(el, ev)` when it does not — so a handler
// that takes arguments never receives the event at all. The version of this
// editor that read `ev.target.value` was therefore reading undefined and
// storing "" every time somebody chose a sensor from the dropdown: the control
// looked like it worked and the value it set was empty.
//
// `this` is the element in both cases, which is what makes it the reliable one
// to read. The event is kept as a fallback for a caller that passes one.
function kdEventValue(self, ev, prop) {
  var el = (self && self.nodeType === 1) ? self : (ev && ev.target);
  return el ? el[prop] : undefined;
}

function kindleZoneEdit(key, field, ev) {
  var s = kdZone(key);
  var v = kdEventValue(this, ev, "value");
  if (v === undefined) return;
  if (field === "decimals" || field === "ink") s[field] = parseInt(v, 10);
  else                                        s[field] = v;

  // Only the sensor changes what the other controls can offer.
  // Changing the sensor can invalidate the metric — a BME688's "aqi" means
  // nothing on a BMP280. Reset to the new sensor's first reading rather than
  // leaving a pairing that will never resolve.
  if (field !== "sensor") return;
  if (!v) { s.metric = ""; }
  else {
    var ms = kdMetricsFor(v);
    if (ms.indexOf(s.metric) < 0) s.metric = ms.length ? ms[0] : "";
  }
  // ONLY the sensor redraws. The dispatcher listens on "input" as well as
  // "change", so redrawing on a caption edit would rebuild the form on every
  // keystroke and take the reader's cursor with it.
  kdRenderSlots();
}

function kindleZoneFlag(key, bit, ev) {
  var s = kdZone(key);
  var on = kdEventValue(this, ev, "checked");
  if (on === undefined) return;
  s.flags = on ? (s.flags | bit) : (s.flags & ~bit);
}

function kindleZoneClear(key) {
  kdZones[key] = { sensor: "", metric: "", label: "", shown: "",
                   flags: kdFlags.unit, decimals: kdAutoDec, ink: 0 };
  kdRenderSlots();
}

function kindleGroupEdit(which, ev) {
  var v = kdEventValue(this, ev, "value");
  if (v !== undefined) kdGroups[which] = v;
}

function kindleSlotsLoad() {
  return fetchWithTimeout("/api/kindle/slots", {}, 10000)
    .then(function(r) { return r.ok ? r.json() : null; })
    .then(function(d) {
      if (!d) return;
      kdZones = d.zones || {};
      kdOrder = d.order || [];
      kdFlags = { bold: d.flag_bold, unit: d.flag_unit, age: d.flag_age, trend: d.flag_trend };
      kdInks  = d.inks || [];
      kdAutoDec = d.auto_decimals;
      kdGroups  = { out: d.group_out_set || "", in: d.group_in_set || "" };
      kdGroupPh = { out: d.group_out || "OUTSIDE", in: d.group_in || "INSIDE" };
      kdRenderSlots();
    })
    .catch(function() {});
}

function kindleSlotsSave() {
  // ONLY THE FIELDS THE DEVICE READS. `shown` is what the caption WILL render
  // as, derived on the collector from the metric table — it comes down with the
  // layout so the form can offer it as a placeholder, and sending it back is a
  // quarter of the payload spent on a value the firmware ignores. On a body
  // that reaches two kilobytes fully filled in, and travels to a device whose
  // segments are about 1.4 KB, a quarter matters.
  var out = {};
  Object.keys(kdZones).forEach(function (k) {
    var z = kdZones[k];
    out[k] = { sensor: z.sensor || "", metric: z.metric || "",
               label: z.label || "", flags: z.flags | 0,
               decimals: z.decimals | 0, ink: z.ink | 0 };
  });

  return postWithCsrf("/api/kindle/slots", {
    body: JSON.stringify({ zones: out,
                           group_out: kdGroups.out, group_in: kdGroups.in }),
    headers: { "Content-Type": "application/json" }
  })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d && d.ok) {
        kdSlotMsg("Saved " + d.count + " reading" + (d.count === 1 ? "" : "s") + ".", "ok");
        kindleSlotsLoad();
      } else {
        kdSlotMsg((d && d.error) || "Save failed.", "err");
      }
    })
    .catch(function() { kdSlotMsg("Save failed.", "err"); });
}

function kindleSlotsReset() {
  // The built-in layout, rebuilt from the two sensors the page is pointed at,
  // so "back to the built-in design" means the same thing here as on the button
  // above it. The last two grid places are left EMPTY on purpose: a dashboard
  // that arrives showing a metric the hardware does not have is showing a dash.
  var out = (document.getElementById("kd-outdoor-sensor") || {}).value || "";
  var inn = (document.getElementById("kd-indoor-sensor") || {}).value || "";
  if (!out && kdSensors.length) out = kdSensors[0].id;
  if (!inn && kdSensors.length) inn = kdSensors[kdSensors.length > 1 ? 1 : 0].id;

  function z(sensor, metric, flags, ink) {
    return { sensor: sensor, metric: metric, label: "", shown: "",
             flags: flags, decimals: kdAutoDec, ink: ink || 0 };
  }
  function none() { return z("", "", kdFlags.unit, 0); }

  kdZones = {
    hero: z(out, "temperature", kdFlags.bold | kdFlags.unit | kdFlags.age),
    big:  z(out, "humidity",    kdFlags.unit, 1),
    g1:   z(out, "pressure",    kdFlags.unit | kdFlags.trend),
    g2:   z(out, "dew_point",   kdFlags.unit),
    g3:   none(),
    g4:   none(),
    g5:   none(),
    g6:   none(),
    in1:  z(inn, "temperature", kdFlags.unit | kdFlags.age),
    in2:  z(inn, "humidity",    kdFlags.unit, 1),
    in3:  z(inn, "aqi",         kdFlags.unit, 1)
  };
  kdGroups = { out: "", in: "" };
  kdRenderSlots();
  kdSlotMsg("Reset — press Save to keep it.", "ok");
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
  kindleJump: kindleJump,
  kindleFaceChanged: kindleFaceChanged,
  kindleClockChanged: kindleClockChanged,
  kindleZoneEdit: kindleZoneEdit,
  kindleZoneFlag: kindleZoneFlag,
  kindleZoneClear: kindleZoneClear,
  kindleGroupEdit: kindleGroupEdit,
  kindleSlotsSave: kindleSlotsSave,
  kindleSlotsReset: kindleSlotsReset,
});
