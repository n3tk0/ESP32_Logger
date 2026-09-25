// ============================================================================
// kindle.js — the E-ink dashboard settings page (#settings_kindle)
//
// Reads and writes GET/POST /api/kindle/config and /api/kindle/slots. The page
// it configures is at /kindle and has no settings of its own: it is served to
// a reader with no JavaScript and a five-way pad, so a form there would be a
// worse version of this one.
//
// The two bitmask groups are built here rather than written out in the HTML
// partial, because the bit VALUES have to match src/core/Config.h and a list
// that names each bit next to its label is the only form in which that is
// checkable by eye. Getting one wrong would toggle the neighbouring setting,
// which is the kind of bug that looks like a rendering fault.
//
// ── WHAT THIS PAGE LEARNT THE HARD WAY ──────────────────────────────────────
//
// IT CONFIGURES A PICTURE, SO IT SHOWS THE PICTURE. Every question this form
// asks — what does Bold do, how tall is the boxed clock, did switching that
// region off leave a hole — used to be answerable only by saving, walking to
// the reader, and waiting for it to repaint. The panel is drawn beside the
// form now, at its own coordinates, from the values in the form rather than
// from the ones on the device: a change is visible before it is saved.
//
// AND IT IS THE NAVIGATION. Tap a region in it and that region opens. The
// alternative was what this replaced: an ASCII diagram in the markup doing the
// job of saying where things are, a table of thirteen rows of switches, and
// eleven always-open place editors further down — about a hundred controls in
// one scroll, with no way to tell which of them drew the thing you were
// looking at.
//
// ONE SAVE. There were two, over two working copies, with nothing on screen
// saying which covered what: editing a place and pressing the Save at the foot
// of the page stored nothing and said "Saved". The bar at the bottom counts
// what is unsaved and writes all of it — both endpoints, one message.
// ============================================================================

// ── The page, zone by zone ──────────────────────────────────────────────────
//
// ONE ROW PER REGION OF THE PANEL, IN THE ORDER THE PANEL DRAWS THEM, and
// everything about a region inside that row: whether it is drawn, whether its
// figures are heavy, and which reading fills it. Those three used to be three
// separate lists, in three different orders, naming the same regions
// differently — "the grid" appeared in two of them, the forecast in one and
// was not configurable at all — so answering "where do I turn this on" meant
// holding the whole layout in your head and guessing which list owned it.
//
// Every KSHOW bit and every KBOLD bit appears here exactly once, which is what
// makes the mapping checkable by eye against src/core/Config.h, and what
// tests/web/drive_kindle_page.py counts.
//
//   show:  KSHOW_* bit; 0 = always drawn; -1 = a build-time module.
//   bold:  KBOLD_* bit; 0 = no weight of its own.
//   slot:  the one place this region draws; slots: the several it draws.
//   head:  "out"|"in" — the group heading drawn above this region.
//   fill:  ["jump", elementId, label]  a control further down this page
//          ["link", href, label]       the page that owns it
//          ["text", sentence]          nothing configures it
//   Each region's hit target on the preview comes from kdPvBoxes(), which
//   places it where the layout put the region. THEY MUST NOT OVERLAP: the
//   headline's number and the value beside it share a baseline on the real
//   panel, and boxes drawn to match that left the headline's target almost
//   entirely underneath its neighbour's — a region you could only open by
//   aiming at its edge. The three rows that are not regions at all (the
//   tendency arrow, captions, units) get no box, because there is no one
//   place on the panel to point at.
var KD_ZONES = [
  { id:"hero", name:"Headline",
    where:"Top left. The largest number on the page.",
    show:0, bold:0x0001, slot:"hero", head:"out" },

  { id:"big", name:"Beside the headline",
    where:"Shares the headline's baseline, after a slash.",
    show:0x0001, bold:0x0002, slot:"big" },

  { id:"sub", name:"24-hour low-to-high",
    where:"The small line under the headline, with the reading's age.",
    show:0x0008, bold:0,
    fill:["text","Follows the headline's sensor."] },

  { id:"grid", name:"The grid",
    where:"Under that line — up to six, in whichever rows set them largest.",
    show:0x0002, bold:0x0004, slots:["g1","g2","g3","g4","g5","g6"] },

  { id:"tend", name:"Pressure tendency arrow",
    where:"After a pressure reading, wherever one is placed.",
    show:0x0004, bold:0,
    fill:["text","Switched on per place, under “Tendency arrow” — this is the master switch for all of them."] },

  { id:"clock", name:"Clock",
    where:"Top right, above the indoor row.",
    show:0, bold:0x0008,
    fill:["jump","kd-card-clock","Style, time and date"] },

  { id:"inrow", name:"Indoor row",
    where:"Under the clock — up to three readings, sized to fill the column.",
    show:0x0010, bold:0x0010, slots:["in1","in2","in3"], head:"in" },

  { id:"chart", name:"24-hour trend chart",
    where:"Full width, under the two columns.",
    show:0x0020, bold:0,
    fill:["text","Drawn from the stored history of the outdoor and indoor sensors named under “The reader”."] },

  { id:"fc", name:"Weather forecast",
    where:"Under the chart: the condition, and three outlook columns.",
    // NO BOX on a page with no forecast: the band is where the readings above
    // went, so there is nothing to point at. The row stays in the list — its weight
    // switch is a bit of the mask, and a bit with no checkbox in the DOM is a
    // bit dropped on the next Save.
    show:-1, bold:0x0040,
    fill:["link","#settings_modules","Provider, place and outlook"] },

  { id:"week", name:"Week strip",
    where:"The seven days above the footer, today knocked out in black.",
    show:0x0040, bold:0x0080,
    fill:["text","Drawn from the device's own date."] },

  { id:"batt", name:"Low-battery badge",
    where:"Beside the outdoor heading, when a node is nearly flat.",
    show:0x0080, bold:0,
    fill:["link","#settings_nodes","Which nodes report a battery"] },

  // The last two are not regions. They are the two things that appear inside
  // every region, and they own a weight bit each — which is why they used to
  // sit in a "Weight" list alongside eight regions, reading as if they were
  // places on the panel.
  { id:"cap", name:"Captions", span:true,
    where:"The small grey heading above every value, and the two column headings.",
    show:0, bold:0x0100,
    fill:["text","Worded per place, in the region that draws it."] },

  { id:"unit", name:"Units", span:true,
    where:"The suffix after a number — °, %, hPa.",
    show:0, bold:0x0020,
    fill:["jump","kd-card-units","Which units"] }
];

// Kept for the round-trip: kdMaskOf() walks these to collect the checkboxes
// the rows above rendered, and they are the list the firmware's constants are
// compared against. Derived from KD_ZONES rather than written twice, so a zone
// added to the list cannot be left out of the save.
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

// EVERY BIT'S CHECKBOX STAYS IN THE DOM, open row or not — the switches sit on
// the row itself rather than inside the part that expands. That is what keeps
// the masks collectable in one walk, and it is also the right form: whether a
// region is drawn at all is the thing worth seeing without opening anything.
function kdMaskOf(defs, prefix) {
  var mask = 0;
  for (var i = 0; i < defs.length; i++) {
    var el = document.getElementById(prefix + defs[i][0]);
    if (el && el.checked) mask |= defs[i][0];
  }
  return mask;
}

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

function kdSet(id, value) {
  var el = document.getElementById(id);
  if (el) el.value = String(value);
}

function kdVal(id, fallback) {
  var el = document.getElementById(id);
  return el ? el.value : fallback;
}

// ============================================================================
// The working copy
// ============================================================================
// Nothing is sent until Save, so filling in four places is one request rather
// than four and the device can never end up holding half an edit.
var kdZones   = {};     // the eleven places, keyed by place
var kdOrder   = [];     // [{key, group, role}] from the firmware's own enum
var kdSensors = [];     // [{id, name, metrics: []}]
var kdGroups  = { out: "", in: "" };               // the reader's headings
var kdGroupPh = { out: "OUTSIDE", in: "INSIDE" };  // what "" renders as
var kdFlags   = { bold: 1, unit: 2, age: 4, trend: 8 };
var kdInks    = [];     // [{id, css}] from the firmware's own enum
var kdAutoDec = 255;
var kdOpen    = "hero"; // which row is expanded
// Did the DEVICE's places actually arrive? Everything else on this page has a
// sensible empty state; the eleven places do not. An empty kdZones is
// indistinguishable from "eleven empty places", and one Save then posts that
// over a layout nobody has seen — which is a new failure the single Save
// button introduced, because the slots used to have a Save of their own that
// this reader would never have pressed.
// null until the first read answers either way — the rows are drawn once
// before kdLoaded goes up, and "not read yet" must not print the warning.
var kdSlotsOk = null;
var kdBase    = "";     // the snapshot Save/Discard measure against
var kdLoaded  = false;

// The grey levels, in the firmware's order. Named here rather than derived
// from the hex, because "#777" is not a word anybody wants in a dropdown.
var KD_INK_NAME = ["Black", "Dark grey", "Mid grey", "Light grey"];

// What each place is FOR, in the reader's terms. The firmware sends the keys
// and the roles; the wording is the form's own business.
var KD_SLOT_TEXT = {
  hero: ["Headline", "The largest number on the page."],
  big:  ["Beside it", "Shares the headline's baseline, after a slash. Usually the humidity of the same air."],
  g1:   ["First", ""], g2: ["Second", ""], g3: ["Third", ""],
  g4:   ["Fourth", ""], g5: ["Fifth", ""], g6: ["Sixth", ""],
  in1:  ["First", "Set much larger than the other two and drawn with no caption — the heading above it already names the room."],
  in2:  ["Second", ""],
  in3:  ["Third", "Leave it empty for a row of two."]
};

