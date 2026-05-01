/**
 * /www/js/settings.js — all settings sub-pages, changelog, OTA update
 * Loaded after core.js. Depends on globals from core.js (CFG, ST, utils,
 * settingsSave, showRestartPopup) and on /save_* + /api/* endpoints.
 */
"use strict";

// ============================================================================
// ══ SETTINGS: DEVICE ══
// ============================================================================
function sdInit() {
  // Reset changelog state on every page enter so re-navigation works cleanly
  changelogLoaded = false;
  var clEl = document.getElementById("changelog");
  var chev = document.getElementById("changelogChevron");
  if (clEl) clEl.classList.add("hidden");
  if (chev) chev.style.transform = "";

  fetch("/api/status")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      ST = d;
      setVal("sd-devName", d.device || d.deviceName);
      setVal("sd-devId", d.deviceId);
      setVal(
        "sd-defaultStorage",
        d.defaultStorageView !== undefined ? d.defaultStorageView : 0,
      );
      setChk("sd-forceWS", d.forceWebServer);

      // System Info card — matches original .ino "System Info" card content
      var info = document.getElementById("sysInfo");
      if (info) {
        info.innerHTML =
          '<div><strong>Firmware</strong><div class="text-primary">' +
          (d.version || "-") +
          "</div></div>" +
          "<div><strong>Boot Count</strong><div>" +
          (d.boot || 0) +
          "</div></div>" +
          "<div><strong>Mode</strong><div>" +
          (d.mode || "-") +
          "</div></div>" +
          "<div><strong>Free Heap</strong><div>" +
          fmtBytes(d.heap) +
          "</div></div>" +
          "<div><strong>CPU</strong><div>" +
          (d.cpu || "-") +
          " MHz</div></div>" +
          "<div><strong>Chip</strong><div>" +
          (d.chip || "-") +
          "</div></div>";
      }
    });
}

function regenDevId() {
  if (!confirm("Generate new ID based on MAC address?")) return;
  fetch("/api/regen-id", { method: "POST" })
    .then(function (r) {
      return r.text();
    })
    .then(function (id) {
      var inp = document.getElementById("sd-devId");
      if (inp) {
        inp.value = id.trim();
        inp.disabled = false;
      }
      showToast("New ID generated: " + id.trim() + ". Click Save to apply.", "success");
    })
    .catch(function (e) {
      showToast("Error: " + e, "error");
    });
}

// Matches original: function toggleManualId(id)
function toggleManualId(id) {
  var el = document.getElementById(id);
  if (el) el.disabled = !el.disabled;
}

// ============================================================================
// ══ CHANGELOG ══
// Matches original .ino Device page inline JS:
//   fetch('/api/changelog') → render ## sections → first block highlighted
// ============================================================================

// Matches original: onclick="changelogToggle()" on card-head
function changelogToggle() {
  var el = document.getElementById("changelog");
  if (!el) return;

  var isHidden = el.classList.contains("hidden");
  if (isHidden) {
    el.classList.remove("hidden");
    if (!changelogLoaded) changelogLoad();
  } else {
    el.classList.add("hidden");
  }

  // Rotate chevron indicator — matches original .ino #sd-changelogChevron
  var chev = document.getElementById("changelogChevron");
  if (chev)
    chev.style.transform = el.classList.contains("hidden")
      ? ""
      : "rotate(180deg)";
}

// Used by error close button inside changelog body
function changelogClose(ev) {
  if (ev) ev.stopPropagation();
  var el = document.getElementById("changelog");
  if (el) el.classList.add("hidden");
  var chev = document.getElementById("changelogChevron");
  if (chev) chev.style.transform = "";
}

// Matches original .ino Device page:
//   fetch('/api/changelog').then(r=>r.ok?r.text():...).then(txt=>{ render ## sections })
function changelogLoad() {
  var el = document.getElementById("changelog");
  if (!el) return;
  el.innerHTML = "<div class='text-muted' style='padding:.5rem'>Loading…</div>";

  fetch("/api/changelog")
    .then(function (r) {
      if (!r.ok) throw new Error("not found");
      return r.text();
    })
    .then(function (txt) {
      changelogLoaded = true; // mark loaded — re-open skips fetch

      var html = "";
      var lines = txt.trim().split("\n");
      var inVer = false;
      var currentMarked = false;
      var hasEntries = false; // local var — not leaked to global scope

      lines.forEach(function (rawLine) {
        var line = rawLine.trim();
        if (!line) return;
        if (line.indexOf("##") === 0) {
          hasEntries = true;
          if (inVer) html += "</ul></div>";
          var ver = line.substring(2).trim();
          var isCur = !currentMarked;
          if (isCur) currentMarked = true;
          html +=
            '<div style="margin-top:.5rem;padding:.5rem;border-radius:4px;' +
            (isCur
              ? "background:var(--primary);color:#fff"
              : "background:var(--border);color:var(--text-muted)") +
            '">' +
            "<strong>" +
            ver +
            "</strong>" +
            '<ul style="margin:.5rem 0 0 1rem;padding:0;font-size:.9rem">';
          inVer = true;
        } else if (line.indexOf("-") === 0 && inVer) {
          html += "<li>" + line.substring(1).trim() + "</li>";
        }
      });

      if (inVer) html += "</ul></div>";
      if (!hasEntries)
        html += "<div class='text-muted'>No entries found.</div>";
      el.innerHTML = html;
    })
    .catch(function () {
      changelogLoaded = false; // allow retry on next open
      el.innerHTML =
        "<div style='display:flex;justify-content:flex-end;margin-bottom:.5rem'>" +
        '<button type="button" class="btn" data-click="changelogClose">✖ Close</button></div>' +
        "<div class='alert alert-warning'>Changelog not found. Upload <code>/changelog.txt</code> to LittleFS.</div>";
    });
}

// ============================================================================
// ══ SETTINGS: FLOW METER ══
// ============================================================================
function sfInit() {
  fetch("/export_settings")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      CFG = d;
      var fm = d.flowMeter || {};
      setVal("sf-ppl", fm.pulsesPerLiter);
      setVal("sf-cal", fm.calibrationMultiplier);
      setVal("sf-win", fm.monitoringWindowSecs);
      setVal("sf-fl", fm.firstLoopMonitoringWindowSecs);
      setChk("sf-test", fm.testMode);
      setVal("sf-blink", fm.blinkDuration);
      fetch("/api/status")
        .then(function (r2) {
          return r2.json();
        })
        .then(function (s) {
          setEl("sf-boot", s.boot);
        });
    });
  // Unified Sensors page: also populate the additional-sensor list
  // from platform_config.json (the form uses /save_flowmeter; the list
  // uses /save_platform via clSave()).
  if (document.getElementById("cl-sensors-list")) {
    clLoad();
  }
}

