/**
 * /www/js/pages.js — dashboard, files, live pages
 * Loaded after core.js. Depends on globals: ST, CFG, currentPage,
 * setEl/setVal/getVal/showToast/showMsg, navigateTo, fmtBytes.
 */
"use strict";

// ============================================================================
// ══ PAGE: DASHBOARD ══
// Exact port of original .ino embedded JS:
//   loadData() → fetch('/download?file=...') → processData() → renderChart()
// ============================================================================
var _chartJsLoading = false;
var _chartJsLoaded = false;
var _chartJsCbs = [];

function dbLoadChartJs(cb) {
  if (_chartJsLoaded) {
    cb();
    return;
  }
  _chartJsCbs.push(cb);
  if (_chartJsLoading) return;
  _chartJsLoading = true;

  var th = ST.theme || CFG.theme || {};
  var localPath = th.chartLocalPath || "/chart.min.js";
  var cdnPath = "https://cdn.jsdelivr.net/npm/chart.js";
  var wantsCDN = !(th.chartSource === 0 || th.chartSource === "0");
  // Strict CSP (script-src 'self') blocks the CDN.  Try whatever the user
  // configured first, but always fall back to the on-device copy instead of
  // retrying the same CDN URL.
  var primary  = wantsCDN ? cdnPath : localPath;
  var fallback = wantsCDN ? localPath : cdnPath;

  function fire() {
    _chartJsLoaded = true;
    _chartJsLoading = false;
    _chartJsCbs.forEach(function (fn) {
      fn();
    });
    _chartJsCbs = [];
  }

  function _giveUp() {
    window._chartJsLoading = false;
    _chartJsLoading = false;
    var err = document.getElementById("errorMsg");
    if (err) {
      err.innerHTML =
        "<strong>Charts unavailable:</strong> Could not load <code>chart.min.js</code>. " +
        "If you removed it from LittleFS, re-upload it via /upload.";
      err.style.display = "block";
    }
    if (typeof showToast === "function") {
      showToast("Failed to load chart.min.js", "error");
    }
  }

  var s = document.createElement("script");
  s.src = primary;
  s.onload = fire;
  s.onerror = function () {
    var s2 = document.createElement("script");
    s2.src = fallback;
    s2.onload = fire;
    s2.onerror = _giveUp;
    document.head.appendChild(s2);
  };
  document.head.appendChild(s);
}

function dbInit() {
  dbLoadChartJs(function () {
    // Matches original: generateDatalogFileOptions() via select population
    fetch("/api/filelist?filter=log&recursive=1")
      .then(function (r) {
        return r.json();
      })
      .then(function (d) {
        var sel = document.getElementById("fileSelect");
        if (!sel) return;
        sel.innerHTML = "";
        var curFile = d.currentFile || ST.currentFile || "";
        if (!d.files || !d.files.length) {
          sel.innerHTML = "<option>No log files found</option>";
          return;
        }
        d.files.forEach(function (f) {
          var opt = document.createElement("option");
          opt.value = f.path;
          opt.textContent = f.path;
          if (curFile && f.path === curFile) opt.selected = true;
          sel.appendChild(opt);
        });
        dbLoadData(); // matches original: window.onload = loadData
      })
      .catch(function (e) {
        var err = document.getElementById("errorMsg");
        if (err) {
          err.textContent = "Error loading file list: " + e.message;
          err.style.display = "block";
        }
      });
  });
}

// Matches original: function loadData()
function dbLoadData() {
  var file = getVal("fileSelect");
  if (!file || file === "No log files found") return;
  var err = document.getElementById("errorMsg");
  if (err) err.style.display = "none";
  fetch("/download?file=" + encodeURIComponent(file))
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.text();
    })
    .then(function (data) {
      dbRawData = data;
      dbApplyFilters();
    })
    .catch(function (e) {
      if (err) {
        err.textContent = "Error loading: " + e.message;
        err.style.display = "block";
      }
    });
}