// ── Example readings, for the preview ───────────────────────────────────────
// THE LAYOUT IS WHAT THE PREVIEW IS FOR, not the weather: it answers "where
// will this land and how wide is it", and the device's own /kindle page — one
// click away in the header — answers "what does it say right now". So the
// figures are representative ones per metric, and the preview says so.
var KD_SAMPLE = {
  temperature:"8.4", humidity:"71", humidity_amb:"68", pressure:"1008",
  dew_point:"3.1", aqi:"42", co2:"640", pm25:"12", pm10:"18", voc:"0.4",
  lux:"320", battery:"3.91", rssi:"-68", rain:"0.2", wind:"5"
};
// HOW THE DEVICE FORMATS EACH ONE — decimals and the unit it prints — mirrored
// from KD_METRIC_STYLE in src/web/KindleSlots.h, which is what the panel reads.
// "Automatic" decimals is that table's number, NOT the decimals control on this
// page: that control is the temperature's, and the firmware applies it to
// temperatures and to nothing else. Without this the preview drew "640.0 ppm"
// and "71.0 %" — over-stating the width of every integer reading on a page
// whose whole question is what fits.
var KD_METRIC = {
  temperature:{ d:1, u:"°" },      humidity:{ d:0, u:"%" },
  humidity_amb:{ d:0, u:"%" },     dew_point:{ d:1, u:"°" },
  pressure:{ d:0, u:"" },          aqi:{ d:0, u:"" },
  co2:{ d:0, u:"ppm" },            eco2:{ d:0, u:"ppm" },
  tvoc:{ d:0, u:"ppb" },           pm1:{ d:0, u:"µg/m³" },
  pm25:{ d:0, u:"µg/m³" },         pm4:{ d:0, u:"µg/m³" },
  pm10:{ d:0, u:"µg/m³" },         lux:{ d:0, u:"lx" },
  uva:{ d:1, u:"" },               uvb:{ d:1, u:"" },
  rain:{ d:1, u:"mm" },            rain_rate:{ d:1, u:"mm/h" },
  rain_total:{ d:1, u:"mm" },      wind:{ d:1, u:"" },
  wind_speed:{ d:1, u:"" },        wind_direction:{ d:0, u:"°" },
  soil_moisture:{ d:0, u:"%" },    flow_rate:{ d:1, u:"" },
  battery_voltage:{ d:2, u:"V" },  battery_percent:{ d:0, u:"%" },
  battery_days:{ d:0, u:"d" }
};
// What a metric not in that table falls back to: the firmware uses one decimal
// and whatever unit the reading itself carried, which this page does not have —
// so these are the sensible guesses for the machine metrics a node also sends.
var KD_UNIT = {
  battery:"V", rssi:"dBm", voc:"ppb", wind:"m/s"
};

function kdSlot(key) {
  if (!kdZones[key]) {
    kdZones[key] = { sensor:"", metric:"", label:"", shown:"",
                     flags:kdFlags.unit, decimals:kdAutoDec, ink:0 };
  }
  return kdZones[key];
}

function kdMetricsFor(sensorId) {
  for (var i = 0; i < kdSensors.length; i++) {
    if (kdSensors[i].id === sensorId) return kdSensors[i].metrics || [];
  }
  return [];
}

// The reader's own name for a sensor where they gave it one, because the id is
// the less human of the two and the zone editors used to show only the id
// while the two dropdowns under "The reader" showed both.
function kdSensorLabel(id) {
  for (var i = 0; i < kdSensors.length; i++) {
    if (kdSensors[i].id === id) {
      return kdSensors[i].name ? (id + " — " + kdSensors[i].name) : id;
    }
  }
  return id;
}

// ============================================================================
// The panel, drawn at its own coordinates
// ============================================================================
// Every number below is the number kindle/layout/600x800.conf carries, so the
// preview and the FBInk panel cannot drift on where a thing sits. The browser
// page at /kindle is laid out from the same design.

// How wide a string comes out.
//
// THE COLLECTOR MEASURES THIS FOR THE REAL PANEL — kdAdvanceMille() sends the
// advance of every value in thousandths of the type size, precisely because
// FBInk will not report it and a layout that guesses puts the headline's
// second value through the divider. This is that measurement, estimated per
// character for a lining-figure serif: enough for a preview, and the reason
// the preview needs nothing from the device to draw one.
var KD_EM = {
  "0":.52,"1":.52,"2":.52,"3":.52,"4":.52,"5":.52,"6":.52,"7":.52,"8":.52,
  "9":.52,".":.26,",":.26,"-":.33,"°":.40,"%":.80," ":.25,"/":.30,":":.28
};
function kdTw(text, size) {
  var t = String(text == null ? "" : text), w = 0;
  for (var i = 0; i < t.length; i++) {
    w += (KD_EM[t[i]] !== undefined ? KD_EM[t[i]] : 0.50) * size;
  }
  return Math.round(w);
}

function kdPvInk(n) {
  for (var i = 0; i < kdInks.length; i++) if (kdInks[i].id === n) return kdInks[i].css;
  return ["#000000","#444444","#777777","#aaaaaa"][n] || "#000000";
}

function kdT(x, y, size, text, o) {
  if (text === "" || text == null) return "";
  o = o || {};
  return "<i style='left:" + x + "px;top:" + y + "px;font-size:" + size + "px;color:" +
    (o.ink || "#111111") + ";font-weight:" + (o.bold ? 600 : 400) +
    (o.ls ? ";letter-spacing:" + o.ls + "px" : "") + "'>" + kdEsc(text) + "</i>";
}
function kdBox(x, y, w, h, cls) {
  return "<u class='" + cls + "' style='left:" + x + "px;top:" + y + "px;width:" +
         w + "px;height:" + h + "px'></u>";
}

function kdPvValue(z) {
  if (!z.sensor || !z.metric) return "";
  var v = KD_SAMPLE[z.metric];
  if (v === undefined) return "42";
  // Pressure is re-united here rather than re-formatted: the unit it is shown
  // in decides both the number and its decimals, exactly as the firmware does.
  if (z.metric === "pressure") {
    var n = parseFloat(v), u = kdVal("kd-press", "0") | 0;
    if (u === 1) return String(Math.round(n * 0.750062));
    if (u === 2) return (n * 0.02953).toFixed(2);
    return String(Math.round(n));
  }
  var st = KD_METRIC[z.metric];
  var dec = (z.decimals === kdAutoDec) ? (st ? st.d : 1) : (z.decimals | 0);
  // The decimals control predates the places and still wins for temperatures,
  // which is what the firmware does with it — and only for those.
  if (z.metric === "temperature") dec = kdVal("kd-dec", "1") | 0;
  return parseFloat(v).toFixed(dec > 3 ? 3 : dec);
}
function kdPvUnit(z) {
  if (!(z.flags & kdFlags.unit) || !z.metric) return "";
  if (z.metric === "pressure") return ["hPa","mmHg","inHg"][kdVal("kd-press","0") | 0];
  var st = KD_METRIC[z.metric];
  if (st) return st.u;
  return KD_UNIT[z.metric] || "";
}
function kdPvCaption(z) { return z.label || z.shown || ""; }
function kdPvClock() { return (kdVal("kd-time","0") | 0) === 2 ? "5:40pm" : "17:40"; }
function kdPvDate() {
  return ["27 august","august 27","27.08.2026","2026-08-27"][kdVal("kd-date","0") | 0];
}

// ── Where everything goes: src/web/KindleFlow.h, again ─────────────────────
// The page is not two fixed layouts any more. The collector works out, on
// every render, where each section goes and how large each reading is set
// from what is on the page — which sections are on, whether there is a
// forecast, how many places have a reading and how wide the widest thing each
// can print is — and sends the result to both renderers. This is that same
// calculation, rule for rule, so the preview can draw the page the form
// describes before it is saved. tools/check_kindle_flow_parity.py runs both
// over the same inputs and fails CI on the first number they disagree on;
// change one, change the other.
//
// C's integer division truncates toward zero; kdQ() is that, and every
// division below goes through it so the two agree to the pixel.
function kdQ(a, b) { return (a / b) | 0; }
function kdScaleG(v, g) { return kdQ(v * g + 500, 1000); }

var KDF = {
  FOOT_Y:764, WEEK_H:88, FC_H:124, TOP_Y:20, CHART_ABOVE:26, CHART_BELOW:24,
  CHART_MIN:220, TOP_MIN:262, TOP_GROWN:386, COL_L:270, COL_R:252, CL_Y:26,
  HERO_Y:38, CELL_PAD:6, IN_CAP_W:66, GRID_GAP:6,
  GROW_MAX:1180, GROW_CLOCK:1146, GROW_BIG:1090, GROW_SUB:1120
};

// kdAdvanceMille(): a string's width in thousandths of its type size. The C
// walks UTF-8 bytes; this walks code points to the same answer.
function kdAdvanceMille(s) {
  var t = String(s == null ? "" : s), total = 0;
  for (var i = 0; i < t.length; i++) {
    var c = t.charCodeAt(i), w;
    if (c >= 0xDC00 && c <= 0xDFFF) continue;     // the second half of a pair
    if (c >= 0x80 && c < 0xC0)            w = c === 0xB0 ? 330 : (c === 0xB5 ? 520 : 550);
    else if (c >= 0xC0)                   w = 620;
    else if (c >= 48 && c <= 57)          w = 500;
    else if (".,:'".indexOf(t[i]) >= 0)   w = 260;
    else if ("-+/".indexOf(t[i]) >= 0)    w = 330;
    else if (c === 32)                    w = 250;
    else if (c === 37)                    w = 800;
    else if (c >= 65 && c <= 90)          w = 620;
    else if (c >= 97 && c <= 122)         w = 500;
    else                                  w = 550;
    total += w;
  }
  return total;
}

function kdFlowMinDigits(metric, unit) {
  if (!metric) return 0;
  if (metric === "temperature" || metric === "dew_point") return 2;
  if (["humidity","humidity_amb","soil_moisture","battery_percent",
       "wind_direction"].indexOf(metric) >= 0) return 3;
  if (metric === "pressure") return unit === "mmHg" ? 3 : (unit === "inHg" ? 2 : 4);
  return 0;
}

function kdFlowWorstAdvance(metric, text, unit, arrow) {
  var p = String(text == null ? "" : text), neg = false, worst = "", k = 0;
  if (p[0] === "-" || p[0] === "+") { neg = p[0] === "-"; p = p.slice(1); }
  while (k < p.length && p.charCodeAt(k) >= 48 && p.charCodeAt(k) <= 57) k++;
  var need = Math.max(1, Math.max(k, kdFlowMinDigits(metric, unit)));
  if (neg || metric === "temperature" || metric === "dew_point") worst = "-";
  for (var i = 0; i < need; i++) worst += "0";
  worst += p.slice(k);
  var adv = kdAdvanceMille(worst);
  if (unit) {
    if (unit === "°")    adv += kdQ(330 * 34, 100);
    else if (unit === "%")    adv += kdQ(800 * 42, 100);
    else                      adv += kdQ((250 + kdAdvanceMille(unit)) * 42, 100);
  }
  if (arrow) adv += 350;
  return adv;
}

function kdFlowSplit(n, r) {
  var base = kdQ(n, r), extra = n - base * r, rows = [];
  for (var i = 0; i < r; i++) { rows.push(base + (extra > 0 ? 1 : 0)); if (extra > 0) extra--; }
  return rows;
}

function kdFlowInNeed(s1, a1, adv, m) {
  var w = kdQ(s1 * a1, 1000) + KDF.CELL_PAD, s2 = kdQ(s1 * 6, 10);
  for (var i = 1; i < m; i++) {
    var a = adv[i] || 1000;
    w += Math.max(kdQ(s2 * a, 1000) + KDF.CELL_PAD, KDF.IN_CAP_W);
  }
  return w;
}

