/**
 * /www/js/theme-boot.js — early-paint theme bootstrap.
 *
 * Loaded synchronously (no defer/async) from <head> before any stylesheet is
 * evaluated, so the html root gets the theme-{light,dark,auto} class before
 * paint and there is no flash-of-light-theme on dark systems. The full theme
 * (custom colours, etc.) is layered on later by applyStatus() in core.js.
 *
 * Extracted from an inline <script> so script-src CSP can drop 'unsafe-inline'.
 */
(function () {
  try {
    var pref = localStorage.getItem("themeOverride") || "auto";
    var cls = "theme-" + (pref === "light" || pref === "dark" ? pref : "auto");
    document.documentElement.classList.add(cls);
  } catch (e) {
    document.documentElement.classList.add("theme-auto");
  }
})();