// ============================================================================
// ══ SETTINGS: HARDWARE ══
// ============================================================================
// First settings page migrated to schema-driven Form.bind (Pass 4 A3).
// The HTML partial provides only #hw-host; the entire form is rendered
// from HW_SCHEMA against CFG.hardware.
var HW_SCHEMA = {
  saveUrl: "/save_hardware",
  restart: true,
  confirm: "Settings will be saved and device will restart. Continue?",
  submitLabel: "💾 Save & Restart",
  sections: [
    { title: "💾 Storage", fields: [
        { name: "storageType", label: "Type", type: "select", options: [
            ["0", "LittleFS (Internal)"],
            ["1", "SD Card (SPI)"],
        ]},
        { row: [
            { name: "pinSdCS",   label: "CS",   type: "number", showWhen: { storageType: "1" } },
            { name: "pinSdMOSI", label: "MOSI", type: "number", showWhen: { storageType: "1" } },
            { name: "pinSdMISO", label: "MISO", type: "number", showWhen: { storageType: "1" } },
            { name: "pinSdSCK",  label: "SCK",  type: "number", showWhen: { storageType: "1" } },
        ]},
    ]},
    { title: "😴 Wakeup Mode", fields: [
        { name: "wakeupMode", label: "Button Active Level", type: "select",
          hint: "HIGH = button connects to VCC, LOW = to GND.",
          options: [
            ["0", "Active HIGH (VCC)"],
            ["1", "Active LOW (GND)"],
        ]},
        { name: "debounceMs", label: "Debounce (ms)", type: "number",
          min: 20, max: 500,
          hint: "Higher = fewer false triggers but slower response." },
    ]},
    { title: "🔘 Pin Configuration",
      hint: "GPIO pin numbers for ESP32-C3",
      fields: [
        { row: [
            { name: "pinWifiTrigger", label: "WiFi Trigger",   type: "number" },
            { name: "pinWakeupFF",    label: "Full Flush Btn", type: "number" },
            { name: "pinWakeupPF",    label: "Part Flush Btn", type: "number" },
            { name: "pinFlowSensor",  label: "Flow Sensor",    type: "number" },
        ]},
    ]},
    { title: "🕐 RTC DS1302", fields: [
        { row: [
            { name: "pinRtcCE",   label: "CE (RST)",   type: "number" },
            { name: "pinRtcIO",   label: "IO (DAT)",   type: "number" },
            { name: "pinRtcSCLK", label: "CLK (SCLK)", type: "number" },
        ]},
    ]},
    { title: "⚡ CPU Frequency", fields: [
        { name: "cpuFreqMHz", type: "select", options: [
            ["80",  "80 MHz"],
            ["160", "160 MHz"],
        ]},
    ]},
  ],
};

function hwInit() {
  fetch("/export_settings")
    .then(function (r) { return r.json(); })
    .then(function (d) {
      CFG = d;
      var hw = d.hardware || {};
      var defaults = {
        storageType: 0, wakeupMode: 0, debounceMs: 100, cpuFreqMHz: 80,
      };
      for (var k in defaults) if (hw[k] === undefined) hw[k] = defaults[k];

      Form.bind("hw-host", HW_SCHEMA, hw);

      var th = (ST && ST.theme) || (CFG && CFG.theme) || {};
      if (th.boardDiagramPath) {
        var card = document.getElementById("boardDiagramCard");
        var img  = document.getElementById("boardDiagram");
        if (card) card.style.display = "block";
        if (img)  img.src = th.boardDiagramPath + "?v=" + Date.now();
      }
    });
}

// ============================================================================
// ══ SETTINGS: THEME ══
// ============================================================================
function thInit() {
  // Wire .seg buttons → hidden #th-mode select.  Done once per init.
  var seg = document.getElementById("th-mode-seg");
  if (seg && !seg._wired) {
    seg._wired = true;
    seg.querySelectorAll("button").forEach(function (b) {
      b.addEventListener("click", function () {
        var v = b.getAttribute("data-v");
        seg.querySelectorAll("button").forEach(function (x) {
          x.classList.toggle("active", x === b);
        });
        var sel = document.getElementById("th-mode");
        if (sel) { sel.value = v; sel.dispatchEvent(new Event("change")); }
      });
    });
  }
  fetch("/export_settings")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      CFG = d;
      var th = d.theme || {};
      var mode = th.mode !== undefined ? th.mode : 0;
      setVal("th-mode", mode);
      if (seg) seg.querySelectorAll("button").forEach(function (b) {
        b.classList.toggle("active", String(b.getAttribute("data-v")) === String(mode));
      });
      setChk("th-icons", th.showIcons);

      var isDark =
        mode === 1 ||
        (mode === 2 &&
          window.matchMedia &&
          window.matchMedia("(prefers-color-scheme: dark)").matches);

      setVal("th-primary", th.primaryColor || "#275673");
      setVal("th-secondary", th.secondaryColor || "#4a5568");
      window._thData = th;
      function updateColorPickers(m) {
        var isD =
          m === 1 ||
          (m === 2 &&
            window.matchMedia &&
            window.matchMedia("(prefers-color-scheme: dark)").matches);
        setVal(
          "th-bg",
          isD
            ? window._thData.darkBgColor || "#0f172a"
            : window._thData.lightBgColor || "#f0f2f5",
        );
        setVal(
          "th-text",
          isD
            ? window._thData.darkTextColor || "#e2e8f0"
            : window._thData.lightTextColor || "#2d3748",
        );
      }
      updateColorPickers(mode);

      var modeSelect = document.getElementById("th-mode");
      if (modeSelect && !window._modeListenerAppended) {
        modeSelect.addEventListener("change", function (e) {
          updateColorPickers(parseInt(e.target.value));
        });
        window._modeListenerAppended = true;
      }
      setVal("th-ff", th.ffColor || "#275673");
      setVal("th-pf", th.pfColor || "#7eb0d5");
      setVal("th-other", th.otherColor || "#a0aec0");
      setVal("th-bar", th.storageBarColor || "#27ae60");
      setVal("th-bar70", th.storageBar70Color || "#f39c12");
      setVal("th-bar90", th.storageBar90Color || "#e74c3c");
      setVal("th-barB", th.storageBarBorder || "#cccccc");
      setVal("th-logo", th.logoSource);
      setVal("th-favicon", th.faviconPath);
      setVal("th-board", th.boardDiagramPath);
      setVal("th-chartSrc", th.chartSource !== undefined ? th.chartSource : 0);
      var pr = document.getElementById("chartPathRow");
      if (pr)
        pr.style.display =
          th.chartSource == 0 || !th.chartSource ? "block" : "none";
      setVal("th-chartPath", th.chartLocalPath);
      setVal(
        "th-labelFmt",
        th.chartLabelFormat !== undefined ? th.chartLabelFormat : 0,
      );
    });
}

