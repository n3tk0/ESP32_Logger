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
    // Set data-theme for the design-system CSS variables ([data-theme="dark"]).
    // "auto" resolves against the OS preference; JS in core.js re-evaluates it
    // once and sets the attribute definitively on first paint.
    var dt = pref === "dark" ? "dark"
           : pref === "light" ? "light"
           : (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light");
    document.documentElement.setAttribute("data-theme", dt);
  } catch (e) {
    document.documentElement.classList.add("theme-auto");
    document.documentElement.setAttribute("data-theme", "light");
  }
  // Claude Design phase 4a — restore sidebar rail preference pre-paint to
  // avoid FOUC (body doesn't exist yet; attach to documentElement).
  try {
    if (localStorage.getItem("esp32-sidebar-rail") === "1") {
      document.documentElement.classList.add("sidebar-rail");
    }
  } catch (e) {}
  // Restore accent + density pre-paint so [data-accent] / [data-density]
  // selectors in style.css apply on first frame.  Default to "cyan" /
  // "comfortable" when storage is empty so the first paint matches the UI
  // defaults shown in settings_theme.html (gemini review PR #64).
  // Whitelisted so a bad localStorage value can't inject arbitrary content.
  try {
    var accent = localStorage.getItem("accentColor") || "cyan";
    if (accent === "cyan" || accent === "amber" ||
        accent === "green" || accent === "violet") {
      document.documentElement.setAttribute("data-accent", accent);
    }
  } catch (e) {}
  try {
    var density = localStorage.getItem("uiDensity") || "comfortable";
    if (density === "comfortable" || density === "compact") {
      document.documentElement.setAttribute("data-density", density);
    }
  } catch (e) {}
})();
