/**
 * /www/js/i18n.js  –  EN/BG UI translation layer
 *
 * Plain global-scope script, same convention as core.js: no bundler, no
 * module system. Loaded right after core.js (before any page module) so
 * registerHandlers() and h() already exist and every later module can call
 * I18n.register() at its own top level.
 *
 * Each module owns its own namespace and calls, once, at load time:
 *
 *   I18n.register("settingsHub", {
 *     en: { title: "Settings", ... },
 *     bg: { title: "Настройки", ... },
 *   });
 *
 * Keys are looked up as "namespace.key" everywhere else:
 *
 *   I18n.t("settingsHub.title")
 *   I18n.t("common.save")            // shared words live in "common"
 *   I18n.t("nodes.offlineFor", { n: 3 })   // {n} placeholders
 *
 * Because each module writes only into its own namespace object, two
 * modules translated in parallel never touch the same file — merge-safe
 * by construction.
 *
 * Markup (data-i18n family, applied by I18n.apply()):
 *   <span data-i18n="common.save">Save</span>                 → textContent
 *   <input data-i18n-placeholder="common.searchPh">
 *   <button data-i18n-aria-label="common.close">
 *   <span data-i18n-title="...">
 * A missing key falls back to English, then to the key itself — text never
 * goes blank if a translation hasn't been written yet.
 */

"use strict";

var I18n = (function () {
  var DICTS = { en: {}, bg: {} };
  var LANG = "en";

  function register(ns, defs) {
    if (!ns || !defs) return;
    if (defs.en) {
      for (var k in defs.en) {
        if (Object.prototype.hasOwnProperty.call(defs.en, k)) {
          DICTS.en[ns + "." + k] = defs.en[k];
        }
      }
    }
    if (defs.bg) {
      for (var k2 in defs.bg) {
        if (Object.prototype.hasOwnProperty.call(defs.bg, k2)) {
          DICTS.bg[ns + "." + k2] = defs.bg[k2];
        }
      }
    }
  }

  function interpolate(s, vars) {
    if (!vars) return s;
    for (var k in vars) {
      if (!Object.prototype.hasOwnProperty.call(vars, k)) continue;
      s = s.split("{" + k + "}").join(String(vars[k]));
    }
    return s;
  }

  function t(key, vars) {
    if (!key) return "";
    var s = DICTS[LANG] && DICTS[LANG][key];
    if (s === undefined) s = DICTS.en[key];
    if (s === undefined) return key;
    return interpolate(s, vars);
  }

  var ATTR_MAP = {
    "data-i18n-placeholder": "placeholder",
    "data-i18n-aria-label": "aria-label",
    "data-i18n-title": "title",
    "data-i18n-value": "value",
    "data-i18n-alt": "alt",
  };

  function apply(root) {
    root = root || document;
    if (root.querySelectorAll) {
      root.querySelectorAll("[data-i18n]").forEach(function (el) {
        el.textContent = t(el.getAttribute("data-i18n"));
      });
      root.querySelectorAll("[data-i18n-html]").forEach(function (el) {
        // Only for markup already trusted at the call site (e.g. a fixed
        // string containing <strong>) — never with user-supplied vars.
        el.innerHTML = t(el.getAttribute("data-i18n-html"));
      });
      Object.keys(ATTR_MAP).forEach(function (dataAttr) {
        root.querySelectorAll("[" + dataAttr + "]").forEach(function (el) {
          el.setAttribute(ATTR_MAP[dataAttr], t(el.getAttribute(dataAttr)));
        });
      });
    }
  }

  function _syncToggleBtn() {
    var btn = document.getElementById("langToggleBtn");
    if (!btn) return;
    var label = document.getElementById("langToggleLabel");
    var next = LANG === "en" ? "BG" : "EN";
    if (label) label.textContent = LANG === "en" ? "EN" : "BG";
    var title = t("common.langToggle", { next: next });
    btn.title = title;
    btn.setAttribute("aria-label", title);
  }

  function setLang(lang) {
    if (lang !== "en" && lang !== "bg") return;
    LANG = lang;
    try { localStorage.setItem("uiLang", lang); } catch (e) {}
    document.documentElement.setAttribute("lang", lang === "bg" ? "bg" : "en");
    apply(document);
    _syncToggleBtn();
    document.dispatchEvent(new CustomEvent("i18n:change", { detail: { lang: lang } }));
  }

  function getLang() { return LANG; }

  function quickLangToggle() {
    setLang(LANG === "en" ? "bg" : "en");
  }

  (function initLang() {
    var pref = "en";
    try { pref = localStorage.getItem("uiLang") || "en"; } catch (e) {}
    LANG = pref === "bg" ? "bg" : "en";
  })();

  return {
    register: register,
    t: t,
    apply: apply,
    setLang: setLang,
    getLang: getLang,
    quickLangToggle: quickLangToggle,
    _syncToggleBtn: _syncToggleBtn,
  };
})();
window.I18n = I18n;
window.quickLangToggle = I18n.quickLangToggle;

document.addEventListener("DOMContentLoaded", function () {
  document.documentElement.setAttribute("lang", I18n.getLang() === "bg" ? "bg" : "en");
  if (typeof registerHandlers === "function") {
    registerHandlers({ quickLangToggle: I18n.quickLangToggle });
  }
  I18n.apply(document);
  I18n._syncToggleBtn();
});