function themeSave(e, form) {
  if (e && e.preventDefault) e.preventDefault();
  if (!form) form = e && e.target;
  var fd = new FormData(form);

  var mode = parseInt(fd.get("themeMode") || "0");
  var isDark =
    mode === 1 ||
    (mode === 2 &&
      window.matchMedia &&
      window.matchMedia("(prefers-color-scheme: dark)").matches);

  var currentBg = fd.get("bgColor") || "";
  var currentText = fd.get("textColor") || "";
  fd.delete("bgColor");
  fd.delete("textColor");

  if (isDark) {
    fd.append("darkBgColor", currentBg);
    fd.append("darkTextColor", currentText);
    if (window._thData && window._thData.lightBgColor)
      fd.append("lightBgColor", window._thData.lightBgColor);
    if (window._thData && window._thData.lightTextColor)
      fd.append("lightTextColor", window._thData.lightTextColor);
  } else {
    fd.append("lightBgColor", currentBg);
    fd.append("lightTextColor", currentText);
    if (window._thData && window._thData.darkBgColor)
      fd.append("darkBgColor", window._thData.darkBgColor);
    if (window._thData && window._thData.darkTextColor)
      fd.append("darkTextColor", window._thData.darkTextColor);
  }

  var defs = {
    primaryColor: "#275673",
    secondaryColor: "#4a5568",
    lightBgColor: "#f0f2f5",
    lightTextColor: "#2d3748",
    darkBgColor: "#0f172a",
    darkTextColor: "#e2e8f0",
    ffColor: "#275673",
    pfColor: "#7eb0d5",
    otherColor: "#a0aec0",
    storageBarColor: "#27ae60",
    storageBar70Color: "#f39c12",
    storageBar90Color: "#e74c3c",
    storageBarBorder: "#cccccc",
  };

  for (var key in defs) {
    var val = fd.get(key);
    if (val && val.toLowerCase() === defs[key].toLowerCase()) {
      fd.set(key, "");
    }
  }

  var btn = form.querySelector('button[type="submit"]');
  var old = btn.innerHTML;
  btn.innerHTML = "Saving...";
  btn.disabled = true;

  fetch("/save_theme", { method: "POST", body: fd })
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      btn.innerHTML = old;
      btn.disabled = false;
      if (d.ok) {
        var m = document.getElementById("th-msg");
        if (m)
          m.innerHTML =
            '<div class="alert alert-success mt-1 mb-1">Theme saved! Rebooting...</div>';
        setTimeout(function () {
          location.reload();
        }, 1000);
      } else {
        showToast("Save failed.", "error");
      }
    })
    .catch(function (err) {
      btn.innerHTML = old;
      btn.disabled = false;
      showToast("Error: " + err, "error");
    });
}

function themeToggleChartPath() {
  var pr = document.getElementById("chartPathRow"),
    sel = document.getElementById("th-chartSrc");
  if (pr && sel) pr.style.display = sel.value === "0" ? "block" : "none";
}

function themeRestoreDefault() {
  if (
    !confirm(
      "Are you sure you want to restore the default theme colors? This will wipe your custom choices.",
    )
  )
    return;
  var fd = new FormData();
  fd.append("themeMode", "0");
  fd.append("primaryColor", "");
  fd.append("secondaryColor", "");
  fd.append("lightBgColor", "");
  fd.append("lightTextColor", "");
  fd.append("darkBgColor", "");
  fd.append("darkTextColor", "");
  fd.append("ffColor", "");
  fd.append("pfColor", "");
  fd.append("otherColor", "");
  fd.append("storageBarColor", "");
  fd.append("storageBar70Color", "");
  fd.append("storageBar90Color", "");
  fd.append("storageBarBorder", "");
  fetch("/save_theme", { method: "POST", body: fd })
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      if (d.ok) {
        showToast("Theme restored to defaults! Rebooting...", "success");
        location.reload();
      } else {
        showToast("Failed to restore theme defaults.", "error");
      }
    })
    .catch(function () {
      showToast("Theme restored to defaults! Rebooting...", "success");
      location.reload();
    });
}

// ============================================================================
// ══ SETTINGS: NETWORK ══
// ============================================================================
function netInit() {
  fetch("/api/status")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      ST = d;
      setEl(
        "net-status",
        d.wifi === "client" ? "Connected: " + (d.network || "") : "AP Mode",
      );
      var rssiVal = d.rssi !== undefined ? d.rssi : -100;
      var rSvg = getRssiInfo(rssiVal);
      var textEl = document.getElementById("net-rssi");
      if (textEl) {
        textEl.innerText = d.rssi !== undefined ? d.rssi + " dBm" : "-";
        textEl.style.color = ""; // Remove inline color
      }
      var iconEl = document.getElementById("net-rssi-icon");
      if (iconEl) {
        iconEl.innerHTML = rSvg;
        iconEl.style.color = ""; // Remove inline color
      }
      if (d.wifi === "client") {
        setVal("net-ip2-current", d.ip || "");
        setVal("net-gw-current", d.gateway || "");
        setVal("net-sn-current", d.subnet || "");
        setVal("net-dns-current", d.dns || "");
      }
      netToggleStatic(); // Trigger UI update based on new loaded current properties
    });
  fetch("/export_settings")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      CFG = d;
      var net = d.network || {};
      setVal("net-mode", net.wifiMode !== undefined ? net.wifiMode : 0);
      netToggleMode();
      setVal("net-apSSID", net.apSSID);
      setVal("net-apPass", net.apPassword || "");
      setVal("net-apIP", net.apIP || "");
      setVal("net-apGW", net.apGateway || "");
      setVal("net-apSN", net.apSubnet || "");
      setVal("net-cSSID", net.clientSSID);
      setVal("net-cPass", net.clientPassword || "");
      setChk("net-staticCheck", net.useStaticIP);
      setVal("net-ip2", net.staticIP || "0.0.0.0");
      setVal("net-gw", net.gateway || "0.0.0.0");
      setVal("net-sn", net.subnet || "0.0.0.0");
      setVal("net-dns", net.dns || "0.0.0.0");
      netToggleStatic();
    });
}

function getRssiInfo(rssi) {
  var bars = 0,
    cls = "text-muted";
  if (rssi >= -50) {
    bars = 4;
    cls = "text-success";
  } else if (rssi >= -70) {
    bars = 3;
    cls = "text-primary";
  } else if (rssi >= -80) {
    bars = 2;
    cls = "text-warning";
  } else if (rssi >= -90) {
    bars = 1;
    cls = "text-danger";
  }

  var svg =
    '<svg width="16" height="14" viewBox="0 0 16 14" class="' +
    cls +
    '" fill="currentColor" style="vertical-align:middle">';
  svg +=
    '<rect x="0" y="10" width="3" height="4" rx="1" fill="' +
    (bars >= 1 ? "currentColor" : "#ccc") +
    '"/>';
  svg +=
    '<rect x="4" y="7" width="3" height="7" rx="1" fill="' +
    (bars >= 2 ? "currentColor" : "#ccc") +
    '"/>';
  svg +=
    '<rect x="8" y="4" width="3" height="10" rx="1" fill="' +
    (bars >= 3 ? "currentColor" : "#ccc") +
    '"/>';
  svg +=
    '<rect x="12" y="0" width="3" height="14" rx="1" fill="' +
    (bars >= 4 ? "currentColor" : "#ccc") +
    '"/>';
  svg += "</svg>";
  return svg;
}

// Matches original: function toggleMode()
function netToggleMode() {
  var m = getVal("net-mode");
  var ap = document.getElementById("apSection");
  var cl = document.getElementById("clientSection");
  if (ap) ap.style.display = m === "0" ? "block" : "none";
  if (cl) cl.style.display = m === "1" ? "block" : "none";
}

