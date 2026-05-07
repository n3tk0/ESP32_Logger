/**
 * /www/js/icons.js — Lucide-style SVG icon helper (Claude Design phase 2).
 *
 * Minimal inline-SVG replacement for the emoji icons used across the UI.
 * Elements tagged `<span data-icon="name"></span>` are swapped on DOM ready
 * (and anywhere `Icons.swap(root)` is called manually after injecting
 * dynamic markup).  Every icon path is copied verbatim from Lucide 0.441
 * (lucide.dev / MIT) so we don't hit the CDN at runtime.
 *
 * Adding a new icon: drop its body into ICON_PATHS. Bodies are the inner
 * contents of the <svg> element — no <svg> wrapper, no xmlns.
 */
(function () {
  "use strict";

  var ICON_PATHS = {
    "layout-dashboard":
      '<rect width="7" height="9" x="3" y="3" rx="1"></rect>' +
      '<rect width="7" height="5" x="14" y="3" rx="1"></rect>' +
      '<rect width="7" height="9" x="14" y="12" rx="1"></rect>' +
      '<rect width="7" height="5" x="3" y="16" rx="1"></rect>',
    "folder":
      '<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"></path>',
    "activity":
      '<path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"></path>',
    "gauge":
      '<path d="m12 14 4-4"></path>' +
      '<path d="M3.34 19a10 10 0 1 1 17.32 0"></path>',
    "settings":
      '<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"></path>' +
      '<circle cx="12" cy="12" r="3"></circle>',
    "cloud-upload":
      '<path d="M12 13v8"></path>' +
      '<path d="M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242"></path>' +
      '<path d="m8 17 4-4 4 4"></path>',
    "moon":
      '<path d="M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z"></path>',
    "sun":
      '<circle cx="12" cy="12" r="4"></circle>' +
      '<path d="M12 2v2"></path><path d="M12 20v2"></path>' +
      '<path d="m4.93 4.93 1.41 1.41"></path>' +
      '<path d="m17.66 17.66 1.41 1.41"></path>' +
      '<path d="M2 12h2"></path><path d="M20 12h2"></path>' +
      '<path d="m6.34 17.66-1.41 1.41"></path>' +
      '<path d="m19.07 4.93-1.41 1.41"></path>',
    "menu":
      '<line x1="4" x2="20" y1="12" y2="12"></line>' +
      '<line x1="4" x2="20" y1="6" y2="6"></line>' +
      '<line x1="4" x2="20" y1="18" y2="18"></line>',
    "rotate-ccw":
      '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"></path>' +
      '<path d="M3 3v5h5"></path>',
    "chevron-left":
      '<path d="m15 18-6-6 6-6"></path>',
    "droplets":
      '<path d="M7 16.3c2.2 0 4-1.83 4-4.05 0-1.16-.57-2.26-1.71-3.19S7.29 6.75 7 5.3c-.29 1.45-1.14 2.84-2.29 3.76S3 11.1 3 12.25c0 2.22 1.8 4.05 4 4.05z"></path>' +
      '<path d="M12.56 6.6A10.97 10.97 0 0 0 14 3.02c.5 2.5 2 4.9 4 6.5s3 3.5 3 5.5a6.98 6.98 0 0 1-11.91 4.97"></path>',
    "bell":
      '<path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"></path>' +
      '<path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"></path>',
    "x":
      '<path d="M18 6 6 18"></path><path d="m6 6 12 12"></path>',
    "check":
      '<path d="M20 6 9 17l-5-5"></path>',
    "info":
      '<circle cx="12" cy="12" r="10"></circle>' +
      '<path d="M12 16v-4"></path><path d="M12 8h.01"></path>',
    "alert-triangle":
      '<path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"></path>' +
      '<path d="M12 9v4"></path><path d="M12 17h.01"></path>',
    "search":
      '<circle cx="11" cy="11" r="8"></circle>' +
      '<path d="m21 21-4.35-4.35"></path>',
    "download":
      '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>' +
      '<polyline points="7 10 12 15 17 10"></polyline>' +
      '<line x1="12" x2="12" y1="15" y2="3"></line>',
    "cpu":
      '<rect x="4" y="4" width="16" height="16" rx="2"></rect>' +
      '<rect x="9" y="9" width="6" height="6"></rect>' +
      '<path d="M15 2v2"></path><path d="M15 20v2"></path>' +
      '<path d="M2 15h2"></path><path d="M2 9h2"></path>' +
      '<path d="M20 15h2"></path><path d="M20 9h2"></path>' +
      '<path d="M9 2v2"></path><path d="M9 20v2"></path>',

    /* ── Pass 4 B2 — extended icon set for page-body emoji replacement ── */
    "save":
      '<path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"></path>' +
      '<path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"></path>' +
      '<path d="M7 3v4a1 1 0 0 0 1 1h7"></path>',
    "refresh-cw":
      '<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"></path>' +
      '<path d="M21 3v5h-5"></path>' +
      '<path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"></path>' +
      '<path d="M8 16H3v5"></path>',
    "wifi":
      '<path d="M12 20h.01"></path>' +
      '<path d="M2 8.82a15 15 0 0 1 20 0"></path>' +
      '<path d="M5 12.859a10 10 0 0 1 14 0"></path>' +
      '<path d="M8.5 16.429a5 5 0 0 1 7 0"></path>',
    "bar-chart-3":
      '<path d="M3 3v16a2 2 0 0 0 2 2h16"></path>' +
      '<path d="M18 17V9"></path><path d="M13 17V5"></path><path d="M8 17v-3"></path>',
    "line-chart":
      '<path d="M3 3v16a2 2 0 0 0 2 2h16"></path>' +
      '<path d="m19 9-5 5-4-4-3 3"></path>',
    "globe":
      '<circle cx="12" cy="12" r="10"></circle>' +
      '<path d="M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20"></path>' +
      '<path d="M2 12h20"></path>',
    "wrench":
      '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"></path>',
    "plus":
      '<path d="M5 12h14"></path><path d="M12 5v14"></path>',
    "minus":
      '<path d="M5 12h14"></path>',
    "pencil":
      '<path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"></path>' +
      '<path d="m15 5 4 4"></path>',
    "eye":
      '<path d="M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0"></path>' +
      '<circle cx="12" cy="12" r="3"></circle>',
    "thermometer":
      '<path d="M14 4v10.54a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z"></path>',
    "file-text":
      '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"></path>' +
      '<path d="M14 2v4a2 2 0 0 0 2 2h4"></path>' +
      '<path d="M10 9H8"></path><path d="M16 13H8"></path><path d="M16 17H8"></path>',
    "palette":
      '<circle cx="13.5" cy="6.5" r=".5" fill="currentColor"></circle>' +
      '<circle cx="17.5" cy="10.5" r=".5" fill="currentColor"></circle>' +
      '<circle cx="8.5" cy="7.5" r=".5" fill="currentColor"></circle>' +
      '<circle cx="6.5" cy="12.5" r=".5" fill="currentColor"></circle>' +
      '<path d="M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10c.926 0 1.648-.746 1.648-1.688 0-.437-.18-.835-.437-1.125-.29-.289-.438-.652-.438-1.125a1.64 1.64 0 0 1 1.668-1.668h1.996c3.051 0 5.555-2.503 5.555-5.554C21.965 6.012 17.461 2 12 2z"></path>',
    "rocket":
      '<path d="M4.5 16.5c-1.5 1.26-2 5-2 5s3.74-.5 5-2c.71-.84.7-2.13-.09-2.91a2.18 2.18 0 0 0-2.91-.09z"></path>' +
      '<path d="m12 15-3-3a22 22 0 0 1 2-3.95A12.88 12.88 0 0 1 22 2c0 2.72-.78 7.5-6 11a22.35 22.35 0 0 1-4 2z"></path>' +
      '<path d="M9 12H4s.55-3.03 2-4c1.62-1.08 5 0 5 0"></path>' +
      '<path d="M12 15v5s3.03-.55 4-2c1.08-1.62 0-5 0-5"></path>',
    "calendar":
      '<path d="M8 2v4"></path><path d="M16 2v4"></path>' +
      '<rect width="18" height="18" x="3" y="4" rx="2"></rect>' +
      '<path d="M3 10h18"></path>',
    "clock":
      '<circle cx="12" cy="12" r="10"></circle>' +
      '<polyline points="12 6 12 12 16 14"></polyline>',
    "list":
      '<line x1="8" x2="21" y1="6" y2="6"></line>' +
      '<line x1="8" x2="21" y1="12" y2="12"></line>' +
      '<line x1="8" x2="21" y1="18" y2="18"></line>' +
      '<line x1="3" x2="3.01" y1="6" y2="6"></line>' +
      '<line x1="3" x2="3.01" y1="12" y2="12"></line>' +
      '<line x1="3" x2="3.01" y1="18" y2="18"></line>',
    "circle-dot":
      '<circle cx="12" cy="12" r="10"></circle>' +
      '<circle cx="12" cy="12" r="1" fill="currentColor"></circle>',
    "layers":
      '<path d="m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z"></path>' +
      '<path d="M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12"></path>' +
      '<path d="M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17"></path>'
  };

  function svg(name) {
    var body = ICON_PATHS[name];
    if (!body) return "";
    return (
      '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" ' +
      'viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" ' +
      'data-lucide="' + name + '" aria-hidden="true" focusable="false">' +
      body + "</svg>"
    );
  }

  function swap(root) {
    var scope = root || document;
    var nodes = scope.querySelectorAll("[data-icon]");
    for (var i = 0; i < nodes.length; i++) {
      var el = nodes[i];
      var name = el.getAttribute("data-icon");
      if (!name || !ICON_PATHS[name]) continue;
      // Idempotent — skip if already swapped (contains an <svg> child).
      if (el.firstElementChild && el.firstElementChild.tagName.toLowerCase() === "svg") {
        continue;
      }
      el.innerHTML = svg(name);
    }
  }

  window.Icons = { svg: svg, swap: swap };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", function () { swap(); });
  } else {
    swap();
  }
})();