// Matches original: function applyFilters()
function dbApplyFilters() {
  if (!dbRawData) {
    dbLoadData();
    return;
  }
  dbProcessData(dbRawData);
}

// Matches original: function processData(data) — exact port of .ino embedded JS
function dbProcessData(data) {
  var lines = data.trim().split("\n");
  var filtered = [];
  var startVal = getVal("startDate");
  var endVal = getVal("endDate");
  var filterType = getVal("eventFilter");
  var pressType = getVal("pressFilter");
  var excZ =
    document.getElementById("excludeZero") &&
    document.getElementById("excludeZero").checked;
  var tVol = 0,
    tFF = 0,
    tPF = 0;

  lines.forEach(function (line) {
    var p = line.split("|");
    if (p.length < 2) return;
    var dateStr = "",
      timeStr = "",
      endStr = "",
      boot = "",
      reason = "",
      vol = 0,
      ff = 0,
      pf = 0,
      i = 0;

    // Auto-detect date format (DD/MM/YYYY ┬╖ DD.MM.YYYY ┬╖ YYYY-MM-DD)
    if (
      p[0].match(/\d{2}[\/\.\-]\d{2}[\/\.\-]\d{4}/) ||
      p[0].match(/\d{4}\-\d{2}\-\d{2}/)
    ) {
      dateStr = p[0];
      i = 1;
    }
    if (p[i] && p[i].indexOf(":") >= 0) {
      timeStr = p[i];
      i++;
    }
    if (p[i] && (p[i].indexOf(":") >= 0 || p[i].match(/^\d+s$/))) {
      endStr = p[i];
      i++;
    }
    if (p[i] && p[i].indexOf("#:") === 0) {
      boot = p[i].substring(2);
      i++;
    }
    if (
      p[i] &&
      (p[i].indexOf("FF") >= 0 || p[i].indexOf("PF") >= 0 || p[i] === "IDLE")
    ) {
      reason = p[i];
      i++;
    }
    if (p[i]) {
      var vs = p[i].replace("L:", "").replace(",", ".");
      vol = parseFloat(vs) || 0;
      i++;
    }
    if (p[i] && p[i].indexOf("FF") === 0) {
      ff = parseInt(p[i].replace("FF", "")) || 0;
      i++;
    }
    if (p[i] && p[i].indexOf("PF") === 0) {
      pf = parseInt(p[i].replace("PF", "")) || 0;
    }

    var entryDate = "";
    if (dateStr) {
      var m;
      if ((m = dateStr.match(/(\d{2})[\/\.](\d{2})[\/\.](\d{4})/)))
        entryDate = m[3] + "-" + m[2] + "-" + m[1];
      else if ((m = dateStr.match(/(\d{4})\-(\d{2})\-(\d{2})/)))
        entryDate = m[1] + "-" + m[2] + "-" + m[3];
    }

    // Filters — exact logic from original .ino
    if (startVal && entryDate && entryDate < startVal) return;
    if (endVal && entryDate && entryDate > endVal) return;
    if (
      filterType === "BTN" &&
      reason.indexOf("FF") < 0 &&
      reason.indexOf("PF") < 0
    )
      return;
    if (filterType === "FF" && reason.indexOf("FF") < 0) return;
    if (filterType === "PF" && reason.indexOf("PF") < 0) return;
    if (pressType === "EXTRA" && ff === 0 && pf === 0) return;
    if (pressType === "NONE" && (ff > 0 || pf > 0)) return;
    if (excZ && vol === 0) return;

    tFF += ff;
    tPF += pf;
    tVol += vol;
    var fullTime = timeStr + (endStr ? "-" + endStr : "");
    filtered.push({
      date: dateStr || "N/A",
      time: timeStr,
      fullTime: fullTime,
      boot: boot,
      vol: vol,
      reason: reason,
      ff: ff,
      pf: pf,
    });
  });

  dbFilteredData = filtered;
  // Element IDs match original: totalVol, eventCount, totalFF, totalPF
  setEl("db-totalVol", tVol.toFixed(2) + " L");
  setEl("db-eventCount", filtered.length);
  setEl("db-totalFF", tFF);
  setEl("db-totalPF", tPF);
  dbRenderChart(filtered);
}