// Matches original: function toggleStatic()
function netToggleStatic() {
  var en =
    document.getElementById("staticCheck") &&
    document.getElementById("staticCheck").checked;

  // Manage which values are in the text inputs: current connection vs static config
  if (!en && ST && ST.wifi === "client") {
    // Not using static, and we are connected: show current DHCP details
    setVal("net-ip2", getVal("net-ip2-current"));
    setVal("net-gw", getVal("net-gw-current"));
    setVal("net-sn", getVal("net-sn-current"));
    setVal("net-dns", getVal("net-dns-current"));
  } else if (CFG && CFG.network) {
    // Enforce restoring CFG when we switch back to 'static'
    setVal("net-ip2", CFG.network.staticIP || "0.0.0.0");
    setVal("net-gw", CFG.network.gateway || "0.0.0.0");
    setVal("net-sn", CFG.network.subnet || "0.0.0.0");
    setVal("net-dns", CFG.network.dns || "0.0.0.0");
  }

  ["net-ip2", "net-gw", "net-sn", "net-dns"].forEach(function (id) {
    var el = document.getElementById(id);
    if (!el) return;
    el.disabled = !en;
    el.style.opacity = en ? "1" : "0.5";
    el.style.cursor = en ? "text" : "not-allowed";
  });
}

// Matches original: function scanWifi() / function checkScanResult()
function netScanWifi() {
  var list = document.getElementById("wifiList");
  if (!list) return;
  list.innerHTML = "<div class='list-item'>🔍 Scanning…</div>";
  list.style.display = "block";
  netScanRetries = 0;
  fetch("/wifi_scan_start").then(function () {
    setTimeout(netCheckScan, 2000);
  });
}
function netCheckScan() {
  fetch("/wifi_scan_result")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      var list = document.getElementById("wifiList");
      if (!list) return;
      if (d.scanning) {
        netScanRetries++;
        if (netScanRetries < 10) {
          list.innerHTML =
            "<div class='list-item'>🔍 Scanning… (" +
            netScanRetries +
            ")</div>";
          setTimeout(netCheckScan, 1000);
        } else list.innerHTML = "<div class='list-item'>⏱️ Scan timeout</div>";
      } else if (d.error) {
        list.innerHTML = "";
        var errRow = document.createElement("div");
        errRow.className = "list-item";
        errRow.textContent = "❌ " + d.error;
        list.appendChild(errRow);
      } else if (!d.networks || !d.networks.length) {
        list.innerHTML = "<div class='list-item'>📶 No networks found</div>";
      } else {
        // Build rows via DOM so SSID content is always text, never HTML.
        list.innerHTML = "";
        d.networks.forEach(function (n) {
          var row = document.createElement("div");
          row.className = "list-item";
          row.style.cursor = "pointer";
          row.appendChild(
            document.createTextNode((n.secure ? "🔒" : "📡") + " " + n.ssid + " ")
          );
          var rssi = document.createElement("small");
          rssi.className = "text-muted";
          rssi.textContent = "(" + n.rssi + " dBm)";
          row.appendChild(rssi);
          row.addEventListener("click", function () {
            var input = document.getElementById("net-cSSID");
            if (input) input.value = n.ssid;
            list.style.display = "none";
          });
          list.appendChild(row);
        });
      }
    })
    .catch(function (e) {
      var l = document.getElementById("wifiList");
      if (!l) return;
      l.innerHTML = "";
      var row = document.createElement("div");
      row.className = "list-item";
      row.textContent = "❌ Error: " + e;
      l.appendChild(row);
    });
}

// POST /api/modules/wifi/test → the server runs the probe in a worker task
// and responds 202 immediately, then we poll GET /api/modules/wifi/test for
// the result.  Keeps the async web server responsive for other clients.
var netTestPollTimer = null;
function netTestPoll(out, tries) {
  if (!out) return;
  if (tries > 20) {          // 20×600ms ≈ 12s — safely over the 8s server cap
    out.textContent = "❌ Timed out waiting for result";
    out.style.color = "var(--danger)";
    return;
  }
  fetch("/api/modules/wifi/test")
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d.state === "success") {
        out.textContent = "✅ Connected (" + d.rssi + " dBm, " + d.ip + ")";
        out.style.color = "var(--success)";
      } else if (d.state === "failed") {
        out.textContent = "❌ " + (d.error || "Failed to connect");
        out.style.color = "var(--danger)";
      } else if (d.state === "running") {
        netTestPollTimer = setTimeout(function () {
          netTestPoll(out, tries + 1);
        }, 600);
      } else {
        out.textContent = "Result expired — click Test again";
        out.style.color = "var(--text-muted)";
      }
    })
    .catch(function (e) {
      out.textContent = "❌ " + e;
      out.style.color = "var(--danger)";
    });
}
function netTestWifi() {
  var ssidEl = document.getElementById("net-cSSID");
  var passEl = document.getElementById("net-cPass");
  var out    = document.getElementById("net-testResult");
  var ssid   = ssidEl ? ssidEl.value.trim() : "";
  var pass   = passEl ? passEl.value        : "";
  if (!ssid) {
    if (out) { out.textContent = "Enter an SSID first"; out.style.color = "var(--danger)"; }
    return;
  }
  if (netTestPollTimer) { clearTimeout(netTestPollTimer); netTestPollTimer = null; }
  if (out) { out.textContent = "🧪 Testing… (up to 8s)"; out.style.color = "var(--text-muted)"; }
  fetch("/api/modules/wifi/test", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ssid: ssid, password: pass })
  })
    .then(function (r) { return r.json().then(function (d) { return { status: r.status, body: d }; }); })
    .then(function (resp) {
      if (!out) return;
      if (resp.status === 202) {
        netTestPoll(out, 0);
      } else {
        out.textContent = "❌ " + (resp.body.error || "Start failed");
        out.style.color = "var(--danger)";
      }
    })
    .catch(function (e) {
      if (!out) return;
      out.textContent = "❌ " + e;
      out.style.color = "var(--danger)";
    });
}

// ============================================================================
// ══ SETTINGS: TIME ══
// ============================================================================
function timeInit() {
  fetch("/api/status")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      ST = d;
      setEl("time-rtcTime", d.time || "--:--:--");
      setEl("time-boot", d.boot);
      var bak = document.getElementById("bootBak");
      if (bak) bak.textContent = "-";
      var status = document.getElementById("rtcStatus");
      if (status) {
        status.className = !d.rtcRunning
          ? "alert alert-error"
          : "alert alert-success";
        status.innerHTML = !d.rtcRunning ? "❌ RTC Error" : "✅ RTC OK";
      }
      var detail = document.getElementById("rtcDetail");
      if (detail)
        detail.textContent =
          "Protected: " +
          (d.rtcProtected ? "Yes" : "No") +
          " | Running: " +
          (d.rtcRunning ? "Yes" : "No");
      setChk("time-rtcProt", d.rtcProtected);
      var ntpSt = document.getElementById("ntpStatus");
      if (ntpSt) {
        ntpSt.className =
          d.wifi === "client" ? "alert alert-success" : "alert alert-warning";
        ntpSt.innerHTML =
          d.wifi === "client"
            ? "✅ WiFi Connected – NTP available"
            : "⚠️ Not connected (AP mode) – NTP unavailable";
      }
      var dateEl = document.getElementById("date");
      if (dateEl && !dateEl.value)
        dateEl.value = new Date().toISOString().slice(0, 10);
    });
  fetch("/export_settings")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      CFG = d;
      var net = d.network || {};
      setVal("time-ntp", net.ntpServer || "pool.ntp.org");
      setVal("time-tz", net.timezone !== undefined ? net.timezone : 0);
    });
}