function kdFlowTop(inp, T, f) {
  var K = KDF, i, r, c;
  f.topBot = K.TOP_Y + T;
  var g = 1000 + kdQ((K.GROW_MAX - 1000) * (T - K.TOP_MIN), K.TOP_GROWN - K.TOP_MIN);
  g = Math.max(1000, Math.min(K.GROW_MAX, g));
  f.grow = g;
  f.labSz   = g >= K.GROW_BIG ? 15 : 14;
  f.heroSz  = kdScaleG(88, g);
  f.bigSz   = kdScaleG(44, Math.min(g, K.GROW_BIG));
  f.headGap = kdScaleG(8, g);
  f.slashW  = kdScaleG(22, g);
  f.subSz   = kdScaleG(17, Math.min(g, K.GROW_SUB));
  f.heroY   = K.HERO_Y;
  var air = g - 1000;
  f.subY = f.heroY + f.heroSz + 2 + kdQ(air * 14, K.GROW_MAX - 1000);
  var gridTop = inp.sub ? f.subY + f.subSz + 13 + kdQ(air * 6, K.GROW_MAX - 1000) : f.subY;
  var bot = f.topBot - 8;

  // The grid
  f.gridRows = []; f.gridY = gridTop; f.gridRowH = 0; f.gridValSz = 0;
  var n = Math.min(6, inp.nGrid);
  if (n > 0) {
    var areaH = bot - gridTop, cap = kdQ(f.heroSz * 60, 100);
    var rMin = kdQ(n + 2, 3), best = -1, bestR = rMin;
    for (r = rMin; r <= n; r++) {
      var rows = kdFlowSplit(n, r), pitch = kdQ(areaH, r);
      var v = pitch - f.labSz - 4 - K.GRID_GAP, at = 0;
      for (var k = 0; k < r; k++) {
        var cellW = kdQ(K.COL_L, rows[k]) - K.CELL_PAD;
        for (c = 0; c < rows[k]; c++, at++) {
          v = Math.min(v, kdQ(cellW * 1000, inp.gridAdv[at] || 1000));
        }
      }
      v = Math.min(v, cap);
      if (r === rMin) {
        var floorV = rows[0] >= 3 ? 27 : 34;
        v = Math.max(v, Math.min(floorV, pitch - f.labSz - 4 - K.GRID_GAP));
      }
      if (v >= best) { best = v; bestR = r; }
    }
    f.gridRows = kdFlowSplit(n, bestR);
    f.gridValSz = Math.max(best, 10);
    f.gridRowH = kdQ(areaH, bestR);
    f.gridY = gridTop + Math.max(0, kdQ(f.gridRowH - (f.labSz + 4 + f.gridValSz), 2));
  }

  // The clock
  var gc = Math.min(g, K.GROW_CLOCK);
  f.clGrow = gc;
  f.clSize = kdQ(96 * gc, 1000);  f.clBoxed = kdQ(84 * gc, 1000);
  f.clRuled = kdQ(72 * gc, 1000); f.clRuledPad = kdQ(16 * gc, 1000);
  f.clDated = kdQ(66 * gc, 1000); f.clDateSz = kdQ(15 * gc, 1000);
  f.clDateGap = kdQ(6 * gc, 1000);
  f.clH = f.clSize + 1;

  // The indoor row
  f.inRuleY = K.CL_Y + f.clH + 1;
  f.inLabY = f.inRuleY + 10 + kdQ(air * 4, K.GROW_MAX - 1000);
  f.inValSz1 = f.inValSz = 0; f.inW1Pm = 1000; f.inStack = false;
  f.inValY = f.inVal2Y = f.inLabY + f.labSz + 18;
  var m = Math.min(3, inp.nIn);
  if (m > 0) {
    var top = f.inLabY + f.labSz + 18, ah = bot - top, icap = kdQ(f.heroSz * 80, 100);
    var a1 = inp.inAdv[0] || 1000, others = 0;
    for (i = 1; i < m; i++) others += inp.inAdv[i] || 1000;
    var s1 = m === 1 ? kdQ((K.COL_R - K.CELL_PAD) * 1000, a1)
                     : kdQ((K.COL_R - K.CELL_PAD * m) * 1000, a1 + kdQ(others * 6, 10));
    s1 = Math.min(s1, ah, icap);
    s1 = Math.max(s1, Math.min(52, ah));
    while (m > 1 && s1 > 40 && kdFlowInNeed(s1, a1, inp.inAdv, m) > K.COL_R) s1--;
    var s1s = 0;
    if (m >= 2) {
      s1s = kdQ((K.COL_R - K.CELL_PAD) * 1000, a1);
      var s2 = kdQ((K.COL_R - K.CELL_PAD * (m - 1)) * 1000, others);
      s1s = Math.min(s1s, kdQ(s2 * 10, 6), kdQ((ah - 14 - f.labSz) * 10, 16), icap);
    }
    if (s1s > s1 + 2) {
      f.inStack = true; f.inValSz1 = s1s; f.inValSz = kdQ(s1s * 6, 10);
      var hh = f.inValSz1 + 10 + f.labSz + 4 + f.inValSz;
      f.inValY = top + Math.max(0, kdQ(ah - hh, 2));
      f.inVal2Y = f.inValY + f.inValSz1 + 10 + f.labSz + 4;
    } else {
      f.inValSz1 = s1; f.inValSz = kdQ(s1 * 6, 10);
      f.inValY = top + Math.max(0, kdQ(ah - s1, 2));
      f.inVal2Y = f.inValY + f.inValSz1 - f.inValSz;
      if (m > 1) {
        var need1 = kdQ(f.inValSz1 * a1, 1000) + K.CELL_PAD;
        var needO = kdFlowInNeed(f.inValSz1, a1, inp.inAdv, m) - need1;
        f.inW1Pm = kdQ(need1 * 1000, Math.max(1, need1 + needO));
      }
    }
  }
  f.sepH = T - 10;
  return f;
}

function kdFlowSameType(a, b) {
  return a.heroSz === b.heroSz && a.clSize === b.clSize && a.labSz === b.labSz &&
         a.gridValSz === b.gridValSz && a.gridRows.length === b.gridRows.length &&
         a.inValSz1 === b.inValSz1 && a.inStack === b.inStack;
}

// inp: { chart, forecast, week, sub, nGrid, gridAdv[], nIn, inAdv[] }
function kdFlowCompute(inp) {
  var K = KDF, f = { chart:!!inp.chart, forecast:!!inp.forecast, week:!!inp.week };
  f.wkRuleY = inp.week ? K.FOOT_Y - K.WEEK_H : K.FOOT_Y;
  var below = f.wkRuleY - (inp.forecast ? K.FC_H : 0);
  var avail = below - K.TOP_Y - (inp.chart ? K.CHART_ABOVE + K.CHART_BELOW : 0);
  var T = Math.max(K.TOP_MIN, inp.chart ? avail - K.CHART_MIN : avail);
  kdFlowTop(inp, T, f);
  if (inp.chart) {
    for (var t = K.TOP_MIN; t < T; t++) {
      if (kdFlowSameType(kdFlowTop(inp, t, {}), f)) { T = t; break; }
    }
    kdFlowTop(inp, T, f);
    f.rule2Y = f.topBot;
    f.grY = f.rule2Y + K.CHART_ABOVE;
    f.grH = below - K.CHART_BELOW - f.grY;
    f.rule3Y = below;
  } else {
    f.rule2Y = f.rule3Y = f.grY = f.topBot;
    f.grH = 0;
  }
  return f;
}

// Whether the page the preview draws has a forecast band. `auto` is not an
// answer — it is the collector deciding, minute by minute, and the page cannot
// know today's; it draws the ordinary page and says so under the control.
function kdPvForecast() {
  return kdVal("kd-layout", "0") !== "2";
}

// What the collector would work the layout out from, read off the form.
function kdFlowInput(show) {
  var inp = { chart:!!(show & 0x0020), week:!!(show & 0x0040),
              forecast:kdPvForecast(), sub:!!(show & 0x0008),
              nGrid:0, gridAdv:[], nIn:0, inAdv:[], grid:[], inside:[] };
  function adv(key) {
    var z = kdSlot(key), v = kdPvValue(z);
    if (v === "") return 0;
    var arrow = !!(z.flags & kdFlags.trend) && !!(show & 0x0004) && z.metric === "pressure";
    return kdFlowWorstAdvance(z.metric, v, kdPvUnit(z), arrow);
  }
  var i, a;
  if (show & 0x0002) {
    for (i = 1; i <= 6; i++) {
      if ((a = adv("g" + i))) { inp.grid.push("g" + i); inp.gridAdv.push(a); }
    }
  }
  if (show & 0x0010) {
    for (i = 1; i <= 3; i++) {
      if ((a = adv("in" + i))) { inp.inside.push("in" + i); inp.inAdv.push(a); }
    }
  }
  inp.nGrid = inp.grid.length;
  inp.nIn = inp.inside.length;
  return inp;
}

// The layout the preview draws, with the places that are in it.
function kdShape() {
  var show = kdLoaded ? kdMaskOf(KD_SHOW, "kd-s-") : kdShowInit;
  var inp = kdFlowInput(show), f = kdFlowCompute(inp);
  f.grid = inp.grid;
  f.inside = inp.inside;
  return f;
}

// Where each region's hit target goes on this layout, [x, y, w, h]. THEY MUST
// NOT OVERLAP (see KD_ZONES), and on the ordinary page they are exactly the
// rectangles that table used to carry. A region switched off keeps a thin
// target where it would start, so it can still be opened and switched back
// on, without covering what took its room.
function kdPvBoxes(L) {
  var g = L.grow - 1000, b = {};
  var heroW = 136 + kdQ(g * 16, 180), bigX = 12 + heroW;
  b.hero  = [10, 12, heroW, L.subY - 16];
  b.big   = [bigX, 40 + kdQ(g * 8, 180), 294 - bigX, 64 + kdQ(g * 16, 180)];
  b.sub   = [10, L.subY - 2, 284, L.subSz + 7];
  var gy = L.subY + L.subSz + 7;
  b.grid  = [10, gy, 284, L.topBot - 8 - gy];
  b.clock = [314, 20, 272, L.inRuleY - 22];
  b.inrow = [314, L.inRuleY + 2, 272, L.topBot - 8 - (L.inRuleY + 2)];
  b.chart = L.chart ? [8, L.rule2Y, 584, L.rule3Y - L.rule2Y - 10]
                    : [8, L.rule2Y - 6, 584, 6];
  if (L.forecast) b.fc = [8, L.rule3Y, 584, 118];
  b.week  = L.week ? [10, L.wkRuleY - 2, 580, 84] : [10, 758, 580, 6];
  b.batt  = [234, 12, 30, 26];
  return b;
}

