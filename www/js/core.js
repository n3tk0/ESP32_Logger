/**
 * /www/js/core.js  –  Water Logger SPA – core runtime  v4.1.5
 *
 * Module split (loaded in this order from index.html):
 *   1. core.js     – globals, bootstrap, /api/status, theme, routing, utils,
 *                    settingsSave, restart popup
 *   2. pages.js    – dashboard, files, live
 *   3. settings.js – every settings sub-page, changelog, OTA update
 *   4. sensors.js  – sensors grid, Core Logic editor, platform_config IO,
 *                    settings import/export
 *
 * No bundler, no module system: every file is a plain script in the same
 * global scope, ordered by load. Each file has its own "use strict".
 *
 * Architecture:
 *   – On load: fetch /api/status + /export_settings, apply theme, route to hash page
 *   – Pages are hidden/shown via class toggling; partials lazy-loaded from /pages/
 *   – Live page prefers SSE /api/events with polling fallback
 *   – Footer refreshes boot+heap from live channel; chip/version from /api/status
 *   – Form saves use fetch() to /save_* endpoints
 *   – Settings pages load config from /export_settings (full nested objects)
 */

"use strict";

// ============================================================================
// FETCH WITH TIMEOUT (R13 4.8)
// ============================================================================
function fetchWithTimeout(url, opts, timeoutMs) {
  opts = opts || {};
  var ctrl = new AbortController();
  var t = setTimeout(function () { ctrl.abort(); }, timeoutMs || 15000);
  opts.signal = ctrl.signal;
  return fetch(url, opts).finally(function () { clearTimeout(t); });
}
window.fetchWithTimeout = fetchWithTimeout;

// ── Connection state chip (sidebar footer) ─────────────────────────────────
// Previously the "Online" chip was static HTML that never changed. Every
// successful/failed round-trip through the shared fetch helpers now reports
// here; the SSE channel in pages.js does too.
var _connOnline = null;
function setConnState(ok) {
  ok = !!ok;
  if (_connOnline === ok) return;
  _connOnline = ok;
  var chip = document.getElementById("sstat-conn");
  var label = document.getElementById("sstat-conn-label");
  if (chip) {
    chip.classList.toggle("ok", ok);
    chip.classList.toggle("err", !ok);
  }
  if (label) label.textContent = ok ? "Online" : "Offline";
}
window.setConnState = setConnState;

// Same single-flight pattern for /api/sensors — three call sites all want
// the same payload (Overview Diagnostics card, the Sensors page grid, and
// the Sensor-chart metric dropdown).  Default 5 s cache window.
var _sensorsCache    = null;
var _sensorsFetchedAt = 0;
var _sensorsInflight  = null;
function getSensors(opts) {
  opts = opts || {};
  var maxAge = (opts.maxAgeMs !== undefined) ? opts.maxAgeMs : 5000;
  var now = Date.now();
  if (_sensorsCache && _sensorsFetchedAt && (now - _sensorsFetchedAt) < maxAge) {
    return Promise.resolve(_sensorsCache);
  }
  if (_sensorsInflight) return _sensorsInflight;
  _sensorsInflight = fetchWithTimeout("/api/sensors", {}, 15000)
    .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
    .then(function (data) {
      _sensorsCache = data;
      _sensorsFetchedAt = Date.now();
      setConnState(true);
      return data;
    })
    .catch(function (e) { setConnState(false); return Promise.reject(e); })
    .finally(function () { _sensorsInflight = null; });
  return _sensorsInflight;
}
window.getSensors = getSensors;

// getStatus({maxAgeMs}) — single-flight cache around /api/status.
// Many pages (5 settings sub-pages + 2 IoT-extensions sites) all call the
// same endpoint within seconds of each other.  This funnels them through
// a shared cache + dedupe so the ESP only answers once per `maxAgeMs`
// window.  Default is 5 s — small enough that no UI feels stale, large
// enough to collapse a settings-sub-page navigation burst into one round
// trip.  Pass {maxAgeMs:0} to force-refresh.
function getStatus(opts) {
  opts = opts || {};
  var maxAge = (opts.maxAgeMs !== undefined) ? opts.maxAgeMs : 5000;
  var now = Date.now();
  if (ST && _stFetchedAt && (now - _stFetchedAt) < maxAge) {
    return Promise.resolve(ST);
  }
  if (_stInflight) return _stInflight;
  _stInflight = fetchWithTimeout("/api/status", {}, 15000)
    .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
    .then(function (data) {
      ST = data;
      _stFetchedAt = Date.now();
      setConnState(true);
      return data;
    })
    .catch(function (e) { setConnState(false); return Promise.reject(e); })
    .finally(function () { _stInflight = null; });
  return _stInflight;
}
window.getStatus = getStatus;

// ============================================================================
// GLOBALS
// ============================================================================
var ST = {}; // cached /api/status payload
var CFG = {}; // cached /export_settings payload
var _stFetchedAt = 0;     // ms epoch when ST was last filled
var _stInflight  = null;  // dedupe concurrent callers
var dbChart = null; // uPlot instance on dashboard
var dbRawData = ""; // raw log text for dashboard
var dbFilteredData = []; // filtered, parsed rows
var liveTimer = null; // live page polling interval (fallback)
var liveES    = null; // live page EventSource (preferred transport)
var liveLogsTimer = null; // live logs interval
var currentPage = ""; // active page id (without 'page-' prefix)
var currentFilesDir = "/";
var currentFilesStorage = "internal";
var filesEditMode = false;
var netScanRetries = 0;
var changelogLoaded = false;

// ============================================================================
// EVENT DISPATCHER  (Pass 4 A4: replaces inline on* handlers)
// ----------------------------------------------------------------------------
// Markup convention:
//   <button data-click="dbExportCSV">…</button>
//   <input  data-change="dbApplyFilters">…
//   <form   data-submit="settingsSaveForm" data-save-url="/save_hardware"
//           data-save-restart>…</form>
//   <button data-click="filesDelete" data-args='["/foo.txt"]'>🗑️</button>
//   <img    data-error="hideParent">
//
// Arguments come from the optional `data-args` JSON array. If absent, the
// handler is called as fn(event), with `this` bound to the delegated element.
// Functions are looked up on `window`. A returned false => preventDefault().
// A small set of built-ins (hideParent, navPage, settingsSaveForm) handles
// the patterns previously done as inline JS.
// ============================================================================
// Handlers registry — whitelist of functions callable via data-click / data-change /
// data-input / data-submit / data-error / data-backdrop-fn.  Using a dedicated map
// (instead of window[name]) closes a CSP-adjacent risk: if HTML injection ever lands
// in a sensor id / file name / etc., the injected data-click can still only invoke a
// function the application explicitly enrolled here.  Each JS module calls
// registerHandlers({...}) to add its public entries; anything else is un-callable.
var Handlers = Object.create(null);
function registerHandlers(obj) {
  for (var k in obj) if (typeof obj[k] === "function") Handlers[k] = obj[k];
}

function _dispatchEvent(eventName) {
  return function (ev) {
    // closest() stops at the NEAREST ancestor — outer ancestors with the same
    // data-* attribute are intentionally shadowed (single-fire bubble policy).
    var t = ev.target.closest("[data-" + eventName + "]");
    if (!t) return;
    var name = t.getAttribute("data-" + eventName);
    var fn = Handlers[name];
    if (typeof fn !== "function") {
      console.warn("_dispatchEvent: no handler registered for", JSON.stringify(name), "on", t);
      return;
    }
    var args;
    var raw = t.getAttribute("data-args");
    if (raw) {
      try {
        if (raw.length > 4096) { console.warn("data-args too large on", t); return; }
        args = JSON.parse(raw);
        // Accept a scalar/object for convenience (data-args="5" or '{"x":1}')
        // by wrapping it; fn.apply strictly requires an array.
        if (args !== null && !Array.isArray(args)) args = [args];
      } catch (e) { console.warn("bad data-args on", t, raw); args = []; }
    }
    var result = args ? fn.apply(t, args) : fn.call(t, ev);
    if (result === false) ev.preventDefault();
  };
}