function timeSetManual(ev) {
  ev.preventDefault();
  var form = ev.target;
  var fd = new FormData(form);
  fetch("/set_time", { method: "POST", body: fd })
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      showMsg(
        "time-msg",
        d.ok
          ? "<div class='alert alert-success'>✅ Time set!</div>"
          : "<div class='alert alert-error'>❌ " +
              (d.error || "Failed") +
              "</div>",
        true,
      );
      if (d.ok) timeInit();
    });
}

function timeSyncNTP(ev) {
  if (ev) ev.preventDefault();
  showMsg(
    "time-msg",
    "<div class='alert alert-info'>⏳ Syncing from NTP…</div>",
    true,
  );
  fetch("/sync_time", { method: "POST" })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (!d.ok) {
        showMsg("time-msg",
          "<div class='alert alert-error'>❌ NTP sync failed to start</div>",
          true);
        return;
      }
      // Server runs the sync on the main task (up to ~10 s). Poll the status
      // endpoint until result != 0.
      var attempts = 0;
      var poll = function () {
        attempts++;
        fetch("/api/time_sync_status")
          .then(function (r2) { return r2.json(); })
          .then(function (s) {
            if (s.result === 1) {
              showMsg("time-msg",
                "<div class='alert alert-success'>✅ Time synced!</div>",
                true);
              timeInit();
            } else if (s.result === -1) {
              showMsg("time-msg",
                "<div class='alert alert-error'>❌ NTP sync failed</div>",
                true);
            } else if (attempts < 30) {
              setTimeout(poll, 500);
            } else {
              showMsg("time-msg",
                "<div class='alert alert-error'>❌ NTP sync timed out</div>",
                true);
            }
          })
          .catch(function () {
            if (attempts < 30) setTimeout(poll, 500);
          });
      };
      setTimeout(poll, 500);
    });
}

function timeRtcProtect(ev) {
  if (ev) ev.preventDefault();
  var fd = new FormData();
  var chk = document.getElementById("time-rtcProt");
  if (chk && chk.checked) fd.append("protect", "1");
  fetch("/rtc_protect", { method: "POST", body: fd });
}
function timeFlushLogs() {
  fetch("/flush_logs", { method: "POST" }).then(function () {
    showMsg(
      "time-msg",
      "<div class='alert alert-success'>✅ Log buffer flushed</div>",
      true,
    );
  });
}
function timeBackupBoot() {
  fetch("/backup_bootcount", { method: "POST" }).then(function () {
    showMsg(
      "time-msg",
      "<div class='alert alert-success'>✅ Boot count backed up</div>",
      true,
    );
  });
}
function timeRestoreBoot() {
  if (!confirm("Restore boot count from backup?")) return;
  fetch("/restore_bootcount", { method: "POST" })
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      showMsg(
        "time-msg",
        d.ok
          ? "<div class='alert alert-success'>✅ Restored: " +
              d.old +
              " → " +
              d["new"] +
              "</div>"
          : "<div class='alert alert-error'>❌ Restore failed</div>",
        true,
      );
      if (d.ok) timeInit();
    });
}

// ============================================================================
// ══ SETTINGS: DATALOG ══
// ============================================================================
function dlInit() {
  fetch("/api/filelist?filter=log&recursive=1")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      var sel = document.getElementById("curFile");
      if (!sel) return;
      sel.innerHTML = "";
      var curFile = d.currentFile || "";
      (d.files || []).forEach(function (f) {
        var opt = document.createElement("option");
        opt.value = f.path;
        opt.textContent = f.path;
        sel.appendChild(opt);
      });
      fetch("/export_settings")
        .then(function (r2) {
          return r2.json();
        })
        .then(function (cfg) {
          CFG = cfg;
          var dl = cfg.datalog || {};
          if (curFile) sel.value = curFile;
          setVal("dl-prefix", dl.prefix || "datalog");
          setVal("dl-folder", dl.folder || "");
          setVal("dl-rotation", dl.rotation !== undefined ? dl.rotation : 0);
          var msGrp = document.getElementById("maxSizeGroup");
          if (msGrp) msGrp.style.display = dl.rotation == 4 ? "block" : "none";
          setVal("dl-maxSize", dl.maxSizeKB || 500);
          setChk("dl-tsFile", dl.timestampFilename || false);
          setChk("dl-devId", dl.includeDeviceId || false);
          setVal("dl-date", dl.dateFormat !== undefined ? dl.dateFormat : 1);
          setVal("dl-time", dl.timeFormat !== undefined ? dl.timeFormat : 0);
          setVal("dl-end", dl.endFormat !== undefined ? dl.endFormat : 0);
          setVal("dl-boot", dl.includeBootCount ? "1" : "0");
          setVal("dl-vol", dl.volumeFormat !== undefined ? dl.volumeFormat : 0);
          setVal("dl-extra", dl.includeExtraPresses ? "1" : "0");
          setChk("dl-pcEnabled", dl.postCorrectionEnabled);
          var pcF = document.getElementById("pcFields");
          if (pcF)
            pcF.style.display = dl.postCorrectionEnabled ? "block" : "none";
          setVal("dl-pfff", dl.pfToFfThreshold);
          setVal("dl-ffpf", dl.ffToPfThreshold);
          setVal("dl-hold", dl.manualPressThresholdMs);
          dlUpdatePreview();
        });
    });
  dlLoadFiles();
}

function dlLoadFiles() {
  var el = document.getElementById("dl-files");
  if (!el) return;
  fetch("/api/filelist?filter=log&recursive=1")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      var files = d.files || [],
        curFile = d.currentFile || "";
      if (!files.length) {
        el.innerHTML = "<div class='list-item text-muted'>No log files</div>";
        return;
      }
      var html = "";
      files.forEach(function (f) {
        var isCur = f.path === curFile;
        html +=
          "<div class='list-item'><span>" +
          (isCur ? "<strong class='text-success'>✓ " : "") +
          f.path +
          ' <small class="text-muted">(' +
          fmtBytes(f.size) +
          ")</small>" +
          (isCur ? "</strong>" : "") +
          "</span><span class='btn-group'>" +
          "<a href='/download?file=" +
          encodeURIComponent(f.path) +
          "' class='btn'>📥</a>";
        if (!isCur) {
          html +=
            ' <button data-click="dlDeleteFile" data-args="' +
            esc(JSON.stringify([f.path])) +
            '" class=\'btn\'>🗑️</button>';
        }
        html += "</span></div>";
      });
      el.innerHTML = html;
    });
}