// Matches original: function renderChart(data)
function dbRenderChart(data) {
  var ctx = document.getElementById("chart");
  if (!ctx) return;
  if (typeof Chart === "undefined") {
    dbLoadChartJs(function () {
      dbRenderChart(data);
    });
    return;
  }
  if (dbChart) {
    dbChart.destroy();
    dbChart = null;
  }

  var th = ST.theme || CFG.theme || {};
  var rootStyle = getComputedStyle(document.documentElement);
  var ffColor =
    th.ffColor || rootStyle.getPropertyValue("--ff-color").trim() || "#275673";
  var pfColor =
    th.pfColor || rootStyle.getPropertyValue("--pf-color").trim() || "#7eb0d5";
  var otherColor =
    th.otherColor ||
    rootStyle.getPropertyValue("--other-color").trim() ||
    "#a0aec0";

  var clr = data.map(function (d) {
    if (d.reason.indexOf("FF") >= 0) return ffColor;
    if (d.reason.indexOf("PF") >= 0) return pfColor;
    return otherColor;
  });

  // Matches original: var lblFmt = config.theme.chartLabelFormat
  var lblFmt = th.chartLabelFormat !== undefined ? th.chartLabelFormat : 0;
  var lbls = data.map(function (d) {
    if (lblFmt === 1) return d.boot ? "#" + d.boot : "#?";
    if (lblFmt === 2)
      return d.date + " " + d.time + (d.boot ? " #" + d.boot : "");
    return d.date + " " + d.time;
  });

  // Phase 5c-5 — crosshair plugin draws a vertical guide line from chart
  // top to bottom anchored at the currently-hovered data index.  Reads
  // active tooltip element list set by Chart.js's index-mode interaction.
  var crosshairPlugin = {
    id: "dashboardCrosshair",
    afterDraw: function (chart) {
      var tt = chart.tooltip;
      if (!tt || !tt._active || !tt._active.length) return;
      var active = tt._active[0];
      var x = active.element.x;
      var c = chart.ctx;
      var top = chart.chartArea.top;
      var bottom = chart.chartArea.bottom;
      c.save();
      c.beginPath();
      c.moveTo(x, top);
      c.lineTo(x, bottom);
      c.lineWidth = 1;
      c.strokeStyle = "rgba(8, 145, 178, 0.4)";  /* design --d-accent */
      c.setLineDash([4, 4]);
      c.stroke();
      c.restore();
    }
  };

  dbChart = new Chart(ctx, {
    type: "bar",
    data: {
      labels: lbls,
      datasets: [
        {
          label: "Liters (L)",
          data: data.map(function (d) {
            return d.vol;
          }),
          backgroundColor: clr,
          borderWidth: 0,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      // index-mode + intersect:false makes the crosshair track even when
      // the cursor is between bars, matching the design's expected feel.
      interaction: { mode: "index", intersect: false },
      plugins: {
        tooltip: {
          callbacks: {
            afterLabel: function (c) {
              var d = data[c.dataIndex];
              return [
                "Trigger: " + d.reason,
                "Boot: " + (d.boot || "N/A"),
                "Extra FF: " + d.ff,
                "Extra PF: " + d.pf,
              ];
            },
          },
        },
      },
      scales: {
        y: { beginAtZero: true, title: { display: true, text: "Liters" } },
      },
    },
    plugins: [crosshairPlugin],
  });
}

// Matches original: function exportCSV()
function dbExportCSV() {
  if (!dbFilteredData.length) {
    showToast("No data to export", "error");
    return;
  }
  var csv = "Date,Time,Boot,Volume (L),Trigger,Extra FF,Extra PF\n";
  dbFilteredData.forEach(function (d) {
    csv +=
      d.date +
      "," +
      d.fullTime +
      "," +
      (d.boot || "") +
      "," +
      d.vol.toFixed(2) +
      "," +
      d.reason +
      "," +
      d.ff +
      "," +
      d.pf +
      "\n";
  });
  // Filename: deviceId_filters_date.csv  — exact match to original .ino
  var f = ST.deviceId || CFG.deviceId || "logger";
  var ft = getVal("eventFilter");
  if (ft !== "ALL") f += "_" + ft;
  var pt = getVal("pressFilter");
  if (pt !== "ALL") f += "_" + pt;
  var excZ = document.getElementById("excludeZero");
  if (excZ && excZ.checked) f += "_noZero";
  var sd = getVal("startDate");
  if (sd) f += "_from" + sd;
  var ed = getVal("endDate");
  if (ed) f += "_to" + ed;
  f += "_" + new Date().toISOString().slice(0, 10) + ".csv";
  var blob = new Blob([csv], { type: "text/csv" });
  var url = URL.createObjectURL(blob);
  var a = document.createElement("a");
  a.href = url;
  a.download = f;
  a.click();
  URL.revokeObjectURL(url);
}

// ============================================================================
// ══ PAGE: FILES ══
// ============================================================================
function filesInit() {
  filesEditMode = false;
  currentFilesDir = "/";
  var hw = CFG.hardware || {};
  currentFilesStorage = hw.defaultStorageView === 1 ? "sdcard" : "internal";
  var list = document.getElementById("list");
  if (list && list.innerHTML.trim().length === 0) list.innerHTML = "<div class='list-item text-muted'>Loading…</div>";
  filesRender();
}

function filesRender() {
  var tabs = document.getElementById("tabs");
  if (tabs) {
    tabs.innerHTML =
      '<button data-click="filesSetStorage" data-args=\'["internal"]\' class="' +
      (currentFilesStorage === "internal" ? "active" : "") +
      '"><span data-icon="microchip"></span> LittleFS</button>' +
      '<button data-click="filesSetStorage" data-args=\'["sdcard"]\' class="' +
      (currentFilesStorage === "sdcard" ? "active" : "") +
      '"><span data-icon="hard-drive"></span> SD Card</button>';
  }

  fetch(
    "/api/filelist?storage=" +
      currentFilesStorage +
      "&dir=" +
      encodeURIComponent(currentFilesDir),
  )
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      var pct = d.percent || 0;
      setEl("files-usage", fmtBytes(d.used) + " / " + fmtBytes(d.total));
      setEl("files-pct", pct + "%");
      var bar = document.getElementById("bar");
      if (bar) {
        bar.style.width = pct + "%";
        bar.className = pct >= 70 ? "warn" : "";
      }

      var lbl = document.getElementById("files-dirLabel");
      if (lbl) {
        lbl.innerHTML =
          '<span class="mono">' +
          (currentFilesStorage === "sdcard" ? "SD:" : "FS:") +
          "</span> " + esc(currentFilesDir);
      }
      var upBtn = document.getElementById("upBtn");
      if (upBtn) upBtn.style.display = currentFilesDir === "/" ? "none" : "";
      var btnEdit = document.getElementById("btnEdit");
      var btnDone = document.getElementById("btnDone");
      if (btnEdit) btnEdit.style.display = filesEditMode ? "none" : "";
      if (btnDone) btnDone.style.display = filesEditMode ? "" : "none";
      var tools = document.getElementById("editTools");
      if (tools) tools.style.display = filesEditMode ? "block" : "none";

      var list = document.getElementById("list");
      if (!list) return;
      var files = d.files || [];
      if (!files.length) {
        list.innerHTML = "";
        list.appendChild(emptyState({
          icon: "folder",
          title: "No files",
          msg: "This directory is empty. Upload a file or create a subfolder to get started."
        }));
        return;
      }

      var rows = "";
      if (d.truncated) {
        rows +=
          '<tr><td colspan="5" style="color:var(--warn);font-size:11.5px;padding:6px 14px">' +
          "Listing truncated at 500 entries — refine with a subfolder." +
          "</td></tr>";
      }
      files.forEach(function (f) {
        var icon = f.isDir
          ? '<span data-icon="folder"></span>'
          : (/\.gz$/i.test(f.name)
              ? '<span data-icon="file-archive"></span>'
              : (/\.(jsonl?|csv|txt|log)$/i.test(f.name)
                  ? '<span data-icon="file-text"></span>'
                  : '<span data-icon="file"></span>'));
        var nameCell = f.isDir
          ? '<a class="fname dir" data-click="filesEnterDir" data-args="' +
            esc(JSON.stringify([f.path])) + '">' + esc(f.name) + "</a>"
          : '<span class="fname">' + esc(f.name) + "</span>";
        var actions = "";
        if (!f.isDir) {
          actions +=
            '<a class="btn-mini" title="Download" href="/download?file=' +
            encodeURIComponent(f.path) +
            "&storage=" + currentFilesStorage + '">' +
            '<span data-icon="download"></span></a>';
        }
        if (filesEditMode) {
          if (!f.isDir) {
            actions +=
              '<button class="btn-mini" title="Move/Rename" data-click="showMovePopup" data-args="' +
              esc(JSON.stringify([f.path, f.name])) + '">' +
              '<span data-icon="pencil"></span></button>';
          }
          actions +=
            '<button class="btn-mini" title="Delete" data-click="filesDelete" data-args="' +
            esc(JSON.stringify([f.path])) + '">' +
            '<span data-icon="trash-2"></span></button>';
        }
        rows +=
          '<tr><td style="width:32px">' + icon + "</td>" +
          "<td>" + nameCell + "</td>" +
          "<td>" + (f.isDir ? "—" : fmtBytes(f.size)) + "</td>" +
          "<td>" + (f.modified ? esc(f.modified) : "") + "</td>" +
          '<td><div class="row-acts">' + actions + "</div></td></tr>";
      });
      list.innerHTML =
        '<table class="ftable">' +
        "<thead><tr><th></th><th>Name</th><th>Size</th><th>Modified</th><th></th></tr></thead>" +
        "<tbody>" + rows + "</tbody></table>";
      if (window.Icons && Icons.swap) Icons.swap(list);
    })
    .catch(function (e) {
      var list = document.getElementById("list");
      if (list)
        list.innerHTML =
          '<div style="padding:14px;color:var(--err)">Error: ' + esc(String(e)) + "</div>";
    });
}