function installEventDispatcher() {
  ["click", "change", "input"].forEach(function (name) {
    document.addEventListener(name, _dispatchEvent(name));
  });
  // Every form submit in this app is AJAX; preventDefault unconditionally so
  // handlers don't each have to remember to block the native POST navigation.
  document.addEventListener("submit", function (ev) {
    if (!ev.target.closest("[data-submit]")) return;
    ev.preventDefault();
    _dispatchEvent("submit")(ev);
  }, true);
  // onerror does not bubble: wire direct listeners on every [data-error] node
  // present at bootstrap. Nodes added later via innerHTML miss this wiring;
  // re-run wireLateErrorHandlers() after any such injection.
  wireLateErrorHandlers(document);
}

function wireLateErrorHandlers(root) {
  (root || document).querySelectorAll("[data-error]").forEach(function (el) {
    if (el._dataErrorWired) return;
    var fn = Handlers[el.getAttribute("data-error")];
    if (typeof fn === "function") {
      el.addEventListener("error", fn);
      el._dataErrorWired = true;
    }
  });
}

// Submit handler for settings forms with data-save-url / data-save-restart.
// Replaces the old onsubmit="settingsSave(event,'/save_x',this,true)" pattern.
function settingsSaveForm(ev) {
  var f = ev.target;
  settingsSave(
    ev,
    f.getAttribute("data-save-url"),
    f,
    f.hasAttribute("data-save-restart")
  );
}

// Hide the element's grandparent — used by images whose card should
// disappear if the image fails to load.
function hideParent(ev) {
  var el = ev.target;
  if (el && el.parentElement && el.parentElement.parentElement) {
    el.parentElement.parentElement.style.display = "none";
  }
}

// Sidebar/header nav link → returns false so dispatcher calls preventDefault.
function navPage() { return nav(this); }

// Submit the containing form. Replaces onchange="this.form.submit()".
// Note: programmatic .submit() skips submit event listeners, so we dispatch
// a proper 'submit' event to go through our dispatcher.
function submitParentForm() {
  var f = this.form || (this.closest && this.closest("form"));
  if (f) f.dispatchEvent(new Event("submit", { cancelable: true, bubbles: true }));
}

// Popup helpers. Replace inline style="display:flex/none" mutation.
// Named hidePopup (not closePopup) because settings.js defines its own
// zero-arg closePopup() tied to id="popup" that we don't want to shadow.
var LEGAL_POPUP_IDS = ["restartPopup", "popup", "movePopup", "sensorPopup", "kbPopup"];
function showPopup(id) {
  if (LEGAL_POPUP_IDS.indexOf(id) === -1) { console.warn("showPopup: unknown id", id); return; }
  var el = document.getElementById(id); if (el) el.style.display = "flex";
}
function hidePopup(id) {
  if (LEGAL_POPUP_IDS.indexOf(id) === -1) { console.warn("hidePopup: unknown id", id); return; }
  var el = document.getElementById(id); if (el) el.style.display = "none";
}

// Backdrop click-to-close. Attach to the .popup-overlay. The default action
// hides the overlay; if data-backdrop-fn is set, that window-level function is
// called instead (e.g. sapClose, sepClose which reset state as well as hide).
function backdropClose(ev) {
  if (ev.target !== this) return;
  var fn = this.getAttribute("data-backdrop-fn");
  if (fn && typeof Handlers[fn] === "function") Handlers[fn]();
  else this.style.display = "none";
}

// ============================================================================
// BOOTSTRAP
// ============================================================================
window.addEventListener("DOMContentLoaded", function () {
  installEventDispatcher();
  Promise.all([
    fetchWithTimeout("/api/status")
      .then(function (r) {
        return r.json();
      })
      .catch(function () {
        return {};
      }),
    fetchWithTimeout("/export_settings")
      .then(function (r) {
        return r.json();
      })
      .catch(function () {
        return {};
      }),
  ]).then(function (results) {
    ST = results[0];
    CFG = results[1];
    _stFetchedAt = Date.now();   // seed the cache used by getStatus()
    applyStatus(ST);
    var hash = location.hash.replace("#", "") || "dashboard";
    navigateTo(hash);
  });
});

window.addEventListener("hashchange", function () {
  var hash = location.hash.replace("#", "") || "dashboard";
  navigateTo(hash);
});