// Uses storage=internal explicitly — matches original failsafe fix
function dlDeleteFile(path) {
  if (!confirm("Delete " + path + "?")) return;
  fetch("/delete?path=" + encodeURIComponent(path) + "&storage=internal", {
    method: "POST",
  }).then(function () {
    dlLoadFiles();
  });
}

// Matches original: function updatePreview()
function dlUpdatePreview() {
  var p = [],
    d = new Date();
  var df = getVal("dl-date"),
    tf = getVal("dl-time"),
    ef = getVal("dl-end");
  var dd = String(d.getDate()).padStart(2, "0"),
    mm = String(d.getMonth() + 1).padStart(2, "0"),
    yy = d.getFullYear();
  var hh = String(d.getHours()).padStart(2, "0"),
    mi = String(d.getMinutes()).padStart(2, "0"),
    ss = String(d.getSeconds()).padStart(2, "0");

  if (df === "1") p.push(dd + "/" + mm + "/" + yy);
  else if (df === "2") p.push(mm + "/" + dd + "/" + yy);
  else if (df === "3") p.push(yy + "-" + mm + "-" + dd);
  else if (df === "4") p.push(dd + "." + mm + "." + yy);

  var tStr = "";
  if (tf === "0") tStr = hh + ":" + mi + ":" + ss;
  else if (tf === "1") tStr = hh + ":" + mi;
  else {
    var h12 = d.getHours() % 12 || 12;
    tStr = h12 + ":" + mi + ":" + ss + (d.getHours() < 12 ? "AM" : "PM");
  }
  p.push(tStr);
  if (ef === "0") p.push(tStr);
  else if (ef === "1") p.push("45s");
  if (getVal("dl-boot") === "1") p.push("#:1234");
  p.push("FF_BTN");
  var vf = getVal("dl-vol");
  if (vf === "0") p.push("L:2,50");
  else if (vf === "1") p.push("L:2.50");
  else if (vf === "2") p.push("2.50");
  if (getVal("dl-extra") === "1") {
    p.push("FF0");
    p.push("PF1");
  }
  setEl("dl-preview", p.join("|"));
}

// ============================================================================
// ══ SETTINGS: IMPORT / EXPORT ══
// ============================================================================
function settingsImport() {
  var file = document.getElementById("importFile");
  if (!file || !file.files.length) return;
  var fd = new FormData();
  fd.append("settings", file.files[0]);
  var prog = document.getElementById("importProg"),
    bar = document.getElementById("importBar"),
    pct = document.getElementById("importPct");
  if (prog) prog.style.display = "block";
  var xhr = new XMLHttpRequest();
  xhr.upload.onprogress = function (ev) {
    if (ev.lengthComputable) {
      var p = Math.round((ev.loaded / ev.total) * 100);
      if (bar) bar.style.width = p + "%";
      if (pct) pct.textContent = p + "%";
    }
  };
  xhr.onload = function () {
    if (xhr.status === 200) {
      showToast("Settings imported!", "success");
      location.reload();
    } else {
      showToast("Import failed: " + xhr.responseText, "error");
      if (prog) prog.style.display = "none";
    }
  };
  xhr.onerror = function () {
    showToast("Import failed", "error");
    if (prog) prog.style.display = "none";
  };
  xhr.open("POST", "/import_settings");
  xhr.send(fd);
}

// ============================================================================
// ══ OTA UPDATE ══
// ============================================================================
function otaInit() {
  fetch("/api/status")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      ST = d;
      setEl("ota-currentVer", d.version || "--");
      setEl("fwDevice", d.chip || "--");
      setEl("fwHeap", fmtBytes(d.heap));
      setEl("fwStorage", fmtBytes(d.freeSketch));
    });
}


function otaFileSelected() {
  var fileInput = document.getElementById("fwFile");
  var uploadBtn = document.getElementById("otaUploadBtn");
  var fileInfo = document.getElementById("otaFileInfo");
  var dropzone = fileInput && fileInput.closest(".dropzone");
  var file = fileInput.files[0];

  if (!file) {
    uploadBtn.disabled = true;
    if (fileInfo) fileInfo.textContent = "";
    if (dropzone) dropzone.classList.remove("has-file");
    return;
  }

  var errors = [];
  if (!file.name.toLowerCase().endsWith(".bin"))
    errors.push("File must be a .bin file");
  if (file.size < 10000) errors.push("File too small (min 10KB)");

  if (errors.length > 0) {
    if (fileInfo) fileInfo.innerHTML =
      '<span style="color:var(--err)">' + errors.join("<br>") + "</span>";
    if (dropzone) dropzone.classList.add("has-file");
    uploadBtn.disabled = true;
    return;
  }

  var reader = new FileReader();
  reader.onload = function (e) {
    var arr = new Uint8Array(e.target.result);
    if (arr[0] !== 0xe9) {
      if (fileInfo) fileInfo.innerHTML =
        '<span style="color:var(--err)">Invalid firmware (wrong magic byte)</span>';
      if (dropzone) dropzone.classList.add("has-file");
      uploadBtn.disabled = true;
      return;
    }
    if (fileInfo) fileInfo.textContent =
      file.name + " (" + Math.round(file.size / 1024) + " KB)";
    if (dropzone) dropzone.classList.add("has-file");
    uploadBtn.disabled = false;
  };
  reader.readAsArrayBuffer(file.slice(0, 4));
}

// otaShowPopup — `icon` may be a Lucide icon name (e.g. "cloud-upload",
// "check", "x", "alert-triangle") OR a literal emoji string for backward
// compat with older callsites.  Lucide names are detected via the global
// Icons module; anything else is rendered as text.
function otaShowPopup(icon, title, msg, showProgress, showClose) {
  var p = document.getElementById("popup");
  if (p) p.style.display = "flex";
  var iconEl = document.getElementById("popupIcon");
  if (iconEl) {
    if (window.Icons && Icons.svg && Icons.svg(icon)) {
      iconEl.innerHTML = Icons.svg(icon);
      iconEl.style.fontSize = "0";   // collapse the emoji-sized line-height
    } else {
      iconEl.textContent = icon;
      iconEl.style.fontSize = "";
    }
  }
  setEl("popupTitle", title);
  var elMsg = document.getElementById("popupMsg");
  if (elMsg) elMsg.innerHTML = msg;
  var elProg = document.getElementById("popupProgress");
  if (elProg) elProg.style.display = showProgress ? "block" : "none";
  var elClose = document.getElementById("popupClose");
  if (elClose) elClose.style.display = showClose ? "inline-block" : "none";
}

function otaUpdatePopupProgress(pct, text) {
  var bar = document.getElementById("popupBar");
  if (bar) bar.style.width = pct + "%";
  setEl("popupCounter", text);
}

// Pass 5 5.6 — client-side SHA-256 of the selected .bin so the firmware
// can verify the image before committing.  Returns "" when SubtleCrypto
// is unavailable (HTTP context, very old browser); the server treats an
// empty/missing param as "verification not requested".
function _otaSha256(file) {
  return new Promise(function (resolve) {
    if (!window.crypto || !window.crypto.subtle) { resolve(""); return; }
    var rd = new FileReader();
    rd.onload = function (e) {
      window.crypto.subtle.digest("SHA-256", e.target.result).then(function (buf) {
        var arr = new Uint8Array(buf);
        var hex = "";
        for (var i = 0; i < arr.length; i++) {
          hex += ("0" + arr[i].toString(16)).slice(-2);
        }
        resolve(hex);
      }).catch(function () { resolve(""); });
    };
    rd.onerror = function () { resolve(""); };
    rd.readAsArrayBuffer(file);
  });
}