function filesSetStorage(s) {
  currentFilesStorage = s;
  currentFilesDir = "/";
  filesRender();
}
function filesEnterDir(d) {
  currentFilesDir = d;
  filesRender();
}
function filesGoUp() {
  var p = currentFilesDir.lastIndexOf("/");
  currentFilesDir = p <= 0 ? "/" : currentFilesDir.substring(0, p);
  filesRender();
}
function filesToggleEdit() {
  filesEditMode = !filesEditMode;
  filesRender();
}

function filesDelete(path) {
  if (!confirm("Delete " + path + "?")) return;
  fetch(
    "/delete?path=" +
      encodeURIComponent(path) +
      "&storage=" +
      currentFilesStorage,
  )
    .then(function () {
      filesRender();
    })
    .catch(function (e) {
      showToast("Error: " + e, "error");
    });
}

function filesUpload() {
  var inp = document.getElementById("fileInput");
  if (!inp || !inp.files.length) return;
  var files = inp.files,
    i = 0;
  var prog = document.getElementById("uploadProg");
  var bar = document.getElementById("uploadBar");
  var pct = document.getElementById("uploadPct");
  if (prog) prog.style.display = "block";
  function next() {
    if (i >= files.length) {
      if (prog) prog.style.display = "none";
      if (bar) bar.style.width = "0%";
      inp.value = "";
      filesRender();
      return;
    }
    var fd = new FormData();
    fd.append("path", currentFilesDir);
    fd.append("storage", currentFilesStorage);
    fd.append("file", files[i]);
    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function (ev) {
      if (ev.lengthComputable) {
        var p = Math.round((ev.loaded / ev.total) * 100);
        if (bar) bar.style.width = p + "%";
        if (pct) pct.textContent = p + "%";
      }
    };
    xhr.onload = function () {
      i++;
      next();
    };
    xhr.onerror = function () {
      showToast("Upload failed: " + files[i].name, "error");
      if (prog) prog.style.display = "none";
    };
    xhr.open(
      "POST",
      "/upload?path=" +
        encodeURIComponent(currentFilesDir) +
        "&storage=" +
        encodeURIComponent(currentFilesStorage),
    );
    xhr.send(fd);
  }
  next();
}

