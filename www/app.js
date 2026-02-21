// ============================================================================
// app.js – Water Logger shared runtime
// Loaded by every HTML page. Provides: theme, nav, status bar, API helpers.
// ============================================================================

// ── API helpers ──────────────────────────────────────────────────────────────
async function api(url, opts = {}) {
  const r = await fetch(url, opts);
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}

async function postForm(url, formData) {
  const r = await fetch(url, { method: 'POST', body: formData });
  return r;
}

// ── Theme engine ─────────────────────────────────────────────────────────────
let _status = null;

async function loadStatus() {
  try {
    _status = await api('/api/status');
    applyTheme(_status);
    updateStatusBar(_status);
    markActiveNav();
  } catch (e) {
    console.warn('Status load failed:', e);
  }
  return _status;
}

function applyTheme(s) {
  if (!s) return;
  const root = document.documentElement;
  root.className = s.theme || 'theme-light';
  root.style.setProperty('--primary',   s.primary   || '#275673');
  root.style.setProperty('--secondary', s.secondary || '#2ecc71');

  // Dynamic favicon
  if (s.logoSrc) {
    let link = document.querySelector("link[rel~='icon']");
    if (!link) { link = document.createElement('link'); link.rel = 'icon'; document.head.appendChild(link); }
    link.href = s.logoSrc;
  }

  // Device name in title
  if (s.device) document.title = document.title.replace('Water Logger', s.device);
}

function updateStatusBar(s) {
  const el = document.getElementById('statusNetwork');
  if (el && s) el.textContent = s.network;
  const el2 = document.getElementById('statusTime');
  if (el2 && s) el2.textContent = s.time;
  const el3 = document.getElementById('statusDevice');
  if (el3 && s) el3.textContent = s.device;
}

// ── Storage bar helper ────────────────────────────────────────────────────────
function storageBar(used, total, percent) {
  const cls = percent >= 90 ? 'progress-bar-danger' : percent >= 70 ? 'progress-bar-warning' : 'progress-bar-success';
  return `<div class="progress"><div class="progress-bar ${cls}" style="width:${percent}%"></div></div>
          <small class="text-muted">${fmtBytes(used)} / ${fmtBytes(total)} (${percent}%)</small>`;
}

// ── Sidebar ───────────────────────────────────────────────────────────────────
function buildSidebar(s) {
  const name = s?.device || 'Water Logger';
  const logo = s?.logoSrc ? `<img src="${s.logoSrc}" onerror="this.style.display='none'" alt="">` : '';
  return `
  <aside class="sidebar">
    <div class="sidebar-header">
      ${logo}
      <span class="logo">${name}</span>
    </div>
    <nav>
      <div class="nav-section">Main</div>
      <a href="/dashboard.html" class="nav-item" data-page="dashboard">📊 <span>Dashboard</span></a>
      <a href="/files.html"     class="nav-item" data-page="files">📁 <span>Files</span></a>
      <a href="/live.html"      class="nav-item" data-page="live">📡 <span>Live</span></a>
      <div class="nav-section">System</div>
      <a href="/settings.html"  class="nav-item" data-page="settings">⚙️ <span>Settings</span></a>
    </nav>
  </aside>`;
}

function buildBottomNav() {
  return `
  <nav class="bottom-nav">
    <a href="/dashboard.html" data-page="dashboard"><span class="icon">📊</span>Home</a>
    <a href="/files.html"     data-page="files"><span class="icon">📁</span>Files</a>
    <a href="/live.html"      data-page="live"><span class="icon">📡</span>Live</a>
    <a href="/settings.html"  data-page="settings"><span class="icon">⚙️</span>Settings</a>
  </nav>`;
}

function buildHeader(s) {
  const name  = s?.device  || 'Water Logger';
  const time  = s?.time    || '--:--:--';
  const net   = s?.network || '';
  const logo  = s?.logoSrc ? `<img src="${s.logoSrc}" onerror="this.style.display='none'" alt="">` : '';
  return `
  <header class="app-header hide-desktop">
    <div class="logo">${logo}${name}</div>
    <div class="status">
      <div class="time" id="statusTime">${time}</div>
      <span id="statusNetwork">${net}</span>
    </div>
  </header>`;
}