function otaUpload() {
  var fileInput = document.getElementById("fwFile");
  var uploadBtn = document.getElementById("otaUploadBtn");
  var progressDiv = document.getElementById("otaProgress");
  var progressBar = document.getElementById("otaProgressBar");
  var progressText = document.getElementById("otaProgressText");

  if (!fileInput || !fileInput.files.length) return;
  var file = fileInput.files[0];

  uploadBtn.disabled = true;
  fileInput.disabled = true;
  otaShowPopup(
    "cpu",
    "Hashing firmware…",
    "Computing SHA-256 before upload.",
    true,
    false,
  );

  // Compute the hash first, then start the actual upload with the digest
  // appended as a query param.  HTTP context (no SubtleCrypto) yields ""
  // and we fall back to the no-verification path the server already handles.
  _otaSha256(file).then(function (sha) {
    otaShowPopup(
      "cloud-upload",
      "Uploading firmware…",
      sha
        ? "Image hashed. Uploading and verifying on-device…"
        : "Uploading firmware (SHA-256 unavailable on this browser).",
      true,
      false,
    );

    if (progressDiv) progressDiv.style.display = "block";

    var xhr = new XMLHttpRequest();
  xhr.upload.onprogress = function (e) {
    if (e.lengthComputable) {
      var pct = Math.round((e.loaded / e.total) * 100);
      if (progressBar) progressBar.style.width = pct + "%";
      if (progressText) progressText.textContent = "Uploading firmware…";
      var progressPct = document.getElementById("otaProgressPct");
      if (progressPct) progressPct.textContent = pct + "%";
      otaUpdatePopupProgress(
        pct,
        Math.round(e.loaded / 1024) +
          " / " +
          Math.round(e.total / 1024) +
          " KB",
      );
    }
  };
  // Once the upload byte stream is fully on the device, the server still
  // needs ~5 s to verify + write flash before xhr.onload fires.  Surface
  // that phase explicitly so users don't think the UI froze.
  xhr.upload.onload = function () {
    otaShowPopup(
      "cpu",
      "Verifying firmware…",
      "Upload complete. The device is checking and flashing the binary.",
      true,
      false,
    );
    otaUpdatePopupProgress(100, "Flashing…");
  };
  xhr.onload = function () {
    if (progressDiv) progressDiv.style.display = "none";

    if (xhr.status === 200) {
      try {
        var resp = JSON.parse(xhr.responseText);
        if (resp.success) {
          var seconds = 5;
          var tick = function () {
            otaShowPopup(
              "check",
              "Update Complete!",
              "Device will restart...<br>Redirecting in <strong>" +
                seconds +
                "</strong> seconds",
              true,
              false,
            );
            otaUpdatePopupProgress((5 - seconds) * 20, "");
            if (seconds <= 0) {
              window.location.href = "/";
              window.location.reload();
            } else {
              seconds--;
              setTimeout(tick, 1000);
            }
          };
          tick();
        } else {
          otaShowPopup(
            "alert-triangle",
            "Update Failed",
            resp.message || "Unknown error",
            false,
            true,
          );
          uploadBtn.disabled = false;
          fileInput.disabled = false;
        }
      } catch (e) {
        // Failsafe if not JSON
        var seconds = 5;
        var tick = function () {
          otaShowPopup(
            "check",
            "Update sent",
            "Device is restarting...<br>Redirecting in <strong>" +
              seconds +
              "</strong> seconds",
            true,
            false,
          );
          otaUpdatePopupProgress((5 - seconds) * 20, "");
          if (seconds <= 0) {
            window.location.href = "/";
            window.location.reload();
          } else {
            seconds--;
            setTimeout(tick, 1000);
          }
        };
        tick();
      }
    } else {
      otaShowPopup(
        "alert-triangle",
        "Upload Error",
        "Server returned: " + xhr.statusText,
        false,
        true,
      );
      uploadBtn.disabled = false;
      fileInput.disabled = false;
    }
  };
  xhr.onerror = function () {
    if (progressDiv) progressDiv.style.display = "none";
    otaShowPopup(
      "alert-triangle",
      "Connection Error",
      "Could not connect to device",
      false,
      true,
    );
    uploadBtn.disabled = false;
    fileInput.disabled = false;
  };

    var formData = new FormData();
    formData.append("firmware", file);
    var url = sha ? "/do_update?sha256=" + sha : "/do_update";
    xhr.open("POST", url);
    xhr.send(formData);
  });   // end _otaSha256().then
}

function dlToggleMaxSize() {
  var mg = document.getElementById("maxSizeGroup"),
    rot = document.getElementById("dl-rotation");
  if (mg && rot) mg.style.display = rot.value === "4" ? "block" : "none";
}
function dlTogglePcFields() {
  var f = document.getElementById("pcFields"),
    cb = document.getElementById("dl-pcEnabled");
  if (f && cb) f.style.display = cb.checked ? "block" : "none";
}
function closePopup() {
  var p = document.getElementById("popup");
  if (p) p.style.display = "none";
}