function filesMkdir() {
  var name = document.getElementById("newFolder");
  if (!name || !name.value.trim()) return;
  fetch(
    "/mkdir?name=" +
      encodeURIComponent(name.value.trim()) +
      "&dir=" +
      encodeURIComponent(currentFilesDir) +
      "&storage=" +
      currentFilesStorage,
    { method: "POST" },
  ).then(function () {
    name.value = "";
    filesRender();
  });
}

var mvSrcPath = "";
function showMovePopup(path, name) {
  mvSrcPath = path;
  var inp = document.getElementById("name");
  if (inp) inp.value = name;
  document.getElementById("movePopup").style.display = "flex";
}
function filesApplyMove() {
  var newName = getVal("name").trim(),
    destDir = getVal("dest");
  if (!newName) return;
  var url =
    "/move_file?src=" +
    encodeURIComponent(mvSrcPath) +
    "&newName=" +
    encodeURIComponent(newName) +
    "&storage=" +
    currentFilesStorage;
  if (destDir) url += "&destDir=" + encodeURIComponent(destDir);
  fetch(url, { method: "POST" })
    .then(function () {
      document.getElementById("movePopup").style.display = "none";
      filesRender();
    })
    .catch(function (e) {
      showToast("Error: " + e, "error");
    });
}

