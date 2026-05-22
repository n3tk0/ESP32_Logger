/**
 * /www/js/command-palette.js — ⌘K / Ctrl+K command palette
 *
 * Single overlay that searches the most-used surfaces:
 *   • Pages (sidebar nav)
 *   • Sensors (CFG.platform.sensors, jumps to sensor edit)
 *   • Settings sub-pages (Device, Hardware, Core logic, …)
 *   • Actions (toggle theme, switch accent, restart device, …)
 *
 * Open with ⌘K (mac) / Ctrl+K (everywhere else) or `/` from any non-input.
 * Arrow keys + Enter navigate the results; Esc closes.
 *
 * Items are fuzzy-matched on title + keywords, ranked by:
 *   1. exact prefix match
 *   2. word-prefix match
 *   3. substring match
 *
 * No external deps.  Loaded via <script defer> in index.html after core.js.
 */

"use strict";

(function () {

  // ── Sources ──────────────────────────────────────────────────────────────
  // Static registry; sensor list is rebuilt each time the palette opens so
  // newly-added sensors show up without a reload.
  var STATIC_ITEMS = [
    // Pages
    { id: "p-overview",  group: "Page", title: "Overview",          icon: "layout-grid",      kw: "home iot",                 act: function () { navigateTo("overview");  } },
    { id: "p-dashboard", group: "Page", title: "Dashboard",         icon: "layout-dashboard", kw: "water legacy",             act: function () { navigateTo("dashboard"); } },
    { id: "p-sensors",   group: "Page", title: "Sensors",           icon: "thermometer",      kw: "readings env",             act: function () { navigateTo("sensors");   } },
    { id: "p-alerts",    group: "Page", title: "Alerts",            icon: "bell-ring",        kw: "rules notifications",      act: function () { navigateTo("alerts");    } },
    { id: "p-logs",      group: "Page", title: "Log viewer",        icon: "book-text",        kw: "flowmeter",                act: function () { navigateTo("logs");      } },
    { id: "p-files",     group: "Page", title: "Files",             icon: "folder",           kw: "browser littlefs sd",      act: function () { navigateTo("files");     } },
    { id: "p-settings",  group: "Page", title: "Settings",          icon: "settings",         kw: "options",                  act: function () { navigateTo("settings");  } },
    { id: "p-update",    group: "Page", title: "Firmware update",   icon: "cloud-upload",     kw: "ota upload",               act: function () { navigateTo("update");    } },

    // Settings sub-pages
    { id: "s-device",   group: "Settings", title: "Device",          icon: "settings", kw: "name id",                 act: function () { location.hash = "settings_device";   } },
    { id: "s-platform", group: "Settings", title: "Platform",        icon: "cpu",      kw: "hardware core logic mode pins sensors modules", act: function () { location.hash = "settings_platform"; } },
    { id: "s-netime",   group: "Settings", title: "Network & Time",  icon: "wifi",     kw: "wifi ssid ap ntp timezone", act: function () { location.hash = "settings_netime"; } },
    { id: "s-datalog",  group: "Settings", title: "Data log",        icon: "file-text", kw: "rotation retention csv",  act: function () { location.hash = "settings_datalog"; } },
    { id: "s-export",   group: "Settings", title: "Export",          icon: "cloud-upload", kw: "mqtt http opensensemap webhook", act: function () { location.hash = "settings_export";  } },

    // Actions — theme.  Delegate to core.setTheme() so the data-theme
    // attribute, the theme-X class on <html>, localStorage, AND the topbar
    // toggle icon all update in one place (gemini review PR #108).
    { id: "a-theme-dark",  group: "Action", title: "Switch to dark theme",  icon: "moon", kw: "appearance", act: function () { window.setTheme && setTheme("dark");  } },
    { id: "a-theme-light", group: "Action", title: "Switch to light theme", icon: "sun",  kw: "appearance", act: function () { window.setTheme && setTheme("light"); } },
    { id: "a-theme-auto",  group: "Action", title: "Theme follows OS",      icon: "moon", kw: "auto",       act: function () { window.setTheme && setTheme("auto");  } },
    // Actions — accent
    { id: "a-accent-cyan",   group: "Action", title: "Accent: cyan",   icon: "palette", kw: "color theme default", act: function () { window.setAccent && setAccent("cyan");   } },
    { id: "a-accent-amber",  group: "Action", title: "Accent: amber",  icon: "palette", kw: "color theme",         act: function () { window.setAccent && setAccent("amber");  } },
    { id: "a-accent-green",  group: "Action", title: "Accent: green",  icon: "palette", kw: "color theme",         act: function () { window.setAccent && setAccent("green");  } },
    { id: "a-accent-violet", group: "Action", title: "Accent: violet", icon: "palette", kw: "color theme",         act: function () { window.setAccent && setAccent("violet"); } },
    // Actions — density
    { id: "a-density-toggle", group: "Action", title: "Toggle compact density", icon: "rows-3", kw: "comfortable spacing", act: function () { window.quickDensityToggle && quickDensityToggle(); } },
    // Actions — sidebar
    { id: "a-sidebar-toggle", group: "Action", title: "Toggle sidebar rail",    icon: "menu",   kw: "collapse expand",     act: function () { window.sidebarRailToggle && sidebarRailToggle(); } },
    // Actions — destructive
    { id: "a-restart", group: "Action", title: "Restart device…",  icon: "rotate-ccw", kw: "reboot",     act: function () { window.showPopup && showPopup("restartPopup"); } },
    { id: "a-add-sensor", group: "Action", title: "Add sensor…",   icon: "plus",       kw: "new",       act: function () { window.openSensorWizard && openSensorWizard(); } },
    // Actions — quick settings drawer
    { id: "a-quick-settings", group: "Action", title: "Open quick settings",  icon: "sliders-horizontal", kw: "drawer panel", act: function () { window.QuickSettings && QuickSettings.open(); } },
  ];

  function buildItems() {
    var items = STATIC_ITEMS.slice();

    // Sensors from current platform config
    if (window.PCFG && PCFG.sensors && PCFG.sensors.length) {
      PCFG.sensors.forEach(function (s, idx) {
        var label = (s.id || s.type) + " · " + (s.type || "—");
        items.push({
          id: "sensor-" + (s.id || idx),
          group: "Sensor",
          title: label,
          icon: "thermometer",
          kw: (s.zone || "") + " " + (s.interface || ""),
          act: function () {
            navigateTo("sensors");
            // Wait for the actual sensor row to render before opening edit —
            // sensorsLoad is async (fetches /api/sensors) and on slower
            // devices the 250 ms blanket timeout we used to ship missed
            // the window.  Poll for the row up to ~2 s; clEditSensor
            // gracefully falls back to the modal if the row never
            // appears (gemini review PR #108).
            var attempts = 0;
            (function waitForRow() {
              if (typeof clEditSensor !== "function") return;
              var row = document.querySelector(
                '.sensor-list-row[data-sensor-idx="' + idx + '"]'
              );
              if (row || attempts >= 20) { clEditSensor(idx); return; }
              attempts++;
              setTimeout(waitForRow, 100);
            })();
          },
        });
      });
    }
    return items;
  }

  // ── Fuzzy match + ranking ────────────────────────────────────────────────
  function score(item, q) {
    if (!q) return 1; // everything matches when query empty
    var lq = q.toLowerCase();
    var lt = item.title.toLowerCase();
    var lk = (item.kw || "").toLowerCase();
    if (lt.indexOf(lq) === 0) return 100;           // title prefix
    var words = lt.split(/[\s·-]+/);
    for (var i = 0; i < words.length; i++) {
      if (words[i].indexOf(lq) === 0) return 80;     // word prefix
    }
    if (lt.indexOf(lq) !== -1) return 60;            // title substring
    if (lk.indexOf(lq) !== -1) return 30;            // keyword substring
    // Character subsequence match (very loose) — last-resort
    var ti = 0;
    for (var k = 0; k < lq.length; k++) {
      var c = lq.charAt(k);
      var found = -1;
      for (var j = ti; j < lt.length; j++) {
        if (lt.charAt(j) === c) { found = j; break; }
      }
      if (found < 0) return 0;
      ti = found + 1;
    }
    return 10;
  }

  // ── State + DOM ──────────────────────────────────────────────────────────
  var palette = null;
  var input   = null;
  var listEl  = null;
  var items   = [];
  var matches = [];
  var selectedIdx = 0;

  function build() {
    palette = document.createElement("div");
    palette.id = "cmdPalette";
    palette.className = "cmd-palette";
    palette.setAttribute("role", "dialog");
    palette.setAttribute("aria-modal", "true");
    palette.setAttribute("aria-label", "Command palette");
    palette.innerHTML =
      '<div class="cmd-backdrop" data-role="backdrop"></div>' +
      '<div class="cmd-sheet">' +
        '<div class="cmd-input-row">' +
          '<span class="cmd-search-icon" data-icon="search"></span>' +
          '<input type="search" class="cmd-input" autocomplete="off" autocorrect="off" spellcheck="false" placeholder="Search pages, sensors, settings, actions…"/>' +
          '<kbd class="cmd-kbd">Esc</kbd>' +
        '</div>' +
        '<div class="cmd-list" role="listbox"></div>' +
        '<div class="cmd-foot">' +
          '<span><kbd>↑</kbd><kbd>↓</kbd> navigate</span>' +
          '<span><kbd>↵</kbd> open</span>' +
          '<span><kbd>Esc</kbd> close</span>' +
        '</div>' +
      '</div>';
    document.body.appendChild(palette);
    if (window.Icons && Icons.swap) Icons.swap(palette);

    input  = palette.querySelector(".cmd-input");
    listEl = palette.querySelector(".cmd-list");

    input.addEventListener("input", function () { rerank(); });
    input.addEventListener("keydown", onKey);
    palette.querySelector('[data-role="backdrop"]').addEventListener("click", close);
  }

  function open() {
    if (!palette) build();
    items = buildItems();
    input.value = "";
    selectedIdx = 0;
    palette.classList.add("open");
    document.body.classList.add("cmd-palette-open");
    rerank();
    // Slight delay so the iOS soft-keyboard pops after the layout settles
    setTimeout(function () { input.focus(); input.select(); }, 30);
  }
  function close() {
    if (!palette) return;
    palette.classList.remove("open");
    document.body.classList.remove("cmd-palette-open");
  }
  function isOpen() { return palette && palette.classList.contains("open"); }

  function rerank() {
    var q = input.value.trim();
    matches = items
      .map(function (it) { return { it: it, s: score(it, q) }; })
      .filter(function (r) { return r.s > 0; })
      .sort(function (a, b) { return b.s - a.s; });
    if (matches.length === 0) { selectedIdx = 0; }
    if (selectedIdx >= matches.length) selectedIdx = Math.max(0, matches.length - 1);
    renderList();
  }

  function renderList() {
    listEl.innerHTML = "";
    if (matches.length === 0) {
      listEl.innerHTML = '<div class="cmd-empty">No matches.</div>';
      return;
    }
    var lastGroup = null;
    matches.forEach(function (m, idx) {
      var it = m.it;
      if (it.group !== lastGroup) {
        var grp = document.createElement("div");
        grp.className = "cmd-group";
        grp.textContent = it.group.toUpperCase();
        listEl.appendChild(grp);
        lastGroup = it.group;
      }
      var row = document.createElement("button");
      row.type = "button";
      row.className = "cmd-row" + (idx === selectedIdx ? " active" : "");
      row.setAttribute("role", "option");
      row.dataset.idx = idx;
      row.innerHTML =
        '<span class="cmd-row-icon" data-icon="' + (it.icon || "circle") + '"></span>' +
        '<span class="cmd-row-title"></span>';
      row.querySelector(".cmd-row-title").textContent = it.title;
      row.addEventListener("mousemove", function () {
        if (selectedIdx !== idx) { selectedIdx = idx; renderList(); }
      });
      row.addEventListener("click", function () { fire(idx); });
      listEl.appendChild(row);
    });
    if (window.Icons && Icons.swap) Icons.swap(listEl);
    // Scroll selected into view
    var active = listEl.querySelector(".cmd-row.active");
    if (active && active.scrollIntoView) active.scrollIntoView({ block: "nearest" });
  }

  function fire(idx) {
    var m = matches[idx]; if (!m) return;
    close();
    setTimeout(function () { try { m.it.act(); } catch (e) {} }, 50);
  }

  function onKey(ev) {
    if (ev.key === "Escape") { ev.preventDefault(); close(); return; }
    if (ev.key === "ArrowDown") {
      ev.preventDefault();
      selectedIdx = Math.min(matches.length - 1, selectedIdx + 1);
      renderList();
      return;
    }
    if (ev.key === "ArrowUp") {
      ev.preventDefault();
      selectedIdx = Math.max(0, selectedIdx - 1);
      renderList();
      return;
    }
    if (ev.key === "Enter") {
      ev.preventDefault();
      fire(selectedIdx);
      return;
    }
  }

  // ── Global key trigger ───────────────────────────────────────────────────
  document.addEventListener("keydown", function (e) {
    // ⌘K / Ctrl+K from anywhere
    var isK = e.key === "k" || e.key === "K";
    if (isK && (e.metaKey || e.ctrlKey)) {
      e.preventDefault();
      if (isOpen()) close(); else open();
      return;
    }
    // `/` as a Slack-style trigger, but only outside text inputs.
    if (e.key === "/" && !e.metaKey && !e.ctrlKey && !e.altKey) {
      var t = e.target;
      var tag = (t.tagName || "").toLowerCase();
      if (tag === "input" || tag === "textarea" || tag === "select" || t.isContentEditable) return;
      e.preventDefault();
      open();
    }
  });

  window.CommandPalette = { open: open, close: close, isOpen: isOpen };
})();