// ============================================================================
// Pass 5 phase 4 — schema-driven settings via /api/modules
// ----------------------------------------------------------------------------
// Each module reports a compact JSON schema (see audit §5.4).  Field types:
//   string | password | int | bool | ipv4 | color | enum
// Optional keys: min, max, label, options (for enum), showIf (string or object).
// ============================================================================
var Modules = (function () {
  var current = null;  // currently selected module id

  function escAttr(v) { return esc(String(v == null ? "" : v)); }

  function renderField(f, data) {
    var id = f.id;
    var val = (data && id in data) ? data[id] : "";
    var cls = "input";
    var input;

    if (f.type === "bool") {
      // Let bool fall through to the shared showIf/wrapper tail below so
      // checkboxes honour conditional visibility (e.g. WiFiModule's
      // `useStaticIP` toggling the IPv4 field group).
      input =
        '<label class="field-label">' +
          '<input type="checkbox" name="' + escAttr(id) + '"' +
          (val ? " checked" : "") + '> ' + escAttr(f.label || id) +
        '</label>';
    } else if (f.type === "enum") {
      var opts = (f.options || []).map(function (o) {
        return '<option value="' + escAttr(o.v) + '"' +
               (String(val) === String(o.v) ? " selected" : "") + '>' +
               escAttr(o.l) + '</option>';
      }).join("");
      input = '<select class="' + cls + ' input" name="' +
              escAttr(id) + '">' + opts + '</select>';
    } else if (f.type === "color") {
      input = '<input type="color" class="' + cls + '" name="' +
              escAttr(id) + '" value="' + escAttr(val) + '">';
    } else if (f.type === "password") {
      input = '<input type="password" class="' + cls + '" name="' +
              escAttr(id) + '" value="' + escAttr(val) + '"' +
              (f.max ? ' maxlength="' + Number(f.max) + '"' : '') + '>';
    } else if (f.type === "int") {
      input = '<input type="number" class="' + cls + '" name="' +
              escAttr(id) + '" value="' + escAttr(val) + '"' +
              (f.min != null ? ' min="' + Number(f.min) + '"' : '') +
              (f.max != null ? ' max="' + Number(f.max) + '"' : '') + '>';
    } else {
      // string (default) or ipv4 — both just text inputs with validation.
      var pattern = f.type === "ipv4"
        ? ' pattern="^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$"'
        : "";
      input = '<input type="text" class="' + cls + '" name="' +
              escAttr(id) + '" value="' + escAttr(val) + '"' +
              (f.max ? ' maxlength="' + Number(f.max) + '"' : '') +
              pattern + '>';
    }
    var showIf = f.showIf
      ? ' data-showif="' + esc(JSON.stringify(f.showIf)) + '"'
      : "";
    return (
      '<div class="field" data-field="' + escAttr(id) + '"' + showIf + '>' +
        (f.label ? '<label class="field-label">' + escAttr(f.label) + '</label>' : '') +
        input +
      '</div>'
    );
  }

  // Evaluate showIf spec against current form values.
  // spec = "otherField"  → truthy when that field is truthy.
  // spec = { field: value } → value-match.
  function applyShowIf(form) {
    var groups = form.querySelectorAll("[data-showif]");
    if (!groups.length) return;
    function vOf(name) {
      var el = form.elements[name];
      if (!el) return "";
      return el.type === "checkbox" ? (el.checked ? "1" : "") : el.value;
    }
    function match(spec) {
      if (typeof spec === "string") return !!vOf(spec);
      for (var k in spec) {
        if (String(vOf(k)) !== String(spec[k])) return false;
      }
      return true;
    }
    function refresh() {
      groups.forEach(function (g) {
        var spec;
        try { spec = JSON.parse(g.getAttribute("data-showif")); }
        catch (e) { return; }
        g.style.display = match(spec) ? "" : "none";
      });
    }
    form.addEventListener("change", refresh);
    refresh();
  }

  function collect(form, schema) {
    var out = {};
    (schema.fields || []).forEach(function (f) {
      var el = form.elements[f.id];
      if (!el) return;
      var v;
      if (f.type === "bool")       v = el.checked;
      else if (f.type === "int")   v = el.value === "" ? 0 : parseInt(el.value, 10);
      else                         v = el.value;
      out[f.id] = v;
    });
    return out;
  }

  function loadList() {
    return fetch("/api/modules").then(function (r) { return r.json(); });
  }
  function loadDetail(id) {
    return fetch("/api/modules/" + encodeURIComponent(id))
      .then(function (r) { return r.json(); });
  }
  function save(id, body) {
    return fetch("/api/modules/" + encodeURIComponent(id), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    }).then(function (r) { return r.json(); });
  }

  function renderTabs(list) {
    var host = document.getElementById("mod-tabs");
    if (!host) return;
    host.innerHTML = list.map(function (m) {
      var active = (current === m.id) ? " active" : "";
      return (
        '<button class="btn tab' + active + '"' +
        ' data-click="modulesSelect"' +
        ' data-args="' + esc(JSON.stringify([m.id])) + '">' +
          escAttr(m.name) +
          (m.enabled ? "" : " <span class='badge'>off</span>") +
        '</button>'
      );
    }).join(" ");
  }

  function renderDetail(detail) {
    var host = document.getElementById("mod-host");
    if (!host) return;
    if (!detail.hasUI || !detail.schema) {
      host.innerHTML =
        '<p class="hint">' +
          'This module has no form. ' +
          (detail.config
            ? '<pre>' + esc(JSON.stringify(detail.config, null, 2)) + '</pre>'
            : "") +
        '</p>';
      return;
    }
    var schema;
    try { schema = JSON.parse(detail.schema); }
    catch (e) {
      host.innerHTML = '<p class="hint">Bad schema JSON.</p>';
      return;
    }
    var fields = (schema.fields || [])
      .map(function (f) { return renderField(f, detail.config || {}); })
      .join("");
    host.innerHTML =
      '<form id="mod-form">' +
        '<div class="field">' +
          '<label class="field-label">' +
            '<input type="checkbox" name="__enabled"' +
            (detail.enabled ? " checked" : "") + '> Enabled' +
          '</label>' +
        '</div>' +
        fields +
        '<button type="submit" class="btn primary">💾 Save</button>' +
      '</form>';
    var form = document.getElementById("mod-form");
    applyShowIf(form);
    form.addEventListener("submit", function (ev) {
      ev.preventDefault();
      var msg = document.getElementById("mod-msg");
      var body = {
        enabled: form.elements["__enabled"].checked,
        config:  collect(form, schema)
      };
      save(detail.id, body).then(function (res) {
        if (msg) {
          msg.innerHTML = res && res.ok
            ? '<div class="alert alert-success">Saved ' + escAttr(detail.id) + '</div>'
            : '<div class="alert alert-error">' +
                escAttr((res && res.error) || "save failed") +
              '</div>';
        }
      });
    });
  }

  function select(id) {
    current = id;
    var msg = document.getElementById("mod-msg");
    if (msg) msg.innerHTML = "";
    loadDetail(id).then(renderDetail);
    loadList().then(renderTabs);  // refresh highlight
  }

  function init() {
    loadList().then(function (list) {
      if (!list || !list.length) {
        var h = document.getElementById("mod-host");
        if (h) h.innerHTML = '<p class="hint">No modules registered.</p>';
        return;
      }
      current = list[0].id;
      renderTabs(list);
      select(list[0].id);
    });
  }

  return { init: init, select: select };
})();

// Event-dispatcher entry points
function modulesInit()       { Modules.init(); }
function modulesSelect(id)   { Modules.select(id); }

// Enrol markup-reachable handlers.  See core.js::Handlers for the whitelist
// rationale.  modulesInit is called internally by pageInit, not via markup,
// so it is left out here.
registerHandlers({
  regenDevId: regenDevId,
  toggleManualId: toggleManualId,
  changelogToggle: changelogToggle,
  changelogClose: changelogClose,
  themeSave: themeSave,
  themeToggleChartPath: themeToggleChartPath,
  themeRestoreDefault: themeRestoreDefault,
  netToggleMode: netToggleMode,
  netToggleStatic: netToggleStatic,
  netScanWifi: netScanWifi,
  netTestWifi: netTestWifi,
  timeSetManual: timeSetManual,
  timeSyncNTP: timeSyncNTP,
  timeRtcProtect: timeRtcProtect,
  timeFlushLogs: timeFlushLogs,
  timeRestoreBoot: timeRestoreBoot,
  dlDeleteFile: dlDeleteFile,
  dlUpdatePreview: dlUpdatePreview,
  dlToggleMaxSize: dlToggleMaxSize,
  dlTogglePcFields: dlTogglePcFields,
  settingsImport: settingsImport,
  otaFileSelected: otaFileSelected,
  otaUpload: otaUpload,
  closePopup: closePopup,
  modulesSelect: modulesSelect,
});
