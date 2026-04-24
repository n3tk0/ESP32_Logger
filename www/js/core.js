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
// GLOBALS
// ============================================================================
var ST = {}; // cached /api/status payload
var CFG = {}; // cached /export_settings payload
var dbChart = null; // Chart.js instance on dashboard
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
    var t = ev.target.closest("[data-" + eventName + "]");
    if (!t) return;
    var name = t.getAttribute("data-" + eventName);
    var fn = Handlers[name];
    if (typeof fn !== "function") return;
    var args;
    var raw = t.getAttribute("data-args");
    if (raw) {
      try {
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
    if (ev.target.closest("[data-submit]")) ev.preventDefault();
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
function showPopup(id) { var el = document.getElementById(id); if (el) el.style.display = "flex"; }
function hidePopup(id) { var el = document.getElementById(id); if (el) el.style.display = "none"; }

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
    fetch("/api/status")
      .then(function (r) {
        return r.json();
      })
      .catch(function () {
        return {};
      }),
    fetch("/export_settings")
      .then(function (r) {
        return r.json();
      })
      .catch(function () {
        return {};
      }),
  ]).then(function (results) {
    ST = results[0];
    CFG = results[1];
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
      if (effective === "dark") {
        actDark = true;
      } else if (effective === "auto") {
        actDark =
          window.matchMedia &&
          window.matchMedia("(prefers-color-scheme: dark)").matches;
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
      }
      _themeUpdateToggleIcon(effective);
    }

    var vars = ":root{";
    if (th.primaryColor) vars += "--primary:" + th.primaryColor + ";";
    if (th.secondaryColor) vars += "--secondary:" + th.secondaryColor + ";";
    if (actDark) {
      if (th.darkBgColor) vars += "--bg:" + th.darkBgColor + ";";
      if (th.darkTextColor) vars += "--text:" + th.darkTextColor + ";";
    } else {
      if (th.lightBgColor) vars += "--bg:" + th.lightBgColor + ";";
      if (th.lightTextColor) vars += "--text:" + th.lightTextColor + ";";
    }
    vars += "}";
    var style = document.getElementById("themeVars");
    if (style) style.textContent = vars;

    // Dashboard legend dot colors — matches original inline style in .ino
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
  if (d.cpu !== undefined && d.cpu !== null) setEl("footer-cpu", d.cpu);
  if (d.heap !== undefined && d.heapTotal !== undefined)
    setEl("footer-heap", fmtBytes(d.heap) + " / " + fmtBytes(d.heapTotal));
  if (d.network !== undefined && d.network !== null)
    setEl("footer-net", d.network);
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

function quickThemeToggle() {
  var current;
  try { current = localStorage.getItem("themeOverride") || "auto"; }
  catch (e) { current = "auto"; }
  // Cycle: auto → dark → light → auto
  var next = current === "auto" ? "dark" : (current === "dark" ? "light" : "auto");
  _themeApplyOverride(next);
}

// Initialise toggle icon on first script run (DOM is ready since we're at the
// bottom of <body>).
(function () {
  var pref = "auto";
  try { pref = localStorage.getItem("themeOverride") || "auto"; } catch (e) {}
  _themeUpdateToggleIcon(pref);
})();

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
  settings_flowmeter: 1,
  settings_hardware:  1,
  settings_datalog:   1,
  settings_corelogic: 1,
  settings_export:    1,
  settings_theme:     1,
  settings_network:   1,
  settings_time:      1,
  settings_modules:   1,
  update:             1,
};
var _loadedPartials = {};   // page name → true once injected
var _inflightPartials = {}; // page name → Promise in flight