// ============================================================================
// STATUS  (apply theme, populate footer, header, etc.)
// ============================================================================
function applyStatus(d) {
  document.title = (d.device || "Water Logger") + " – Logger";

  // Device names
  var sn = document.getElementById("sidebarName");
  var hn = document.getElementById("headerName");
  if (sn) sn.textContent = d.device || "Water Logger";
  if (hn) hn.textContent = d.device || "Water Logger";

  // Logo
  if (d.theme && d.theme.logoSource) {
    ["sidebarLogo", "headerLogo"].forEach(function (id) {
      var el = document.getElementById(id);
      if (el) {
        el.src = d.theme.logoSource + "?v=" + Date.now();
        el.style.display = "";
      }
    });
  }

  // Favicon
  if (d.theme && d.theme.faviconPath) {
    var link =
      document.querySelector("link[rel='icon']") ||
      document.createElement("link");
    link.rel = "icon";
    link.href = d.theme.faviconPath + "?v=" + Date.now();
    document.head.appendChild(link);
  }

  // CSS custom properties (theme colors)
  if (d.theme) {
    var th = d.theme;
    // Theme class on <html> element
    var html = document.getElementById("htmlRoot");
    var m = th.mode;
    var actDark = false;

    if (html) {
      // Per-client override (set by quickThemeToggle) wins over server config
      // so the toggle is responsive without a round-trip.
      var override = null;
      try { override = localStorage.getItem("themeOverride"); } catch (e) {}
      var effective = override || (m === 0 || m === "0" ? "light"
                                : m === 1 || m === "1" ? "dark"
                                : "auto");
      html.classList.remove("theme-light", "theme-dark", "theme-auto");
      html.classList.add("theme-" + effective);
      // Keep the design-system [data-theme] attribute in sync so CSS variables
      // that use [data-theme="dark"] selectors work alongside the legacy classes.
      if (effective === "dark") {
        actDark = true;
        html.setAttribute("data-theme", "dark");
      } else if (effective === "auto") {
        actDark =
          window.matchMedia &&
          window.matchMedia("(prefers-color-scheme: dark)").matches;
        html.setAttribute("data-theme", actDark ? "dark" : "light");
        // Add listener to hot-reload if OS theme changes while in auto mode
        if (!window._actDarkListenerAppended) {
          window
            .matchMedia("(prefers-color-scheme: dark)")
            .addEventListener("change", function (e) {
              var ov = null;
              try { ov = localStorage.getItem("themeOverride"); } catch (e2) {}
              var inAuto = ov ? ov === "auto"
                              : (ST && ST.theme && (ST.theme.mode === 2 || ST.theme.mode === "2"));
              if (inAuto) applyStatus(ST);
            });
          window._actDarkListenerAppended = true;
        }
      } else {
        // effective === "light"
        html.setAttribute("data-theme", "light");
      }
      _themeUpdateToggleIcon(effective);
    }

    // Sanitize a CSS value to prevent injection via ; { } */ sequences.
    function _safeCssVal(v) {
      if (!v || typeof v !== "string") return "";
      return v.replace(/[;{}\/*]/g, "");
    }
    var vars = ":root{";
    if (th.primaryColor) vars += "--primary:" + _safeCssVal(th.primaryColor) + ";";
    if (th.secondaryColor) vars += "--secondary:" + _safeCssVal(th.secondaryColor) + ";";
    if (actDark) {
      if (th.darkBgColor) vars += "--bg:" + _safeCssVal(th.darkBgColor) + ";";
      if (th.darkTextColor) vars += "--text:" + _safeCssVal(th.darkTextColor) + ";";
    } else {
      if (th.lightBgColor) vars += "--bg:" + _safeCssVal(th.lightBgColor) + ";";
      if (th.lightTextColor) vars += "--text:" + _safeCssVal(th.lightTextColor) + ";";
    }
    vars += "}";
    var style = document.getElementById("themeVars");
    if (style) style.textContent = vars;

    // Dashboard legend dot colors — matches original inline style in .ino
    setElStyle("db-legendFF", "background", th.ffColor || "var(--ff-color)");
    setElStyle("db-legendPF", "background", th.pfColor || "var(--pf-color)");
    setElStyle(
      "db-legendOther",
      "background",
      th.otherColor || "var(--other-color)",
    );
    // Stat card text colors — matches original: style='color:%s'
    setElStyle("db-totalFF", "color", th.ffColor || "var(--ff-color)");
    setElStyle("db-totalPF", "color", th.pfColor || "var(--pf-color)");
    setElStyle("live-ffCount", "color", th.ffColor || "var(--ff-color)");
    setElStyle("live-pfCount", "color", th.pfColor || "var(--pf-color)");
  }

  // Footer — matches original .ino footer construction exactly:
  //   footer-grid:    Boot: N | <cpu>MHz | <heap free> / <heap total>
  //   footer-row:     <networkDisplay>   |  IP: <ip>
  //   footer-version: Board: <chip> – Firmware: <version>
  updateFooter(d);

  // Topbar logo-meta: chip model · IP address (matches design spec)
  var logoMeta = document.getElementById("logo-meta");
  if (logoMeta && (d.chip || d.ip)) {
    logoMeta.textContent = (d.chip || "ESP32") + " · " + (d.ip || "--");
  }

  // Mobile header
  setEl("headerNet", d.network);
  var timePart = (d.time || "").split(" ")[1] || d.time || "--:--";
  setEl("headerTime", timePart);

  // OTA page version badge
  setEl("ota-currentVer", d.version || "-");
}

// Partial footer update — called by both applyStatus() and liveUpdate()
// Only updates fields present in the supplied object
function updateFooter(d) {
  if (d.boot !== undefined && d.boot !== null) setEl("footer-boot", d.boot);
  if (d.cpu !== undefined && d.cpu !== null) {
    setEl("footer-cpu", d.cpu);
    setEl("sstat-cpu", d.cpu + " MHz");
  }
  if (d.heap !== undefined && d.heapTotal !== undefined) {
    setEl("footer-heap", fmtBytes(d.heap) + " / " + fmtBytes(d.heapTotal));
    // sstat-storage sits under a hard-drive icon — show FS usage, not heap
    if (d.fsUsed !== undefined && d.fsTotal !== undefined) {
      setEl("sstat-storage", fmtBytes(d.fsUsed) + " / " + fmtBytes(d.fsTotal));
    } else {
      setEl("sstat-storage", fmtBytes(d.heap) + " free");
    }
  }
  if (d.network !== undefined && d.network !== null) {
    setEl("footer-net", d.network);
    setEl("sstat-wifi", d.network);
  }
  if (d.ip !== undefined && d.ip !== null) setEl("footer-ip", d.ip);
  if (d.chip !== undefined && d.chip !== null) setEl("footer-chip", d.chip);
  if (d.version !== undefined && d.version !== null)
    setEl("footer-ver", d.version);
}

// ============================================================================
// THEME (client-side override; server config is the source of truth and is
// applied by applyStatus().  This lets the user flip the theme instantly
// without waiting for a /save_theme round-trip.)
// ============================================================================
function _themeApplyOverride(mode) {
  // mode: 'light' | 'dark' | 'auto'
  var html = document.documentElement;
  html.classList.remove("theme-light", "theme-dark", "theme-auto");
  html.classList.add("theme-" + mode);
  // Keep design-system [data-theme] in sync
  var dt = mode === "dark" ? "dark"
         : mode === "light" ? "light"
         : (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
  html.setAttribute("data-theme", dt);
  try { localStorage.setItem("themeOverride", mode); } catch (e) {}
  _themeUpdateToggleIcon(mode);
}

function _themeUpdateToggleIcon(mode) {
  var btn = document.getElementById("themeToggleBtn");
  if (!btn) return;
  // Show the icon for the mode you'd switch INTO so the affordance is obvious.
  btn.textContent = mode === "dark" ? "☀️" : (mode === "light" ? "🌓" : "🌙");
  btn.title = "Theme: " + mode + " (click to change)";
}

// Public alias — other modules (command-palette, quick-settings) call this
// so the data-theme attribute, class list, localStorage, AND topbar button
// icon all update in one place (gemini review PR #108).
function setTheme(mode) {
  if (mode !== "light" && mode !== "dark" && mode !== "auto") return;
  _themeApplyOverride(mode);
}
window.setTheme = setTheme;

function quickThemeToggle() {
  var current;
  try { current = localStorage.getItem("themeOverride") || "auto"; }
  catch (e) { current = "auto"; }
  // Cycle: auto → dark → light → auto
  var next = current === "auto" ? "dark" : (current === "dark" ? "light" : "auto");
  _themeApplyOverride(next);
}

// ── Compact-density toggle (topbar) ────────────────────────────────────────
// Cycles between comfortable (default) and compact via [data-density] on
// <html>.  Mirrors the theme-toggle pattern; persists to localStorage so
// the choice survives a reload.
function quickDensityToggle() {
  var root = document.documentElement;
  var curr = root.getAttribute("data-density") || "comfortable";
  var next = curr === "compact" ? "comfortable" : "compact";
  root.setAttribute("data-density", next);
  try { localStorage.setItem("density", next); } catch (e) {}
  _densitySyncBtn(next);
}
function _densitySyncBtn(d) {
  var btn = document.getElementById("densityToggleBtn");
  if (!btn) return;
  btn.title = "Density: " + d + " (click to switch)";
  btn.setAttribute("aria-label", btn.title);
}

// ── Accent color picker (topbar) ───────────────────────────────────────────
// One-click popup with four swatches.  Sets [data-accent] on <html>; the
// CSS in colors_and_type already handles the rest.
function accentPickerToggle() {
  var picker = document.getElementById("accentPicker");
  if (!picker) return;
  var open = picker.classList.toggle("open");
  var btn = document.getElementById("accentPickerBtn");
  if (btn) btn.setAttribute("aria-expanded", open ? "true" : "false");
}
function setAccent(name) {
  var allowed = { cyan:1, amber:1, green:1, violet:1 };
  if (!allowed[name]) return;
  document.documentElement.setAttribute("data-accent", name);
  try { localStorage.setItem("accent", name); } catch (e) {}
  // Mark the active swatch
  var pop = document.querySelectorAll(".accent-swatch");
  for (var i = 0; i < pop.length; i++) {
    pop[i].classList.toggle("active", pop[i].dataset.accent === name);
  }
  // Close the popup after picking
  var picker = document.getElementById("accentPicker");
  if (picker) picker.classList.remove("open");
  var btn = document.getElementById("accentPickerBtn");
  if (btn) btn.setAttribute("aria-expanded", "false");
}

// Close accent picker on outside click
document.addEventListener("click", function (e) {
  var picker = document.getElementById("accentPicker");
  if (!picker || !picker.classList.contains("open")) return;
  if (picker.contains(e.target)) return;
  picker.classList.remove("open");
  var btn = document.getElementById("accentPickerBtn");
  if (btn) btn.setAttribute("aria-expanded", "false");
});

// Restore persisted density + accent on script load (DOM is ready — this
// script is at the bottom of <body>).  theme-boot.js handles theme; these
// two are not visually-critical pre-paint so it's safe to apply here.
(function _restoreDensityAndAccent() {
  try {
    var d = localStorage.getItem("density");
    if (d === "compact" || d === "comfortable") {
      document.documentElement.setAttribute("data-density", d);
    }
    var a = localStorage.getItem("accent");
    if (a && /^(cyan|amber|green|violet)$/.test(a)) {
      document.documentElement.setAttribute("data-accent", a);
    }
  } catch (e) {}
  // Sync UI affordances after DOM is parsed
  function sync() {
    _densitySyncBtn(document.documentElement.getAttribute("data-density") || "comfortable");
    var acc = document.documentElement.getAttribute("data-accent") || "cyan";
    var swatches = document.querySelectorAll(".accent-swatch");
    for (var i = 0; i < swatches.length; i++) {
      swatches[i].classList.toggle("active", swatches[i].dataset.accent === acc);
    }
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", sync);
  } else {
    sync();
  }
})();

// ── Sticky page-head shadow observer (DISABLED) ─────────────────────────────
// Page headers intentionally scroll with the content now, so the .is-stuck
// observer below is short-circuited. Kept for history; the early return makes
// it a no-op without removing the surrounding structure.
(function _stickyPageHead() {
  return; // page-head is non-sticky — observer disabled
  if (typeof IntersectionObserver === "undefined") return;
  // Re-attach observers when nav changes the active page.  We don't know
  // the exact moment .page-head is mounted, so use a MutationObserver on
  // <main> as a low-cost proxy.
  var main = document.getElementById("main-content") || document.querySelector(".main");
  if (!main) return;
  var io = null;
  function attach() {
    if (io) try { io.disconnect(); } catch (e) {}
    var heads = document.querySelectorAll(".page.active .page-head");
    if (!heads.length) return;
    io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        // When the sentinel above the page-head is no longer intersecting,
        // we're scrolled past it and the head is "stuck".
        e.target.classList.toggle("is-stuck", e.intersectionRatio < 1);
      });
    }, { threshold: [1], root: main });
    Array.prototype.forEach.call(heads, function (h) { io.observe(h); });
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", attach);
  } else {
    attach();
  }
  // Re-attach when pages swap active class
  // Re-attach when pages swap active class.  Filter to .page targets only —
  // observing the full subtree fires on every pulsing badge, sensor-card
  // update, and chart tween, which is wasted work (gemini review PR #108).
  var mo = new MutationObserver(function (records) {
    for (var i = 0; i < records.length; i++) {
      var t = records[i].target;
      if (t && t.classList && t.classList.contains("page")) { attach(); return; }
    }
  });
  mo.observe(main, { attributes: true, subtree: true, attributeFilter: ["class"] });
})();

// Collapsible sidebar rail (Claude Design phase 4a).  Toggles 60px rail
// width on desktop only; mobile ignores the class since the bottom-nav
// handles navigation there.  Preference persists across reloads.
function sidebarRailToggle() {
  // Class lives on <html> so theme-boot.js can restore it pre-paint without
  // waiting for <body> (gemini review PR #47 — avoids FOUC on reload).
  var isRail = document.documentElement.classList.toggle("sidebar-rail");
  try { localStorage.setItem("esp32-sidebar-rail", isRail ? "1" : "0"); } catch (e) {}
  _sidebarRailSyncBtn(isRail);
}

// Keep the toggle button's ARIA + tooltip in sync with current state.
// Called from sidebarRailToggle() and on DOMContentLoaded so a page that
// loads already-railed (pre-paint class in theme-boot.js) still gets the
// correct "Expand" labels.
function _sidebarRailSyncBtn(isRail) {
  var btn = document.getElementById("sidebarRailBtn");
  if (!btn) return;
  btn.setAttribute("aria-label", isRail ? "Expand sidebar" : "Collapse sidebar");
  btn.setAttribute("title",      isRail ? "Expand sidebar" : "Collapse sidebar");
}
document.addEventListener("DOMContentLoaded", function () {
  _sidebarRailSyncBtn(document.documentElement.classList.contains("sidebar-rail"));
});

// WCAG 2.4.1 skip-to-content — programmatic focus instead of #anchor so
// we don't trigger the SPA hash router (gemini review PR #47).  Focuses
// whichever <main class="page active"> is currently visible.
function skipToContent() {
  var target = document.querySelector(".page.active") ||
               document.querySelector(".page");
  if (!target) return;
  if (target.getAttribute("tabindex") === null) {
    target.setAttribute("tabindex", "-1");
  }
  target.focus({ preventScroll: false });
}

// Initialise toggle icon on first script run (DOM is ready since we're at the
// bottom of <body>).
(function () {
  var pref = "auto";
  try { pref = localStorage.getItem("themeOverride") || "auto"; } catch (e) {}
  _themeUpdateToggleIcon(pref);
})();

// Pass 7 CSRF — returns a Promise<string> with the per-boot token.
// Caches the token in window.__csrfToken and deduplicates concurrent fetches
// by holding the in-flight Promise in _csrfFetch until it settles.
var _csrfFetch = null;
function getCsrfToken() {
  if (window.__csrfToken) return Promise.resolve(window.__csrfToken);
  if (_csrfFetch) return _csrfFetch;
  _csrfFetch = fetchWithTimeout("/api/csrf-token", { credentials: "same-origin" }, 15000)
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      if (d && d.token) window.__csrfToken = d.token;
      return window.__csrfToken || "";
    })
    .catch(function () { return ""; })
    .finally(function () { _csrfFetch = null; });
  return _csrfFetch;
}
// Warm the cache early so the first mutating request doesn't pay a round-trip.
getCsrfToken();

// Mutating helper: appends ?csrf=<token> to url and retries once on 403.
// Usage: postWithCsrf(url, opts, timeoutMs).then(r => r.json())
function postWithCsrf(url, opts, timeoutMs) {
  opts = opts || {};
  opts.method = opts.method || "POST";
  return getCsrfToken().then(function (token) {
    var u = token ? url + (url.indexOf("?") >= 0 ? "&" : "?") + "csrf=" + encodeURIComponent(token) : url;
    return fetchWithTimeout(u, opts, timeoutMs || 15000).then(function (r) {
      if (r.status === 403) {
        window.__csrfToken = null;
        return getCsrfToken().then(function (t) {
          var u2 = t ? url + (url.indexOf("?") >= 0 ? "&" : "?") + "csrf=" + encodeURIComponent(t) : url;
          return fetchWithTimeout(u2, opts, timeoutMs || 15000);
        });
      }
      return r;
    });
  });
}
window.postWithCsrf = postWithCsrf;

// ── Board restricted-pin helper (sensor pin warnings) ───────────────────────
// Caches the ACTIVE board's restricted-pin sets from /api/board-profiles so the
// sensor wizard + editor can warn when a chosen GPIO is risky.
var _boardPinsCache = null;
function getBoardPins() {
  if (_boardPinsCache) return Promise.resolve(_boardPinsCache);
  var EMPTY = { strap: [], flash: [], reserved: [], usb: [], maxGpio: 255 };
  return fetchWithTimeout("/api/board-profiles", {}, 15000)
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (d) {
      if (!d || !d.profiles) return EMPTY;
      var activeId = d.active && d.active.id;
      var p = null;
      for (var i = 0; i < d.profiles.length; i++) {
        if (d.profiles[i].id === activeId) { p = d.profiles[i]; break; }
      }
      if (!p) return EMPTY;
      _boardPinsCache = {
        strap:    p.strapPins    || [],
        flash:    p.flashPins    || [],
        reserved: p.reservedPins || [],
        usb:      p.usbPins      || [],
        maxGpio:  (typeof p.maxGpio === "number") ? p.maxGpio : 255,
      };
      return _boardPinsCache;
    })
    .catch(function () { return EMPTY; });
}
// Classify a pin against the board. Returns null if safe, else
// { reason, hard } — hard=true means it can NOT be overridden (flash bus /
// out of range); hard=false is risky-but-usable-with-pull-ups (strapping /
// reserved / USB), matching firmware's allow_unsafe_pins semantics.
function pinRisk(pins, pinVal) {
  var pin = parseInt(pinVal, 10);
  if (isNaN(pin) || pin < 0) return null;
  if ((pins.flash || []).indexOf(pin) >= 0) return { reason: "SPI flash-bus pin", hard: true };
  if (pin > pins.maxGpio)                    return { reason: "GPIO out of range for board", hard: true };
  if ((pins.strap || []).indexOf(pin) >= 0)  return { reason: "bootstrap/strapping pin (boot-mode risk)", hard: false };
  if ((pins.reserved || []).indexOf(pin) >= 0) return { reason: "reserved (UART0 console)", hard: false };
  if ((pins.usb || []).indexOf(pin) >= 0)    return { reason: "USB D+/D- pin", hard: false };
  return null;
}
window.getBoardPins = getBoardPins;
window.pinRisk = pinRisk;


// ============================================================================
// NAVIGATION
// ============================================================================
function nav(el) {
  var page = el.getAttribute("data-page");
  location.hash = page;
  return false;
}

// Pages whose markup is shipped as a separate /pages/<name>.html file and
// injected on first navigation.  Keep dashboard/live/files/settings hub
// inlined in index.html for fast first paint.
var LAZY_PAGES = {
  settings_device:    1,
  settings_hardware:  1,
  settings_datalog:   1,
  settings_corelogic: 1,
  settings_export:    1,
  settings_theme:     1,
  settings_network:   1,
  settings_time:      1,
  settings_modules:   1,
  settings_platform:  1, // aggregator: hardware + core logic + modules
  settings_netime:    1, // aggregator: network + time
  update:             1,
};
var _loadedPartials = {};   // page name → true once injected
var _inflightPartials = {}; // page name → Promise in flight

function loadPagePartial(page) {
  if (!LAZY_PAGES[page]) return Promise.resolve();
  if (_loadedPartials[page]) return Promise.resolve();
  if (_inflightPartials[page]) return _inflightPartials[page];

  var url = "/pages/" + page + ".html";
  var p = fetchWithTimeout(url, {}, 15000)
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.text();
    })
    .then(function (html) {
      // Inject into <main id="main-content"> so the partial becomes a real
      // sibling of the inlined .page sections and lands inside the .app CSS
      // grid's "main" area. Appending to <body> puts it OUTSIDE the grid, so
      // it renders below the full-height app — i.e. below the sidebar.
      var host  = document.createElement("div");
      host.innerHTML = html;
      var mount = document.getElementById("main-content") || document.body;
      while (host.firstChild) mount.appendChild(host.firstChild);
      _loadedPartials[page] = true;
      // click/change/submit/input delegate from document already catch events
      // inside the new nodes; onerror does not bubble, so wire those directly.
      wireLateErrorHandlers(document);
      // icons.js only runs swap() once on DOMContentLoaded. Lazily-injected
      // page partials load afterwards, so their <span data-icon> placeholders
      // would otherwise never be replaced with SVGs (blank icons on Network,
      // Hardware, etc.). Swap the freshly-injected page element now.
      if (window.Icons && Icons.swap) {
        // A valid partial always contains its #page-<name> element, so scope
        // the swap to it — never fall back to document.body (that would
        // re-scan every icon already in the SPA on each navigation).
        var injected = document.getElementById("page-" + page);
        if (injected) Icons.swap(injected);
      }
    })
    .catch(function (e) {
      console.error("loadPagePartial(" + page + ") failed:", e);
      // Swallow — navigateTo's existing fallback will land the user on the
      // settings hub if the page element is still missing.
    })
    .then(function () {
      delete _inflightPartials[page];
    });

  _inflightPartials[page] = p;
  return p;
}

function navigateTo(page) {
  // Stop live timers when leaving live page
  if (currentPage === "live" && page !== "live") {
    if (liveTimer) {
      clearInterval(liveTimer);
      liveTimer = null;
    }
    if (liveES) {
      try { liveES.close(); } catch (e) {}
      liveES = null;
    }
    if (liveLogsTimer) {
      clearInterval(liveLogsTimer);
      liveLogsTimer = null;
    }
  }

  // Stop dashboard /api/latest polling when leaving the dashboard
  if (currentPage === "dashboard" && page !== "dashboard") {
    if (typeof dbStopPolling === "function") dbStopPolling();
  }

  loadPagePartial(page).then(function () {
    document.querySelectorAll(".page").forEach(function (p) {
      p.classList.remove("active");
    });
    document.querySelectorAll(".nav-item, .bnav").forEach(function (a) {
      a.classList.remove("active");
    });

    var topPage = page.startsWith("settings") ? "settings" : page;
    currentPage = page;

    var pageEl = document.getElementById("page-" + page);
    if (pageEl) {
      pageEl.classList.add("active");
    } else {
      var hub = document.getElementById("page-settings");
      if (hub) hub.classList.add("active");
      topPage = "settings";
      currentPage = "settings";
    }

    document
      .querySelectorAll('[data-page="' + topPage + '"]')
      .forEach(function (a) {
        a.classList.add("active");
      });

    pageInit(page);
  });
}

function showSubpage(page) {
  location.hash = page;
}

function pageInit(page) {
  switch (page) {
    case "dashboard":
      dbInit();
      break;
    case "logs":
      logsInit();
      break;
    case "files":
      filesInit();
      break;
    case "live":
      liveInit();
      break;
    case "settings_device":
      sdInit();
      break;
    case "settings_hardware":
      hwInit();
      break;
    case "settings_theme":
      thInit();
      break;
    case "settings_network":
      netInit();
      break;
    case "settings_time":
      timeInit();
      break;
    case "settings_datalog":
      dlInit();
      break;
    case "update":
      otaInit();
      break;
    case "sensors":
      sensorsLoad();
      break;
    case "settings_corelogic":
      clLoad();
      break;
    case "settings_export":
      expLoad();
      break;
    case "settings_modules":
      modulesInit();
      break;
    case "settings":
      break;
  }
}

// ============================================================================
// HELPERS
// ============================================================================
// Toast notifications (Claude Design phase 3).
//
// Backward-compatible with the legacy two-arg form:
//     showToast(msg, "success" | "error")
// plus the richer three-arg form from the design spec:
//     showToast(title, msg, "ok" | "warn" | "err" | "info")
// Aliases: "success" → "ok", "error" → "err".  Each toast carries a Lucide
// icon, an optional body message, a close button, and a countdown bar that
// drains across the 3 s lifetime.
function showToast(a, b, c) {
  var container = document.getElementById("toastContainer");
  if (!container) return;

  var title, msg, type;
  if (arguments.length >= 3) {
    title = a || ""; msg = b || ""; type = c || "info";
  } else {
    title = a || "";  msg = "";  type = b || "info";
  }

  // Normalise legacy type names.
  var typeMap = { success: "ok", error: "err" };
  type = typeMap[type] || type;
  // Distinct icons per type — `err` uses alert-triangle (not `x`) so it
  // doesn't visually collide with the toast's close button (gemini review
  // PR #47).  Mirrors the OTA popup's error icon for consistency.
  var ICON = { ok: "check", warn: "alert-triangle", err: "alert-triangle", info: "info" };
  var iconName = ICON[type] || "info";

  var el = document.createElement("div");
  el.className = "toast toast-" + type;
  el.setAttribute("role", type === "err" ? "alert" : "status");
  el.setAttribute("aria-live", type === "err" ? "assertive" : "polite");

  var iconSpan = document.createElement("span");
  iconSpan.className = "toast-icon";
  iconSpan.setAttribute("data-icon", iconName);
  el.appendChild(iconSpan);

  var body = document.createElement("div");
  body.className = "toast-body";
  var titleEl = document.createElement("div");
  titleEl.className = "toast-title";
  titleEl.textContent = title;
  body.appendChild(titleEl);
  if (msg) {
    var msgEl = document.createElement("div");
    msgEl.className = "toast-msg";
    msgEl.textContent = msg;
    body.appendChild(msgEl);
  }
  el.appendChild(body);

  var close = document.createElement("button");
  close.type = "button";
  close.className = "toast-close";
  close.setAttribute("aria-label", "Dismiss notification");
  var closeIcon = document.createElement("span");
  closeIcon.setAttribute("data-icon", "x");
  close.appendChild(closeIcon);
  el.appendChild(close);

  var countdown = document.createElement("div");
  countdown.className = "toast-countdown";
  countdown.style.animationDuration = "3000ms";
  el.appendChild(countdown);

  container.appendChild(el);
  if (window.Icons && Icons.swap) Icons.swap(el);

  function dismiss() {
    if (!container.contains(el)) return;
    el.classList.add("toast-dismissing");
    setTimeout(function () {
      if (container.contains(el)) container.removeChild(el);
    }, 260);
  }
  close.addEventListener("click", dismiss);
  setTimeout(dismiss, 3000);
}

// showUndoToast(title, msg, onUndo, opts?)
// Wraps showToast with an "Undo" affordance for low-risk destructive
// actions (delete file, remove sensor, reset layout).  The action is taken
// optimistically — caller already removed the data — and onUndo() is
// called only if the user clicks Undo before the toast (8 s default) drains.
//   opts.duration  override the 8 000 ms default
//   opts.onCommit  fires on dismiss (timeout or close) when undo was NOT clicked
function showUndoToast(title, msg, onUndo, opts) {
  opts = opts || {};
  var duration = opts.duration || 8000;
  var container = document.getElementById("toastContainer");
  if (!container) return;

  var el = document.createElement("div");
  el.className = "toast toast-info toast-undo";
  el.setAttribute("role", "status");
  el.setAttribute("aria-live", "polite");

  var iconSpan = document.createElement("span");
  iconSpan.className = "toast-icon";
  iconSpan.setAttribute("data-icon", "info");
  el.appendChild(iconSpan);

  var body = document.createElement("div");
  body.className = "toast-body";
  var titleEl = document.createElement("div");
  titleEl.className = "toast-title";
  titleEl.textContent = title || "";
  body.appendChild(titleEl);
  if (msg) {
    var msgEl = document.createElement("div");
    msgEl.className = "toast-msg";
    msgEl.textContent = msg;
    body.appendChild(msgEl);
  }
  el.appendChild(body);

  var undoBtn = document.createElement("button");
  undoBtn.type = "button";
  undoBtn.className = "btn-mini toast-undo-btn";
  undoBtn.textContent = "Undo";
  el.appendChild(undoBtn);

  var close = document.createElement("button");
  close.type = "button";
  close.className = "toast-close";
  close.setAttribute("aria-label", "Dismiss notification");
  var closeIcon = document.createElement("span");
  closeIcon.setAttribute("data-icon", "x");
  close.appendChild(closeIcon);
  el.appendChild(close);

  var countdown = document.createElement("div");
  countdown.className = "toast-countdown";
  countdown.style.animationDuration = duration + "ms";
  el.appendChild(countdown);

  container.appendChild(el);
  if (window.Icons && Icons.swap) Icons.swap(el);

  var dismissed = false;
  var undone   = false;
  var timer    = null;

  function dismiss(committed) {
    if (dismissed) return;
    dismissed = true;
    clearTimeout(timer);
    if (committed && !undone && typeof opts.onCommit === "function") {
      try { opts.onCommit(); } catch (e) {}
    }
    el.classList.add("toast-dismissing");
    setTimeout(function () {
      if (container.contains(el)) container.removeChild(el);
    }, 260);
  }

  undoBtn.addEventListener("click", function () {
    undone = true;
    if (typeof onUndo === "function") {
      try { onUndo(); } catch (e) {}
    }
    dismiss(false);
  });
  close.addEventListener("click", function () { dismiss(true); });
  timer = setTimeout(function () { dismiss(true); }, duration);
}
window.showUndoToast = showUndoToast;

// emptyState(opts) — returns DOM for the design's structured empty state.
// opts: { icon, title, msg, ctaText, ctaPage }.  Title is required;
// everything else is optional.  Used by page renderers to swap raw
// "Loading..." / "No X" strings for an icon + message + optional CTA.
//
//   container.innerHTML = "";
//   container.appendChild(emptyState({
//     icon: "folder", title: "No files",
//     msg: "Upload a file to get started.",
//     ctaText: "Upload", ctaPage: "files",
//   }));
function emptyState(opts) {
  opts = opts || {};
  var wrap = document.createElement("div");
  wrap.className = "empty-state";

  if (opts.icon) {
    var ic = document.createElement("span");
    ic.className = "empty-icon";
    var iSpan = document.createElement("span");
    iSpan.setAttribute("data-icon", opts.icon);
    ic.appendChild(iSpan);
    wrap.appendChild(ic);
  }
  if (opts.title) {
    var t = document.createElement("div");
    t.className = "empty-title";
    t.textContent = opts.title;
    wrap.appendChild(t);
  }
  if (opts.msg) {
    var m = document.createElement("div");
    m.className = "empty-msg";
    m.textContent = opts.msg;
    wrap.appendChild(m);
  }
  if (opts.ctaText && opts.ctaPage) {
    var btn = document.createElement("button");
    btn.type = "button";
    btn.className = "btn primary empty-cta";
    btn.setAttribute("data-click", "navPage");
    btn.setAttribute("data-page", opts.ctaPage);
    btn.textContent = opts.ctaText;
    wrap.appendChild(btn);
  }

  // If Icons module is loaded, swap the data-icon spans now.
  if (window.Icons && Icons.swap) Icons.swap(wrap);
  return wrap;
}

// HTML-escape for safe insertion into innerHTML. Use textContent / DOM
// construction where possible; reach for esc() only when string templates
// are unavoidable. Defined in core.js so all later modules can call it.
function esc(s) {
  if (s === undefined || s === null) return "";
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function setEl(id, val) {
  var e = document.getElementById(id);
  if (e && val !== undefined && val !== null) e.textContent = val;
}
function setElStyle(id, prop, val) {
  var e = document.getElementById(id);
  if (e && val) e.style[prop] = val;
}
function setVal(id, val) {
  var e = document.getElementById(id);
  if (e && val !== undefined && val !== null) e.value = val;
}
function setChk(id, val) {
  var e = document.getElementById(id);
  if (e) e.checked = !!val;
}
function getVal(id) {
  var e = document.getElementById(id);
  return e ? e.value : "";
}

function fmtBytes(b) {
  if (!b && b !== 0) return "-";
  if (b >= 1073741824) return (b / 1073741824).toFixed(2) + " GB";
  if (b >= 1048576) return (b / 1048576).toFixed(1) + " MB";
  if (b >= 1024) return (b / 1024).toFixed(1) + " KB";
  return b + " B";
}

function hexToRgba(hex, a) {
  if (!hex || hex.length < 7) return "rgba(149,165,166," + a + ")";
  var r = parseInt(hex.slice(1, 3), 16),
    g = parseInt(hex.slice(3, 5), 16),
    b = parseInt(hex.slice(5, 7), 16);
  return "rgba(" + r + "," + g + "," + b + "," + a + ")";
}

function togglePass(id) {
  var e = document.getElementById(id);
  if (e) e.type = e.type === "password" ? "text" : "password";
}

function showMsg(containerId, html, autoClear) {
  var el = document.getElementById(containerId);
  if (el) {
    el.innerHTML = html;
    if (autoClear)
      setTimeout(function () {
        el.innerHTML = "";
      }, 4000);
  }
}

var PAGE_MSG_IDS = {
  settings_device: "sd-msg",
  settings_hardware: "hw-msg",
  settings_theme: "th-msg",
  settings_network: "net-msg",
  settings_time: "time-msg",
  settings_datalog: "dl-msg",
  sensors:          "sl-msg",
};

function settingsSave(ev, url, form, restart) {
  if (ev) ev.preventDefault();

  // Pending state: disable the submit button + swap its label for a spinner
  // while the ESP round-trip is in flight (guards against double-submits on
  // slow links). Restart-flagged saves keep the spinner — the device is
  // rebooting and the restart flow owns the rest of the UX.
  var submitBtn = form && form.querySelector('button[type="submit"]');
  function setPending(on) {
    if (!submitBtn) return;
    submitBtn.disabled = on;
    submitBtn.classList.toggle("btn-loading", on);
  }
  function msgIdForPage() {
    return PAGE_MSG_IDS[currentPage] ||
      currentPage.replace("settings_", "") + "-msg";
  }

  function doPost(token, isRetry) {
    var fd = new FormData(form);
    if (token) fd.append("csrf", token);
    var xhr = new XMLHttpRequest();
    xhr.open("POST", url);
    xhr.onload = function () {
      if (restart) return;
      // 403 csrf mismatch — clear cache and retry once with a fresh token.
      if (xhr.status === 403 && !isRetry) {
        window.__csrfToken = null;
        getCsrfToken().then(function (t) { doPost(t, true); });
        return;
      }
      setPending(false);
      try {
        var r = JSON.parse(xhr.responseText);
        if (r.ok) {
          // No full-page reload: the form already shows what was saved.
          // Confirm via toast, then re-sync the shared caches (CFG +
          // /api/status) and app chrome that other pages read from.
          showToast("Settings saved", "ok");
          fetchWithTimeout("/export_settings")
            .then(function (res) { return res.json(); })
            .then(function (cfg) { CFG = cfg; })
            .catch(function () {});
          getStatus({ maxAgeMs: 0 })
            .then(function (d) { applyStatus(d); })
            .catch(function () {});
        } else {
          showMsg(
            msgIdForPage(),
            "<div class='alert alert-error'>" +
              esc(r.error || "Unknown error") + "</div>",
            true,
          );
        }
      } catch (e) {
        showMsg(
          msgIdForPage(),
          "<div class='alert alert-error'>Malformed response from device</div>",
          true,
        );
      }
    };
    xhr.onerror = function () {
      setPending(false);
      showMsg(
        msgIdForPage(),
        "<div class='alert alert-error'>Network error — settings not saved</div>",
        true,
      );
    };
    xhr.send(fd);
  }

  setPending(true);
  getCsrfToken().then(function (token) { doPost(token, false); });
}

// ============================================================================
// COMPONENT HELPERS  (Pass 4 A3)
// ============================================================================
// Tiny DOM builder: h("input", { type: "number", name: "x" })
// Children may be a string, a Node, or an array of those (nested allowed).
function h(tag, attrs, children) {
  var el = document.createElement(tag);
  if (attrs) {
    for (var k in attrs) {
      if (!Object.prototype.hasOwnProperty.call(attrs, k)) continue;
      var v = attrs[k];
      if (v === null || v === undefined || v === false) continue;
      if (k === "class")        el.className = v;
      else if (k === "style")   for (var sk in v) el.style[sk] = v[sk];
      else if (k === "html")    el.innerHTML = v;       // caller is responsible
      else if (k === "text")    el.textContent = v;     // safe
      else if (k.indexOf("on") === 0 && typeof v === "function")
                                el.addEventListener(k.substring(2), v);
      else if (k === "dataset") for (var dk in v) el.dataset[dk] = v[dk];
      else                      el.setAttribute(k, v === true ? "" : v);
    }
  }
  function add(c) {
    if (c == null || c === false) return;
    if (Array.isArray(c)) c.forEach(add);
    else el.appendChild(c.nodeType ? c : document.createTextNode(String(c)));
  }
  add(children);
  return el;
}

// ----------------------------------------------------------------------------
// Form.bind(host, schema, data) — schema-driven settings form renderer.
//
// schema = {
//   saveUrl:   "/save_hardware",       // POST target
//   restart:   true,                   // show restart popup on success
//   confirm:   "Save and restart?",    // optional pre-submit confirm()
//   submitLabel: "💾 Save & Restart",
//   sections: [
//     { title: "💾 Storage", hint: "...optional intro paragraph...",
//       fields: [
//         { name: "storageType", label: "Type", type: "select",
//           options: [["0","LittleFS"],["1","SD"]],
//           hint: "..." },
//         { row: [                                  // horizontal group
//           { name: "pinSdCS",   label: "CS",   type: "number",
//             showWhen: { storageType: "1" } },    // value-conditional
//           { name: "pinSdMOSI", label: "MOSI", type: "number" },
//         ]},
//         { name: "testMode", label: "Enable", type: "checkbox" },
//       ]
//     }
//   ]
// };
//
// Field types: text | number | password | select | checkbox
// Per-field options: min, max, step, placeholder, hint, hidden, showWhen.
//
// Form.bind returns the rendered <form>; values can be re-applied later via
// Form.fill(form, data). Submit is wired to settingsSave().
// ----------------------------------------------------------------------------
var Form = (function () {
  function field(f, data) {
    if (f.row) {
      return h("div", { class: "form-grid" }, f.row.map(function (sub) {
        return field(sub, data);
      }));
    }

    var val = (data != null && f.name in data) ? data[f.name] : f.value;
    var input;

    if (f.type === "select") {
      input = h("select", {
        name: f.name, id: f.id || null,
        class: "input",
      }, (f.options || []).map(function (opt) {
        var ov = Array.isArray(opt) ? opt[0] : opt.value;
        var ol = Array.isArray(opt) ? opt[1] : opt.label;
        var o = h("option", { value: ov, text: ol });
        if (String(val) === String(ov)) o.selected = true;
        return o;
      }));
    } else if (f.type === "checkbox") {
      input = h("input", {
        type: "checkbox", name: f.name, id: f.id || null, value: "1",
      });
      if (val) input.checked = true;
    } else {
      input = h("input", {
        type: f.type || "text", name: f.name, id: f.id || null,
        class: "input",
        min: f.min, max: f.max, step: f.step,
        placeholder: f.placeholder,
      });
      if (val !== undefined && val !== null) input.value = val;
    }

    var label = f.label
      ? h("label", { class: "field-label" },
          f.type === "checkbox" ? [input, " " + f.label] : f.label)
      : null;
    var hint = f.hint ? h("p", { class: "hint", text: f.hint }) : null;

    var group = h("div", {
      class: "field",
      dataset: f.showWhen ? { showwhen: JSON.stringify(f.showWhen) } : null,
    }, f.type === "checkbox" ? [label, hint] : [label, input, hint]);

    if (f.hidden) group.style.display = "none";
    return group;
  }

  function applyShowWhen(form) {
    var groups = form.querySelectorAll("[data-showwhen]");
    if (!groups.length) return;
    function eval1(spec) {
      for (var k in spec) {
        var el = form.elements[k];
        if (!el) return false;
        var v = el.type === "checkbox" ? (el.checked ? "1" : "") : el.value;
        if (String(v) !== String(spec[k])) return false;
      }
      return true;
    }
    function refresh() {
      groups.forEach(function (g) {
        var spec;
        try { spec = JSON.parse(g.dataset.showwhen); } catch (e) { return; }
        g.style.display = eval1(spec) ? "" : "none";
      });
    }
    form.addEventListener("change", refresh);
    refresh();
  }

  function bind(host, schema, data) {
    if (typeof host === "string") host = document.getElementById(host);
    if (!host) return null;
    host.innerHTML = "";

    var msgId = PAGE_MSG_IDS[currentPage] || (currentPage.replace("settings_","") + "-msg");
    var form = h("form", {}, [
      h("div", { id: msgId }),
      schema.sections.map(function (sec) {
        return h("div", { class: "card" }, [
          sec.title
            ? h("div", { class: "card-head", text: sec.title })
            : null,
          h("div", { class: "card-body" }, [
            sec.hint ? h("p", { class: "hint", text: sec.hint }) : null,
            sec.fields.map(function (f) { return field(f, data); }),
          ]),
        ]);
      }),
      h("button", {
        type: "submit", class: "btn primary",
        text: schema.submitLabel || "💾 Save",
      }),
    ]);

    form.addEventListener("submit", function (ev) {
      if (schema.confirm && !confirm(schema.confirm)) {
        ev.preventDefault();
        return;
      }
      settingsSave(ev, schema.saveUrl, form, !!schema.restart);
    });

    host.appendChild(form);
    applyShowWhen(form);
    return form;
  }

  // Re-apply data values (e.g. after a refresh fetch).
  function fill(form, data) {
    if (!form || !data) return;
    Array.prototype.forEach.call(form.elements, function (el) {
      if (!el.name || !(el.name in data)) return;
      if (el.type === "checkbox") el.checked = !!data[el.name];
      else el.value = data[el.name];
    });
    form.dispatchEvent(new Event("change"));
  }

  return { bind: bind, fill: fill };
})();

// ============================================================================
// RESTART POPUP
// ============================================================================
function confirmRestart() {
  document.getElementById("rPopButtons").style.display = "none";
  document.getElementById("rPopProgress").style.display = "block";
  setEl("rPopIcon", "⏳");
  setEl("rPopTitle", "Restarting…");
  var s = 5,
    bar = document.getElementById("rPopBar");
  var tick = function () {
    document.getElementById("rPopMsg").innerHTML =
      "Redirecting in <strong>" + s + "</strong> seconds…";
    if (bar) bar.style.width = (5 - s) * 20 + "%";
    if (s <= 0) {
      // /restart is CSRF-gated — use postWithCsrf so the device actually
      // restarts instead of silently 403-ing.
      postWithCsrf("/restart", { method: "POST" }, 10000)
        .catch(function () { /* device is rebooting — the dropped connection is expected */ })
        .finally(function () {
          location.hash = "dashboard";
          location.reload();
        });
    } else {
      s--;
      setTimeout(tick, 1000);
    }
  };
  tick();
}

