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
      '<path d="M9 2v2"></path><path d="M9 20v2"></path>'
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