function kdRenderPreview() {
  var el = document.getElementById("kd-panel");
  if (!el) return;
  var show = kdMaskOf(KD_SHOW, "kd-s-"), bold = kdMaskOf(KD_BOLD, "kd-b-");
  var capB = !!(bold & 0x0100), unitB = !!(bold & 0x0020);
  var L = kdShape();
  var h = "", i, z, v, u, x, ux, usz;
  var gAir = L.grow - 1000;

  // ── Left column: the headline, its line, its grid ──
  h += kdT(18, 20, L.labSz, kdGroups.out || kdGroupPh.out, { ink:"#777777", bold:capB });
  z = kdSlot("hero"); v = kdPvValue(z);
  h += kdT(18, L.heroY, L.heroSz, v || "—",
           { bold:(z.flags & kdFlags.bold) || (bold & 0x0001), ink:kdPvInk(z.ink) });
  x = 18 + kdTw(v || "—", L.heroSz);
  u = kdPvUnit(z);
  usz = Math.round(L.heroSz * 0.34);
  // The unit and the second value hang off the headline's size rather than
  // carrying coordinates of their own: they sit on its baseline, and a table
  // with three numbers that must move together is a table two of them can be
  // forgotten in.
  h += kdT(x, 44 + Math.round((L.heroSz - 88) / 4), usz, u,
           { ink:"#444444", bold:unitB });
  x += kdTw(u, usz);

  if (show & 0x0001) {
    z = kdSlot("big"); v = kdPvValue(z);
    if (v) {
      var by = 56 + Math.round((L.heroSz - 88) / 2);
      x += L.headGap;
      h += kdT(x, by, L.bigSz, "/", { ink:"#aaaaaa" });
      x += kdTw("/", L.bigSz) + 6;
      h += kdT(x, by, L.bigSz, v,
               { bold:(z.flags & kdFlags.bold) || (bold & 0x0002), ink:kdPvInk(z.ink) });
      h += kdT(x + kdTw(v, L.bigSz), by + 6, Math.round(L.bigSz * 0.42),
               kdPvUnit(z), { ink:"#444444", bold:unitB });
    }
  }
  if (show & 0x0008) {
    h += kdT(18, L.subY, L.subSz, "-2.4 to 15.3°  ·  3 min", { ink:"#777777" });
  }

  if (L.grid.length) {
    // The rows the layout chose — two side by side or one under the other,
    // whichever sets them larger — each centred in its share of the height.
    var rows = [], at = 0;
    for (var r0 = 0; r0 < L.gridRows.length; r0++) {
      rows.push(L.grid.slice(at, at + L.gridRows[r0]));
      at += L.gridRows[r0];
    }
    for (var r = 0; r < rows.length; r++) {
      var cw = Math.floor(270 / rows[r].length), gy = L.gridY + r * L.gridRowH;
      var vs = L.gridValSz;
      for (i = 0; i < rows[r].length; i++) {
        z = kdSlot(rows[r][i]); x = 18 + i * cw; v = kdPvValue(z);
        h += kdT(x, gy, L.labSz, kdPvCaption(z), { ink:"#777777", bold:capB });
        h += kdT(x, gy + L.labSz + 6, vs, v,
                 { bold:(z.flags & kdFlags.bold) || (bold & 0x0004), ink:kdPvInk(z.ink) });
        u = kdPvUnit(z);
        ux = x + kdTw(v, vs) + (u === "°" || u === "%" ? 0 : 3);
        usz = Math.round(vs * (u === "°" ? 0.34 : 0.42));
        var uy = gy + L.labSz + 10;
        h += kdT(ux, uy, usz, u, { ink:"#444444", bold:unitB });
        if ((z.flags & kdFlags.trend) && (show & 0x0004) && z.metric === "pressure") {
          h += kdT(ux + kdTw(u, usz) + 4, uy, 16, "↘", { ink:"#444444" });
        }
      }
    }
  }
  if (show & 0x0080) h += kdBox(240, 18, 22, 22, "kd-pl dk");

  // The hairline between the columns
  h += kdBox(300, 20, 1, L.sepH, "kd-rl");

  // ── Right column: the clock, then the indoor row ──
  var cs = kdVal("kd-clock", "0") | 0, cb = !!(bold & 0x0008);
  if (cs === 1) {
    h += kdBox(318, 26, 264, L.clH, "kd-pl dk");
    h += "<i style='left:340px;top:" + (26 + Math.round(L.clH * 0.19)) +
         "px;font-size:" + L.clBoxed + "px;color:#ffffff;font-weight:" +
         (cb ? 600 : 400) + "'>" + kdEsc(kdPvClock()) + "</i>";
  } else if (cs === 2) {
    h += kdBox(318, 26, 264, 1, "kd-rl");
    h += kdT(340, 26 + Math.round(L.clH * 0.21), L.clRuled, kdPvClock(),
             { bold:cb, ls:2 });
  } else if (cs === 3) {
    h += kdT(318, 26, L.clDated, kdPvClock(), { bold:cb });
    h += kdT(318, 26 + L.clDated + 2 * L.clDateGap, L.clDateSz, kdPvDate(), { ink:"#444444" });
  } else {
    h += kdT(318, 26, L.clSize, kdPvClock(), { bold:cb });
  }

  var ilive = L.inside;
  if (ilive.length) {
    {
      h += kdBox(318, L.inRuleY, 264, 1, "kd-rl soft");
      h += kdT(318, L.inLabY, L.labSz, kdGroups["in"] || kdGroupPh["in"],
               { ink:"#777777", bold:capB });
      // The first field's share is what it needs to be set larger, not a
      // fixed fraction; or it has a line of its own and the others share the
      // one under it.
      var w1 = L.inStack ? 0 : Math.round(264 * L.inW1Pm / 1000);
      var cw2 = ilive.length > 1 ? Math.floor((264 - w1) / (ilive.length - 1)) : 264;
      for (i = 0; i < ilive.length; i++) {
        z = kdSlot(ilive[i]);
        var big = i === 0, ivs = big ? L.inValSz1 : L.inValSz;
        var iy = big ? L.inValY : L.inVal2Y;
        x = big ? 318 : 318 + w1 + (i - 1) * cw2;
        if (!big) h += kdT(x, iy - 18, L.labSz, kdPvCaption(z),
                           { ink:"#777777", bold:capB });
        v = kdPvValue(z);
        h += kdT(x, iy, ivs, v,
                 { bold:(z.flags & kdFlags.bold) || (bold & 0x0010), ink:kdPvInk(z.ink) });
        u = kdPvUnit(z);
        h += kdT(x + kdTw(v, ivs), iy + (big ? 16 : 9),
                 Math.round(ivs * (u === "°" ? 0.34 : 0.42)), u,
                 { ink:"#444444", bold:unitB });
      }
    }
  }

  // ── The chart ──
  h += kdBox(18, L.rule2Y, 564, 1, "kd-rl");
  if (L.chart) {
    h += kdT(18, L.rule2Y + 6, L.labSz, "24 HOURS", { ink:"#777777", bold:capB });
    h += kdBox(20, L.grY, 560, L.grH, "kd-pl");
    for (i = 1; i < 4; i++) h += kdBox(20, L.grY + Math.round(i * L.grH / 4), 560, 1, "kd-rl soft");
    h += "<svg style='left:20px;top:" + L.grY +
         "px;width:560px;height:" + L.grH + "px' viewBox='0 0 560 220'" +
         " preserveAspectRatio='none' aria-hidden='true'>" +
         "<polyline points='0,150 70,140 140,158 210,120 280,96 350,110 420,74 490,88 560,66'" +
         " fill='none' stroke='#111111' stroke-width='3'/>" +
         "<polyline points='0,60 70,62 140,58 210,64 280,60 350,56 420,62 490,58 560,60'" +
         " fill='none' stroke='#777777' stroke-width='2' stroke-dasharray='7 5'/></svg>";
    h += kdT(20, L.grY + L.grH + 2, 15, "outside mean", { ink:"#444444" });
  }

  // ── The forecast, on the page that has one ──
  // Not drawn at all on the standalone page: the band is where the readings
  // above went, and drawing it here would be a preview of a page the reader
  // will never see. Its row in the list stays, marked — see kdRenderZones.
  if (L.forecast) {
    var fy = L.rule3Y;
    h += kdBox(18, fy + 0, 564, 1, "kd-rl");
    h += kdT(18, fy + 6, 14, "FORECAST", { ink:"#777777", bold:capB });
    h += kdBox(18, fy + 28, 52, 52, "kd-pl");
    h += kdT(78, fy + 28, 31, "Showers", { bold:!!(bold & 0x0040) });
    h += kdT(78, fy + 62, 33, "14°/3°", { bold:true });
    h += kdT(78, fy + 100, 17, "wind 23 km/h · 8 min", { ink:"#444444" });
    for (i = 0; i < 3; i++) {
      h += kdBox(320 + i * 90, fy + 8, 88, 92, "kd-pl");
      h += kdT(326 + i * 90, fy + 12, 14, ["21:00","00:00","03:00"][i], { ink:"#777777", bold:capB });
      h += kdBox(340 + i * 90, fy + 32, 34, 34, "kd-wk we");
      h += kdT(340 + i * 90, fy + 70, 22, ["6°","4°","3°"][i], { bold:true });
    }
  }

  // ── The week strip ──
  if (L.week) {
    var wy = L.wkRuleY;
    h += kdBox(18, wy + 0, 564, 1, "kd-rl");
    h += kdT(18, wy + 5, 15, "august", { ink:"#777777", bold:capB });
    var nm = ["MO","TU","WE","TH","FR","SA","SU"], dy = [24,25,26,27,28,29,30];
    for (i = 0; i < 7; i++) {
      var cls = i === 3 ? "kd-wk today" : (i >= 5 ? "kd-wk we" : "kd-wk");
      h += kdBox(18 + i * 81, wy + 24, 81, 58, cls);
      h += kdT(40 + i * 81, wy + 30, 14, nm[i], { ink:i === 3 ? "#ffffff" : "#777777" });
      h += kdT(44 + i * 81, wy + 50, 30, dy[i],
               { ink:i === 3 ? "#ffffff" : "#111111", bold:!!(bold & 0x0080) });
    }
  }

  // ── The footer, and the status line the FBInk panel draws ──
  h += kdBox(18, 764, 564, 1, "kd-rl");
  h += kdT(18, 772, 15, "measured on site", { ink:"#545c68" });

  // ── The hit targets ──
  // A button per region, over the drawing, so the picture is the index. Each
  // zone's rectangle is where the layout put that region — kdPvBoxes() — and
  // a region the page does not have gets no target at all: the forecast's row
  // stays in the list on a page with no forecast, but there is nothing on the
  // drawing to point at.
  var boxes = kdPvBoxes(L);
  for (i = 0; i < KD_ZONES.length; i++) {
    var d = KD_ZONES[i];
    var bx = boxes[d.id];
    if (!bx) continue;
    var off = d.show > 0 && !(show & d.show);
    h += "<button type='button' class='kd-hit" + (off ? " off" : "") +
         (kdOpen === d.id ? " on" : "") + "' data-click='kindleZoneOpen'" +
         " data-args='[\"" + d.id + "\"]' title='" + kdEsc(kdZoneText(d, "name")) +
         "' aria-label='" + kdEsc(kdZoneText(d, "name")) + "' style='left:" + bx[0] + "px;top:" +
         bx[1] + "px;width:" + bx[2] + "px;height:" + bx[3] + "px'></button>";
  }

  el.innerHTML = h;
  kdFitPreview();
}

