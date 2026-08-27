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
    // Lucide "radio" and "battery-warning", added for the ESP-NOW battery-node
    // page. Same 24×24 grid and stroke conventions as every entry here, so
    // they inherit the sheet's sizing and currentColor without special-casing.
    "radio":
      '<path d="M4.9 19.1C1 15.2 1 8.8 4.9 4.9"></path>' +
      '<path d="M7.8 16.2c-2.3-2.3-2.3-6.1 0-8.5"></path>' +
      '<circle cx="12" cy="12" r="2"></circle>' +
      '<path d="M16.2 7.8c2.3 2.3 2.3 6.1 0 8.5"></path>' +
      '<path d="M19.1 4.9C23 8.8 23 15.1 19.1 19"></path>',
    "battery":
      '<rect width="16" height="10" x="2" y="7" rx="2" ry="2"></rect>' +
      '<line x1="22" x2="22" y1="11" y2="13"></line>',
    "trash":
      '<path d="M3 6h18"></path>' +
      '<path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"></path>' +
      '<path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path>',
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
    "clock":
      '<circle cx="12" cy="12" r="10"></circle>' +
      '<polyline points="12 6 12 12 16 14"></polyline>',
    "layers":
      '<path d="m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z"></path>' +
      '<path d="M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12"></path>' +
      '<path d="M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17"></path>',

    /* ── Editable-deck + topbar (Claude Design phase 6 / handoff) ─────── */
    "grip-vertical":
      '<circle cx="9" cy="5" r="1"></circle>' +
      '<circle cx="9" cy="12" r="1"></circle>' +
      '<circle cx="9" cy="19" r="1"></circle>' +
      '<circle cx="15" cy="5" r="1"></circle>' +
      '<circle cx="15" cy="12" r="1"></circle>' +
      '<circle cx="15" cy="19" r="1"></circle>',
    "eye-off":
      '<path d="M9.88 9.88a3 3 0 1 0 4.24 4.24"></path>' +
      '<path d="M10.73 5.08A10.43 10.43 0 0 1 12 5c7 0 10 7 10 7a13.16 13.16 0 0 1-1.67 2.68"></path>' +
      '<path d="M6.61 6.61A13.526 13.526 0 0 0 2 12s3 7 10 7a9.74 9.74 0 0 0 5.39-1.61"></path>' +
      '<line x1="2" x2="22" y1="2" y2="22"></line>',
    "sliders-horizontal":
      '<line x1="21" x2="14" y1="4" y2="4"></line>' +
      '<line x1="10" x2="3" y1="4" y2="4"></line>' +
      '<line x1="21" x2="12" y1="12" y2="12"></line>' +
      '<line x1="8" x2="3" y1="12" y2="12"></line>' +
      '<line x1="21" x2="16" y1="20" y2="20"></line>' +
      '<line x1="12" x2="3" y1="20" y2="20"></line>' +
      '<line x1="14" x2="14" y1="2" y2="6"></line>' +
      '<line x1="8" x2="8" y1="10" y2="14"></line>' +
      '<line x1="16" x2="16" y1="18" y2="22"></line>',
    "rows-3":
      '<rect width="18" height="18" x="3" y="3" rx="2"></rect>' +
      '<path d="M21 9H3"></path>' +
      '<path d="M21 15H3"></path>',
    "layout-grid":
      '<rect width="7" height="7" x="3" y="3" rx="1"></rect>' +
      '<rect width="7" height="7" x="14" y="3" rx="1"></rect>' +
      '<rect width="7" height="7" x="14" y="14" rx="1"></rect>' +
      '<rect width="7" height="7" x="3" y="14" rx="1"></rect>',
    "bell-ring":
      '<path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"></path>' +
      '<path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"></path>' +
      '<path d="M4 2C2.8 3.7 2 5.7 2 8"></path>' +
      '<path d="M22 8c0-2.3-.8-4.3-2-6"></path>',
    "book-text":
      '<path d="M4 19.5v-15A2.5 2.5 0 0 1 6.5 2H20v20H6.5a2.5 2.5 0 0 1 0-5H20"></path>' +
      '<path d="M8 7h6"></path>' +
      '<path d="M8 11h8"></path>',
    "trash-2":
      '<path d="M3 6h18"></path><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"></path>' +
      '<path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"></path>' +
      '<line x1="10" x2="10" y1="11" y2="17"></line><line x1="14" x2="14" y1="11" y2="17"></line>',
    "upload":
      '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>' +
      '<polyline points="17 8 12 3 7 8"></polyline><line x1="12" x2="12" y1="3" y2="15"></line>',
    "upload-cloud":
      '<path d="M12 13v8"></path>' +
      '<path d="M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242"></path>' +
      '<path d="m8 17 4-4 4 4"></path>',
    "folder-plus":
      '<path d="M12 10v6"></path><path d="M9 13h6"></path>' +
      '<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"></path>',
    "filter":
      '<polygon points="22 3 2 3 10 12.46 10 19 14 21 14 12.46 22 3"></polygon>',
    "file":
      '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"></path>' +
      '<path d="M14 2v4a2 2 0 0 0 2 2h4"></path>',
    "hard-drive":
      '<line x1="22" x2="2" y1="12" y2="12"></line>' +
      '<path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"></path>' +
      '<line x1="6" x2="6.01" y1="16" y2="16"></line><line x1="10" x2="10.01" y1="16" y2="16"></line>',
    "link":
      '<path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"></path>' +
      '<path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"></path>',
    "zap":
      '<path d="M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z"></path>',
    "smartphone":
      '<rect width="14" height="20" x="5" y="2" rx="2" ry="2"></rect>' +
      '<path d="M12 18h.01"></path>',
    "tag":
      '<path d="M12.586 2.586A2 2 0 0 0 11.172 2H4a2 2 0 0 0-2 2v7.172a2 2 0 0 0 .586 1.414l8.704 8.704a2.426 2.426 0 0 0 3.42 0l6.58-6.58a2.426 2.426 0 0 0 0-3.42z"></path>' +
      '<circle cx="7.5" cy="7.5" r=".5" fill="currentColor"></circle>',
    "circle":
      '<circle cx="12" cy="12" r="10"></circle>',
    "image":
      '<rect width="18" height="18" x="3" y="3" rx="2" ry="2"></rect>' +
      '<circle cx="9" cy="9" r="2"></circle>' +
      '<path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"></path>',
    "archive":
      '<rect width="20" height="5" x="2" y="3" rx="1"></rect>' +
      '<path d="M4 8v11a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8"></path>' +
      '<path d="M10 12h4"></path>',
    "printer":
      '<path d="M6 9V2h12v7"></path>' +
      '<path d="M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2"></path>' +
      '<rect width="12" height="8" x="6" y="14"></rect>',
    "laptop":
      '<path d="M20 16V7a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v9m16 0H4m16 0 1.28 2.55a1 1 0 0 1-.9 1.45H3.62a1 1 0 0 1-.9-1.45L4 16"></path>',
    "cloud-fog":
      '<path d="M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242"></path>' +
      '<path d="M16 17H7"></path>' +
      '<path d="M17 21H9"></path>',
    "sliders":
      '<line x1="4" x2="4" y1="21" y2="14"></line>' +
      '<line x1="4" x2="4" y1="10" y2="3"></line>' +
      '<line x1="12" x2="12" y1="21" y2="12"></line>' +
      '<line x1="12" x2="12" y1="8" y2="3"></line>' +
      '<line x1="20" x2="20" y1="21" y2="16"></line>' +
      '<line x1="20" x2="20" y1="12" y2="3"></line>' +
      '<line x1="2" x2="6" y1="14" y2="14"></line>' +
      '<line x1="10" x2="14" y1="8" y2="8"></line>' +
      '<line x1="18" x2="22" y1="16" y2="16"></line>',
    "arrow-left":
      '<path d="m12 19-7-7 7-7"></path><path d="M19 12H5"></path>',
    "arrow-right":
      '<path d="M5 12h14"></path><path d="m12 5 7 7-7 7"></path>',
    "bell-off":
      '<path d="M8.7 3A6 6 0 0 1 18 8a21.3 21.3 0 0 0 .6 5"></path>' +
      '<path d="M17 17H3s3-2 3-9a4.67 4.67 0 0 1 .3-1.7"></path>' +
      '<path d="M10.3 21a1.94 1.94 0 0 0 3.4 0"></path>' +
      '<path d="m2 2 20 20"></path>',
    "cloud-rain":
      '<path d="M4 14.899A7 7 0 1 1 15.71 8h1.79a4.5 4.5 0 0 1 2.5 8.242"></path>' +
      '<path d="M16 14v6"></path><path d="M8 14v6"></path><path d="M12 16v6"></path>',
    "droplet":
      '<path d="M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15a7 7 0 0 0 7 7z"></path>',
    "git-branch":
      '<line x1="6" x2="6" y1="3" y2="15"></line>' +
      '<circle cx="18" cy="6" r="3"></circle><circle cx="6" cy="18" r="3"></circle>' +
      '<path d="M18 9a9 9 0 0 1-9 9"></path>',
    "grid":
      '<rect width="18" height="18" x="3" y="3" rx="2"></rect>' +
      '<path d="M3 9h18"></path><path d="M3 15h18"></path>' +
      '<path d="M9 3v18"></path><path d="M15 3v18"></path>',
    "heart-pulse":
      '<path d="M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z"></path>' +
      '<path d="M3.22 12H9.5l.5-1 2 4.5 2-7 1.5 3.5h5.27"></path>',
    "history":
      '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"></path>' +
      '<path d="M3 3v5h5"></path><path d="M12 7v5l4 2"></path>',
    "list-checks":
      '<path d="m3 17 2 2 4-4"></path><path d="m3 7 2 2 4-4"></path>' +
      '<path d="M13 6h8"></path><path d="M13 12h8"></path><path d="M13 18h8"></path>',
    "plus-circle":
      '<circle cx="12" cy="12" r="10"></circle>' +
      '<path d="M8 12h8"></path><path d="M12 8v8"></path>',
    "wind":
      '<path d="M12.8 19.6A2 2 0 1 0 14 16H2"></path>' +
      '<path d="M17.5 8a2.5 2.5 0 1 1 2 4H2"></path>' +
      '<path d="M9.8 4.4A2 2 0 1 1 11 8H2"></path>'
  };

  // Late additions (moved here from raw inline SVGs in index.html).
  ICON_PATHS["terminal"] =
    '<polyline points="4 17 10 11 4 5"></polyline>' +
    '<line x1="12" x2="20" y1="19" y2="19"></line>';
  ICON_PATHS["circle-dot"] =
    '<circle cx="12" cy="12" r="10"></circle>' +
    '<circle cx="12" cy="12" r="1"></circle>';
  ICON_PATHS["memory-stick"] =
    '<path d="M6 19v-3"></path><path d="M10 19v-3"></path>' +
    '<path d="M14 19v-3"></path><path d="M18 19v-3"></path>' +
    '<path d="M8 11V9"></path><path d="M16 11V9"></path>' +
    '<path d="M12 11V9"></path><path d="M2 15h20"></path>' +
    '<path d="M2 7a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v1.1a2 2 0 0 0 0 3.837V17a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2v-5.1a2 2 0 0 0 0-3.837Z"></path>';

  // Aliases — names referenced by the UI for which we don't ship a dedicated
  // glyph yet. Map each to the closest existing icon so it renders instead of
  // showing blank. Assigned after the literal so the source paths exist.
  ICON_PATHS["puzzle"]       = ICON_PATHS["layers"];
  ICON_PATHS["satellite"]    = ICON_PATHS["globe"];
  ICON_PATHS["microchip"]    = ICON_PATHS["cpu"];
  ICON_PATHS["file-archive"] = ICON_PATHS["archive"];
  ICON_PATHS["bell-plus"]    = ICON_PATHS["plus-circle"];

  function svg(name) {
    var body = ICON_PATHS[name];
    if (!body) return "";
    return (
      '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" ' +
      'viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" ' +
      'data-lucide="' + esc(name) + '" aria-hidden="true" focusable="false">' +
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
