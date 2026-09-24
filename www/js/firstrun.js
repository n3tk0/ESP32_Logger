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
    data: null,            // Pins.load() result: profiles + header drawings
    ctx: null,             // Pins.ctx() of the selected profile
    wired: null,           // repaint-all function from Pins.wire()
  };

  function $(id) { return document.getElementById(id); }

  function loadProfiles() {
    return Pins.load()
      .then(function (data) {
        if (!data.profiles.length) throw new Error("no profiles");
        state.data = data;
        state.profiles = data.profiles;
        renderProfileSelect();
        // The board this image was built for, offered — never applied
        // without the user seeing it: the select shows it and Save is theirs.
        if (data.suggested && !$("profile").value) {
          $("profile").value = data.suggested;
          onProfileChange();
        }
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
    state.ctx = state.data ? Pins.ctx(state.data, id) : { profile: null, board: null };
    var hint = $("profileHint");
    var disc = $("customDisclaimer");
    if (state.selectedProfile && state.selectedProfile.id === "custom") {
      hint.textContent = t("firstrun.customValidationOff", "Validation disabled. Any GPIO 0–48 allowed.");
      disc.classList.remove("hidden");
    } else if (state.selectedProfile) {
      hint.textContent = state.ctx.board
        ? t("pins.boardHint", "Type a pin the way the board prints it (D6) or as a GPIO (12). Yellow pins work with the right wiring; red ones never do.")
        : t("pins.gridHint", "This profile has no single board to draw, so every GPIO of the chip is shown.");
      disc.classList.add("hidden");
      $("customAck").checked = false;
    } else {
      hint.textContent = "";
      disc.classList.add("hidden");
    }
    // Entered GPIOs stay what they are; a new board only relabels them.
    revalidateAllPins();
  }

  function renderPinGrid() {
    var grid = $("pinGrid");
    var c = state.ctx || { profile: null, board: null };
    grid.innerHTML = PIN_FIELDS.map(function (f) {
      return Pins.field(f.key, fieldLabel(f) + (f.required ? " *" : ""), -1, c, {
        target: "gpio-" + f.key,
        attrs: ' data-legacy-only="' + (f.legacyOnly ? "1" : "0") + '"',
      });
    }).join("");
    state.wired = Pins.wire(grid, c, pinUses, function (uses) {
      var m = $("pinMap");
      if (m) m.innerHTML = Pins.diagram(state.ctx, uses);
    }, function (inp) {
      var key = inp.getAttribute("data-pin");
      for (var i = 0; i < PIN_FIELDS.length; i++) {
        if (PIN_FIELDS[i].key === key) return { required: PIN_FIELDS[i].required };
      }
      return null;
    });
  }

  // Every pin the form claims — the duplicate check and the "used" pads.
  // Only visible fields count: continuous mode hides the legacy-only ones
  // and the backend ignores them there.
  function pinUses() {
    var c = state.ctx, out = [];
    PIN_FIELDS.forEach(function (f) {
      var inp = $("pin-" + f.key);
      if (!inp || inp.offsetParent === null) return;
      var r = Pins.parse(c, inp.value);
      if (r.gpio != null) out.push({ key: f.key, g: r.gpio, who: fieldLabel(f) });
    });
    return out;
  }

  // Pins.wire() was bound to the context current when the grid was built;
  // a new board means a new context, so the grid is rebuilt around the
  // values already typed (kept as GPIOs in the hidden inputs).
  function revalidateAllPins() {
    var keep = {};
    PIN_FIELDS.forEach(function (f) {
      var hid = document.querySelector('input[name="gpio-' + f.key + '"]');
      var inp = $("pin-" + f.key);
      keep[f.key] = { g: hid ? parseInt(hid.value, 10) : -1, raw: inp ? inp.value : "" };
    });
    renderPinGrid();
    PIN_FIELDS.forEach(function (f) {
      var inp = $("pin-" + f.key), k = keep[f.key];
      if (!inp) return;
      inp.value = k.g >= 0 ? Pins.text(state.ctx, k.g) : k.raw;
    });
    onModeChange();
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
    if (state.wired) state.wired();
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
    // Only red is refused, as on the device: a yellow pin (strap, console)
    // is the user's call and the wizard has already said why it is risky.
    var allOk = state.wired ? state.wired() : true;
    PIN_FIELDS.forEach(function (f) {
      // Skip legacy-only fields in continuous mode — they're hidden from
      // the UI and the backend ignores them for non-legacy modes anyway.
      if (f.legacyOnly && !legacy) return;
      var r = Pins.parse(state.ctx, $("pin-" + f.key).value);
      body.pins[f.key] = r.gpio == null ? -1 : r.gpio;
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
    // renderProfileSelect() rebuilds the <option> list from scratch, so the
    // selection has to be put back by value afterwards. onProfileChange()
    // then rebuilds the pin grid around the GPIOs already entered.
    var chosen = $("profile") ? $("profile").value : "";
    renderProfileSelect();
    if ($("profile")) $("profile").value = chosen;
    onProfileChange();
  });
})();
