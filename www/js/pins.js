// ============================================================================
// /www/js/pins.js — one pin field for every page of the Collector
//
// The node's own setup page (node_portal/, docs/NODE_CONFIG.md §6) taught the
// rule this file applies to the Collector: a pin is typed the way the board
// prints it ("D6") or as a GPIO ("12", "GPIO12"), the line under the field
// always says what it resolved to, the header is drawn with every pad
// coloured by what it costs and who uses it, and only what no wiring can fix
// is refused. Before this, the Collector had four pin forms (first-run,
// Hardware, sensor editor, IoT wizard), each with its own idea of a bad pin.
//
// WHERE THE KNOWLEDGE LIVES
//   /api/board-profiles  the firmware's lists (strap, flash, reserved, usb,
//                        absent, maxGpio) — the only authority on refusals.
//   /boards.json         silkscreen labels and header pads per profile, for
//                        drawing only (LittleFS, so it costs the app image
//                        nothing). tools/check_boards_json.py keeps it honest.
//
// LEVELS (same as the node, and the firmware's pinHardReason / validateAttachPin)
//   err   not on this chip, or the SPI flash bus — no wiring makes it work.
//   warn  a boot strap, the console UART, a USB pin, a pad the board does not
//         break out — usable when you know why. A sensor saved with one gets
//         allow_unsafe_pins set by the page (Pins.needsUnsafe).
//   ok    nothing else wants it.
//
// Self-contained on purpose: firstrun.html loads it without core.js.
// ============================================================================
(function () {
  "use strict";

  function t(key, fallback, vars) {
    var s = window.I18n ? I18n.t("pins." + key, vars) : null;
    if (!s || s === "pins." + key) {
      s = fallback;
      for (var k in vars || {}) s = s.split("{" + k + "}").join(vars[k]);
    }
    return s;
  }
  function esc(s) {
    return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
      .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }
  function inList(l, g) { return Array.isArray(l) && l.indexOf(g) !== -1; }

  // ── Loading ───────────────────────────────────────────────────────────────
  var _load = null;
  function getJson(url) {
    return fetch(url, { credentials: "same-origin" }).then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    });
  }
  /// { profiles:[…], active:"id", suggested:"id"|"", boards:{id:{pins,left,right}} }
  /// A missing /boards.json only costs the drawings; a missing profile list
  /// leaves every pin unchecked (level ok), never blocked.
  /// A failed profile fetch is not cached: the next call asks again, so one
  /// dropped request does not leave every later page on an unchecked context.
  function load(force) {
    if (_load && !force) return _load;
    var failed = false;
    var p = _load = Promise.all([
      getJson("/api/board-profiles").catch(function () { failed = true; return {}; }),
      getJson("/boards.json").catch(function () { return {}; }),
    ]).then(function (r) {
      if (failed && _load === p) _load = null;
      var bp = r[0] || {};
      return {
        profiles: bp.profiles || [],
        active: (bp.active && bp.active.id) || "",
        suggested: bp.suggested || "",
        boards: r[1] || {},
      };
    });
    return _load;
  }

  /// The context every other call takes: one profile and its drawing.
  function ctx(data, profileId) {
    var p = null;
    (data.profiles || []).forEach(function (x) { if (x.id === profileId) p = x; });
    var b = (p && data.boards && data.boards[p.id]) || null;
    return { profile: p, board: b && b.pins ? b : null };
  }

  // ── Parsing and naming ────────────────────────────────────────────────────
  /// text → { gpio, empty, bad }. "", "-1" and "255" (PIN_UNSET) are empty.
  function parse(c, text) {
    var s = String(text == null ? "" : text).trim();
    if (s === "" || s === "-1" || s === "255") return { gpio: null, empty: true, bad: false };
    var pins = (c && c.board && c.board.pins) || {};
    for (var lbl in pins) {
      if (lbl.toLowerCase() === s.toLowerCase()) return { gpio: pins[lbl], empty: false, bad: false };
    }
    var m = /^(?:gpio|io)?\s*(\d{1,2})$/i.exec(s);
    if (m) return { gpio: parseInt(m[1], 10), empty: false, bad: false };
    return { gpio: null, empty: false, bad: true };
  }
  /// The board's own label for a GPIO, or "" when it prints none (or prints
  /// the bare number, which would only repeat it).
  function labelOf(c, g) {
    var pins = (c && c.board && c.board.pins) || {};
    for (var lbl in pins) if (pins[lbl] === g && lbl !== String(g)) return lbl;
    return "";
  }
  /// What a pin field shows for a stored GPIO: the label when there is one.
  function unset(g) { return typeof g !== "number" || isNaN(g) || g < 0 || g === 255; }
  function text(c, g) {
    if (unset(g)) return "";
    var pins = (c && c.board && c.board.pins) || {};
    for (var lbl in pins) if (pins[lbl] === g) return lbl;
    return String(g);
  }

  // ── Risk ──────────────────────────────────────────────────────────────────
  /// { level: "ok"|"warn"|"err", why }
  function risk(c, g) {
    var p = c && c.profile;
    if (g == null) return { level: "ok", why: "" };
    if (!p) return { level: "ok", why: "" };                 // nothing to check against
    if (g < 0 || g > p.maxGpio) return { level: "err", why: t("range", "not on this chip (GPIO 0–{max})", { max: p.maxGpio }) };
    // usbPins is live (USB CDC on in this build), so it applies to custom too.
    if (inList(p.usbPins, g))      return { level: "warn", why: t("usb", "USB D-/D+") };
    if (p.id === "custom") return { level: "ok", why: "" };
    if (inList(p.flashPins, g))    return { level: "err",  why: t("flash", "SPI flash bus, never usable") };
    if (inList(p.strapPins, g))    return { level: "warn", why: t("strap", "boot strap: must not be held low at reset") };
    if (inList(p.reservedPins, g)) return { level: "warn", why: t("uart", "UART0, the serial console") };
    if (inList(p.absentPins, g))   return { level: "warn", why: t("absent", "no header pad on this board") };
    return { level: "ok", why: "" };
  }

  // ── Uses and conflicts ────────────────────────────────────────────────────
  // A use is { key, g, who, share }. Two uses of one GPIO conflict unless
  // both carry the same non-empty `share` — how several I2C sensors on one
  // bus legitimately name the same SDA.
  function conflicts(uses, key) {
    var me = null, out = [];
    uses.forEach(function (u) { if (u.key === key) me = u; });
    if (!me || me.g == null) return out;
    uses.forEach(function (u) {
      if (u.key === key || u.g !== me.g) return;
      if (me.share && u.share === me.share) return;
      if (out.indexOf(u.who) === -1) out.push(u.who);
    });
    return out;
  }

  // The pin-carrying keys of a platform_config.json sensor, as the plugins
  // read them (src/sensors/plugins/*.cpp). I2C sensors on one bus share
  // their SDA/SCL by design, so those two carry a share key.
  var SENSOR_PIN_KEYS = ["sda", "scl", "uart_rx", "uart_tx", "pin", "trig_pin", "echo_pin", "dir_pin"];
  function sensorUses(sensors, skipIdx) {
    var out = [];
    (sensors || []).forEach(function (s, i) {
      if (!s || i === skipIdx || s.enabled === false) return;
      if (s.interface === "http" || s.type === "remote") return;
      SENSOR_PIN_KEYS.forEach(function (k) {
        var g = s[k];
        if (typeof g !== "number" || g < 0 || g === 255) return;
        if ((k === "sda" || k === "scl") && s.interface !== "i2c") return;
        out.push({ key: "sensor:" + i + ":" + k, g: g, who: (s.id || s.type) + " " + k.toUpperCase(),
                   share: (k === "sda" || k === "scl") ? "i2c" + (s.bus || 0) + ":" + k : "" });
      });
    });
    return out;
  }
  /// Every GPIO a stored sensor carries, whichever key holds it (the edit
  /// form shows only some of them; the rest come from advanced JSON).
  function sensorGpios(s) {
    var out = [];
    SENSOR_PIN_KEYS.forEach(function (k) {
      var g = s && s[k];
      if (typeof g === "number" && g >= 0 && g !== 255) out.push(g);
    });
    return out;
  }
  // The Hardware page's pins (config.hardware, as /export_settings sends it).
  // SD pins count only when the SD card is the storage, like the page shows.
  var HW_PINS = [
    ["pinWifiTrigger", "wifiButton", "WiFi button"], ["pinWakeupFF", "ffButton", "FF button"],
    ["pinWakeupPF", "pfButton", "PF button"], ["pinFlowSensor", "flow", "Flow sensor"],
    ["pinRtcCE", "rtc", "RTC", " CE"], ["pinRtcIO", "rtc", "RTC", " IO"], ["pinRtcSCLK", "rtc", "RTC", " SCLK"],
    ["pinSdCS", "sdCard", "SD card", " CS"], ["pinSdMOSI", "sdCard", "SD card", " MOSI"],
    ["pinSdMISO", "sdCard", "SD card", " MISO"], ["pinSdSCK", "sdCard", "SD card", " SCK"],
  ];
  function hardwareUses(hw) {
    var out = [];
    if (!hw) return out;
    HW_PINS.forEach(function (d) {
      var g = hw[d[0]];
      if (typeof g === "string") g = parseInt(g, 10);
      if (typeof g !== "number" || isNaN(g) || g < 0 || g === 255) return;
      if (d[0].indexOf("pinSd") === 0 && String(hw.storageType) !== "1") return;
      out.push({ key: d[0], g: g, who: t(d[1], d[2]) + (d[3] || "") });
    });
    return out;
  }

  /// The line under a field: { cls: "ok"|"warn"|"err", text }.
  function hint(c, raw, uses, key, opts) {
    opts = opts || {};
    var r = parse(c, raw);
    if (r.bad) return { cls: "err", text: t("unknown", "“{v}” is not a pin on this board", { v: String(raw).trim() }) };
    if (r.empty) return opts.required ? { cls: "err", text: t("required", "required") }
                                      : { cls: "", text: t("unset", "not used") };
    var g = r.gpio, lab = labelOf(c, g), head = "GPIO" + g + (lab ? " · " + lab : "");
    var k = risk(c, g);
    if (k.level === "err") {
      var m = head + " — " + k.why;
      // The D6-vs-GPIO6 slip: a bare number the chip refuses, while the board
      // prints that number as a D-label for another GPIO.
      var pins = (c && c.board && c.board.pins) || {};
      if (/^\d+$/.test(String(raw).trim()) && pins["D" + g] != null && risk(c, pins["D" + g]).level !== "err") {
        m += t("mean", " Did you mean D{g} (GPIO{to})?", { g: g, to: pins["D" + g] });
      }
      return { cls: "err", text: m };
    }
    var dup = conflicts(uses || [], key);
    if (dup.length) return { cls: "err", text: head + " — " + t("dup", "also used by {w}", { w: dup.join(", ") }) };
    if (k.level === "warn") return { cls: "warn", text: head + " — " + k.why };
    return { cls: "ok", text: head };
  }

  /// True when any of these GPIOs is a warn-level pin: a sensor using one
  /// needs allow_unsafe_pins, or the firmware will refuse it at init.
  function needsUnsafe(c, gpios) {
    for (var i = 0; i < gpios.length; i++) {
      if (gpios[i] != null && risk(c, gpios[i]).level === "warn") return true;
    }
    return false;
  }

  // ── The header drawing ────────────────────────────────────────────────────
  function chip(c, lab, uses) {
    var pins = (c.board && c.board.pins) || {}, g = pins[lab];
    if (g == null && /^GPIO\d+$/.test(lab)) g = parseInt(lab.slice(4), 10);
    if (g == null) return '<span class="pm-chip pwr">' + esc(lab) + "</span>";
    var who = [];
    (uses || []).forEach(function (u) { if (u.g === g && who.indexOf(u.who) === -1) who.push(u.who); });
    var k = risk(c, g);
    var cls = k.level === "err" ? "err" : who.length ? "use" : k.level === "warn" ? "warn" : "";
    var title = "GPIO" + g + (k.why ? " — " + k.why : "") + (who.length ? " — " + who.join(", ") : "");
    var sub = String(g) === lab || lab === "GPIO" + g ? "" : " " + g;
    return '<span class="pm-chip ' + cls + '" title="' + esc(title) + '"><b>' + esc(lab) + "</b>" + sub + "</span>";
  }
  function diagram(c, uses) {
    if (!c || !c.profile) return "";
    var h = "", b = c.board, i;
    if (b && b.left && b.right) {
      h += '<div class="pm-header"><div class="pm-col l">';
      for (i = 0; i < b.left.length; i++) h += chip(c, b.left[i], uses);
      h += '</div><div class="pm-body"></div><div class="pm-col">';
      for (i = 0; i < b.right.length; i++) h += chip(c, b.right[i], uses);
      h += "</div></div>";
    } else {
      h += '<div class="pm-grid">';
      for (i = 0; i <= c.profile.maxGpio; i++) h += chip(c, "GPIO" + i, uses);
      h += "</div>";
    }
    return h + '<div class="pm-legend"><span class="pm-chip use">' + esc(t("lgUse", "used")) +
      '</span><span class="pm-chip warn">' + esc(t("lgWarn", "use with care")) +
      '</span><span class="pm-chip err">' + esc(t("lgBad", "never usable")) + "</span></div>";
  }

  // ── Wiring a field ────────────────────────────────────────────────────────
  /// Paints the hint element under `input` (found by data-pin-hint="<key>"
  /// in `root`, or passed) and, when the input has data-pin-target, writes
  /// the resolved GPIO (-1 when empty or unparsable) into that hidden input
  /// so a plain form POST sends a number.
  function paint(root, c, input, uses, opts) {
    var key = input.getAttribute("data-pin");
    var h = hint(c, input.value, uses, key, opts);
    var el = root.querySelector('[data-pin-hint="' + key + '"]');
    if (el) { el.className = "pin-hint " + h.cls; el.textContent = h.text; }
    input.classList.toggle("pin-bad", h.cls === "err");
    var tgt = input.getAttribute("data-pin-target");
    if (tgt) {
      var hid = root.querySelector('input[name="' + tgt + '"]');
      var r = parse(c, input.value);
      if (hid) hid.value = r.gpio == null ? "-1" : String(r.gpio);
    }
    return h;
  }
  /// Every [data-pin] input under root, live. `usesFn(root)` returns the
  /// current uses (so a change in one field re-checks the others); `after`
  /// runs after each repaint (e.g. to redraw a diagram). Returns a function
  /// that repaints everything and reports whether any field is in error.
  function wire(root, c, usesFn, after, optsFn) {
    function all() {
      var uses = usesFn ? usesFn(root) : [], bad = false;
      root.querySelectorAll("input[data-pin]").forEach(function (inp) {
        if (inp.offsetParent === null && !inp.hasAttribute("data-pin-always")) {
          var el = root.querySelector('[data-pin-hint="' + inp.getAttribute("data-pin") + '"]');
          if (el) el.textContent = "";
          return;                                      // hidden by showWhen
        }
        var h = paint(root, c, inp, uses, optsFn ? optsFn(inp) : null);
        if (h.cls === "err") bad = true;
      });
      if (after) after(uses);
      return !bad;
    }
    root.addEventListener("input", function (e) { if (e.target && e.target.hasAttribute("data-pin")) all(); });
    root.addEventListener("change", all);
    all();
    return all;
  }

  /// Markup for one field: the text box and its hint line. `target` names a
  /// hidden input that carries the GPIO for form POSTs (optional).
  function field(key, label, value, c, opts) {
    opts = opts || {};
    var id = "pin-" + key.replace(/[^\w-]/g, "_");
    return '<div class="field pin-field' + (opts.cls ? " " + esc(opts.cls) : "") + '"' + (opts.attrs || "") + '>' +
      '<label class="field-label" for="' + esc(id) + '">' + esc(label) + "</label>" +
      '<input id="' + esc(id) + '" class="input mono" type="text" autocomplete="off" spellcheck="false" ' +
      'inputmode="text" data-pin="' + esc(key) + '"' +
      (opts.target ? ' data-pin-target="' + esc(opts.target) + '"' : "") +
      (opts.name ? ' name="' + esc(opts.name) + '"' : "") +
      ' placeholder="' + esc(opts.placeholder || t("ph", "D0 or GPIO2")) + '" value="' + esc(text(c, value)) + '">' +
      (opts.target ? '<input type="hidden" name="' + esc(opts.target) + '" value="' + (unset(value) ? -1 : value) + '">' : "") +
      '<div class="pin-hint" data-pin-hint="' + esc(key) + '" aria-live="polite"></div></div>';
  }

  window.Pins = {
    load: load, ctx: ctx, parse: parse, text: text, labelOf: labelOf, risk: risk,
    hint: hint, conflicts: conflicts, needsUnsafe: needsUnsafe, diagram: diagram,
    sensorUses: sensorUses, sensorGpios: sensorGpios, hardwareUses: hardwareUses, hwLabel: function (name) {
      for (var i = 0; i < HW_PINS.length; i++) if (HW_PINS[i][0] === name) return t(HW_PINS[i][1], HW_PINS[i][2]) + (HW_PINS[i][3] || "");
      return name;
    },
    field: field, wire: wire, paint: paint,
  };
})();