// ============================================================================
// ══ PAGE: LIVE ══
// Matches original: function upd() polling /api/live every 500ms
//                   function updLogs() polling /api/recent_logs every 3s
// ============================================================================
function liveInit() {
  if (ST.chip) setEl("live-chip", ST.chip);
  if (ST.cpu) setEl("live-cpu", ST.cpu);
  if (ST.ip) setEl("live-ip", ST.ip);
  if (ST.network) setEl("live-net", ST.network);

  var hint = document.getElementById("stateHint");
  if (hint) {
    var fm = CFG.flowMeter || {};
    var fl = fm.firstLoopMonitoringWindowSecs || "?";
    var win = fm.monitoringWindowSecs || "?";
    hint.textContent =
      "🔧 IDLE → 🟡 WAIT_FLOW (" +
      fl +
      "s) → 🟢 MONITORING (" +
      win +
      "s idle) → Logging";
  }

  // Prefer Server-Sent Events; fall back to polling on error / unsupported.
  liveStartTransport();
  liveLogsUpdate();
  liveLogsTimer = setInterval(liveLogsUpdate, 3000);
}

function liveStartTransport() {
  // Always do one immediate fetch so the page is populated before the first
  // SSE tick (server pushes at 1 Hz).
  liveUpdate();

  if (typeof EventSource === "undefined") {
    liveStartPolling(500);
    return;
  }
  try {
    liveES = new EventSource("/api/events");
  } catch (e) {
    liveStartPolling(500);
    return;
  }
  liveES.addEventListener("live", function (ev) {
    try { liveRender(JSON.parse(ev.data)); } catch (e) {}
  });
  liveES.onerror = function () {
    // Browser will auto-retry, but surface the disconnect and degrade to
    // polling if the SSE channel never recovers.
    var conn = document.getElementById("conn");
    if (conn) {
      conn.textContent = "● Reconnecting…";
      conn.className = "text-warning";
    }
    if (!liveTimer) liveStartPolling(1000);
  };
}