// Enrol every handler referenced from markup via data-click / data-change /
// data-input / data-submit / data-error / data-backdrop-fn.  Anything not in
// this list is un-callable through the dispatcher even if injected HTML tries.
registerHandlers({
  navPage: navPage,
  quickThemeToggle: quickThemeToggle,
  quickDensityToggle: quickDensityToggle,
  accentPickerToggle: accentPickerToggle,
  setAccent: setAccent,
  sidebarRailToggle: sidebarRailToggle,
  skipToContent: skipToContent,
  showPopup: showPopup,
  hidePopup: hidePopup,
  showSubpage: showSubpage,
  backdropClose: backdropClose,
  hideParent: hideParent,
  togglePass: togglePass,
  settingsSaveForm: settingsSaveForm,
  submitParentForm: submitParentForm,
  confirmRestart: confirmRestart,
});

// ============================================================================
// KEYBOARD SHORTCUTS (Claude Design phase 5a)
//
// Two-key "G <letter>" sequences for top-level page nav; `?` opens a tiny
// help sheet listing every binding.  Skipped when focus is in a text input,
// so typing "go" in an SSID field doesn't hijack the keystroke.
// ============================================================================
(function () {
  "use strict";

  var MAP = {
    d: "dashboard",
    l: "live",
    s: "settings",
    f: "files",
    c: "sensors",   /* "core logic" in the design == sensors in current UI */
    u: "update"
  };

  var waitingForSecond = false;
  var timer = null;

  function inTypableContext(ev) {
    var t = ev.target;
    if (!t) return false;
    var tag = (t.tagName || "").toLowerCase();
    if (tag === "input" || tag === "textarea" || tag === "select") return true;
    if (t.isContentEditable) return true;
    return false;
  }

  // Locally scoped to the shortcut IIFE — named differently from the
  // global navigateTo(url) to avoid shadowing (gemini review PR #47).
  function triggerPageLink(pageId) {
    var link = document.querySelector('a[data-page="' + pageId + '"]');
    if (link) link.click();
  }

  function openHelp() {
    if (document.getElementById("kbHelpSheet")) return;
    var sheet = document.createElement("div");
    sheet.id = "kbHelpSheet";
    sheet.setAttribute("role", "dialog");
    sheet.setAttribute("aria-label", "Keyboard shortcuts");
    sheet.className = "kb-help-sheet";
    sheet.innerHTML =
      '<div class="kb-help-card">' +
        '<div class="kb-help-title">Keyboard shortcuts</div>' +
        '<ul class="kb-help-list">' +
          '<li><kbd>⌘</kbd> <kbd>K</kbd><span>Command palette (also <kbd>Ctrl K</kbd> or <kbd>/</kbd>)</span></li>' +
          '<li><kbd>,</kbd><span>Quick settings drawer</span></li>' +
          '<li><kbd>G</kbd> <kbd>D</kbd><span>Dashboard</span></li>' +
          '<li><kbd>G</kbd> <kbd>O</kbd><span>Overview</span></li>' +
          '<li><kbd>G</kbd> <kbd>A</kbd><span>Alerts</span></li>' +
          '<li><kbd>G</kbd> <kbd>L</kbd><span>Live</span></li>' +
          '<li><kbd>G</kbd> <kbd>F</kbd><span>Files</span></li>' +
          '<li><kbd>G</kbd> <kbd>C</kbd><span>Sensors</span></li>' +
          '<li><kbd>G</kbd> <kbd>S</kbd><span>Settings</span></li>' +
          '<li><kbd>G</kbd> <kbd>U</kbd><span>Update</span></li>' +
          '<li><kbd>?</kbd><span>Show this help</span></li>' +
          '<li><kbd>Esc</kbd><span>Close help</span></li>' +
        '</ul>' +
        '<div class="kb-help-hint">Click outside to dismiss</div>' +
      '</div>';
    sheet.addEventListener("click", function (ev) {
      if (ev.target === sheet) closeHelp();
    });
    document.body.appendChild(sheet);
  }
  function closeHelp() {
    var s = document.getElementById("kbHelpSheet");
    if (s && s.parentNode) s.parentNode.removeChild(s);
  }

  document.addEventListener("keydown", function (ev) {
    if (ev.ctrlKey || ev.metaKey || ev.altKey) return;
    if (inTypableContext(ev)) return;

    var key = (ev.key || "").toLowerCase();

    if (key === "escape") {
      closeHelp();
      return;
    }
    if (ev.key === "?") {
      ev.preventDefault();
      openHelp();
      return;
    }

    if (waitingForSecond) {
      waitingForSecond = false;
      clearTimeout(timer);
      if (MAP[key]) {
        ev.preventDefault();
        triggerPageLink(MAP[key]);
      }
      return;
    }

    if (key === "g") {
      ev.preventDefault();
      waitingForSecond = true;
      timer = setTimeout(function () { waitingForSecond = false; }, 1200);
    }
  });
})();

