// ESP32 Logger — redesign prototype
(function () {
  // ── Tweaks state ──
  const root = document.documentElement;
  const apply = (k, v) => {
    if (k === 'theme') root.dataset.theme = v;
    if (k === 'nav') root.dataset.nav = v;
    if (k === 'density') root.dataset.density = v;
    if (k === 'accent') root.dataset.accent = v;
    TWEAKS[k] = v;
    persist();
  };
  function persist() {
    try { localStorage.setItem('esp32-tweaks', JSON.stringify(TWEAKS)); } catch (e) {}
    try { window.parent.postMessage({ type: '__edit_mode_set_keys', edits: TWEAKS }, '*'); } catch (e) {}
  }
  function loadStored() {
    try {
      const s = JSON.parse(localStorage.getItem('esp32-tweaks') || 'null');
      if (s) Object.assign(TWEAKS, s);
    } catch (e) {}
  }
  loadStored();
  Object.entries(TWEAKS).forEach(([k, v]) => apply(k, v));

  // Tweaks UI sync
  function setupSeg(id, key) {
    const seg = document.getElementById(id);
    if (!seg) return;
    seg.querySelectorAll('button').forEach(b => {
      if (b.dataset.v === TWEAKS[key]) b.classList.add('active');
      b.onclick = () => {
        seg.querySelectorAll('button').forEach(x => {
          x.classList.remove('active');
          x.setAttribute('aria-pressed', 'false');
        });
        b.classList.add('active');
        b.setAttribute('aria-pressed', 'true');
        apply(key, b.dataset.v);
      };
    });
  }
  setupSeg('tw-theme', 'theme');
  setupSeg('tw-nav', 'nav');
  setupSeg('tw-density', 'density');
  setupSeg('tw-accent', 'accent');

  // Theme button (header)
  document.getElementById('themeBtn').onclick = () => {
    apply('theme', TWEAKS.theme === 'dark' ? 'light' : 'dark');
    document.querySelectorAll('#tw-theme button').forEach(b => b.classList.toggle('active', b.dataset.v === TWEAKS.theme));
  };

  // ── View toggle (audit / proto) ──
  document.querySelectorAll('.ftab').forEach(t => {
    t.onclick = () => {
      document.querySelectorAll('.ftab').forEach(x => x.classList.remove('active'));
      t.classList.add('active');
      const v = t.dataset.view;
      document.getElementById('view-proto').style.display = v === 'proto' ? '' : 'none';
      document.getElementById('view-audit').style.display = v === 'audit' ? '' : 'none';
    };
  });

  // ── Nav (sidebar/top/bottom) ──
  function bindNav(sel) {
    document.querySelectorAll(sel).forEach(n => {
      n.onclick = () => {
        const page = n.dataset.page;
        document.querySelectorAll('.nav-item, .bnav').forEach(x => {
          const active = x.dataset.page === page;
          x.classList.toggle('active', active);
          if (active) x.setAttribute('aria-current', 'page');
          else x.removeAttribute('aria-current');
        });
        document.querySelectorAll('.page').forEach(p => {
          p.classList.toggle('active', p.dataset.page === page);
        });
        // re-run a11y on page switch
        setTimeout(applyA11y, 50);
      };
    });
  }
  bindNav('.nav-item');
  bindNav('.bnav');

  // ── Live clock ──
  const clk = document.getElementById('topClock');
  function tick() {
    const d = new Date();
    clk.textContent = d.toLocaleTimeString('en-GB');
  }
  tick(); setInterval(tick, 1000);

  // ── Dashboard chart (synthetic 7-day) ──
  function buildChart() {
    const wrap = document.getElementById('chartWrap');
    if (!wrap) return;
    const bars = document.getElementById('chartBars');
    const data = [
      24, 18, 32, 14, 45, 22, 28, 38, 15, 42, 26, 31, 19, 34,
      29, 41, 23, 37, 16, 49, 28, 33, 21, 39, 25, 44, 17, 36
    ];
    const w = 700, h = 220, n = data.length;
    const bw = (w - 20) / n;
    const max = Math.max(...data) * 1.2;
    let bhtml = '';
    let pts = [];
    data.forEach((v, i) => {
      const x = 10 + i * bw;
      const bh = (v / max) * (h - 20);
      const y = h - bh - 10;
      const color = i % 3 === 0 ? 'var(--accent)' : i % 3 === 1 ? 'var(--accent-2)' : 'var(--text-4)';
      bhtml += `<rect x="${x + 1}" y="${y}" width="${bw - 2}" height="${bh}" fill="${color}" opacity="${i % 3 === 0 ? 0.85 : 0.45}" rx="1"/>`;
      pts.push([x + bw / 2, y]);
    });
    bars.innerHTML = bhtml;
    // smoothed area path through tops
    let pathD = `M ${pts[0][0]},${h - 10} `;
    pts.forEach(p => pathD += `L ${p[0]},${p[1]} `);
    pathD += `L ${pts[pts.length - 1][0]},${h - 10} Z`;
    document.getElementById('areaPath').setAttribute('d', pathD);
    // axis labels
    const ax = document.getElementById('chartAxis');
    const days = ['Mon 11', 'Tue 12', 'Wed 13', 'Thu 14', 'Fri 15', 'Sat 16', 'Sun 17', 'Mon 18'];
    ax.innerHTML = days.map(d => `<span>${d}</span>`).join('');
  }
  buildChart();

  // ── Recent events ──
  const events = [
    { t: '14:32:08', tag: 'FF', vol: '6.2 L', dur: '14s', extra: '+1' },
    { t: '14:28:41', tag: 'PF', vol: '1.8 L', dur: '6s', extra: '' },
    { t: '14:14:02', tag: 'PF', vol: '2.1 L', dur: '7s', extra: '' },
    { t: '13:57:19', tag: 'FF', vol: '5.8 L', dur: '12s', extra: '' },
    { t: '13:42:55', tag: 'PF', vol: '0.9 L', dur: '4s', extra: '' },
    { t: '13:18:33', tag: 'PF', vol: '2.4 L', dur: '8s', extra: '+1' },
    { t: '12:47:11', tag: 'FF', vol: '6.0 L', dur: '13s', extra: '' },
  ];
  const evList = document.getElementById('evList');
  if (evList) {
    evList.innerHTML = events.map(e => `
      <div class="ev">
        <div class="ev-tag ${e.tag.toLowerCase()}">${e.tag}</div>
        <div class="ev-meta">
          <div class="l1">${e.tag === 'FF' ? 'Full Flush' : 'Part Flush'} ${e.extra ? `<span class="badge warn" style="margin-left:6px">corrected ${e.extra}</span>` : ''}</div>
          <div class="l2">${e.t} · ${e.dur}</div>
        </div>
        <div class="ev-vol">${e.vol}</div>
      </div>
    `).join('');
  }

  // ── Live flow chart (animated) ──
  let flowData = Array(80).fill(0);
  function pushFlow() {
    const v = 8 + Math.sin(Date.now() / 400) * 3 + Math.random() * 2;
    flowData.push(v); flowData.shift();
    const w = 600, h = 80, max = 14;
    const pts = flowData.map((v, i) => `${(i / (flowData.length - 1)) * w},${h - (v / max) * (h - 8) - 4}`).join(' ');
    const fp = document.getElementById('flowPath');
    if (fp) fp.setAttribute('points', pts);
    const ff = document.getElementById('flowFill');
    if (ff) ff.setAttribute('d', `M0,${h} L ${pts.split(' ').join(' L ')} L${w},${h} Z`);
    const fr = document.getElementById('flowRate');
    if (fr) fr.textContent = v.toFixed(1);
  }
  setInterval(pushFlow, 250);

  // ── Sensor chart (sinusoidal) ──
  function sensorChart() {
    const w = 700, h = 200;
    const pts = [];
    for (let i = 0; i <= 100; i++) {
      const x = (i / 100) * w;
      const y = h / 2 + Math.sin(i / 8) * 40 + Math.cos(i / 3) * 12 + (Math.random() - 0.5) * 8;
      pts.push(`${x.toFixed(1)},${y.toFixed(1)}`);
    }
    const sp = document.getElementById('sensorPath');
    if (sp) sp.setAttribute('points', pts.join(' '));
    const pts2 = [];
    for (let i = 0; i <= 100; i++) {
      const x = (i / 100) * w;
      const y = h / 2 + Math.sin(i / 6 + 2) * 30 - 10;
      pts2.push(`${x.toFixed(1)},${y.toFixed(1)}`);
    }
    const sp2 = document.getElementById('sensorPath2');
    if (sp2) sp2.setAttribute('points', pts2.join(' '));
  }
  sensorChart();

  // ── Logs feed ──
  const logFeed = document.getElementById('logFeed');
  const logLines = [
    ['14:32:08.214', 'info', 'FF_BTN pressed (D1) ⇢ enter MEASURE'],
    ['14:32:08.198', 'ok',   'WAKE → ARM transition (3 ms)'],
    ['14:32:07.640', 'info', 'Pulse detected on D6 · cycle #1428'],
    ['14:31:54.012', 'warn', 'PF→FF correction applied (2.4 L > 2.0 L)'],
    ['14:31:42.880', 'info', 'Storage write OK · /logs/2026-04-18.jsonl (+218 B)'],
    ['14:31:42.671', 'ok',   'Export queued · MQTT home/water/event'],
    ['14:30:11.005', 'err',  'SCD4x I2C ack failed @ 0x62 (retry 2/3)'],
    ['14:29:58.412', 'info', 'BME280 read · 22.4°C / 47.2% / 1013.4 hPa'],
    ['14:29:46.221', 'info', 'Sensor sweep complete · 7 of 8 OK'],
    ['14:29:34.117', 'ok',   'WiFi connected · -54 dBm · 192.168.1.42'],
  ];
  if (logFeed) {
    logFeed.innerHTML = logLines.map(([t, lv, m]) =>
      `<div class="line"><span class="t">${t}</span><span class="lv ${lv}">${lv.toUpperCase()}</span><span class="msg">${m}</span></div>`
    ).join('');
  }

  // ── Sensors grid ──
  const sensors = [
    { id: 'env_indoor',   name: 'BME280',    icon: 'thermometer',  val: '22.4', unit: '°C', metrics: ['Humidity 47.2%','Pressure 1013.4 hPa'], status: 'ok',  iface: 'I2C 0x76' },
    { id: 'air_quality',  name: 'SGP30',     icon: 'wind',         val: '412',  unit: 'ppm', metrics: ['TVOC 87 ppb','eCO₂ 412 ppm'], status: 'ok', iface: 'I2C 0x58' },
    { id: 'pm_outdoor',   name: 'SDS011',    icon: 'cloud-fog',    val: '12.4', unit: 'µg/m³', metrics: ['PM2.5 12.4','PM10 18.2'], status: 'ok', iface: 'UART1' },
    { id: 'rain_gauge',   name: 'Rain',      icon: 'cloud-rain',   val: '0.0', unit: 'mm/h', metrics: ['Total 4.6 mm'], status: 'ok', iface: 'GPIO 7' },
    { id: 'wind',         name: 'Wind',      icon: 'wind',         val: '3.2', unit: 'm/s', metrics: ['Gust 5.8 m/s'], status: 'ok', iface: 'GPIO 8' },
    { id: 'flow_main',    name: 'YF-S201',   icon: 'droplets',     val: '0.0', unit: 'L/min', metrics: ['Total 142.6 L'], status: 'ok', iface: 'GPIO 21' },
    { id: 'co2_sensor',   name: 'SCD4x',     icon: 'leaf',         val: '—',   unit: '', metrics: ['I2C ack failed'], status: 'err', iface: 'I2C 0x62' },
    { id: 'soil_dry',     name: 'Soil',      icon: 'sprout',       val: '34', unit: '%', metrics: ['Calibrated'], status: 'dis', iface: 'ADC 3' },
  ];
  const sg = document.getElementById('sensorGrid');
  if (sg) {
    sg.innerHTML = sensors.map(s => `
      <div class="sensor ${s.status === 'err' ? 'err' : s.status === 'dis' ? 'dis' : ''}">
        <div class="s-head">
          <div>
            <div class="s-name"><i data-lucide="${s.icon}"></i> ${s.name}</div>
            <div class="s-id">${s.id} · ${s.iface}</div>
          </div>
          <span class="badge ${s.status === 'ok' ? 'ok' : s.status === 'err' ? 'err' : 'dim'}">${s.status === 'ok' ? 'OK' : s.status === 'err' ? 'ERR' : 'OFF'}</span>
        </div>
        <div class="s-val"><span class="n">${s.val}</span><span class="u">${s.unit}</span></div>
        <svg class="s-spark" viewBox="0 0 100 36" preserveAspectRatio="none">
          <polyline points="${Array.from({length:30},(_,i)=>`${i*3.4},${18+Math.sin(i/2+s.id.length)*10+Math.random()*4}`).join(' ')}" fill="none" stroke="currentColor" stroke-width="1.4"/>
        </svg>
        <div class="s-metrics">${s.metrics.map(m => `<span class="badge dim">${m}</span>`).join('')}</div>
        <div class="s-foot"><span>last 0.4s ago</span><span>${s.iface}</span></div>
      </div>
    `).join('');
  }

  // ── Files ──
  const files = [
    { name: 'logs', kind: 'dir', size: '—', date: '18 Apr 14:30' },
    { name: 'config', kind: 'dir', size: '—', date: '14 Apr 03:00' },
    { name: '2026-04-18.jsonl', kind: 'jsonl', size: '218 KB', date: '18 Apr 14:32' },
    { name: '2026-04-17.jsonl', kind: 'jsonl', size: '412 KB', date: '17 Apr 23:59' },
    { name: '2026-04-16.jsonl', kind: 'jsonl', size: '386 KB', date: '16 Apr 23:59' },
    { name: '2026-04-15.jsonl', kind: 'jsonl', size: '298 KB', date: '15 Apr 23:59' },
    { name: 'platform_config.json', kind: 'json', size: '8.8 KB', date: '12 Apr 09:14' },
    { name: 'index.html.gz', kind: 'gz', size: '34.1 KB', date: '12 Apr 09:14' },
    { name: 'web.js.gz', kind: 'gz', size: '38.4 KB', date: '12 Apr 09:14' },
    { name: 'style.css.gz', kind: 'gz', size: '4.2 KB', date: '12 Apr 09:14' },
  ];
  const fb = document.getElementById('fileBody');
  if (fb) {
    const iconFor = k => ({ dir: 'folder', jsonl: 'file-text', json: 'braces', gz: 'file-archive' }[k] || 'file');
    fb.innerHTML = files.map(f => `
      <tr>
        <td style="width:32px"><i data-lucide="${iconFor(f.kind)}"></i></td>
        <td><div class="fname ${f.kind === 'dir' ? 'dir' : ''}">${f.name}</div></td>
        <td>${f.size}</td>
        <td>${f.date}</td>
        <td>
          <div class="row-acts">
            <button class="btn-mini" title="Download"><i data-lucide="download"></i></button>
            <button class="btn-mini" title="Rename"><i data-lucide="pencil"></i></button>
            <button class="btn-mini" title="Delete"><i data-lucide="trash-2"></i></button>
          </div>
        </td>
      </tr>
    `).join('');
  }

  // ── Settings hub ──
  const setCards = [
    ['device', 'Device', 'Name, ID, default storage view', 'smartphone'],
    ['flowmeter', 'Flow meter', 'Calibration, timing, test mode', 'droplets'],
    ['hardware', 'Hardware', 'Pins, RTC, SD, CPU frequency', 'cpu'],
    ['datalog', 'Data log', 'Rotation, format, retention', 'database'],
    ['corelogic', 'Core Logic', 'Operating mode, sensors, aggregation', 'puzzle'],
    ['export', 'Export', 'MQTT, HTTP, openSenseMap, SC', 'cloud-upload'],
    ['theme', 'Theme', 'Colors, typography, density', 'palette'],
    ['network', 'Network', 'WiFi, AP, captive portal', 'wifi'],
    ['time', 'Time sync', 'NTP, RTC, timezone', 'clock'],
    ['files', 'Files', 'Manage device storage', 'folder'],
  ];
  const sgr = document.getElementById('settingsGrid');
  if (sgr) {
    sgr.innerHTML = setCards.map(([id, t, d, ic]) => `
      <div class="set-card">
        <div class="set-card-icon"><i data-lucide="${ic}"></i></div>
        <div class="set-card-t">${t}</div>
        <div class="set-card-d">${d}</div>
      </div>
    `).join('');
  }

  // ── Core logic sensors table ──
  const clRows = [
    ['ok', 'env_indoor', 'BME280', 'I2C 0x76', '10s', 'temp · humidity · pressure'],
    ['ok', 'air_quality', 'SGP30', 'I2C 0x58', '15s', 'tvoc · eco2'],
    ['ok', 'pm_outdoor', 'SDS011', 'UART1', '120s', 'pm2.5 · pm10'],
    ['ok', 'rain_gauge', 'RainSensor', 'GPIO 7 (ISR)', 'event', 'rain_rate · rain_total'],
    ['ok', 'wind', 'WindSensor', 'GPIO 8 (ISR)', '5s', 'wind_speed'],
    ['ok', 'flow_main', 'YF-S201', 'GPIO 21 (ISR)', 'event', 'flow_rate · volume'],
    ['err', 'co2_sensor', 'SCD4x', 'I2C 0x62', '30s', 'co2 · temp · humidity'],
    ['dis', 'soil_dry', 'SoilMoisture', 'ADC 3', '60s', 'moisture'],
  ];
  const clBody = document.querySelector('#clSensors tbody');
  if (clBody) {
    clBody.innerHTML = clRows.map(([s, id, type, iface, intv, metrics]) => `
      <tr>
        <td><span class="badge ${s === 'ok' ? 'ok' : s === 'err' ? 'err' : 'dim'}">${s.toUpperCase()}</span></td>
        <td class="mono" style="font-size:11.5px">${id}</td>
        <td>${type}</td>
        <td class="mono" style="font-size:11.5px;color:var(--text-3)">${iface}</td>
        <td class="mono">${intv}</td>
        <td style="color:var(--text-3);font-size:11.5px">${metrics}</td>
        <td><div class="row-acts"><button class="btn-mini"><i data-lucide="pencil"></i></button><button class="btn-mini"><i data-lucide="trash-2"></i></button></div></td>
      </tr>
    `).join('');
  }

  // ── AUDIT content ──
  const audit = [
    {
      group: 'Critical',
      items: [
        { id: 'A1', title: 'Single 90 KB index.html with 25+ pages inline', body: 'The entire SPA — every form, every settings subpage — is one HTML document. Editing one screen risks breaking all of them. The file is harder to diff than the firmware that serves it.', rec: 'Split per route and inline at build time, or load partials over fetch. Keep the failsafe single-file fallback.' },
        { id: 'A2', title: 'Two duplicate <code>id="db-error"</code> elements on the dashboard', body: 'Lines 113 and 115 of <code>index.html</code> both declare <code>id="db-error"</code>. Browsers ignore the second; whichever element <code>web.js</code> targets first wins, silently. Classic copy-paste bug.', rec: 'Remove the duplicate. Add an HTML lint step to CI.' },
      ]
    },
    {
      group: 'High',
      items: [
        { id: 'B1', title: 'Emoji-as-icons everywhere', body: 'Navigation, buttons, badges and headings rely on system emoji (🌡️ 📊 ⚙️). They render differently on iOS, Android, Windows and the captive-portal browser. Some are plain symbols (▼) styled as chevrons — they cannot be tinted with CSS <code>color</code>.', rec: 'Adopt a real icon set (Lucide / Material / Tabler SVG sprites). 2 KB inline sprite covers every glyph in the UI.' },
        { id: 'B2', title: 'Sidebar + bottom nav + header + page header — four chrome layers', body: 'Desktop renders the sidebar plus the page-header H1 plus a status pill row plus, on narrower widths, the mobile header. The actual content area gets ~55% of the viewport.', rec: 'Pick one nav surface per breakpoint. Move device status into a single persistent topbar; demote the H1 since the active sidebar item already names the page.' },
        { id: 'B3', title: 'No information density — gigantic cards everywhere', body: '<code>--radius:12px</code>, <code>1rem</code> padding, <code>1.5rem</code> page padding, generous margins between every card. A 24-inch monitor shows the same 4 KPIs as a phone. There is no compact mode for power users actually monitoring this thing.', rec: 'Add a density toggle. Default to a tighter grid on desktop (12-col, 14 px gutters, 6 px radii).' },
        { id: 'B4', title: 'No tabular numerics anywhere — values jiggle when they update', body: 'Live values like <code>0.00 → 12.4</code> shift left/right because the default font has proportional digits. On a polling dashboard this looks broken.', rec: 'Use <code>font-variant-numeric: tabular-nums</code> on every numeric. Pair with a real mono font for raw telemetry.' },
        { id: 'B5', title: 'Inline styles for everything custom', body: 'Hundreds of <code>style="display:flex;gap:.5rem;..."</code> attributes throughout <code>index.html</code>. New colors, new spacings, new font sizes appear on a per-element basis with no system.', rec: 'Define utility classes or replace ad-hoc styling with composable components.' },
      ]
    },
    {
      group: 'Medium',
      items: [
        { id: 'C1', title: 'Color palette is one teal blob', body: 'Primary, accent and chart-FF are all variations of the same teal (<code>#275673</code>, <code>#3182ce</code>, <code>#7eb0d5</code>). State machine, badges and chart legend rely on color contrast that mostly does not exist.', rec: 'One brand accent. State colors (ok/warn/err) should be perceptually distinct, not just hue-shifted teal.' },
        { id: 'C2', title: 'Native <code>alert()</code> for user feedback', body: 'Theme save, settings import, OTA upload — all surface errors via <code>alert()</code>. Acknowledged in the existing audit but still in the codebase.', rec: 'Toast component already half-built in <code>style.css</code>; finish wiring it.' },
        { id: 'C3', title: 'State machine is invisible when stopped', body: 'Live page shows current state as a colored chip with a 2-line label. The pipeline diagram from <code>README.md</code> is more informative than the actual UI.', rec: 'Render the state pipeline as a 5-step horizontal track with done/active/pending styling. The redesign shows it.' },
        { id: 'C4', title: 'Filters live in their own card, not on the chart', body: 'Date range, trigger filter, threshold and "exclude 0.00 L" each occupy a row in a dedicated card. By the time you scroll back to the chart, you have forgotten the filter values.', rec: 'Inline the time-range as a segmented control on the chart card. Keep the date range for power users in a popover.' },
        { id: 'C5', title: 'Sensor cards have no signal of "is this fresh?"', body: '<code>.sensor-card</code> shows the current value and a status badge but no timestamp, sparkline or staleness indicator. A frozen sensor looks identical to a healthy one.', rec: 'Add "last update" timestamp + a 24-point sparkline + an err-state border for cards over their <code>read_interval_ms</code> threshold.' },
      ]
    },
    {
      group: 'Low',
      items: [
        { id: 'D1', title: 'Light + dark themes share the same chart grid color', body: 'Chart.js uses default gridlines; in dark mode they are nearly invisible.', rec: 'Theme tokens for chart axes, gridlines, tooltip background.' },
        { id: 'D2', title: 'Form labels are plain text, hints are below the input', body: 'Every settings page has the same pattern: bold label, input, gray hint. The hint usually contains the most important info (default value, valid range). It is the smallest text on the page.', rec: 'Hint above input, secondary; keep label as the primary anchor. Or use a placeholder with a real default.' },
      ]
    }
  ];
  const ab = document.getElementById('auditBody');
  if (ab) {
    ab.innerHTML = `
      <div style="font-size:13px;color:var(--text-2);margin-bottom:18px;line-height:1.6">
        <p>Walked the SPA via the source — <code>www/index.html</code>, <code>www/style.css</code>, <code>www/web.js</code>. Firmware-side concerns are out of scope; the existing <code>audit_webui.md</code> covers those.</p>
        <p style="margin-top:8px">The redesign in the other tab is one possible response to the issues below — neutral slate panels, single accent, density toggle, real icons, tabular numerics on every value. It does not change a single API contract.</p>
      </div>
    ` + audit.map(g => `
      <div class="audit-section">
        <h2>${g.group} <span class="mono" style="color:var(--text-4);font-weight:500">· ${g.items.length}</span></h2>
        ${g.items.map(it => `
          <div class="issue">
            <div class="issue-meta">
              <span class="issue-id">${it.id}</span>
              <span class="badge ${g.group === 'Critical' ? 'err' : g.group === 'High' ? 'warn' : g.group === 'Medium' ? 'acc' : 'dim'}">${g.group}</span>
            </div>
            <div>
              <div class="issue-title">${it.title}</div>
              <div class="issue-body">${it.body}</div>
              <div class="issue-rec">${it.rec}</div>
            </div>
          </div>
        `).join('')}
      </div>
    `).join('') + `
      <div class="audit-section">
        <h2>Strengths · keep</h2>
        <ul style="font-size:13px;color:var(--text-2);line-height:1.7;padding-left:18px">
          <li><strong>Clean URL structure</strong> — hash-based SPA with predictable <code>#dashboard</code>, <code>#settings_corelogic</code> routes.</li>
          <li><strong>Failsafe recovery page</strong> — graceful degradation when <code>/www/index.html</code> is missing.</li>
          <li><strong>LTTB aggregation surfaced in the UI</strong> — power users can see and tune the visualisation pipeline.</li>
          <li><strong>Plugin-aware sensor architecture mirrored in the UI</strong> — Core Logic page maps 1:1 to <code>platform_config.json</code>.</li>
          <li><strong>Sensible separation of legacy / continuous / hybrid modes</strong> — UI does not hide complexity.</li>
        </ul>
      </div>
    `;
    if (window.lucide) lucide.createIcons();
  }

  // ── WCAG 2.2: Apply ARIA + label associations after DOM is ready ──
  function applyA11y() {
    // 1. Pair every .field label with its control by position (no text matching)
    let fIdx = 0;
    document.querySelectorAll('.field').forEach(field => {
      const lbl  = field.querySelector('label');
      const ctrl = field.querySelector('input,select,textarea');
      if (!lbl || !ctrl) return;
      if (!ctrl.id) ctrl.id = 'fc-' + (fIdx++);
      lbl.setAttribute('for', ctrl.id);
    });

    // 2. Storage progress bars
    const storageLabels = ['SD Card storage','LittleFS storage','Export queue'];
    document.querySelectorAll('.storage-bar').forEach((bar, i) => {
      const span = bar.querySelector('span');
      const pct  = span ? parseInt(span.style.width) || 0 : 0;
      bar.setAttribute('role','progressbar');
      bar.setAttribute('aria-valuenow', pct);
      bar.setAttribute('aria-valuemin','0');
      bar.setAttribute('aria-valuemax','100');
      bar.setAttribute('aria-label', (storageLabels[i] || 'Usage') + ': ' + pct + '%');
    });

    // 3. KPI bar — decorative
    document.querySelectorAll('.kpi .bar').forEach(bar => bar.setAttribute('role','presentation'));

    // 4. Segmented controls
    document.querySelectorAll('.seg').forEach(seg => {
      if (!seg.getAttribute('role')) seg.setAttribute('role','group');
      seg.querySelectorAll('button').forEach(b =>
        b.setAttribute('aria-pressed', b.classList.contains('active') ? 'true' : 'false')
      );
    });

    // 5. Nav aria-current
    document.querySelectorAll('.nav-item, .bnav').forEach(n => {
      if (n.classList.contains('active')) n.setAttribute('aria-current','page');
      else n.removeAttribute('aria-current');
    });

    // 6. Live badge
    const lb = document.querySelector('.badge.pulse');
    if (lb) { lb.setAttribute('role','status'); lb.setAttribute('aria-live','polite'); }

    // 7. btn-mini — infer aria-label from SVG class
    document.querySelectorAll('.btn-mini:not([aria-label])').forEach(b => {
      const cls = (b.querySelector('svg') || {}).getAttribute?.('class') || '';
      const map = {download:'Download', pencil:'Edit', trash:'Delete',
                   copy:'Copy', 'more-horizontal':'More options',
                   'chevron-left':'Go up', rotate:'Reset', search:'Zoom'};
      for (const [k, v] of Object.entries(map)) {
        if (cls.includes(k)) { b.setAttribute('aria-label', v); break; }
      }
    });

    // 8. Toggle switches
    document.querySelectorAll('.toggle-row').forEach(row => {
      const text = row.querySelector('span:first-child')?.textContent?.trim();
      const sw   = row.querySelector('input[type=checkbox]');
      if (text && sw && !sw.getAttribute('aria-label')) sw.setAttribute('aria-label', text);
    });

    // 9. Mode radio cards
    document.querySelectorAll('.mode-card').forEach(card => {
      const radio = card.querySelector('input[type=radio]');
      const title = card.querySelector('.mode-h')?.textContent?.trim();
      if (radio && title && !radio.getAttribute('aria-label')) radio.setAttribute('aria-label', title);
    });

    // 10. File table actions
    const fileActions = ['Download','Rename','Delete'];
    document.querySelectorAll('.ftable .row-acts button').forEach((b, i) => {
      if (!b.getAttribute('aria-label')) b.setAttribute('aria-label', fileActions[i % 3]);
    });
  }

  setTimeout(applyA11y, 200);
  window.addEventListener('message', e => {
    const t = e.data && e.data.type;
    if (t === '__activate_edit_mode')   document.getElementById('tweaks').style.display = '';
    if (t === '__deactivate_edit_mode') document.getElementById('tweaks').style.display = 'none';
  });
  try { window.parent.postMessage({ type: '__edit_mode_available' }, '*'); } catch (e) {}

  // ── Render lucide icons (twice — after dynamic content) ──
  if (window.lucide) lucide.createIcons();
  setTimeout(() => window.lucide && lucide.createIcons(), 100);

  // ══════════════════════════════════════════════
  // 1. TOAST NOTIFICATION SYSTEM
  // ══════════════════════════════════════════════
  const rack = document.createElement('div');
  rack.className = 'toast-rack';
  rack.setAttribute('aria-live', 'polite');
  rack.setAttribute('aria-atomic', 'false');
  document.body.appendChild(rack);

  window.showToast = function(title, msg='', type='info', duration=4000) {
    const icons = { ok:'check-circle', warn:'alert-triangle', err:'x-circle', info:'info' };
    const t = document.createElement('div');
    t.className = `toast ${type}`;
    t.setAttribute('role', 'alert');
    t.innerHTML = `
      <i class="toast-icon" data-lucide="${icons[type]||'info'}"></i>
      <div class="toast-body">
        <div class="toast-title">${title}</div>
        ${msg ? `<div class="toast-msg">${msg}</div>` : ''}
      </div>
      <button class="toast-close" aria-label="Dismiss notification"><i data-lucide="x"></i></button>
      <div class="bar-prog" style="width:100%"></div>
    `;
    rack.appendChild(t);
    if (window.lucide) lucide.createIcons({ nodes: [t] });

    // Progress bar countdown
    const bar = t.querySelector('.bar-prog');
    const start = Date.now();
    const tick = () => {
      const pct = Math.max(0, 100 - ((Date.now()-start)/duration)*100);
      bar.style.width = pct + '%';
      if (pct > 0) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);

    const dismiss = () => {
      t.classList.add('out');
      t.addEventListener('animationend', () => t.remove(), { once:true });
    };
    t.querySelector('.toast-close').onclick = dismiss;
    setTimeout(dismiss, duration);
    return dismiss;
  };

  // Demo toasts on load
  setTimeout(() => showToast('Sensors loaded', '8 plugins registered, 1 error', 'warn', 5000), 800);
  setTimeout(() => showToast('MQTT connected', '192.168.1.10:1883 · QoS 0', 'ok', 4000), 1800);

  // ══════════════════════════════════════════════
  // 2. SENSOR STALENESS
  // ══════════════════════════════════════════════
  const staleThresholds = {
    env_indoor:5, air_quality:8, pm_outdoor:60, rain_gauge:999,
    wind:8, flow_main:999, co2_sensor:999, soil_dry:999
  };
  const sensorAges = {
    env_indoor:0.4, air_quality:0.4, pm_outdoor:1.2, rain_gauge:4.2,
    wind:0.4, flow_main:0.0, co2_sensor:45, soil_dry:0
  };

  function updateStaleness() {
    document.querySelectorAll('.sensor[data-sid]').forEach(card => {
      const sid = card.dataset.sid;
      let age = sensorAges[sid] || 0;
      const thresh = (staleThresholds[sid] || 30) * 2;
      const ageEl = card.querySelector('.s-age');
      if (!ageEl) return;

      card.classList.remove('stale','dead','fresh');
      if (sid === 'co2_sensor') {
        card.classList.add('dead');
        ageEl.innerHTML = `<i data-lucide="alert-circle"></i> error`;
      } else if (age > thresh) {
        card.classList.add('stale');
        ageEl.innerHTML = `<i data-lucide="clock"></i> ${age.toFixed(1)}s ago`;
      } else {
        card.classList.add('fresh');
        ageEl.innerHTML = `<i data-lucide="check"></i> ${age < 1 ? '<1s' : age.toFixed(1)+'s'} ago`;
      }
      if (window.lucide) lucide.createIcons({ nodes:[ageEl] });
    });
  }

  // Patch sensor card HTML to add data-sid + s-age + s-foot
  const origSensorGrid = document.getElementById('sensorGrid');
  if (origSensorGrid) {
    const sensors2 = [
      { id:'env_indoor', name:'BME280', icon:'thermometer', val:'22.4', unit:'°C', metrics:['Humidity 47.2%','Pressure 1013.4 hPa'], status:'ok', iface:'I2C 0x76' },
      { id:'air_quality', name:'SGP30', icon:'wind', val:'412', unit:'ppm', metrics:['TVOC 87 ppb','eCO₂ 412 ppm'], status:'ok', iface:'I2C 0x58' },
      { id:'pm_outdoor', name:'SDS011', icon:'cloud-fog', val:'12.4', unit:'µg/m³', metrics:['PM2.5 12.4','PM10 18.2'], status:'ok', iface:'UART1' },
      { id:'rain_gauge', name:'Rain', icon:'cloud-rain', val:'0.0', unit:'mm/h', metrics:['Total 4.6 mm'], status:'ok', iface:'GPIO 7' },
      { id:'wind', name:'Wind', icon:'wind', val:'3.2', unit:'m/s', metrics:['Gust 5.8 m/s'], status:'ok', iface:'GPIO 8' },
      { id:'flow_main', name:'YF-S201', icon:'droplets', val:'0.0', unit:'L/min', metrics:['Total 142.6 L'], status:'ok', iface:'GPIO 21' },
      { id:'co2_sensor', name:'SCD4x', icon:'leaf', val:'—', unit:'', metrics:['I2C ack failed'], status:'err', iface:'I2C 0x62' },
      { id:'soil_dry', name:'Soil', icon:'sprout', val:'34', unit:'%', metrics:['Calibrated'], status:'dis', iface:'ADC 3' },
    ];
    origSensorGrid.innerHTML = sensors2.map(s => `
      <div class="sensor ${s.status==='err'?'err':s.status==='dis'?'dis':''}" data-sid="${s.id}">
        <div class="s-head">
          <div>
            <div class="s-name"><i data-lucide="${s.icon}"></i> ${s.name}</div>
            <div class="s-id">${s.id} · ${s.iface}</div>
          </div>
          <span class="badge ${s.status==='ok'?'ok':s.status==='err'?'err':'dim'}">${s.status==='ok'?'OK':s.status==='err'?'ERR':'OFF'}</span>
        </div>
        <div class="s-val"><span class="n">${s.val}</span><span class="u">${s.unit}</span></div>
        <svg class="s-spark" viewBox="0 0 100 36" preserveAspectRatio="none">
          <polyline points="${Array.from({length:30},(_,i)=>`${i*3.4},${18+Math.sin(i/2+s.id.length)*10+Math.random()*4}`).join(' ')}" fill="none" stroke="currentColor" stroke-width="1.4"/>
        </svg>
        <div class="s-metrics">${s.metrics.map(m=>`<span class="badge dim">${m}</span>`).join('')}</div>
        <div class="s-foot">
          <span class="s-age"><i data-lucide="clock"></i> …</span>
          <span>${s.iface}</span>
        </div>
      </div>
    `).join('');
    if (window.lucide) lucide.createIcons({ nodes:[origSensorGrid] });
    updateStaleness();
    setInterval(updateStaleness, 2000);
  }

  // ══════════════════════════════════════════════
  // 3. COLLAPSIBLE SIDEBAR RAIL
  // ══════════════════════════════════════════════
  const railBtn = document.createElement('button');
  railBtn.className = 'rail-toggle';
  railBtn.setAttribute('aria-label', 'Toggle sidebar rail mode');
  railBtn.innerHTML = '<i data-lucide="panel-left-close"></i>';
  const sidebar = document.getElementById('sidebar');
  if (sidebar) {
    sidebar.insertBefore(railBtn, sidebar.firstChild);
    if (window.lucide) lucide.createIcons({ nodes:[railBtn] });
    let isRail = false;
    railBtn.onclick = () => {
      isRail = !isRail;
      document.querySelector('.app').classList.toggle('rail', isRail);
      railBtn.setAttribute('aria-expanded', String(!isRail));
      railBtn.innerHTML = isRail ? '<i data-lucide="panel-left-open"></i>' : '<i data-lucide="panel-left-close"></i>';
      if (window.lucide) lucide.createIcons({ nodes:[railBtn] });
    };
  }

  // ══════════════════════════════════════════════
  // 4. CHART CROSSHAIR + TOOLTIP
  // ══════════════════════════════════════════════
  const chartWrap2 = document.getElementById('chartWrap');
  if (chartWrap2) {
    const tooltip = document.createElement('div');
    tooltip.className = 'chart-tooltip';
    tooltip.innerHTML = `<div class="ct-time"></div><div class="ct-row"><span class="ct-dot" style="background:var(--accent)"></span><span class="ct-lbl">Volume</span><span class="ct-val"></span></div>`;
    chartWrap2.appendChild(tooltip);

    const crosshair = document.createElement('div');
    crosshair.className = 'chart-crosshair';
    chartWrap2.appendChild(crosshair);

    const chartDays = ['Mon 11','Tue 12','Wed 13','Thu 14','Fri 15','Sat 16','Sun 17','Mon 18'];
    const chartData2 = [24,18,32,14,45,22,28,38,15,42,26,31,19,34,29,41,23,37,16,49,28,33,21,39,25,44,17,36];

    const svg2 = chartWrap2.querySelector('.chart-svg');
    if (svg2) {
      svg2.addEventListener('mousemove', e => {
        const r = svg2.getBoundingClientRect();
        const x = e.clientX - r.left;
        const xPct = x / r.width;
        const idx = Math.round(xPct * (chartData2.length - 1));
        const clampedIdx = Math.max(0, Math.min(chartData2.length-1, idx));
        const val = chartData2[clampedIdx];
        const dayIdx = Math.floor(clampedIdx / (chartData2.length / chartDays.length));

        tooltip.querySelector('.ct-time').textContent = chartDays[Math.min(dayIdx, chartDays.length-1)];
        tooltip.querySelector('.ct-val').textContent = val.toFixed(1) + ' L';
        tooltip.classList.add('visible');
        crosshair.classList.add('visible');

        const ttW = 140;
        const left = x + 12 + ttW > r.width ? x - ttW - 12 : x + 12;
        tooltip.style.left = left + 'px';
        tooltip.style.top = '8px';
        crosshair.style.left = x + 'px';
      });
      svg2.addEventListener('mouseleave', () => {
        tooltip.classList.remove('visible');
        crosshair.classList.remove('visible');
      });
    }
  }

  // ══════════════════════════════════════════════
  // 5. SETTINGS SEARCH — NAVIGATE + HIGHLIGHT
  // ══════════════════════════════════════════════
  const settingsSearch = document.querySelector('#page-settings .input[placeholder*="Filter"]');
  if (settingsSearch) {
    settingsSearch.addEventListener('input', () => {
      const q = settingsSearch.value.toLowerCase().trim();
      document.querySelectorAll('.set-card').forEach(card => {
        const text = card.textContent.toLowerCase();
        if (!q) {
          card.classList.remove('search-match','search-hide');
          // remove any jump buttons
          card.querySelectorAll('.search-jump-btn').forEach(b => b.remove());
          return;
        }
        const match = text.includes(q);
        card.classList.toggle('search-match', match);
        card.classList.toggle('search-hide', !match);

        // Add jump button on match
        if (match && !card.querySelector('.search-jump-btn')) {
          const page = card.querySelector('.set-card-t')?.textContent.toLowerCase().replace(/\s+/g,'')||'';
          const pageMap = { device:'settings_device', flowmeter:'settings_flowmeter', hardware:'settings_hardware', datalog:'settings_datalog', corelogic:'corelogic', export:'export', theme:'settings_theme', network:'settings_network', time:'settings_time', files:'files' };
          const target = pageMap[page];
          if (target) {
            const jb = document.createElement('button');
            jb.className = 'search-jump-btn';
            jb.innerHTML = `<i data-lucide="arrow-right"></i> Open`;
            jb.onclick = (e) => {
              e.stopPropagation();
              document.querySelectorAll('.nav-item,.bnav').forEach(n => {
                const a = n.dataset.page === target;
                n.classList.toggle('active', a);
                if (a) n.setAttribute('aria-current','page');
                else n.removeAttribute('aria-current');
              });
              document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === target));
              settingsSearch.value = '';
              document.querySelectorAll('.set-card').forEach(c => c.classList.remove('search-match','search-hide'));
            };
            card.querySelector('.card-body, .set-card-d')?.appendChild(jb) || card.appendChild(jb);
            if (window.lucide) lucide.createIcons({ nodes:[jb] });
          }
        } else if (!match) {
          card.querySelectorAll('.search-jump-btn').forEach(b => b.remove());
        }
      });
    });
  }

  // ══════════════════════════════════════════════
  // 6. LIVE LOG — LEVEL FILTER + FREE-TEXT SEARCH
  // ══════════════════════════════════════════════
  const logFeedEl = document.getElementById('logFeed');
  if (logFeedEl) {
    const logCardBody = logFeedEl.parentElement;
    const toolbar = document.createElement('div');
    toolbar.className = 'log-toolbar';
    toolbar.innerHTML = `
      <input class="log-search" placeholder="Filter log…" aria-label="Filter log messages" type="search"/>
      <button class="lv-btn active info" data-lv="info">INFO</button>
      <button class="lv-btn active warn" data-lv="warn">WARN</button>
      <button class="lv-btn active err"  data-lv="err">ERR</button>
      <button class="lv-btn active ok"   data-lv="ok">OK</button>
      <button class="btn-mini" id="logClearBtn" aria-label="Clear log"><i data-lucide="trash-2"></i></button>
    `;
    logCardBody.insertBefore(toolbar, logFeedEl);
    if (window.lucide) lucide.createIcons({ nodes:[toolbar] });

    const activeLevels = new Set(['info','warn','err','ok']);
    let logSearchText = '';

    function filterLog() {
      logFeedEl.querySelectorAll('.line').forEach(line => {
        const lv = (line.querySelector('.lv')?.className.split(' ')[1] || 'info');
        const text = line.textContent.toLowerCase();
        const visible = activeLevels.has(lv) && (!logSearchText || text.includes(logSearchText));
        line.classList.toggle('hidden-log', !visible);
      });
    }

    toolbar.querySelectorAll('.lv-btn').forEach(btn => {
      btn.onclick = () => {
        const lv = btn.dataset.lv;
        if (activeLevels.has(lv)) activeLevels.delete(lv);
        else activeLevels.add(lv);
        btn.classList.toggle('active', activeLevels.has(lv));
        filterLog();
      };
    });

    toolbar.querySelector('.log-search').oninput = e => {
      logSearchText = e.target.value.toLowerCase().trim();
      filterLog();
    };

    document.getElementById('logClearBtn').onclick = () => {
      logFeedEl.innerHTML = '';
      showToast('Log cleared', '', 'info', 2000);
    };
  }

  // ══════════════════════════════════════════════
  // 7. OTA UPLOAD PROGRESS
  // ══════════════════════════════════════════════
  const dropzone = document.querySelector('.dropzone');
  const otaFlashBtn = document.querySelector('.page[data-page="update"] .btn.primary');
  if (dropzone && otaFlashBtn) {
    // Wrap existing content as .drop-idle
    const idleWrapper = document.createElement('div');
    idleWrapper.className = 'drop-idle';
    idleWrapper.style.cssText = 'display:flex;flex-direction:column;align-items:center;gap:8px;';
    while (dropzone.firstChild) idleWrapper.appendChild(dropzone.firstChild);
    dropzone.appendChild(idleWrapper);

    // Ready state (file selected)
    const readyEl = document.createElement('div');
    readyEl.className = 'drop-ready';
    readyEl.innerHTML = `
      <i data-lucide="file-check-2" style="width:32px;height:32px;color:var(--accent)"></i>
      <div style="font-weight:600" id="otaFileName">firmware.bin</div>
      <div style="color:var(--text-3);font-size:12px" id="otaFileSize">—</div>
    `;
    dropzone.appendChild(readyEl);

    // Progress section
    const progEl = document.createElement('div');
    progEl.className = 'ota-progress';
    progEl.innerHTML = `
      <div class="ota-bar-wrap">
        <div class="ota-bar"><span id="otaBarFill" style="width:0%"></span></div>
        <span class="ota-pct" id="otaPct">0%</span>
      </div>
      <div class="ota-status" id="otaStatus">Initialising…</div>
    `;
    dropzone.parentElement.insertBefore(progEl, dropzone.nextSibling);

    // Drag + drop
    dropzone.addEventListener('dragover', e => { e.preventDefault(); dropzone.classList.add('has-file'); });
    dropzone.addEventListener('dragleave', () => {});
    dropzone.addEventListener('drop', e => {
      e.preventDefault();
      const f = e.dataTransfer.files[0];
      if (f) setOtaFile(f);
    });
    // Click to browse (simulated)
    dropzone.querySelector('a')?.addEventListener('click', e => {
      e.preventDefault();
      setOtaFile({ name:'firmware_v5.3.0.bin', size: 1048576 });
    });

    function setOtaFile(f) {
      dropzone.classList.add('has-file');
      document.getElementById('otaFileName').textContent = f.name;
      document.getElementById('otaFileSize').textContent = f.size ? (f.size/1024).toFixed(1)+' KB' : '—';
      otaFlashBtn.removeAttribute('disabled');
      if (window.lucide) lucide.createIcons({ nodes:[dropzone] });
    }

    otaFlashBtn.onclick = () => {
      if (otaFlashBtn.disabled) return;
      progEl.classList.add('visible');
      otaFlashBtn.setAttribute('disabled','');
      const steps = [
        [5,  'Validating firmware binary…'],
        [20, 'Erasing flash partition…'],
        [55, 'Writing firmware…'],
        [80, 'Verifying checksum…'],
        [95, 'Applying and rebooting…'],
        [100,'Flash complete — rebooting'],
      ];
      let i = 0;
      const run = () => {
        if (i >= steps.length) {
          showToast('Flash complete', 'Device rebooting…', 'ok', 5000);
          return;
        }
        const [pct, msg] = steps[i++];
        document.getElementById('otaBarFill').style.width = pct + '%';
        document.getElementById('otaPct').textContent = pct + '%';
        document.getElementById('otaStatus').textContent = msg;
        setTimeout(run, 600 + Math.random()*400);
      };
      setTimeout(run, 200);
    };
    if (window.lucide) lucide.createIcons({ nodes:[dropzone, progEl] });
  }

  // ══════════════════════════════════════════════
  // 8. KEYBOARD SHORTCUTS
  // ══════════════════════════════════════════════
  const shortcuts = [
    { keys:['G','D'], label:'Dashboard',   page:'dashboard' },
    { keys:['G','L'], label:'Live',         page:'live'      },
    { keys:['G','S'], label:'Sensors',      page:'sensors'   },
    { keys:['G','F'], label:'Files',        page:'files'     },
    { keys:['G','C'], label:'Core Logic',   page:'corelogic' },
    { keys:['G','E'], label:'Export',       page:'export'    },
    { keys:['?'],     label:'Shortcuts',    page:null        },
    { keys:['Esc'],   label:'Close panel',  page:null        },
  ];

  // Append kbd hints to nav items
  document.querySelectorAll('.nav-item[data-page]').forEach(item => {
    const sc = shortcuts.find(s => s.page === item.dataset.page);
    if (!sc) return;
    const kbd = document.createElement('span');
    kbd.className = 'kbd';
    kbd.setAttribute('aria-hidden','true');
    sc.keys.forEach(k => { const s=document.createElement('span'); s.className='key'; s.textContent=k; kbd.appendChild(s); });
    item.appendChild(kbd);
  });

  // Keyboard shortcut overlay
  const kbOverlay = document.createElement('div');
  kbOverlay.className = 'kb-overlay';
  kbOverlay.setAttribute('role','dialog');
  kbOverlay.setAttribute('aria-label','Keyboard shortcuts');
  kbOverlay.innerHTML = `
    <div class="kb-panel">
      <h3>Keyboard shortcuts <button class="btn-mini" id="kbClose" aria-label="Close"><i data-lucide="x"></i></button></h3>
      ${shortcuts.filter(s=>s.page).map(s=>`
        <div class="kb-row">
          <span>${s.label}</span>
          <span class="kb-keys">${s.keys.map(k=>`<span class="key">${k}</span>`).join('')}</span>
        </div>
      `).join('')}
      <div class="kb-row"><span>Show this panel</span><span class="kb-keys"><span class="key">?</span></span></div>
    </div>
  `;
  document.body.appendChild(kbOverlay);
  if (window.lucide) lucide.createIcons({ nodes:[kbOverlay] });
  document.getElementById('kbClose').onclick = () => kbOverlay.classList.remove('visible');
  kbOverlay.onclick = e => { if (e.target === kbOverlay) kbOverlay.classList.remove('visible'); };

  // Key handler
  let gPrefix = false;
  document.addEventListener('keydown', e => {
    if (['INPUT','TEXTAREA','SELECT'].includes(e.target.tagName)) return;
    if (e.key === '?') { kbOverlay.classList.toggle('visible'); return; }
    if (e.key === 'Escape') { kbOverlay.classList.remove('visible'); return; }
    if (e.key.toUpperCase() === 'G' && !gPrefix) { gPrefix=true; setTimeout(()=>gPrefix=false,1000); return; }
    if (gPrefix) {
      gPrefix = false;
      const sc = shortcuts.find(s => s.keys.length===2 && s.keys[1]===e.key.toUpperCase() && s.page);
      if (sc) {
        document.querySelectorAll('.nav-item,.bnav').forEach(n=>{
          const a=n.dataset.page===sc.page; n.classList.toggle('active',a);
          if(a) n.setAttribute('aria-current','page'); else n.removeAttribute('aria-current');
        });
        document.querySelectorAll('.page').forEach(p=>p.classList.toggle('active',p.dataset.page===sc.page));
        showToast(`Navigated to ${sc.label}`, `G → ${e.key.toUpperCase()}`, 'info', 1800);
      }
    }
  });

  // ══════════════════════════════════════════════
  // 9. PRINT BUTTON
  // ══════════════════════════════════════════════
  document.querySelectorAll('.page-actions').forEach(actions => {
    const page = actions.closest('.page');
    if (!page || !['dashboard','sensors','files'].includes(page.dataset.page)) return;
    const printBtn = document.createElement('button');
    printBtn.className = 'btn';
    printBtn.setAttribute('aria-label', 'Print or export as PDF');
    printBtn.innerHTML = '<i data-lucide="printer"></i> Print';
    printBtn.onclick = () => window.print();
    actions.appendChild(printBtn);
    if (window.lucide) lucide.createIcons({ nodes:[printBtn] });
  });

  // ══════════════════════════════════════════════
  // 10. EMPTY STATES
  // ══════════════════════════════════════════════
  function makeEmpty(icon, title, sub, ctaLabel, ctaFn) {
    const div = document.createElement('div');
    div.className = 'empty';
    div.innerHTML = `
      <div class="empty-icon"><i data-lucide="${icon}"></i></div>
      <div class="empty-title">${title}</div>
      <div class="empty-sub">${sub}</div>
      ${ctaLabel ? `<button class="btn primary empty-cta"><i data-lucide="plus"></i> ${ctaLabel}</button>` : ''}
    `;
    if (ctaFn) div.querySelector('.empty-cta')?.addEventListener('click', ctaFn);
    if (window.lucide) lucide.createIcons({ nodes:[div] });
    return div;
  }

  // Sensor grid empty state (shown on "no sensors" — demo toggle via tweak)
  const sgEl = document.getElementById('sensorGrid');
  const sgEmpty = makeEmpty('thermometer','No sensors configured','Add a sensor plugin in Core Logic to start collecting data.','Add sensor', () => {
    document.querySelectorAll('.nav-item,.bnav').forEach(n=>{const a=n.dataset.page==='corelogic';n.classList.toggle('active',a);if(a)n.setAttribute('aria-current','page');else n.removeAttribute('aria-current');});
    document.querySelectorAll('.page').forEach(p=>p.classList.toggle('active',p.dataset.page==='corelogic'));
  });
  sgEl?.parentElement.insertBefore(sgEmpty, sgEl);
  sgEmpty.style.display = 'none'; // hidden by default — has sensors

  // File table empty state
  const fbEl = document.getElementById('fileBody');
  if (fbEl && fbEl.closest('.card')) {
    const fbEmpty = makeEmpty('folder-open','This folder is empty','Upload files or create a new folder to get started.','Upload');
    fbEl.closest('.card').querySelector('.card-body')?.appendChild(fbEmpty);
    fbEmpty.style.display = 'none'; // has files
  }

  // Events list empty state
  const evListEl = document.getElementById('evList');
  if (evListEl) {
    const evEmpty = makeEmpty('droplets','No events yet','Events appear here when Full Flush or Part Flush buttons are triggered.','');
    evListEl.parentElement?.insertBefore(evEmpty, evListEl);
    evEmpty.style.display = evListEl.children.length === 0 ? '' : 'none';
  }


})();