// The 600x800 panel is drawn at its real size and scaled as one block, so
// every coordinate above is the coordinate the layout file carries.
function kdFitPreview() {
  var scaler = document.getElementById("kd-pv-scaler");
  var panel = document.getElementById("kd-panel");
  if (!scaler || !panel || !scaler.parentElement) return;
  // clientWidth includes the stage's padding on both sides.
  var avail = scaler.parentElement.clientWidth - 28;
  if (avail <= 0) return;
  var k = Math.min(1, avail / 600);
  panel.style.transform = "scale(" + k + ")";
  scaler.style.width = Math.round(600 * k) + "px";
  scaler.style.height = Math.round(800 * k) + "px";
}

// ============================================================================
// The zone list: one row per region, the open one carrying its editors
// ============================================================================
// The zone table above keeps its English inline, next to each region's bit
// values — that adjacency is what makes the mapping checkable by eye against
// src/core/Config.h, so it is not replaced by keys. Instead the three text
// fields are looked up per region at render time and fall back to the
// table's own wording when a translation is absent.
//   kdZoneText(d, "name") → kindle.z_<id>_name
function kdZoneText(d, field) {
  var fallback = field === "fill" ? (d.fill && (d.fill[0] === "text" ? d.fill[1] : d.fill[2])) : d[field];
  if (!window.I18n) return fallback;
  var key = "kindle.z_" + d.id + "_" + field;
  var s = I18n.t(key);
  return s === key ? fallback : s;   // t() echoes the key when it has none
}

// NOT kdT() — that is the panel's text-drawing helper (x, y, size, …). This
// is the page's translator.
function kdI18n(key, vars) {
  return window.I18n ? I18n.t("kindle." + key, vars) : key;
}

function kdSwitch(bit, prefix, mask, label, none) {
  if (bit > 0) {
    return "<label class='kd-sw'><input type='checkbox' id='" + prefix + bit + "'" +
           ((mask & bit) ? " checked" : "") + " data-change='kindleTouched'>" +
           kdEsc(label) + "</label>";
  }
  return "<span class='kd-sw dim'>" + kdEsc(none) + "</span>";
}

function kdFillNote(fill, d) {
  if (!fill) return "";
  var label = d ? kdZoneText(d, "fill") : (fill[0] === "text" ? fill[1] : fill[2]);
  if (fill[0] === "jump") {
    return "<button class='btn sm' data-click='kindleJump' data-args='[\"" +
           fill[1] + "\"]'>" + kdEsc(label) + " <span data-icon='arrow-down'></span></button>";
  }
  if (fill[0] === "link") {
    return "<a class='btn sm' href='" + kdEsc(fill[1]) + "'>" + kdEsc(label) +
           " <span data-icon='arrow-right'></span></a>";
  }
  return "<p class='hint' style='margin:0'>" + kdEsc(label) + "</p>";
}

// What the collapsed row says about what is in the region: the reading for a
// region that draws one, how many of them for a region that draws several.
function kdZoneSummary(d) {
  if (d.slot) {
    var z = kdSlot(d.slot);
    if (!z.sensor || !z.metric) return "<span class='badge dim'>" + kdEsc(kdI18n("zoneEmpty")) + "</span>";
    return kdEsc(z.sensor + " · " + z.metric);
  }
  if (d.slots) {
    var n = 0;
    for (var i = 0; i < d.slots.length; i++) {
      var s = kdSlot(d.slots[i]);
      if (s.sensor && s.metric) n++;
    }
    return n + " of " + d.slots.length + " filled";
  }
  return "";
}

function kdHeadingField(which) {
  return "<div class='field' style='max-width:280px'>" +
    "<label class='field-label'>Heading on the page</label>" +
    "<input class='input' maxlength='16' placeholder='" + kdEsc(kdGroupPh[which]) +
      "' value='" + kdEsc(kdGroups[which] || "") + "' data-change='kindleGroupEdit'" +
      " data-args='[\"" + which + "\"]'>" +
    "<p class='hint'>Leave it blank for the built-in wording.</p></div>";
}

// One place: which sensor, which reading, what to call it, and how it is drawn.
//
// The sensor and metric dropdowns come from GET /api/sensors, which already
// reports the metrics each sensor publishes. That is what makes the editor
// honest about hardware: a BMP280 offers temperature and pressure and no
// humidity, because that is what it measures, and a BME688 offers AQI because
// it has one.
function kdSlotEditor(key) {
  var z = kdSlot(key), text = KD_SLOT_TEXT[key] || [key, ""];

  // The sensor this place names may not be among the configured ones — a node
  // that has been removed, or a layout restored from another device. Kept as an
  // option rather than silently reassigned: dropping it would rewrite the
  // reader's page on their behalf just because a sensor was offline.
  var sensorOpts = "<option value=''" + (z.sensor ? "" : " selected") +
                   ">&mdash; empty &mdash;</option>";
  var known = false;
  for (var j = 0; j < kdSensors.length; j++) {
    var sel = kdSensors[j].id === z.sensor;
    if (sel) known = true;
    sensorOpts += "<option value='" + kdEsc(kdSensors[j].id) + "'" +
                  (sel ? " selected" : "") + ">" + kdEsc(kdSensorLabel(kdSensors[j].id)) +
                  "</option>";
  }
  if (!known && z.sensor) {
    sensorOpts += "<option value='" + kdEsc(z.sensor) + "' selected>" +
                  kdEsc(z.sensor) + " (not configured)</option>";
  }

  var metrics = kdMetricsFor(z.sensor), metricOpts = "", mKnown = false;
  for (var m = 0; m < metrics.length; m++) {
    var msel = metrics[m] === z.metric;
    if (msel) mKnown = true;
    metricOpts += "<option value='" + kdEsc(metrics[m]) + "'" +
                  (msel ? " selected" : "") + ">" + kdEsc(metrics[m]) + "</option>";
  }
  if (!mKnown && z.metric) {
    metricOpts = "<option value='" + kdEsc(z.metric) + "' selected>" +
                 kdEsc(z.metric) + " (not reported)</option>" + metricOpts;
  }

  var empty = !z.sensor || !z.metric;
  // The id is what the preview's hit target and the zone rows scroll to.
  return "<div class='kd-slot' id='kd-zone-" + key + "'>" +
    "<div class='kd-slot-head'>" +
      "<strong>" + kdEsc(text[0]) + "</strong>" +
      (empty ? "<span class='badge dim'>" + kdEsc(kdI18n("zoneEmpty")) + "</span>"
             : "<span class='badge acc'>" +
               kdEsc(kdPvValue(z) + kdPvUnit(z)) + "</span>") +
      "<span style='flex:1'></span>" +
      (empty ? "" : "<button class='btn sm' data-click='kindleSlotClear' data-args='[\"" +
                    key + "\"]'><span data-icon='trash'></span> Empty it</button>") +
    "</div>" +
    (text[1] ? "<p class='hint' style='margin:0 0 8px'>" + kdEsc(text[1]) + "</p>" : "") +
    "<div class='kd-fields'>" +
      "<div class='field'><label class='field-label'>Sensor</label>" +
        "<select class='input' data-change='kindleSlotEdit' data-args='[\"" + key +
        "\",\"sensor\"]'>" + sensorOpts + "</select></div>" +
      "<div class='field'><label class='field-label'>Reading</label>" +
        "<select class='input' data-change='kindleSlotEdit' data-args='[\"" + key +
        "\",\"metric\"]'>" + metricOpts + "</select></div>" +
      "<div class='field'><label class='field-label'>Caption</label>" +
        "<input class='input' maxlength='16' placeholder='" + kdEsc(z.shown || "") +
        "' value='" + kdEsc(z.label || "") + "' data-change='kindleSlotEdit'" +
        " data-args='[\"" + key + "\",\"label\"]'></div>" +
      "<div class='field'><label class='field-label'>Decimals</label>" +
        kdDecimalSelect(key, z.decimals) + "</div>" +
      "<div class='field'><label class='field-label'>Ink</label>" +
        kdInkSelect(key, z.ink | 0) + "</div>" +
    "</div>" +
    "<div class='kd-flags'>" +
      kdFlagBox(key, kdFlags.unit,  kdI18n("flagUnit")) +
      kdFlagBox(key, kdFlags.bold,  kdI18n("swBold")) +
      kdFlagBox(key, kdFlags.age,   kdI18n("flagAge")) +
      (z.metric === "pressure"
        ? kdFlagBox(key, kdFlags.trend, kdI18n("flagTrend")) : "") +
    "</div></div>";
}

function kdDecimalSelect(key, dec) {
  var html = "<select class='input' data-change='kindleSlotEdit' data-args='[\"" +
             key + "\",\"decimals\"]'><option value='" + kdAutoDec + "'" +
             (dec === kdAutoDec ? " selected" : "") + ">Automatic</option>";
  for (var d = 0; d <= 3; d++) {
    html += "<option value='" + d + "'" + (dec === d ? " selected" : "") + ">" + d + "</option>";
  }
  return html + "</select>";
}

// How dark the value is drawn. Four levels rather than a colour picker: the
// panel has sixteen real grey levels and the ones worth having are the ones far
// enough apart to render solid, which is what the page's palette already is.
function kdInkSelect(key, ink) {
  var html = "<select class='input' data-change='kindleSlotEdit' data-args='[\"" +
             key + "\",\"ink\"]'>";
  var n = kdInks.length ? kdInks.length : KD_INK_NAME.length;
  for (var i = 0; i < n; i++) {
    html += "<option value='" + i + "'" + (ink === i ? " selected" : "") + ">" +
            kdEsc(KD_INK_NAME[i] || ("Level " + i)) + "</option>";
  }
  return html + "</select>";
}

function kdFlagBox(key, bit, label) {
  var on = (kdSlot(key).flags & bit) !== 0;
  return "<label class='kd-flag'><input type='checkbox'" + (on ? " checked" : "") +
         " data-change='kindleSlotFlag' data-args='[\"" + key + "\"," + bit + "]'>" +
         kdEsc(label) + "</label>";
}

