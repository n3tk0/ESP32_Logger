// R11 first-run wizard. Self-contained — does not load core.js / settings.js
// so it works on a fresh device before any settings persist. The page is
// served unauthenticated (FirstRunGate whitelist).

(function () {
  "use strict";

  // t(key, fallback, vars): translated string with an explicit English
  // fallback, same guarded pattern used across the rest of the app (see
  // core.js / nodes.js / settings.js). firstrun.html always loads i18n.js
  // now, but the guard keeps this file safe to load on its own too.
  function t(key, fallback, vars) {
    return window.I18n ? I18n.t(key, vars) : fallback;
  }

  // legacyOnly: hidden when mode is "continuous" (those pins only matter
  // for the legacy flow pipeline). Always-shown pins (WiFi-trigger,
  // buttons) drive the device's physical UI in every mode and must be
  // collected even in continuous mode — leaving them unset breaks the
  // AP-trigger button and post-correction buttons.
  var PIN_FIELDS = [
    { key: "wifiTrigger", labelKey: "firstrun.pinWifiTrigger", label: "WiFi-trigger button",      required: false, legacyOnly: false },
    { key: "wakeupFF",    labelKey: "firstrun.pinWakeupFF",    label: "Wakeup (FF / manual)",     required: false, legacyOnly: false },
    { key: "wakeupPF",    labelKey: "firstrun.pinWakeupPF",    label: "Wakeup (PF / auto)",       required: false, legacyOnly: false },
    { key: "flowSensor",  labelKey: "firstrun.pinFlowSensor",  label: "Flow sensor input",        required: true,  legacyOnly: true  },
    { key: "rtcCE",       labelKey: "firstrun.pinRtcCE",       label: "RTC chip-enable (DS1302)", required: false, legacyOnly: true  },
    { key: "rtcIO",       labelKey: "firstrun.pinRtcIO",       label: "RTC data IO",              required: false, legacyOnly: true  },
    { key: "rtcSCLK",     labelKey: "firstrun.pinRtcSCLK",     label: "RTC clock",                required: false, legacyOnly: true  },
  ];

  function fieldLabel(f) { return t(f.labelKey, f.label); }

  // Escape any text we render into the DOM. The profile name / pin
  // reject reason strings come from the backend; treat them as untrusted.
  function esc(s) {
    if (s == null) return "";
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                    .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  var state = {
    profiles: [],          // [{id,name,maxGpio,strapPins,usbPins,...}]
    selectedProfile: null, // pointer into state.profiles
  };

  function $(id) { return document.getElementById(id); }

  function loadProfiles() {
    return fetch("/api/board-profiles")
      .then(function (r) { return r.json(); })
      .then(function (data) {
        state.profiles = data.profiles || [];
        renderProfileSelect();
      })
      .catch(function (e) {
        showStatus(t("firstrun.statusLoadProfilesFailed", "Failed to load board profiles: {msg}", { msg: esc(e.message) }), "err");
      });
  }

  function renderProfileSelect() {
    var sel = $("profile");
    sel.innerHTML = "<option value=\"\">" + esc(t("firstrun.chooseBoard", "— Choose a board —")) + "</option>";
    state.profiles.forEach(function (p) {
      var opt = document.createElement("option");
      opt.value = p.id;
      opt.textContent = p.name;
      sel.appendChild(opt);
    });
    sel.onchange = onProfileChange;
  }

  function onProfileChange() {
    var id = $("profile").value;
    state.selectedProfile = state.profiles.find(function (p) { return p.id === id; }) || null;
    var hint = $("profileHint");
    var disc = $("customDisclaimer");
    if (state.selectedProfile && state.selectedProfile.id === "custom") {
      hint.textContent = t("firstrun.customValidationOff", "Validation disabled. Any GPIO 0–48 allowed.");
      disc.classList.remove("hidden");
    } else if (state.selectedProfile) {
      var noneWord = t("firstrun.none", "none");
      var summary = t("firstrun.hintSummary", "Strap: {strap}  •  USB: {usb}  •  max GPIO: {max}", {
        strap: state.selectedProfile.strapPins.join(",") || noneWord,
        usb: state.selectedProfile.usbPins.join(",") || noneWord,
        max: state.selectedProfile.maxGpio,
      });
      // Only board-specific profiles carry this; older firmware omits the key.
      var absent = state.selectedProfile.absentPins || [];
      if (absent.length) {
        summary += t("firstrun.hintNoHeaderPad", "  •  no header pad: {pins}", { pins: absent.join(",") });
      }
      hint.textContent = summary;
      disc.classList.add("hidden");
      $("customAck").checked = false;
    } else {
      hint.textContent = "";
      disc.classList.add("hidden");
    }
    revalidateAllPins();
  }

  function renderPinGrid() {
    var grid = $("pinGrid");
    grid.innerHTML = "";
    PIN_FIELDS.forEach(function (f) {
      var labelEl = document.createElement("label");
      labelEl.textContent = fieldLabel(f) + (f.required ? " *" : "");
      labelEl.setAttribute("for", "pin-" + f.key);
      labelEl.dataset.legacyOnly = f.legacyOnly ? "1" : "0";
      var input = document.createElement("input");
      input.type = "number"; input.id = "pin-" + f.key;
      input.min = -1; input.max = 48; input.value = -1;
      input.dataset.key = f.key;
      input.dataset.legacyOnly = f.legacyOnly ? "1" : "0";
      if (f.required) input.setAttribute("aria-required", "true");
      input.oninput = function () { revalidatePin(f.key); };
      var msg = document.createElement("div");
      msg.id = "msg-" + f.key; msg.className = "ok";
      msg.dataset.legacyOnly = f.legacyOnly ? "1" : "0";
      grid.appendChild(labelEl);
      grid.appendChild(input);
      grid.appendChild(msg);
    });
  }

  function inList(list, pin) {
    return Array.isArray(list) && list.indexOf(pin) !== -1;
  }

  // Mirror of isPinAllowed() in src/core/BoardProfiles.cpp. Kept in sync
  // by the GET /api/board-profiles response containing the same lists.
  function pinReason(profile, pin) {
    if (!profile)           return { ok: false, reason: t("firstrun.reasonNoProfile", "no board profile selected") };
    if (pin === -1)         return { ok: true,  reason: t("firstrun.reasonUnassigned", "unassigned (optional)") };
    if (pin < 0)            return { ok: false, reason: t("firstrun.reasonNegative", "negative GPIO") };
    if (pin > profile.maxGpio) return { ok: false, reason: t("firstrun.reasonMaxGpio", "GPIO > {max} for this board", { max: profile.maxGpio }) };
    if (profile.id === "custom") return { ok: true, reason: t("firstrun.reasonCustomOff", "custom — validation off") };
    if (inList(profile.strapPins,    pin)) return { ok: false, reason: t("firstrun.reasonBootstrap", "bootstrap pin (boot-mode risk)") };
    if (inList(profile.usbPins,      pin)) return { ok: false, reason: t("firstrun.reasonUsbCdc", "USB CDC pin (D+/D-)") };
    if (inList(profile.flashPins,    pin)) return { ok: false, reason: t("firstrun.reasonSpiFlash", "SPI flash bus pin") };
    if (inList(profile.reservedPins, pin)) return { ok: false, reason: t("firstrun.reasonUart0", "UART0 console (you would lose serial debug)") };
    if (inList(profile.absentPins,   pin)) return { ok: false, reason: t("firstrun.reasonAbsent", "not broken out on this board") };
    return { ok: true, reason: t("firstrun.reasonOk", "ok") };
  }

  function revalidatePin(key) {
    var input = $("pin-" + key);
    var msg   = $("msg-" + key);
    var pin   = parseInt(input.value, 10);
    if (isNaN(pin)) pin = -1;
    var res = pinReason(state.selectedProfile, pin);

    // Duplicate detection against other assigned pins.
    if (res.ok && pin !== -1) {
      for (var i = 0; i < PIN_FIELDS.length; i++) {
        var k = PIN_FIELDS[i].key;
        if (k === key) continue;
        var other = parseInt($("pin-" + k).value, 10);
        if (other === pin) {
          res = { ok: false, reason: t("firstrun.reasonDuplicate", "duplicate of {label}", { label: fieldLabel(PIN_FIELDS[i]) }) };
          break;
        }
      }
    }

    msg.className = res.ok ? "ok" : "err";
    msg.textContent = res.reason;
  }

  function revalidateAllPins() {
    PIN_FIELDS.forEach(function (f) { revalidatePin(f.key); });
  }

  function onModeChange() {
    var mode = $("mode").value;
    var legacy = (mode === "legacy" || mode === "hybrid");
    // Show/hide ONLY the legacy-only pin rows. Universal pins (buttons +
    // WiFi-trigger) stay visible in continuous mode so the wizard still
    // collects them — those drive the device's physical UI in every mode.
    var els = document.querySelectorAll('[data-legacy-only="1"]');
    for (var i = 0; i < els.length; i++) {
      els[i].style.display = legacy ? "" : "none";
    }
  }

  function showStatus(msg, kind) {
    var s = $("status");
    s.className = "status " + (kind || "");
    s.textContent = msg;
    s.style.display = "block";
  }

  function onSave() {
    var profile = state.selectedProfile;
    if (!profile) { showStatus(t("firstrun.statusPickProfile", "Pick a board profile first."), "err"); return; }
    if (profile.id === "custom" && !$("customAck").checked) {
      showStatus(t("firstrun.statusCheckCustomAck", "Check the Custom acknowledgement to proceed."), "err");
      return;
    }
    var mode = $("mode").value;
    var body = { profile: profile.id, mode: mode, pins: {} };

    var legacy = (mode === "legacy" || mode === "hybrid");
    var allOk = true;
    PIN_FIELDS.forEach(function (f) {
      // Skip legacy-only fields in continuous mode — they're hidden from
      // the UI and the backend ignores them for non-legacy modes anyway.
      if (f.legacyOnly && !legacy) return;
      var pin = parseInt($("pin-" + f.key).value, 10);
      if (isNaN(pin)) pin = -1;
      if (f.required && pin === -1) {
        $("msg-" + f.key).className = "err";
        $("msg-" + f.key).textContent = t("firstrun.reasonRequired", "required");
        allOk = false;
      }
      var msgEl = $("msg-" + f.key);
      if (msgEl && msgEl.className === "err") allOk = false;
      body.pins[f.key] = pin;
    });
    if (!allOk) { showStatus(t("firstrun.statusFixPins", "Fix the highlighted pins above."), "err"); return; }

    $("saveBtn").disabled = true;
    showStatus(t("firstrun.statusSaving", "Saving and rebooting…"), "ok");

    fetch("/api/firstrun", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    })
      .then(function (r) {
        return r.text().then(function (respText) {
          var data; try { data = JSON.parse(respText); } catch (e) { data = { ok: false, error: respText }; }
          if (!data.ok) {
            $("saveBtn").disabled = false;
            showStatus(t("firstrun.statusError", "Error: {error}", { error: esc(data.error || ("HTTP " + r.status)) }), "err");
            return;
          }
          showStatus(t("firstrun.statusSaved", "Saved. Device is rebooting — this page will reload in 8 seconds."), "ok");
          setTimeout(function () { location.href = "/"; }, 8000);
        });
      })
      .catch(function (e) {
        $("saveBtn").disabled = false;
        showStatus(t("firstrun.statusNetworkError", "Network error: {msg}", { msg: esc(e.message) }), "err");
      });
  }

  // Init
  document.addEventListener("DOMContentLoaded", function () {
    renderPinGrid();
    revalidateAllPins();
    $("mode").onchange = onModeChange;
    $("saveBtn").onclick = onSave;
    // firstrun.html doesn't load core.js, so the data-click delegation
    // dispatcher (installEventDispatcher() in core.js) never runs here.
    // Wire the lang-toggle button directly instead.
    var langBtn = $("langToggleBtn");
    if (langBtn) {
      langBtn.onclick = function () {
        if (window.I18n) I18n.quickLangToggle();
      };
    }
    onModeChange();
    loadProfiles();
  });

  // I18n.apply() only reaches elements carrying data-i18n, and none of the
  // wizard's generated content does: the pin labels, the per-pin verdicts,
  // the "— Choose a board —" option and the profile hint are all written with
  // textContent from JS and never revisited. Re-render them on a language
  // switch, carrying the user's typed pins and chosen board across.
  document.addEventListener("i18n:change", function () {
    if (!$("pinGrid")) return;            // DOMContentLoaded has not run yet

    var typed = {};
    PIN_FIELDS.forEach(function (f) {
      var el = $("pin-" + f.key);
      if (el) typed[f.key] = el.value;
    });
    renderPinGrid();
    PIN_FIELDS.forEach(function (f) {
      var el = $("pin-" + f.key);
      if (el && typed[f.key] !== undefined) el.value = typed[f.key];
    });
    onModeChange();                       // re-hide the legacy-only rows

    // renderProfileSelect() rebuilds the <option> list from scratch, so the
    // selection has to be put back by value afterwards.
    var chosen = $("profile") ? $("profile").value : "";
    renderProfileSelect();
    if ($("profile")) $("profile").value = chosen;
    onProfileChange();                    // redraws the hint, then revalidates
  });
})();
