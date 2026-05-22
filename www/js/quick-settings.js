/**
 * /www/js/quick-settings.js — slide-in panel of most-used settings
 *
 * Cuts most config trips from 3 clicks (Settings → subpage → field) to 1.
 * Surfaces:
 *   • Theme (light / dark / auto)
 *   • Accent color (cyan / amber / green / violet)
 *   • Density (comfortable / compact)
 *   • Sidebar rail toggle
 *   • Hostname display (read-only summary; click to jump to full settings)
 *   • WiFi summary + reconnect button
 *   • Restart device button
 *
 * Opens from the topbar gear button (`data-click="quickSettingsOpen"`),
 * the command palette ("Open quick settings"), or the `,` keyboard
 * shortcut.  Esc closes; backdrop click closes.
 *
 * Reads live data from ST (cached /api/status) when available.  All
 * controls call existing handlers — this is a UI shortcut, not a new
 * configuration path.
 */

"use strict";

(function () {

  var panel = null;

  function build() {
    panel = document.createElement("div");
    panel.id = "quickSettingsPanel";
    panel.className = "qsp";
    panel.setAttribute("role", "dialog");
    panel.setAttribute("aria-modal", "true");
    panel.setAttribute("aria-label", "Quick settings");
    panel.innerHTML =
      '<div class="qsp-backdrop" data-role="backdrop"></div>' +
      '<aside class="qsp-sheet" role="region">' +
        '<header class="qsp-head">' +
          '<div class="qsp-title"><span data-icon="sliders-horizontal"></span> Quick settings</div>' +
          '<button type="button" class="btn-mini qsp-close" aria-label="Close"><span data-icon="x"></span></button>' +
        '</header>' +
        '<div class="qsp-body">' +
          // ── Appearance ─────────────────────────────────────────────────
          '<section class="qsp-section">' +
            '<div class="qsp-eyebrow">APPEARANCE</div>' +
            '<div class="qsp-row">' +
              '<div class="qsp-label">Theme</div>' +
              '<div class="qsp-seg" role="group" aria-label="Theme">' +
                '<button type="button" data-theme="light"><span data-icon="sun"></span> Light</button>' +
                '<button type="button" data-theme="dark"><span data-icon="moon"></span> Dark</button>' +
                '<button type="button" data-theme="auto">Auto</button>' +
              '</div>' +
            '</div>' +
            '<div class="qsp-row">' +
              '<div class="qsp-label">Accent</div>' +
              '<div class="qsp-swatches">' +
                '<button type="button" class="accent-swatch" data-accent="cyan"   aria-label="Cyan"></button>' +
                '<button type="button" class="accent-swatch" data-accent="amber"  aria-label="Amber"></button>' +
                '<button type="button" class="accent-swatch" data-accent="green"  aria-label="Green"></button>' +
                '<button type="button" class="accent-swatch" data-accent="violet" aria-label="Violet"></button>' +
              '</div>' +
            '</div>' +
            '<div class="qsp-row">' +
              '<div class="qsp-label">Density</div>' +
              '<div class="qsp-seg" role="group" aria-label="Density">' +
                '<button type="button" data-density="comfortable">Comfortable</button>' +
                '<button type="button" data-density="compact">Compact</button>' +
              '</div>' +
            '</div>' +
            '<div class="qsp-row">' +
              '<div class="qsp-label">Sidebar</div>' +
              '<button type="button" class="btn qsp-flex" data-role="sidebar-toggle"><span data-icon="menu"></span> Toggle rail</button>' +
            '</div>' +
          '</section>' +
          // ── Connectivity ───────────────────────────────────────────────
          '<section class="qsp-section">' +
            '<div class="qsp-eyebrow">CONNECTIVITY</div>' +
            '<div class="qsp-summary">' +
              '<div class="qsp-summary-row"><span class="qsp-label">Hostname</span><span class="mono" data-role="hostname">—</span></div>' +
              '<div class="qsp-summary-row"><span class="qsp-label">WiFi</span><span class="mono" data-role="wifi">—</span></div>' +
              '<div class="qsp-summary-row"><span class="qsp-label">IP</span><span class="mono" data-role="ip">—</span></div>' +
              '<div class="qsp-summary-row"><span class="qsp-label">Mode</span><span class="mono" data-role="mode">—</span></div>' +
            '</div>' +
            '<div class="qsp-row qsp-row-actions">' +
              '<button type="button" class="btn" data-role="goto-network"><span data-icon="wifi"></span> Network settings</button>' +
              '<button type="button" class="btn" data-role="goto-platform"><span data-icon="cpu"></span> Platform</button>' +
            '</div>' +
          '</section>' +
          // ── System ─────────────────────────────────────────────────────
          '<section class="qsp-section">' +
            '<div class="qsp-eyebrow">SYSTEM</div>' +
            '<div class="qsp-summary">' +
              '<div class="qsp-summary-row"><span class="qsp-label">Firmware</span><span class="mono" data-role="fw">—</span></div>' +
              '<div class="qsp-summary-row"><span class="qsp-label">Uptime</span><span class="mono" data-role="uptime">—</span></div>' +
              '<div class="qsp-summary-row"><span class="qsp-label">Free heap</span><span class="mono" data-role="heap">—</span></div>' +
            '</div>' +
            '<div class="qsp-row qsp-row-actions">' +
              '<button type="button" class="btn" data-role="goto-update"><span data-icon="cloud-upload"></span> Update firmware</button>' +
              '<button type="button" class="btn warn" data-role="restart"><span data-icon="rotate-ccw"></span> Restart device</button>' +
            '</div>' +
          '</section>' +
        '</div>' +
      '</aside>';
    document.body.appendChild(panel);
    if (window.Icons && Icons.swap) Icons.swap(panel);

    panel.querySelector(".qsp-close").addEventListener("click", close);
    panel.querySelector('[data-role="backdrop"]').addEventListener("click", close);

    // Theme
    panel.querySelectorAll("[data-theme]").forEach(function (b) {
      b.addEventListener("click", function () {
        // Centralized setter — also updates the topbar toggle icon, the
        // theme-X class on <html>, and persists.  Without this we used to
        // set only data-theme + storage, leaving the icon out of sync
        // (gemini review PR #108).
        if (window.setTheme) setTheme(b.dataset.theme);
        syncToggles();
      });
    });
    // Accent
    panel.querySelectorAll(".accent-swatch").forEach(function (b) {
      b.addEventListener("click", function () { window.setAccent && setAccent(b.dataset.accent); syncToggles(); });
    });
    // Density
    panel.querySelectorAll("[data-density]").forEach(function (b) {
      b.addEventListener("click", function () {
        var v = b.dataset.density;
        document.documentElement.setAttribute("data-density", v);
        try { localStorage.setItem("density", v); } catch (e) {}
        syncToggles();
      });
    });
    // Sidebar rail
    panel.querySelector('[data-role="sidebar-toggle"]').addEventListener("click", function () {
      if (window.sidebarRailToggle) sidebarRailToggle();
    });
    // Navigation shortcuts
    panel.querySelector('[data-role="goto-network"]').addEventListener("click", function () {
      close(); setTimeout(function () { location.hash = "settings_netime"; }, 80);
    });
    panel.querySelector('[data-role="goto-platform"]').addEventListener("click", function () {
      close(); setTimeout(function () { location.hash = "settings_platform"; }, 80);
    });
    panel.querySelector('[data-role="goto-update"]').addEventListener("click", function () {
      close(); setTimeout(function () { if (window.navigateTo) navigateTo("update"); }, 80);
    });
    panel.querySelector('[data-role="restart"]').addEventListener("click", function () {
      close(); setTimeout(function () { if (window.showPopup) showPopup("restartPopup"); }, 80);
    });
  }

  function syncToggles() {
    if (!panel) return;
    // Theme
    var theme = "auto";
    try { theme = localStorage.getItem("themeOverride") || "auto"; } catch (e) {}
    panel.querySelectorAll("[data-theme]").forEach(function (b) {
      b.classList.toggle("active", b.dataset.theme === theme);
    });
    // Accent
    var acc = document.documentElement.getAttribute("data-accent") || "cyan";
    panel.querySelectorAll(".accent-swatch").forEach(function (b) {
      b.classList.toggle("active", b.dataset.accent === acc);
    });
    // Density
    var density = document.documentElement.getAttribute("data-density") || "comfortable";
    panel.querySelectorAll("[data-density]").forEach(function (b) {
      b.classList.toggle("active", b.dataset.density === density);
    });
  }

  function refreshSummary() {
    if (!panel) return;
    function set(role, val) {
      var el = panel.querySelector('[data-role="' + role + '"]');
      if (el && val !== undefined && val !== null && val !== "") el.textContent = val;
    }
    var st = window.ST || {};
    var cfg = window.CFG || {};
    set("hostname", st.hostname || (cfg.device && cfg.device.hostname) || "—");
    set("wifi",     st.ssid     || st.wifi     || "—");
    set("ip",       st.ip       || "—");
    set("mode",     (cfg.platform && cfg.platform.mode) || document.documentElement.dataset.mode || "—");
    set("fw",       st.version  || st.fw       || "—");
    set("uptime",   formatUptime(st.uptime));
    set("heap",     st.heap     ? (st.heap + " B") : "—");
  }

  function formatUptime(s) {
    if (s === undefined || s === null || s === "") return "—";
    s = parseInt(s, 10) || 0;
    var d = Math.floor(s / 86400);
    var h = Math.floor((s % 86400) / 3600);
    var m = Math.floor((s % 3600) / 60);
    if (d) return d + "d " + h + "h";
    if (h) return h + "h " + m + "m";
    return m + "m " + (s % 60) + "s";
  }

  function open() {
    if (!panel) build();
    panel.classList.add("open");
    document.body.classList.add("qsp-open");
    syncToggles();
    refreshSummary();
  }
  function close() {
    if (!panel) return;
    panel.classList.remove("open");
    document.body.classList.remove("qsp-open");
  }
  function isOpen() { return panel && panel.classList.contains("open"); }

  // Esc closes
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape" && isOpen()) { close(); return; }
    // `,` opens quick settings (when not typing into an input)
    if (e.key === "," && !e.metaKey && !e.ctrlKey && !e.altKey) {
      var t = e.target;
      var tag = (t.tagName || "").toLowerCase();
      if (tag === "input" || tag === "textarea" || tag === "select" || t.isContentEditable) return;
      e.preventDefault();
      if (isOpen()) close(); else open();
    }
  });

  // Register handler so the topbar button works through data-click dispatch.
  if (typeof registerHandlers === "function") {
    registerHandlers({ quickSettingsOpen: open });
  } else {
    window.quickSettingsOpen = open;
  }

  window.QuickSettings = { open: open, close: close, isOpen: isOpen };
})();