function buildFooter(s) {
  if (!s) return '';
  return `
  <footer class="app-footer">
    <div class="footer-grid">
      <span>🔄 Boot: ${s.boot}</span>
      <span>💾 ${fmtBytes(s.heap)} / ${fmtBytes(s.heapTotal)}</span>
    </div>
    <div class="footer-row">
      <span id="statusNetwork">${s.network}</span>
      <span style="text-align:right">IP: ${s.ip}</span>
    </div>
    <div class="footer-version">Firmware: ${s.version}</div>
  </footer>`;
}

// ── Shell injection ───────────────────────────────────────────────────────────
function injectShell(s) {
  const sidebarEl = document.getElementById('sidebar');
  if (sidebarEl) sidebarEl.innerHTML = buildSidebar(s);
  const headerEl = document.getElementById('appHeader');
  if (headerEl) headerEl.innerHTML = buildHeader(s);
  const footerEl = document.getElementById('appFooter');
  if (footerEl) footerEl.innerHTML = buildFooter(s);
  const bottomEl = document.getElementById('bottomNav');
  if (bottomEl) bottomEl.innerHTML = buildBottomNav();
  markActiveNav();
}

function markActiveNav() {
  const path = location.pathname.replace('/','').replace('.html','') || 'dashboard';
  document.querySelectorAll('[data-page]').forEach(el => {
    el.classList.toggle('active', el.dataset.page === path);
  });
}

// ── Formatters ────────────────────────────────────────────────────────────────
function fmtBytes(b) {
  if (b === undefined || b === null) return '?';
  if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GB';
  if (b >= 1048576)    return (b / 1048576).toFixed(1)    + ' MB';
  if (b >= 1024)       return (b / 1024).toFixed(1)       + ' KB';
  return b + ' B';
}

function hexToRgba(hex, a) {
  const r = parseInt(hex.slice(1,3),16),
        g = parseInt(hex.slice(3,5),16),
        b = parseInt(hex.slice(5,7),16);
  return `rgba(${r},${g},${b},${a})`;
}

// ── Page bootstrap ────────────────────────────────────────────────────────────
// Each page calls: initPage(callback) which loads status then calls callback(status)
async function initPage(cb) {
  const s = await loadStatus();
  injectShell(s);
  if (cb) cb(s);
}

// ── Restart helper ────────────────────────────────────────────────────────────
function restartDevice(msg = 'Restarting...') {
  if (!confirm('Restart device now?')) return;
  fetch('/restart').catch(() => {});
  let sec = 6;
  const toast = document.createElement('div');
  toast.style.cssText = 'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);background:#fff;padding:2rem;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,0.2);text-align:center;z-index:9999';
  toast.innerHTML = `<div style="font-size:3rem">🔄</div><div style="font-weight:bold;margin:1rem 0">${msg}</div><div id="_rsec">${sec}s</div>`;
  document.body.appendChild(toast);
  const t = setInterval(() => {
    sec--;
    const el = document.getElementById('_rsec');
    if (el) el.textContent = sec + 's';
    if (sec <= 0) { clearInterval(t); location.href = '/'; }
  }, 1000);
}

// ── Changelog loader ─────────────────────────────────────────────────────────
function loadChangelog(targetId) {
  const el = document.getElementById(targetId);
  if (!el || el.dataset.loaded) return;
  fetch('/api/changelog')
    .then(r => r.ok ? r.text() : Promise.reject('Not found'))
    .then(txt => {
      let html = '', inVer = false;
      txt.trim().split('\n').forEach((line, i) => {
        line = line.trim(); if (!line) return;
        if (line.startsWith('##')) {
          if (inVer) html += '</ul></div>';
          const isCurrent = i < 3;
          html += `<div style="margin-top:.5rem;padding:.5rem;${isCurrent?'background:var(--primary);color:#fff':'background:var(--bg)'};border-radius:4px"><strong>${line.substring(2).trim()}</strong><ul style="margin:.5rem 0 0 1rem;padding:0;font-size:.9rem">`;
          inVer = true;
        } else if (line.startsWith('-') && inVer) {
          html += `<li>${line.substring(1).trim()}</li>`;
        }
      });
      if (inVer) html += '</ul></div>';
      el.innerHTML = html;
      el.dataset.loaded = '1';
    })
    .catch(() => { el.innerHTML = '<p class="text-muted">Changelog not found.</p>'; el.dataset.loaded = '1'; });
}