function kdRenderZones() {
  var box = document.getElementById("kd-zones");
  if (!box) return;
  var show = kdLoaded ? kdMaskOf(KD_SHOW, "kd-s-") : kdShowInit;
  var bold = kdLoaded ? kdMaskOf(KD_BOLD, "kd-b-") : kdBoldInit;

  var html = "", spanOpened = false;
  // The forecast's row on a page with no forecast band. It stays in the list —
  // its weight switch is a bit of the mask, and a bit with no checkbox in the
  // DOM is a bit dropped on the next Save — but it says what it is.
  var noFc = !kdShape().forecast;
  // SAID WHERE THE ROWS ARE. A slots read that failed leaves every place
  // looking empty, and eleven empty rows are exactly what a device with
  // nothing configured looks like — so without this the form quietly
  // misdescribes a reader that may be fully set up. kindleSave() will not
  // send them; this is the half the reader can see.
  if (kdSlotsOk === false) {
    // Carries its own <strong>; it comes from the static dictionary, never
    // from device data, so it is not escaped.
    html += "<p class='hint' id='kd-zones-unread' style='margin:0 0 10px'>" +
            kdI18n("zonesUnread") + "</p>";
  }
  for (var i = 0; i < KD_ZONES.length; i++) {
    var d = KD_ZONES[i], open = kdOpen === d.id;

    // The two page-wide rows are separated by a heading rather than just sat
    // at the bottom: they are not places on the panel, and a list of regions
    // that ends with two non-regions is the confusion this replaced.
    if (d.span && !spanOpened) {
      spanOpened = true;
      html += "<p class='kd-zsplit'>" + kdEsc(kdI18n("zsplit")) + "</p>";
    }

    var body = "";
    if (open) {
      if (d.head) body += kdHeadingField(d.head);
      if (d.slot) body += kdSlotEditor(d.slot);
      if (d.slots) for (var s = 0; s < d.slots.length; s++) body += kdSlotEditor(d.slots[s]);
      if (d.fill) body += "<div class='kd-fillnote'>" + kdFillNote(d.fill, d) + "</div>";
    }

    html +=
      "<div class='kd-zone" + (open ? " open" : "") + "' id='kd-zrow-" + d.id + "'>" +
        "<div class='kd-zbar'>" +
          "<button type='button' class='kd-zhead' data-click='kindleZoneOpen'" +
            " data-args='[\"" + d.id + "\"]' aria-expanded='" + (open ? "true" : "false") +
            "' aria-controls='kd-zbody-" + d.id + "'>" +
            "<span class='kd-zchev' data-icon='chevron-right'></span>" +
            "<span class='kd-ztext'><strong>" + kdEsc(kdZoneText(d, "name")) + "</strong>" +
              "<span class='kd-zwhere'>" + kdEsc(kdZoneText(d, "where")) + "</span></span>" +
            "<span class='kd-zsum'>" +
              ((noFc && d.id === "fc")
                ? "<span class='badge dim'>" + kdEsc(kdI18n("notOnThisPage")) + "</span>"
                : kdZoneSummary(d)) + "</span>" +
          "</button>" +
          "<div class='kd-zsw'>" +
            kdSwitch(d.show, "kd-s-", show, kdI18n("swShown"), d.show === -1 ? kdI18n("swModule") : kdI18n("swAlways")) +
            kdSwitch(d.bold, "kd-b-", bold, kdI18n("swBold"), "—") +
          "</div>" +
        "</div>" +
        "<div class='kd-zbody' id='kd-zbody-" + d.id + "'" + (open ? "" : " hidden") + ">" +
          body +
        "</div>" +
      "</div>";
  }
  box.innerHTML = html;

  var filled = 0, keys = Object.keys(kdZones);
  for (var k = 0; k < keys.length; k++) {
    if (kdZones[keys[k]].sensor && kdZones[keys[k]].metric) filled++;
  }
  var count = document.getElementById("kd-slot-count");
  if (count) count.textContent = kdI18n("placesFilled", { filled: filled, total: kdOrder.length || 11 });

  if (window.Icons && Icons.swap) Icons.swap(box);
}

// The masks before the first render, when there are no checkboxes to read yet.
var kdShowInit = 0xFF, kdBoldInit = 0;

// ── Opening a region ────────────────────────────────────────────────────────
// From the list or from the panel; either way the row expands and the panel
// marks it. An index whose entries are not links is just a list.
function kindleZoneOpen(id) {
  kdOpen = (kdOpen === id) ? "" : id;
  kdRenderZones();
  kdRenderPreview();
  if (!kdOpen) return;
  var row = document.getElementById("kd-zrow-" + id);
  if (!row) return;
  try { row.scrollIntoView({ block:"nearest", behavior:"smooth" }); }
  catch (e) { /* older engines: the expansion is enough */ }
}

// Take the reader to the control that fills a region, and flash it, rather
// than naming it and leaving them to find it four cards down.
function kindleJump(id) {
  var el = document.getElementById(id);
  if (!el) return;
  // Redesign 3a split the page into Zones / Whole page / Reader tabs, so a
  // target this jumps to (e.g. Clock, from the "hero" zone's fill note) can
  // now be sitting in a tab that isn't the active one — switch first, or the
  // scroll lands on a hidden element and does nothing.
  var panel = el.closest("[data-panel]");
  if (panel && panel.hidden) kdActivateTab(panel.getAttribute("data-panel"));
  try { el.scrollIntoView({ block:"center", behavior:"smooth" }); }
  catch (e) { el.scrollIntoView(); }
  el.classList.add("kd-flash");
  setTimeout(function () { el.classList.remove("kd-flash"); }, 1400);
}

// ── Inspector tabs (redesign 3a) ────────────────────────────────────────────
// Purely a visibility switch over the three [data-panel] groups; nothing
// about the form/save/preview logic changes with the active tab.
function kdActivateTab(name) {
  var tabs = document.getElementById("kd-tabs");
  if (tabs) {
    Array.prototype.forEach.call(tabs.querySelectorAll("button[data-tab]"), function (b) {
      b.classList.toggle("active", b.getAttribute("data-tab") === name);
    });
  }
  Array.prototype.forEach.call(document.querySelectorAll("[data-panel]"), function (p) {
    p.hidden = p.getAttribute("data-panel") !== name;
  });
}

function kdTabInit() {
  var tabs = document.getElementById("kd-tabs");
  if (!tabs || tabs._kdWired) return;
  tabs._kdWired = true;
  tabs.addEventListener("click", function (ev) {
    var b = ev.target.closest("button[data-tab]");
    if (!b) return;
    kdActivateTab(b.getAttribute("data-tab"));
  });
}

// ============================================================================
// One save, and a bar that says what is in it
// ============================================================================
// EVERY EDIT LANDS HERE. The two endpoints are written together by one button,
// so there is no longer a working copy that a Save somewhere else does not
// cover — which is what made editing a place and pressing Save store nothing
// and report success.
function kdSnapshot() {
  return JSON.stringify({
    face:kdVal("kd-face","0"),
    face_custom:(document.getElementById("kd-face-custom") || {}).value || "",
    lang:kdVal("kd-lang","0"),
    clock:kdVal("kd-clock","0"), time:kdVal("kd-time","0"), date:kdVal("kd-date","0"),
    press:kdVal("kd-press","0"), dec:kdVal("kd-dec","1"),
    refresh:kdVal("kd-refresh",""), follow:kdVal("kd-follow","1"),
    pin:kdVal("kd-clockpin","1"), res:kdVal("kd-fbink-res","0"),
    layout:kdVal("kd-layout","0"),
    out:kdVal("kd-outdoor-sensor",""), inn:kdVal("kd-indoor-sensor",""),
    show:kdMaskOf(KD_SHOW,"kd-s-"), bold:kdMaskOf(KD_BOLD,"kd-b-"),
    zones:kdZones, groups:kdGroups
  });
}

function kdDirtyRefresh() {
  var bar = document.getElementById("kd-savebar");
  var out = document.getElementById("kd-dirty");
  if (!bar || !out) return;
  if (!kdLoaded) { bar.hidden = true; return; }
  var now = kdSnapshot();
  if (now === kdBase) { bar.hidden = true; return; }

  // WHAT is unsaved, not just that something is — a count against a page this
  // long is the difference between "I can press Save" and "what did I touch".
  var a = JSON.parse(kdBase), b = JSON.parse(now), parts = [], n = 0, k;
  for (k in b) {
    if (k === "zones" || k === "groups") continue;
    if (String(a[k]) !== String(b[k])) n++;
  }
  if (n) parts.push(n + (n === 1 ? " setting" : " settings"));
  var z = 0;
  for (k in b.zones) {
    if (JSON.stringify(a.zones[k]) !== JSON.stringify(b.zones[k])) z++;
  }
  if (z) parts.push(z + (z === 1 ? " place" : " places"));
  if (JSON.stringify(a.groups) !== JSON.stringify(b.groups)) parts.push("a heading");

  out.innerHTML = "<strong>Unsaved:</strong> " +
                  kdEsc(parts.length ? parts.join(", ") : "changes");
  bar.hidden = false;
}

// Every control on the page routes here through data-change, so nothing can be
// edited without the bar noticing. The ones that also change what the panel
// looks like redraw it; they all do, in practice, which is the point.
//
// IT DOES NOT REBUILD THE REGION ROWS. They are innerHTML, so rebuilding them
// destroys the control the reader has just used and takes the focus out of it
// — a switch that cannot be toggled twice without reaching for it again. The
// rows are rebuilt where their CONTENT changes: a place's sensor (the summary
// says which), a row opening, and the two unit controls, which decide what the
// open row's value badges read.
function kindleTouched(ev) {
  var el = (this && this.nodeType === 1) ? this : (ev && ev.target);
  var id = el ? el.id : "";
  kindleClockChanged();
  // kd-layout is in the list because the region rows say which page they are
  // describing: the forecast's row is marked when the page has no band for it.
  if (id === "kd-press" || id === "kd-dec" || id === "kd-layout") kdRenderZones();
  kdRenderPreview();
  kdCadenceRender();
  kdLayoutRender();
  kdDirtyRefresh();
}