function liveStartPolling(rate) {
  if (liveTimer) { clearInterval(liveTimer); }
  liveTimer = setInterval(liveUpdate, rate || 500);
}

function liveSetRate() {
    var rateEl = document.getElementById('live-refresh-rate');
    if (!rateEl) return;
    var rate = parseInt(rateEl.value, 10) || 500;
    // Manual rate override → close SSE and use polling at the chosen interval.
    if (liveES) { try { liveES.close(); } catch (e) {} liveES = null; }
    liveStartPolling(rate);
}

// Polling fallback — kept identical in shape to the original upd() so the
// liveRender() body works for both EventSource and fetch results.
function liveUpdate() {
  fetch("/api/live")
    .then(function (r) {
      return r.json();
    })
    .then(liveRender)
    .catch(function () {
      var conn = document.getElementById("conn");
      if (conn) {
        conn.textContent = "● Disconnected";
        conn.className = "text-danger";
      }
    });
}

function liveRender(d) {
  if (!d) return;
  var conn = document.getElementById("conn");
  if (conn) {
    conn.textContent = "● Connected";
    conn.className = "text-success";
  }

  setEl("live-time", d.time);
  setEl("live-trigger", d.trigger);
  setEl("live-cycleTime", d.cycleTime);
  setEl("live-pulses", d.pulses);
  setEl("live-liters", parseFloat(d.liters || 0).toFixed(2));
  setEl("live-ffCount", d.ffCount);
  setEl("live-pfCount", d.pfCount);
  setEl("live-boot", d.boot);
  setEl("live-heap", fmtBytes(d.heap));
  setEl("live-heapTotal", fmtBytes(d.heapTotal));
  setEl("live-uptime", d.uptime);
  if (d.fsTotal)
    setEl("live-storage", fmtBytes(d.fsUsed) + "/" + fmtBytes(d.fsTotal));

  var stColors = {
    IDLE: "#3498db",
    WAIT_FLOW: "#f39c12",
    MONITORING: "#27ae60",
    DONE: "#e74c3c",
  };
  var stEl = document.getElementById("state");
  if (stEl) {
    stEl.textContent = d.state;
    stEl.style.background = stColors[d.state] || "#95a5a6";
    stEl.style.color = "#fff";
  }
  var remEl = document.getElementById("stateRem");
  if (remEl)
    remEl.textContent = d.stateRemaining >= 0 ? d.stateRemaining + "s" : "-";

  liveBtn("live-ff", d.ff, "Pressed", "Released", "#27ae60", "#95a5a6");
  liveBtn("live-pf", d.pf, "Pressed", "Released", "#27ae60", "#95a5a6");
  liveBtn("live-wifi", d.wifi, "Pressed", "Released", "#3498db", "#95a5a6");

  var modeEl = document.getElementById("mode");
  if (modeEl) {
    if (d.mode === "online") modeEl.innerHTML = "🌐 Online Logger";
    else if (d.mode === "webonly") modeEl.innerHTML = "📡 Web Only";
    else modeEl.innerHTML = "📊 Logging";
  }

  if (d.time) setEl("headerTime", d.time.split(" ")[1] || d.time);
  updateFooter({ boot: d.boot, heap: d.heap, heapTotal: d.heapTotal });
}

function liveBtn(id, pressed, txtOn, txtOff, colorOn, colorOff) {
  var el = document.getElementById(id);
  if (!el) return;
  el.textContent = pressed ? txtOn : txtOff;
  el.style.background = pressed ? colorOn : colorOff;
}

