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
// BOOTSTRAP
// ============================================================================
window.addEventListener("DOMContentLoaded", function () {
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
  }
}

// ============================================================================
// HELPERS
// ============================================================================
function showToast(msg, type) {
  var c = document.getElementById("toastContainer");
  if (!c) return;
  var el = document.createElement("div");
  el.className = "toast " + (type === "error" ? "toast-error" : "toast-success");
  el.textContent = (type === "error" ? "❌ " : "✅ ") + msg;
  c.appendChild(el);
  setTimeout(function() {
    if(c.contains(el)) c.removeChild(el);
  }, 3000);
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