function kindleSlotEdit(key, field, ev) {
  var z = kdSlot(key);
  var v = kdEventValue(this, ev, "value");
  if (v === undefined) return;
  if (field === "decimals" || field === "ink") z[field] = parseInt(v, 10);
  else                                         z[field] = v;

  // Changing the sensor can invalidate the metric — a BME688's "aqi" means
  // nothing on a BMP280. Reset to the new sensor's first reading rather than
  // leaving a pairing that will never resolve.
  if (field === "sensor") {
    if (!v) { z.metric = ""; }
    else {
      var ms = kdMetricsFor(v);
      if (ms.indexOf(z.metric) < 0) z.metric = ms.length ? ms[0] : "";
    }
  }
  // THE ROW SAYS WHAT IS IN IT, so the three fields that change what it says
  // redraw it. The metric is most of the row: the summary under the name, the
  // value badge beside it, and the Tendency arrow — which exists only for
  // pressure, and stayed on screen with its flag still set after the place was
  // moved to a metric the device ignores it for.
  //
  // Not the caption: it is typed, the dispatcher fires on "input" as well as
  // "change", and rebuilding on a keystroke takes the cursor with it. These
  // three are dropdowns, which commit once.
  if (field === "sensor" || field === "metric" || field === "decimals") {
    kdRenderZones();
  }
  // The panel always, because every one of these fields appears on it — the
  // caption included, which is why a caption being typed still redraws this
  // much and no more.
  kdRenderPreview();
  kdDirtyRefresh();
}

function kindleSlotFlag(key, bit, ev) {
  var on = kdEventValue(this, ev, "checked");
  if (on === undefined) return;
  var z = kdSlot(key);
  z.flags = on ? (z.flags | bit) : (z.flags & ~bit);
  kdRenderPreview();
  kdDirtyRefresh();
}

function kindleSlotClear(key) {
  kdZones[key] = { sensor:"", metric:"", label:"", shown:"",
                   flags:kdFlags.unit, decimals:kdAutoDec, ink:0 };
  kdRenderZones();
  kdRenderPreview();
  kdDirtyRefresh();
}

function kindleGroupEdit(which, ev) {
  var v = kdEventValue(this, ev, "value");
  if (v === undefined) return;
  kdGroups[which] = v;
  kdRenderPreview();
  kdDirtyRefresh();
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

// ── The cadence, as three named choices ─────────────────────────────────────
// "How often should the page reload" is not a question anybody wants to answer
// in seconds against an interval whose cost is not written down.
var KD_CAD = {
  fast:     { sec:60,  follow:1, pin:1,
              says:"Reloads at most once a minute. The clock is never wrong; the panel flashes most often and the battery goes fastest." },
  balanced: { sec:300, follow:1, pin:1,
              says:"Reloads at most every five minutes, and right after new data arrives. This is the built-in behaviour." },
  saver:    { sec:600, follow:0, pin:0,
              says:"Reloads at most every ten minutes and ignores new data until then. The longest battery life; the clock can be ten minutes behind." }
};

// "By hand…" once chosen stays chosen. kdCadenceName() reads the VALUES, and
// the values in the custom fields routinely match a preset — they start at
// whatever the last named choice left in them. So every edit anywhere on the
// page hid the fields the reader had just opened, and typing 600 into the
// interval passed through 60 on the way, which IS a preset: the field being
// typed into vanished after the second keystroke and took the focus with it.
var kdCadOpen = false;

function kdCadenceName() {
  var sec = kdVal("kd-refresh", "") | 0;
  var follow = kdVal("kd-follow", "1"), pin = kdVal("kd-clockpin", "1");
  for (var k in KD_CAD) {
    if (KD_CAD[k].sec === sec && String(KD_CAD[k].follow) === follow &&
        String(KD_CAD[k].pin) === pin) return k;
  }
  return "custom";
}

function kdCadenceRender() {
  var seg = document.getElementById("kd-cad");
  if (!seg) return;
  var name = kdCadenceName();
  var b = seg.querySelectorAll("button");
  var marked = kdCadOpen ? "custom" : name;
  for (var i = 0; i < b.length; i++) {
    var raw = b[i].getAttribute("data-args") || "[]";
    var mine = raw.indexOf('"' + marked + '"') >= 0;
    b[i].classList.toggle("active", mine);
    b[i].setAttribute("aria-pressed", mine ? "true" : "false");
  }
  var custom = document.getElementById("kd-cad-custom");
  if (custom) custom.style.display = (name === "custom" || kdCadOpen) ? "" : "none";
  var says = document.getElementById("kd-cad-says");
  if (says) says.textContent = KD_CAD[name] ? KD_CAD[name].says : "";
  var badge = document.getElementById("kd-cad-badge");
  if (badge) {
    var sec = kdVal("kd-refresh", "") | 0;
    badge.textContent = sec ? ("≤ " + sec + " s") : "as built";
  }
}

function kindleCadence(name) {
  if (name === "custom") {
    // Nothing to set: the fields are simply revealed, holding whatever the
    // named choice last left in them. The latch is what keeps them revealed.
    kdCadOpen = true;
    kdCadenceRender();
    var f = document.getElementById("kd-refresh");
    if (f) f.focus();
    return;
  }
  var c = KD_CAD[name];
  if (!c) return;
  kdCadOpen = false;
  kdSet("kd-refresh", c.sec);
  kdSet("kd-follow", c.follow);
  kdSet("kd-clockpin", c.pin);
  kindleTouched();
}

// What the chosen shape means, said under the control. `auto` is the one that
// needs saying: it is not a shape, it is the collector deciding minute by
// minute, so the preview beside it is the ordinary page and the reader has to
// know that is a default rather than a promise.
var KD_LAYOUT_SAYS = {
  "0": "The collector decides: the standalone page while it is an access point, " +
       "built without the forecast module, or unable to refresh the forecast for " +
       "six hours. The preview shows the ordinary page.",
  "1": "The forecast band is kept even when there is nothing to put in it.",
  "2": "The forecast band is gone on every page, and the readings above are a " +
       "sixth larger."
};

function kdLayoutRender() {
  var el = document.getElementById("kd-layout-says");
  if (el) el.textContent = KD_LAYOUT_SAYS[kdVal("kd-layout", "0")] || "";
}

// ── The custom face field, and the date format ──────────────────────────────
// Both are hidden rather than disabled when they do not apply: a control that
// is visible but does nothing is a question the page is asking and then
// ignoring the answer to.
function kindleFaceChanged() {
  var row = document.getElementById("kd-face-custom-row");
  if (row) row.style.display = (kdVal("kd-face", "0") === "6") ? "" : "none";
  kindleTouched();
}

function kindleClockChanged() {
  var hint = document.getElementById("kd-date-hint");
  var dated = kdVal("kd-clock", "0") === "3";
  if (hint) hint.style.display = dated ? "none" : "";
  var field = document.getElementById("kd-date-field");
  if (field) field.classList.toggle("kd-dim", !dated);
}

// ── Asking the reader what it is ────────────────────────────────────────────
// /kindle/probe is served BY THE COLLECTOR and reports what the requesting
// browser is, so it has to be opened on the reader itself. The page cannot
// answer the question from here — it can say precisely where the answer is.
function kindleProbe() {
  var out = document.getElementById("kd-probe-out");
  if (out) {
    out.innerHTML =
      "Open <code>/kindle/probe</code> <strong>in the reader's own browser</strong> " +
      "— it reports the viewport and user agent of whatever asked for it, so " +
      "asking from here would describe this computer. Set the size it names above.";
  }
  window.open("/kindle/probe", "_blank", "noopener");
}

// ============================================================================
// Load and save
// ============================================================================
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

  kdSet("kd-refresh",   d.refresh_sec || "");
  kdSet("kd-follow",    (d.follow_data === 0) ? 0 : 1);
  kdSet("kd-clockpin",  (d.clock_pin_refresh === 0) ? 0 : 1);
  kdSet("kd-fbink-res", d.fbink_res_w || 0);
  kdSet("kd-layout",    d.layout_mode || 0);
  kdSet("kd-outdoor-sensor", d.outdoor_sensor || "");
  kdSet("kd-indoor-sensor",  d.indoor_sensor || "");

  // THE BADGE IS ABOUT THE DRAWING UNDER IT, which is the 600 px layout these
  // coordinates come from — KINDLE_PAGE_W scales every size in the firmware's
  // stylesheet from that same 600, so the proportions hold at any width. Both
  // the FBInk resolution and a build-time page width are numbers for something
  // else, and either one printed here alone says the preview is a width it is
  // not.
  var size = document.getElementById("kd-pv-size");
  if (size) {
    var w = (d.page_w | 0) || 600;
    size.textContent = (w === 600) ? "600 px wide"
                                   : ("600 px wide · this build draws " + w);
  }

  // Stated, not settable. The width is a build-time constant because every
  // size in the page's stylesheet is derived from it, so a reader on which the
  // layout looks wrong needs to know where the number came from rather than
  // hunting for the control that would change it.
  var pw = document.getElementById("kd-pagew");
  if (pw && d.page_w) {
    pw.innerHTML = "The browser page's layout is " + (d.page_w | 0) +
      " px wide, fixed when the firmware was built; <code>/kindle/probe</code>" +
      " on the reader itself says what it should be.";
  }

  kdShowInit = d.show | 0;
  kdBoldInit = d.bold | 0;
  kdRenderZones();
  kindleFaceChangedQuiet();
  kindleClockChanged();
  kdLayoutRender();
  // What the DEVICE holds decides whether the fields start open; the latch is
  // the reader's choice within this page load, and a reload is not one.
  kdCadOpen = false;
  kdCadenceRender();
  kdRenderPreview();
}

// The same as kindleFaceChanged without the touch: used while loading, where
// every control is being set to what the device already holds.
function kindleFaceChangedQuiet() {
  var row = document.getElementById("kd-face-custom-row");
  if (row) row.style.display = (kdVal("kd-face", "0") === "6") ? "" : "none";
}

function kindleRefresh() {
  kdLoaded = false;
  return Promise.all([
    fetchWithTimeout("/api/kindle/config", {}, 15000).then(function (r) {
      if (r.status === 404) throw new Error("not-in-build");
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    }),
    fetchWithTimeout("/api/kindle/slots", {}, 15000)
      .then(function (r) { return r.ok ? r.json() : null; })
      .catch(function () { return null; })
  ])
    .then(function (both) {
      var s = both[1];
      kdSlotsOk = !!s;
      if (s) {
        kdZones = s.zones || {};
        kdOrder = s.order || [];
        kdFlags = { bold:s.flag_bold, unit:s.flag_unit, age:s.flag_age, trend:s.flag_trend };
        kdInks  = s.inks || [];
        kdAutoDec = s.auto_decimals;
        kdGroups  = { out:s.group_out_set || "", in:s.group_in_set || "" };
        kdGroupPh = { out:s.group_out || "OUTSIDE", in:s.group_in || "INSIDE" };
      }
      kindleRender(both[0]);
      kdLoaded = true;
      kdBase = kdSnapshot();
      kdDirtyRefresh();
    })
    .catch(function (e) {
      var form = document.getElementById("kd-form");
      var bar = document.getElementById("kd-savebar");
      if (bar) bar.hidden = true;
      if (!form) return;
      var t = window.I18n ? I18n.t : function (k) { return k; };
      if (e && e.message === "not-in-build") {
        // Named rather than shown as an empty form, for the same reason the
        // battery-nodes page does it: a page that looks merely blank sends
        // people looking for a fault that is not there.
        // Strings carry their own <code> markup (trusted, from the static
        // i18n dictionary — not escaped, same as the rest of this page's
        // hand-built HTML strings).
        form.innerHTML =
          '<div class="card"><div class="card-body"><p class="hint">' +
          t("kindle.notInBuild") + "</p></div></div>";
      } else {
        form.innerHTML =
          '<div class="card"><div class="card-body"><p class="hint">' +
          esc(t("kindle.couldNotRead")) + "</p></div></div>";
      }
    });
}