// Enrol markup-reachable handlers (data-click / data-change / data-input /
// data-submit).  See core.js::Handlers for why the whitelist exists.
registerHandlers({
  dbLoadData: dbLoadData,
  dbApplyFilters: dbApplyFilters,
  dbExportCSV: dbExportCSV,
  filesSetStorage: filesSetStorage,
  filesEnterDir: filesEnterDir,
  filesGoUp: filesGoUp,
  filesToggleEdit: filesToggleEdit,
  filesDelete: filesDelete,
  filesUpload: filesUpload,
  filesMkdir: filesMkdir,
  showMovePopup: showMovePopup,
  filesApplyMove: filesApplyMove,
  liveSetRate: liveSetRate,
  liveLogsFilter: liveLogsFilter,
  liveLogsFilterClear: liveLogsFilterClear,
});

// Matches original: function updLogs()
// Phase 5c-3: caches the rendered logs in `_liveLogsCache` so the filter
// input can re-render without re-fetching.  liveLogsFilter() reads the
// cache; liveLogsFilterClear() resets the input + re-renders.
var _liveLogsCache = [];
function liveLogsUpdate() {
  fetch("/api/recent_logs")
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      _liveLogsCache = (d && d.logs) || [];
      _liveLogsRender();
    })
    .catch(function () {});
}

function _liveLogsRender() {
  var el = document.getElementById("logs");
  if (!el) return;
  var th = ST.theme || CFG.theme || {};
  var ffC = th.ffColor || "#3498db",
    pfC = th.pfColor || "#e74c3c",
    otC = th.otherColor || "#95a5a6";

  var filterEl = document.getElementById("logsFilter");
  var query = filterEl ? filterEl.value.trim().toLowerCase() : "";
  var rows = _liveLogsCache;
  if (query) {
    rows = rows.filter(function (l) {
      return (
        String(l.time).toLowerCase().indexOf(query) >= 0 ||
        String(l.trigger).toLowerCase().indexOf(query) >= 0 ||
        String(l.volume).toLowerCase().indexOf(query) >= 0 ||
        String(l.ff).toLowerCase().indexOf(query) >= 0 ||
        String(l.pf).toLowerCase().indexOf(query) >= 0
      );
    });
  }

  if (!rows.length) {
    el.innerHTML = "";
    el.appendChild(emptyState({
      icon: "activity",
      title: query ? "No matches" : "No log entries yet",
      msg: query
        ? "Try a different search term, or clear the filter."
        : "Log entries appear here after the first wakeup with flow."
    }));
    return;
  }

  var html =
    '<table style="width:100%;border-collapse:collapse;font-size:.75rem">';
  html +=
    '<tr style="background:var(--bg)"><th style="padding:6px;text-align:left">Time</th><th>Trigger</th><th>Volume</th><th>+FF</th><th>+PF</th></tr>';
  rows.forEach(function (l) {
    var color =
      l.trigger.indexOf("FF") >= 0
        ? ffC
        : l.trigger.indexOf("PF") >= 0
          ? pfC
          : otC;
    var bg = hexToRgba(color, 0.15);
    html +=
      '<tr style="background:' + bg + '">' +
      '<td style="padding:6px">' + esc(l.time) + "</td>" +
      '<td style="color:' + color + ';font-weight:bold;text-align:center">' + esc(l.trigger) + "</td>" +
      '<td style="text-align:center">' + esc(l.volume) + "</td>" +
      '<td style="text-align:center">' + esc(l.ff) + "</td>" +
      '<td style="text-align:center">' + esc(l.pf) + "</td></tr>";
  });
  html += "</table>";
  el.innerHTML = html;
}

function liveLogsFilter() { _liveLogsRender(); }
function liveLogsFilterClear() {
  var f = document.getElementById("logsFilter");
  if (f) f.value = "";
  _liveLogsRender();
}