function loadPagePartial(page) {
  if (!LAZY_PAGES[page]) return Promise.resolve();
  if (_loadedPartials[page]) return Promise.resolve();
  if (_inflightPartials[page]) return _inflightPartials[page];

  var url = "/pages/" + page + ".html";
  var p = fetch(url)
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.text();
    })
    .then(function (html) {
      // Inject as the last child of <body> so it becomes a sibling of the
      // other .page elements and the existing .active toggle logic finds it.
      var host = document.createElement("div");
      host.innerHTML = html;
      while (host.firstChild) document.body.appendChild(host.firstChild);
      _loadedPartials[page] = true;
      // click/change/submit/input delegate from document already catch events
      // inside the new nodes; onerror does not bubble, so wire those directly.
      wireLateErrorHandlers(document);
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

  loadPagePartial(page).then(function () {
    document.querySelectorAll(".page").forEach(function (p) {
      p.classList.remove("active");
    });
    document.querySelectorAll(".nav-item, .bottom-nav a").forEach(function (a) {
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
    applySettingsFlash();
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
    case "files":
      filesInit();
      break;
    case "live":
      liveInit();
      break;
    case "settings_device":
      sdInit();
      break;
    case "settings_flowmeter":
      sfInit();
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
  var ICON = { ok: "check", warn: "alert-triangle", err: "x", info: "info" };
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
  settings_flowmeter: "sf-msg",
  settings_hardware: "hw-msg",
  settings_theme: "th-msg",
  settings_network: "net-msg",
  settings_time: "time-msg",
  settings_datalog: "dl-msg",
};

function settingsSave(ev, url, form, restart) {
  if (ev) ev.preventDefault();
  var fd = new FormData(form);
  var xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  xhr.onload = function () {
    if (restart) return;
    try {
      var r = JSON.parse(xhr.responseText);
      var msgId =
        PAGE_MSG_IDS[currentPage] ||
        currentPage.replace("settings_", "") + "-msg";
      if (r.ok) {
        sessionStorage.setItem(
          "settingsFlash",
          JSON.stringify({
            page: currentPage,
            html: "<div class='alert alert-success'>✅ Settings saved successfully</div>",
          }),
        );
        setTimeout(function () {
          location.reload();
        }, 300);
      } else {
        showMsg(
          msgId,
          "<div class='alert alert-error'>❌ " +
            (r.error || "Unknown error") +
            "</div>",
          true,
        );
      }
    } catch (e) {}
  };
  xhr.onerror = function () {
    var msgId =
      PAGE_MSG_IDS[currentPage] ||
      currentPage.replace("settings_", "") + "-msg";
    showMsg(
      msgId,
      "<div class='alert alert-error'>❌ Network error</div>",
      true,
    );
  };
  xhr.send(fd);
}

function applySettingsFlash() {
  var raw = sessionStorage.getItem("settingsFlash");
  if (!raw) return;
  sessionStorage.removeItem("settingsFlash");
  try {
    var f = JSON.parse(raw);
    if (!f || !f.page || !f.html) return;
    var msgId =
      PAGE_MSG_IDS[f.page] || f.page.replace("settings_", "") + "-msg";
    showMsg(msgId, f.html, true);
  } catch (e) {}
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
      return h("div", { class: "form-row" }, f.row.map(function (sub) {
        return field(sub, data);
      }));
    }

    var val = (data != null && f.name in data) ? data[f.name] : f.value;
    var input;

    if (f.type === "select") {
      input = h("select", {
        name: f.name, id: f.id || null,
        class: "form-input form-select",
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
        class: "form-input",
        min: f.min, max: f.max, step: f.step,
        placeholder: f.placeholder,
      });
      if (val !== undefined && val !== null) input.value = val;
    }

    var label = f.label
      ? h("label", { class: "form-label" },
          f.type === "checkbox" ? [input, " " + f.label] : f.label)
      : null;
    var hint = f.hint ? h("p", { class: "form-hint", text: f.hint }) : null;

    var group = h("div", {
      class: "form-group",
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
            ? h("div", { class: "card-header", text: sec.title })
            : null,
          h("div", { class: "card-body" }, [
            sec.hint ? h("p", { class: "form-hint", text: sec.hint }) : null,
            sec.fields.map(function (f) { return field(f, data); }),
          ]),
        ]);
      }),
      h("button", {
        type: "submit", class: "btn btn-primary btn-block",
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
function showRestartPopup() {
  setEl("rPopIcon", "🔄");
  setEl("rPopTitle", "Restart Device?");
  setEl(
    "rPopMsg",
    "The device will restart. Any unsaved changes will be lost.",
  );
  document.getElementById("rPopProgress").style.display = "none";
  document.getElementById("rPopButtons").style.display = "flex";
  document.getElementById("restartPopup").style.display = "flex";
}
function closeRestart() {
  document.getElementById("restartPopup").style.display = "none";
}
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
      fetch("/restart", { method: "POST" }).finally(function () {
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