function kdConfigBody() {
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
  body.set("refresh_sec",   kdVal("kd-refresh", "") || "0");
  body.set("follow_data",   kdVal("kd-follow", "1"));
  body.set("clock_pin_refresh", kdVal("kd-clockpin", "1"));
  body.set("fbink_res_w",   kdVal("kd-fbink-res", "0"));
  body.set("layout_mode",   kdVal("kd-layout", "0"));
  body.set("outdoor_sensor", (document.getElementById("kd-outdoor-sensor") || {}).value || "");
  body.set("indoor_sensor",  (document.getElementById("kd-indoor-sensor") || {}).value || "");
  return body;
}

function kdSlotsBody() {
  // ONLY THE FIELDS THE DEVICE READS. `shown` is what the caption WILL render
  // as, derived on the collector from the metric table — it comes down with the
  // layout so the form can offer it as a placeholder, and sending it back is a
  // quarter of the payload spent on a value the firmware ignores. On a body
  // that reaches two kilobytes fully filled in, and travels to a device whose
  // segments are about 1.4 KB, a quarter matters.
  var out = {};
  Object.keys(kdZones).forEach(function (k) {
    var z = kdZones[k];
    out[k] = { sensor:z.sensor || "", metric:z.metric || "", label:z.label || "",
               flags:z.flags | 0, decimals:z.decimals | 0, ink:z.ink | 0 };
  });
  return JSON.stringify({ zones:out, group_out:kdGroups.out, group_in:kdGroups["in"] });
}

// BOTH ENDPOINTS, ONE BUTTON, ONE MESSAGE — and the appearance first, because
// it is the one whose failure the reader can see without opening this page
// again.
//
// Said in terms of the PANEL, not of the server: "Saved" alone would leave
// somebody standing in front of a reader that has not repainted yet wondering
// whether it worked.
function kindleSave() {
  var slots = 0, wrote = "";
  // THE PLACES ARE NOT SENT IF THEY WERE NEVER READ. A slots GET that 404s
  // (older firmware), times out, or comes back refused leaves every place
  // looking empty in a form that cannot tell that from a device with nothing
  // configured — and posting it back erases the reader's whole layout while
  // reporting success. The appearance is still saved: it is on screen, it was
  // read, and it is what the reader came to change.
  if (!kdSlotsOk) {
    return postWithCsrf("/api/kindle/config", {
      body: kdConfigBody(),
      headers: { "Content-Type": "application/x-www-form-urlencoded" }
    })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d || !d.ok) throw new Error((d && d.error) || "the appearance was refused");
        return kindleRefresh();
      })
      .then(function () {
        kdMsg("Saved the appearance. The readings could not be read from the " +
              "reader, so they were left exactly as they are — reload the page " +
              "to try again.", "ok");
      })
      .catch(function (e) {
        kdMsg("Save failed: " + ((e && e.message) || "unknown") +
              ". Nothing on the reader has changed.", "err");
      });
  }
  return postWithCsrf("/api/kindle/config", {
    body: kdConfigBody(),
    headers: { "Content-Type": "application/x-www-form-urlencoded" }
  })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (!d || !d.ok) throw new Error((d && d.error) || "the appearance was refused");
      // Written, and the reader will see it on the next repaint whatever
      // happens to the second half — so a failure below cannot claim nothing
      // changed.
      wrote = "The appearance was saved; the readings were not.";
      return postWithCsrf("/api/kindle/slots", {
        body: kdSlotsBody(),
        headers: { "Content-Type": "application/json" }
      });
    })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (!d || !d.ok) throw new Error((d && d.error) || "the readings were refused");
      slots = d.count | 0;
      return kindleRefresh();
    })
    .then(function () {
      kdMsg("Saved " + slots + " reading" + (slots === 1 ? "" : "s") +
            " and the appearance. The reader picks it up on its next repaint.", "ok");
    })
    .catch(function (e) {
      kdMsg("Save failed: " + ((e && e.message) || "unknown") + ". " +
            (wrote || "Nothing on the reader has changed."), "err");
    });
}

function kindleDiscard() {
  return kindleRefresh().then(function () {
    kdMsg("Back to what the device holds.", "ok");
  });
}

function kindleDefaults() {
  if (!confirm("Put the e-ink page back to the built-in design?\n\n" +
               "Bookerly, nothing bold, every region shown, hPa and one decimal, " +
               "and the eleven places rebuilt from the two sensors named under " +
               "“The reader”.\n\nNothing is written until you press Save."))
    return;

  // Back to "as built", which is where a device that has never been touched
  // sits — not to English, which would be a choice this button did not make.
  kdSet("kd-lang", "0");
  kdSet("kd-face", "0");
  var fc = document.getElementById("kd-face-custom");
  if (fc) fc.value = "";
  kdSet("kd-clock", "0");
  kdSet("kd-time", "0");
  kdSet("kd-date", "0");
  kdSet("kd-press", "0");
  kdSet("kd-dec", "1");
  kdSet("kd-refresh", "");
  kdSet("kd-follow", "1");
  kdSet("kd-clockpin", "1");
  kdSet("kd-fbink-res", "0");
  // Back to "the collector decides", which is what a device that has never
  // been touched holds — not to a shape, which would be a choice this button
  // did not make.
  kdSet("kd-layout", "0");

  kdShowInit = 0xFF;   // KSHOW_ALL
  kdBoldInit = 0;

  // The built-in layout, rebuilt from the two sensors the page is pointed at,
  // so "back to the built-in design" means the same thing for the places as it
  // does for the appearance. The last four grid places are left EMPTY on
  // purpose: a dashboard that arrives showing a metric the hardware does not
  // have is a dashboard showing a dash.
  var out = (document.getElementById("kd-outdoor-sensor") || {}).value || "";
  var inn = (document.getElementById("kd-indoor-sensor") || {}).value || "";
  if (!out && kdSensors.length) out = kdSensors[0].id;
  if (!inn && kdSensors.length) inn = kdSensors[kdSensors.length > 1 ? 1 : 0].id;

  function z(sensor, metric, flags, ink) {
    return { sensor:sensor, metric:metric, label:"", shown:"",
             flags:flags, decimals:kdAutoDec, ink:ink || 0 };
  }
  function none() { return z("", "", kdFlags.unit, 0); }

  kdZones = {
    hero: z(out, "temperature", kdFlags.bold | kdFlags.unit | kdFlags.age),
    big:  z(out, "humidity",    kdFlags.unit, 1),
    g1:   z(out, "pressure",    kdFlags.unit | kdFlags.trend),
    g2:   z(out, "dew_point",   kdFlags.unit),
    g3:   none(), g4: none(), g5: none(), g6: none(),
    in1:  z(inn, "temperature", kdFlags.unit | kdFlags.age),
    in2:  z(inn, "humidity",    kdFlags.unit, 1),
    in3:  z(inn, "aqi",         kdFlags.unit, 1)
  };
  kdGroups = { out:"", in:"" };

  // THE ROWS ARE DRAWN AFTER THE PLACES ARE REPLACED, not before. Rendering
  // first left every row — the open editor's five controls, each collapsed
  // row's "balcony · temperature", the filled counts — describing the layout
  // this button had just thrown away, while the preview beside them showed the
  // new one. Touching any of those stale controls then wrote its old value
  // back into the fresh layout.
  //
  // kdLoaded=false while they are drawn, so the switches take the defaults set
  // above rather than what is still checked in the DOM.
  kdLoaded = false;
  kdRenderZones();
  kdLoaded = true;

  kindleTouched();
  kdMsg("The built-in design is in the form. Press Save to keep it.", "ok");
}

function kindleInit() {
  kdTabInit();
  fetchWithTimeout("/api/sensors", {}, 10000)
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      kdSensors = (d && d.sensors) ? d.sensors : [];
      var options = '<option value="">Default</option>';
      kdSensors.forEach(function (s) {
        options += '<option value="' + esc(s.id) + '">' + esc(kdSensorLabel(s.id)) +
                   "</option>";
      });
      var outSel = document.getElementById("kd-outdoor-sensor");
      var inSel = document.getElementById("kd-indoor-sensor");
      if (outSel) outSel.innerHTML = options.replace("Default", "Default (outdoor)");
      if (inSel) inSel.innerHTML = options.replace("Default", "Default (indoor)");
    })
    .catch(function () {})
    .finally(function () { kindleRefresh(); });

  // The panel is drawn at 600 px and scaled to the column it is in, so it has
  // to be re-fitted when that column changes width. Registered once.
  if (!kindleInit._fit) {
    kindleInit._fit = true;
    window.addEventListener("resize", kdFitPreview);
  }
}

// Through the dispatcher's allowlist, not on window: core.js routes every
// data-click and data-change through Handlers so injected markup cannot reach
// an arbitrary global, and a handler that is only global is a dead button.
// The zone rows, the panel and the cadence/layout sentences are built as
// strings — their wording comes from kdZoneText()/kdI18n() at render time
// (NOT kdT(), which draws text on the panel), so I18n.apply()'s data-i18n
// walk cannot reach them and they have to be redrawn. From the working
// copy, never re-fetched, so a language switch cannot discard unsaved edits.
document.addEventListener("i18n:change", function () {
  if (!kdLoaded || !document.getElementById("kd-zones")) return;
  kdRenderZones();
  kdRenderPreview();
  kdCadenceRender();
  kdLayoutRender();
  kdDirtyRefresh();
});

registerHandlers({
  kindleRefresh: kindleRefresh,
  kindleSave: kindleSave,
  kindleDiscard: kindleDiscard,
  kindleDefaults: kindleDefaults,
  kindleJump: kindleJump,
  kindleZoneOpen: kindleZoneOpen,
  kindleTouched: kindleTouched,
  kindleCadence: kindleCadence,
  kindleProbe: kindleProbe,
  kindleFaceChanged: kindleFaceChanged,
  kindleSlotEdit: kindleSlotEdit,
  kindleSlotFlag: kindleSlotFlag,
  kindleSlotClear: kindleSlotClear,
  kindleGroupEdit: kindleGroupEdit
});
