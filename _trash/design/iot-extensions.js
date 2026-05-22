// ESP32 Logger — IoT Extensions
// Adds: mode awareness, Overview/Alerts/Health pages, wizard, zones, compare mode
(function () {

  const ICON = (n, extra = '') => `<i data-lucide="${n}"${extra ? ' ' + extra : ''}></i>`;
  const re   = () => window.lucide && lucide.createIcons();

  // ─────────────────────────────────────────────
  // Platform-mode state
  // ─────────────────────────────────────────────
  const root = document.documentElement;
  const mode = (window.TWEAKS && TWEAKS.mode) || 'continuous';
  root.dataset.mode = mode;

  function setMode(m) {
    root.dataset.mode = m;
    TWEAKS.mode = m;
    try { localStorage.setItem('esp32-tweaks', JSON.stringify(TWEAKS)); } catch {}
    try { window.parent.postMessage({ type:'__edit_mode_set_keys', edits:{ mode:m } }, '*'); } catch {}
    updateModePill();
    // ensure correct default page is active for new mode
    routeToDefault();
    if (window.showToast) showToast(`Switched to ${m} mode`, 'Sidebar and pages updated', 'info', 2800);
  }

  // ─────────────────────────────────────────────
  // Topbar mode pill
  // ─────────────────────────────────────────────
  const topbar = document.querySelector('.topbar');
  let modePill;
  if (topbar) {
    modePill = document.createElement('span');
    modePill.className = 'mode-pill';
    const logo = topbar.querySelector('.logo');
    logo?.parentNode.insertBefore(modePill, logo.nextSibling);
    updateModePill();
  }
  function updateModePill() {
    if (!modePill) return;
    const m = root.dataset.mode;
    const icons = { legacy:'moon', continuous:'activity', hybrid:'layers' };
    modePill.innerHTML = `${ICON(icons[m])} <span>${m}</span>`;
    re();
  }

  // ─────────────────────────────────────────────
  // Inject new nav items (Overview, Alerts, Health)
  //   …and tag existing nav items with mode visibility
  // ─────────────────────────────────────────────
  const sidebar = document.getElementById('sidebar');
  const bnav    = document.querySelector('.bottomnav');

  // Tag existing items
  const tagItem = (sel, modes) => {
    document.querySelectorAll(sel).forEach(el => el.setAttribute('data-mode-show', modes));
  };
  tagItem('[data-page="dashboard"]', 'legacy hybrid');
  tagItem('[data-page="live"]',      'legacy hybrid');
  tagItem('[data-page="sensors"]',   'continuous hybrid');
  tagItem('[data-page="corelogic"]', 'continuous hybrid');
  tagItem('[data-page="export"]',    'continuous hybrid');

  // Update sidebar labels to mode-appropriate names
  const dashItem = document.querySelector('.nav-item[data-page="dashboard"] span:not(.nbadge):not(.ndot):not(.kbd)');
  if (dashItem) dashItem.textContent = 'Water';

  // Insert new items in sidebar (after Files, before "System" group)
  if (sidebar) {
    const insertNav = (page, label, icon, modes, opts = {}) => {
      const a = document.createElement('a');
      a.className = 'nav-item';
      a.setAttribute('role','link');
      a.dataset.page = page;
      a.setAttribute('data-mode-show', modes);
      a.innerHTML = `${ICON(icon)}<span>${label}</span>${opts.badge ? `<span class="nbadge mono">${opts.badge}</span>` : ''}`;
      // bind click — same logic as core nav
      a.onclick = () => {
        document.querySelectorAll('.nav-item, .bnav').forEach(x => {
          const active = x.dataset.page === page;
          x.classList.toggle('active', active);
          if (active) x.setAttribute('aria-current','page'); else x.removeAttribute('aria-current');
        });
        document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === page));
      };
      return a;
    };

    const filesItem  = sidebar.querySelector('[data-page="files"]');
    // Overview goes first under "Main"
    const overviewItem = insertNav('overview','Overview','layout-grid','continuous hybrid');
    sidebar.querySelector('.nav-group').insertAdjacentElement('afterend', overviewItem);

    // Alerts after files
    const alertsItem = insertNav('alerts','Alerts','bell-ring','continuous hybrid', { badge:'2' });
    filesItem?.insertAdjacentElement('afterend', alertsItem);

    // Health after Alerts
    const healthItem = insertNav('health','Health','heart-pulse','continuous hybrid');
    alertsItem.insertAdjacentElement('afterend', healthItem);

    re();
  }

  // ─────────────────────────────────────────────
  // Inject Tweaks: Platform Mode selector
  // ─────────────────────────────────────────────
  const twBody = document.querySelector('.tweaks-body');
  if (twBody && !document.getElementById('tw-mode')) {
    const field = document.createElement('div');
    field.className = 't-field';
    field.innerHTML = `
      <label>Platform mode</label>
      <div class="seg" id="tw-mode" role="group" aria-label="Platform mode">
        <button data-v="legacy" aria-pressed="${mode==='legacy'}">Legacy</button>
        <button data-v="continuous" aria-pressed="${mode==='continuous'}">Continuous</button>
        <button data-v="hybrid" aria-pressed="${mode==='hybrid'}">Hybrid</button>
      </div>
    `;
    twBody.insertBefore(field, twBody.firstChild);
    const seg = field.querySelector('.seg');
    seg.querySelectorAll('button').forEach(b => {
      if (b.dataset.v === root.dataset.mode) b.classList.add('active');
      b.onclick = () => {
        seg.querySelectorAll('button').forEach(x => {
          x.classList.remove('active');
          x.setAttribute('aria-pressed','false');
        });
        b.classList.add('active');
        b.setAttribute('aria-pressed','true');
        setMode(b.dataset.v);
      };
    });
  }

  // ─────────────────────────────────────────────
  // NEW PAGES — Overview, Alerts, Health, Add-sensor modal
  // ─────────────────────────────────────────────
  const main = document.getElementById('main-content');

  // ── Overview page (IoT-first dashboard) ──
  const overview = document.createElement('section');
  overview.className = 'page';
  overview.dataset.page = 'overview';
  overview.setAttribute('data-mode-show', 'continuous hybrid');
  overview.setAttribute('data-screen-label','01 Overview');
  overview.innerHTML = `
    <div class="page-head">
      <div>
        <h1 class="page-title">Overview</h1>
        <div class="page-sub">8 sensors across 3 zones · streaming since boot 14 Apr</div>
      </div>
      <div class="page-actions">
        <div class="seg" role="group" aria-label="Time range">
          <button aria-pressed="false">1h</button><button class="active" aria-pressed="true">24h</button><button aria-pressed="false">7d</button>
        </div>
        <button class="btn" id="ovAddSensor">${ICON('plus')} Add sensor</button>
      </div>
    </div>

    <div class="grid grid-12">
      <!-- AQI composite -->
      <div class="card span-6">
        <div class="card-head">
          <div class="card-title">${ICON('wind')} Air Quality Index</div>
          <span class="badge ok">GOOD</span>
        </div>
        <div class="card-body" style="padding:0">
          <div class="aqi-card">
            <div class="aqi-gauge">
              <svg viewBox="0 0 120 120">
                <circle class="aqi-track" cx="60" cy="60" r="50"/>
                <circle class="aqi-fill"  cx="60" cy="60" r="50" stroke-dasharray="314" stroke-dashoffset="220"/>
              </svg>
              <div class="aqi-center">
                <div class="aqi-label">AQI</div>
                <div class="aqi-score" id="aqiScore">38</div>
                <div class="aqi-quality aqi-good" id="aqiQuality">Good</div>
              </div>
            </div>
            <div class="aqi-breakdown">
              <div class="aqi-bar"><div class="aqi-bar-name">PM2.5</div><div class="aqi-bar-track"><div class="aqi-bar-fill" style="width:35%;background:var(--ok)"></div></div><div class="aqi-bar-val">12.4 µg</div></div>
              <div class="aqi-bar"><div class="aqi-bar-name">PM10</div><div class="aqi-bar-track"><div class="aqi-bar-fill" style="width:24%;background:var(--ok)"></div></div><div class="aqi-bar-val">18.2 µg</div></div>
              <div class="aqi-bar"><div class="aqi-bar-name">TVOC</div><div class="aqi-bar-track"><div class="aqi-bar-fill" style="width:18%;background:var(--ok)"></div></div><div class="aqi-bar-val">87 ppb</div></div>
              <div class="aqi-bar"><div class="aqi-bar-name">eCO₂</div><div class="aqi-bar-track"><div class="aqi-bar-fill" style="width:42%;background:var(--ok)"></div></div><div class="aqi-bar-val">412 ppm</div></div>
            </div>
          </div>
        </div>
      </div>

      <!-- Environment KPIs -->
      <div class="card span-6">
        <div class="card-head">
          <div class="card-title">${ICON('thermometer')} Environment</div>
          <span class="mono" style="font-size:11px;color:var(--text-3)">env_indoor · BME280</span>
        </div>
        <div class="card-body">
          <div class="grid grid-3" style="gap:10px">
            <div class="kpi" style="padding:14px"><div class="kpi-l">${ICON('thermometer')} Temp</div><div class="kpi-v"><span class="num">22.4</span><span class="unit">°C</span></div><div class="kpi-d">min 19.8 · max 24.1</div></div>
            <div class="kpi" style="padding:14px"><div class="kpi-l">${ICON('droplet')} Humidity</div><div class="kpi-v"><span class="num">47</span><span class="unit">%</span></div><div class="kpi-d">comfort range</div></div>
            <div class="kpi" style="padding:14px"><div class="kpi-l">${ICON('gauge')} Pressure</div><div class="kpi-v"><span class="num">1013</span><span class="unit">hPa</span></div><div class="kpi-d up">stable</div></div>
          </div>
        </div>
      </div>
    </div>

    <div class="grid grid-12" style="margin-top:var(--gap)">
      <!-- Energy -->
      <div class="card span-8">
        <div class="card-head">
          <div class="card-title">${ICON('zap')} Energy</div>
          <span class="mono" style="font-size:11px;color:var(--text-3)">ZMCT103C + ZMPT101B</span>
        </div>
        <div class="card-body">
          <div class="energy-grid">
            <div class="energy-tile live"><div class="energy-tile-l">Voltage</div><div class="energy-tile-v"><span id="ovVolt">231.4</span><span class="u">V</span></div><div class="energy-tile-trend">±0.4 V over 5min</div></div>
            <div class="energy-tile live"><div class="energy-tile-l">Current</div><div class="energy-tile-v"><span id="ovAmp">2.84</span><span class="u">A</span></div><div class="energy-tile-trend">peak 4.21 A</div></div>
            <div class="energy-tile live"><div class="energy-tile-l">Power</div><div class="energy-tile-v"><span id="ovPower">657</span><span class="u">W</span></div><div class="energy-tile-trend">PF 0.96</div></div>
            <div class="energy-tile"><div class="energy-tile-l">Today</div><div class="energy-tile-v">4.82<span class="u">kWh</span></div><div class="energy-tile-trend">↑ 8% vs avg</div></div>
            <div class="energy-tile"><div class="energy-tile-l">This week</div><div class="energy-tile-v">28.4<span class="u">kWh</span></div><div class="energy-tile-trend">— on track</div></div>
            <div class="energy-tile"><div class="energy-tile-l">This month</div><div class="energy-tile-v">112<span class="u">kWh</span></div><div class="energy-tile-trend">↓ 4% vs last</div></div>
          </div>
        </div>
      </div>

      <!-- Water (mode-aware: only legacy/hybrid) -->
      <div class="card span-4" data-mode-show="hybrid">
        <div class="card-head">
          <div class="card-title">${ICON('droplets')} Water</div>
        </div>
        <div class="card-body" style="display:flex;flex-direction:column;gap:14px">
          <div>
            <div style="color:var(--text-3);font-size:11px;text-transform:uppercase;letter-spacing:.05em">Today</div>
            <div class="mono" style="font-size:28px;font-weight:700">23.4<span style="font-size:13px;color:var(--text-3);margin-left:4px">L</span></div>
          </div>
          <div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Events</span><span class="mono">12 · 8 PF / 4 FF</span></div>
          <div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Corrected</span><span class="mono">2</span></div>
        </div>
      </div>

      <!-- Quick fill when not hybrid: weather / outdoor -->
      <div class="card span-4" data-mode-show="continuous">
        <div class="card-head">
          <div class="card-title">${ICON('cloud-rain')} Outdoor</div>
        </div>
        <div class="card-body" style="display:flex;flex-direction:column;gap:14px">
          <div>
            <div style="color:var(--text-3);font-size:11px;text-transform:uppercase;letter-spacing:.05em">Rain today</div>
            <div class="mono" style="font-size:28px;font-weight:700">4.6<span style="font-size:13px;color:var(--text-3);margin-left:4px">mm</span></div>
          </div>
          <div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Wind</span><span class="mono">3.2 m/s · gust 5.8</span></div>
          <div style="display:flex;justify-content:space-between;font-size:12px"><span style="color:var(--text-3)">Last rain</span><span class="mono">14:11</span></div>
        </div>
      </div>
    </div>

    <div class="grid grid-12" style="margin-top:var(--gap)">
      <!-- Recent alerts -->
      <div class="card span-6">
        <div class="card-head">
          <div class="card-title">${ICON('bell')} Recent alerts</div>
          <a class="mono" style="font-size:11px;color:var(--accent);cursor:pointer" data-jump="alerts">View all →</a>
        </div>
        <div class="card-body" style="padding:0" id="ovAlertFeed"></div>
      </div>
      <!-- Sensors snapshot -->
      <div class="card span-6">
        <div class="card-head">
          <div class="card-title">${ICON('thermometer')} Active sensors</div>
          <a class="mono" style="font-size:11px;color:var(--accent);cursor:pointer" data-jump="sensors">All sensors →</a>
        </div>
        <div class="card-body" id="ovSensorsList" style="display:flex;flex-direction:column;gap:6px"></div>
      </div>
    </div>
  `;
  main?.appendChild(overview);

  // populate alerts feed mini
  const ovAlertFeed = overview.querySelector('#ovAlertFeed');
  const alertEvents = [
    { t:'14:21', name:'SCD4x I2C error',    val:'ack failed', sev:'err' },
    { t:'12:08', name:'LittleFS 90% full',  val:'920/1024 KB', sev:'warn' },
    { t:'09:42', name:'Indoor temp > 26°C', val:'26.4°C', sev:'warn' },
    { t:'08:15', name:'MQTT reconnected',   val:'after 4s',   sev:'ok' },
  ];
  ovAlertFeed.innerHTML = alertEvents.map(a => `
    <div class="alert-feed-row">
      <span class="af-time">${a.t}</span>
      <div>
        <div class="af-name">${a.name}</div>
      </div>
      <span class="badge ${a.sev}">${a.sev.toUpperCase()}</span>
    </div>
  `).join('');

  // populate sensor mini-list
  const ovSensors = overview.querySelector('#ovSensorsList');
  const ovSensorData = [
    ['env_indoor','22.4 °C','ok','indoor'],
    ['air_quality','412 ppm','ok','indoor'],
    ['pm_outdoor','12.4 µg/m³','ok','outdoor'],
    ['rain_gauge','0.0 mm/h','ok','outdoor'],
    ['wind','3.2 m/s','ok','outdoor'],
    ['flow_main','0.0 L/min','ok','utility'],
    ['co2_sensor','—','err','indoor'],
  ];
  ovSensors.innerHTML = ovSensorData.map(([id,v,s])=>`
    <div style="display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid var(--border);font-size:12px">
      <span class="mono" style="color:var(--text-2)">${id}</span>
      <span style="display:flex;align-items:center;gap:8px">
        <span class="mono" style="font-weight:600">${v}</span>
        <span class="badge ${s}">${s.toUpperCase()}</span>
      </span>
    </div>
  `).join('');

  // Animate energy values
  setInterval(() => {
    const v = (231 + Math.random()*1.5 - 0.75).toFixed(1);
    const a = (2.7 + Math.random()*0.6 - 0.3).toFixed(2);
    const p = Math.round(v * a * 0.96);
    const vE = overview.querySelector('#ovVolt');
    const aE = overview.querySelector('#ovAmp');
    const pE = overview.querySelector('#ovPower');
    if (vE) vE.textContent = v;
    if (aE) aE.textContent = a;
    if (pE) pE.textContent = p;
  }, 1200);

  // ── Alerts page ──
  const alertsPage = document.createElement('section');
  alertsPage.className = 'page';
  alertsPage.dataset.page = 'alerts';
  alertsPage.setAttribute('data-mode-show', 'continuous hybrid');
  alertsPage.setAttribute('data-screen-label','06 Alerts');
  alertsPage.innerHTML = `
    <div class="page-head">
      <div>
        <h1 class="page-title">Alerts ${ICON('','aria-hidden="true"')}</h1>
        <div class="page-sub">Threshold rules across all sensors · 12 rules · 2 firing</div>
      </div>
      <div class="page-actions">
        <button class="btn">${ICON('history')} History</button>
        <button class="btn primary">${ICON('plus')} New rule</button>
      </div>
    </div>

    <div class="grid grid-4">
      <div class="kpi"><div class="kpi-l">${ICON('bell-ring')} Active rules</div><div class="kpi-v"><span class="num">12</span></div><div class="kpi-d">across 6 sensors</div></div>
      <div class="kpi"><div class="kpi-l">${ICON('alert-triangle')} Firing now</div><div class="kpi-v" style="color:var(--err)"><span class="num">2</span></div><div class="kpi-d down">+2 last hour</div></div>
      <div class="kpi"><div class="kpi-l">${ICON('clock')} Last 24h</div><div class="kpi-v"><span class="num">7</span></div><div class="kpi-d">trips</div></div>
      <div class="kpi"><div class="kpi-l">${ICON('bell-off')} Snoozed</div><div class="kpi-v"><span class="num">1</span></div><div class="kpi-d">until 18:00</div></div>
    </div>

    <div class="card" style="margin-top:var(--gap)">
      <div class="card-head">
        <div class="card-title">${ICON('list-checks')} Rules</div>
        <input class="input" placeholder="Filter rules…" style="width:200px;height:28px"/>
      </div>
      <div class="card-body" style="padding:0" id="alertRules"></div>
    </div>

    <div class="card" style="margin-top:var(--gap)">
      <div class="card-head">
        <div class="card-title">${ICON('history')} Trigger history (last 24h)</div>
      </div>
      <div class="card-body" style="padding:0" id="alertHistory"></div>
    </div>
  `;
  main?.appendChild(alertsPage);

  // Alert rules
  const rules = [
    { state:'firing',  name:'Indoor temp too high',    expr:'env_indoor.temperature > 26 for 5min', trips:3, last:'09:42', actions:['toast','mqtt'] },
    { state:'firing',  name:'SCD4x sensor error',       expr:'co2_sensor.status == ERR for 30s',     trips:1, last:'14:21', actions:['toast','log'] },
    { state:'armed',   name:'PM2.5 elevated',           expr:'pm_outdoor.pm25 > 35 µg/m³',           trips:0, last:'—',     actions:['toast'] },
    { state:'armed',   name:'CO₂ ventilation needed',   expr:'air_quality.eco2 > 1000 ppm for 10min',trips:0, last:'—',     actions:['toast','mqtt'] },
    { state:'armed',   name:'Humidity out of range',    expr:'env_indoor.humidity < 30 OR > 70',     trips:0, last:'2d ago',actions:['toast'] },
    { state:'snoozed', name:'Power draw spike',         expr:'power > 2000 W for 30s',               trips:0, last:'4d ago',actions:['toast','mqtt'] },
    { state:'armed',   name:'Storage 90% full',         expr:'storage.littlefs > 90%',               trips:1, last:'12:08', actions:['toast'] },
    { state:'armed',   name:'No water for 24h',         expr:'water.last_event > 24h',               trips:0, last:'—',     actions:['toast','mqtt'] },
    { state:'armed',   name:'Rain rate severe',         expr:'rain_gauge.rate > 10 mm/h',            trips:0, last:'14d ago',actions:['toast','webhook'] },
    { state:'armed',   name:'Wind gust',                expr:'wind.gust > 15 m/s',                   trips:0, last:'—',     actions:['toast','mqtt'] },
    { state:'armed',   name:'Voltage low',              expr:'energy.voltage < 210 V',               trips:0, last:'—',     actions:['toast','webhook'] },
    { state:'armed',   name:'MQTT broker down',         expr:'mqtt.connected == false for 60s',     trips:1, last:'08:15', actions:['toast','log'] },
  ];
  alertsPage.querySelector('#alertRules').innerHTML = rules.map(r => `
    <div class="alert-rule ${r.state}">
      <span class="ar-state" aria-label="${r.state}"></span>
      <div>
        <div class="ar-name">${r.name}</div>
        <div class="ar-expr">${r.expr}</div>
      </div>
      <div class="ar-meta">
        <span class="ar-trips ${r.trips ? 'warn':''}">${r.trips} trips today</span>
        <span class="ar-last">${r.last}</span>
      </div>
      <div class="ar-actions">
        ${r.actions.map(a => `<span class="badge dim" title="${a}">${a}</span>`).join('')}
        <button class="btn-mini" aria-label="Edit rule">${ICON('pencil')}</button>
        <label class="switch" style="margin-left:4px"><input type="checkbox" ${r.state!=='snoozed'?'checked':''} aria-label="Enable rule"/><span></span></label>
      </div>
    </div>
  `).join('');

  // History
  const history = [
    ['14:21', 'SCD4x sensor error',     'I2C ack failed @ 0x62',  'err',  'sent to MQTT'],
    ['12:08', 'Storage 90% full',       '920 KB / 1024 KB',       'warn', 'toast shown'],
    ['09:42', 'Indoor temp too high',   '26.4°C for 5min',        'warn', 'toast + MQTT'],
    ['08:15', 'MQTT broker down',       'connection lost 4s',     'err',  'auto-recovered'],
    ['03:11', 'Voltage low',            '208.4 V',                'warn', 'webhook called'],
    ['00:42', 'Indoor temp too high',   '26.2°C for 5min',        'warn', 'snoozed'],
    ['Yest', 'Power draw spike',        '2.1 kW for 30s',         'warn', 'snoozed by user'],
  ];
  alertsPage.querySelector('#alertHistory').innerHTML = history.map(([t,n,v,s,o]) => `
    <div class="alert-feed-row">
      <span class="af-time">${t}</span>
      <div>
        <div class="af-name">${n}</div>
        <div class="af-val">${v} · ${o}</div>
      </div>
      <span class="badge ${s}">${s.toUpperCase()}</span>
    </div>
  `).join('');

  // ── Sensor Health page ──
  const healthPage = document.createElement('section');
  healthPage.className = 'page';
  healthPage.dataset.page = 'health';
  healthPage.setAttribute('data-mode-show','continuous hybrid');
  healthPage.setAttribute('data-screen-label','07 Health');
  healthPage.innerHTML = `
    <div class="page-head">
      <div>
        <h1 class="page-title">Sensor health</h1>
        <div class="page-sub">Diagnostics, retry counts, uptime per sensor</div>
      </div>
      <div class="page-actions">
        <div class="seg" role="group" aria-label="Time range">
          <button aria-pressed="false">1h</button><button class="active" aria-pressed="true">24h</button><button aria-pressed="false">7d</button>
        </div>
        <button class="btn">${ICON('download')} Export diagnostics</button>
      </div>
    </div>

    <div class="grid grid-4">
      <div class="kpi"><div class="kpi-l">${ICON('activity')} Up</div><div class="kpi-v" style="color:var(--ok)"><span class="num">6</span><span class="unit">/ 8</span></div><div class="kpi-d up">75%</div></div>
      <div class="kpi"><div class="kpi-l">${ICON('clock-alert')} Stale</div><div class="kpi-v" style="color:var(--warn)"><span class="num">1</span></div><div class="kpi-d">pm_outdoor</div></div>
      <div class="kpi"><div class="kpi-l">${ICON('x-circle')} Errored</div><div class="kpi-v" style="color:var(--err)"><span class="num">1</span></div><div class="kpi-d">SCD4x</div></div>
      <div class="kpi"><div class="kpi-l">${ICON('refresh-cw')} Retries 24h</div><div class="kpi-v"><span class="num">42</span></div><div class="kpi-d">i2c bus</div></div>
    </div>

    <div class="health-grid" style="margin-top:var(--gap)" id="healthGrid"></div>
  `;
  main?.appendChild(healthPage);

  const healthData = [
    { id:'env_indoor',  name:'BME280',   iface:'I2C 0x76', state:'ok',   uptime:99.8, reads:8623, errors:0,  retries:0,  avg:'1.2 ms', last:'0.4s ago' },
    { id:'air_quality', name:'SGP30',    iface:'I2C 0x58', state:'ok',   uptime:99.4, reads:5749, errors:2,  retries:8,  avg:'2.4 ms', last:'0.4s ago' },
    { id:'pm_outdoor',  name:'SDS011',   iface:'UART1',    state:'warn', uptime:96.1, reads:1204, errors:12, retries:24, avg:'48 ms',  last:'1.2s ago' },
    { id:'rain_gauge',  name:'Rain',     iface:'GPIO 7',   state:'ok',   uptime:100,  reads:18,   errors:0,  retries:0,  avg:'<1 ms',  last:'4.2s ago' },
    { id:'wind',        name:'Wind',     iface:'GPIO 8',   state:'ok',   uptime:100,  reads:17280,errors:0,  retries:0,  avg:'<1 ms',  last:'0.4s ago' },
    { id:'flow_main',   name:'YF-S201',  iface:'GPIO 21',  state:'ok',   uptime:100,  reads:142,  errors:0,  retries:0,  avg:'<1 ms',  last:'0.0s ago' },
    { id:'co2_sensor',  name:'SCD4x',    iface:'I2C 0x62', state:'err',  uptime:0,    reads:0,    errors:42, retries:42, avg:'—',      last:'45s ago' },
    { id:'soil_dry',    name:'Soil',     iface:'ADC 3',    state:'dim',  uptime:0,    reads:0,    errors:0,  retries:0,  avg:'—',      last:'disabled' },
  ];
  // Uptime bar — 24 slots of 1 hour each
  const uptimeBar = (state, uptime) => {
    const slots = 24;
    const errSlots = state === 'err' ? slots : Math.max(0, Math.round(slots * (1 - uptime/100)));
    const warnSlots = state === 'warn' ? Math.floor(errSlots/2) : 0;
    let html = '';
    for (let i = 0; i < slots; i++) {
      const isErr = i < errSlots && state !== 'warn';
      const isWarn = state === 'warn' && i >= slots-errSlots && i < slots-errSlots+warnSlots;
      const cls = state === 'dim' ? 'unknown' : isErr ? 'err' : isWarn ? 'warn' : 'ok';
      html += `<span class="${cls}"></span>`;
    }
    return html;
  };
  healthPage.querySelector('#healthGrid').innerHTML = healthData.map(s => `
    <div class="health-tile ${s.state === 'err' ? 'err' : s.state === 'warn' ? 'warn' : ''}">
      <div class="health-tile-head">
        <div class="health-name">${ICON('cpu')} ${s.name}</div>
        <span class="badge ${s.state === 'ok' ? 'ok' : s.state === 'err' ? 'err' : s.state === 'warn' ? 'warn' : 'dim'}">${s.state.toUpperCase()}</span>
      </div>
      <div class="mono" style="font-size:11px;color:var(--text-3)">${s.id} · ${s.iface}</div>
      <div>
        <div class="mono" style="font-size:10px;color:var(--text-3);margin-bottom:4px">24h uptime · ${s.uptime}%</div>
        <div class="health-uptime-bar" aria-label="24-hour uptime bar">${uptimeBar(s.state, s.uptime)}</div>
      </div>
      <div class="health-stats">
        <div class="health-stat"><span class="health-stat-l">Reads</span><span class="health-stat-v">${s.reads.toLocaleString()}</span></div>
        <div class="health-stat"><span class="health-stat-l">Errors</span><span class="health-stat-v ${s.errors>10?'err':s.errors>0?'warn':''}">${s.errors}</span></div>
        <div class="health-stat"><span class="health-stat-l">Retries</span><span class="health-stat-v ${s.retries>10?'warn':''}">${s.retries}</span></div>
        <div class="health-stat"><span class="health-stat-l">Avg latency</span><span class="health-stat-v">${s.avg}</span></div>
      </div>
      <div style="display:flex;justify-content:space-between;font-size:11px;color:var(--text-3);font-family:var(--mono)">
        <span>Last: ${s.last}</span>
        <a style="color:var(--accent);cursor:pointer">Details →</a>
      </div>
    </div>
  `).join('');

  // ─────────────────────────────────────────────
  // SENSOR GRID — zones (modify existing #sensorGrid)
  // ─────────────────────────────────────────────
  const sensorGrid = document.getElementById('sensorGrid');
  if (sensorGrid) {
    const zones = {
      indoor:  { label:'Indoor',  icon:'home',    sensors:['env_indoor','air_quality','co2_sensor'] },
      outdoor: { label:'Outdoor', icon:'sun',     sensors:['pm_outdoor','rain_gauge','wind'] },
      utility: { label:'Utility', icon:'wrench',  sensors:['flow_main','soil_dry'] },
    };
    // Stash existing cards by sid
    const existing = {};
    sensorGrid.querySelectorAll('[data-sid]').forEach(c => { existing[c.dataset.sid] = c; });

    // Rebuild grid with zone sections
    sensorGrid.innerHTML = '';
    sensorGrid.style.display = 'block'; // override .grid-auto

    Object.entries(zones).forEach(([key, zone]) => {
      const sec = document.createElement('div');
      sec.className = 'zone-section';
      const count = zone.sensors.filter(id => existing[id]).length;
      sec.innerHTML = `
        <div class="zone-head">
          <div class="zone-title">${ICON(zone.icon)} ${zone.label}</div>
          <div class="zone-meta">${count} sensor${count!==1?'s':''}</div>
        </div>
        <div class="grid grid-auto"></div>
      `;
      const cardGrid = sec.querySelector('.grid');
      zone.sensors.forEach(id => {
        if (existing[id]) cardGrid.appendChild(existing[id]);
      });
      sensorGrid.appendChild(sec);
    });
    re();
  }

  // ─────────────────────────────────────────────
  // SENSOR CHART — compare mode (multi-overlay)
  // ─────────────────────────────────────────────
  const sensorChartCard = document.querySelector('.page[data-page="sensors"] .card:last-child');
  if (sensorChartCard) {
    // Find the existing single overlay row and replace with chip-based compare
    const oldOverlay = sensorChartCard.querySelector('div[style*="border-top"]');
    if (oldOverlay) {
      oldOverlay.remove();
    }
    const chipBar = document.createElement('div');
    chipBar.className = 'compare-chips';
    chipBar.innerHTML = `
      <span style="color:var(--text-3);align-self:center;margin-right:4px">Compare:</span>
      <span class="cmp-chip"><span class="cmp-dot" style="background:var(--accent)"></span>env_indoor.temp <button class="cmp-chip-rm" aria-label="Remove">×</button></span>
      <span class="cmp-chip"><span class="cmp-dot" style="background:var(--warn)"></span>air_quality.eco2 <button class="cmp-chip-rm" aria-label="Remove">×</button></span>
      <button class="cmp-add">${ICON('plus')} Add series</button>
    `;
    sensorChartCard.querySelector('.card-head').insertAdjacentElement('afterend', chipBar);
    re();
  }

  // ─────────────────────────────────────────────
  // QUICK-ADD SENSOR WIZARD
  // ─────────────────────────────────────────────
  const wiz = document.createElement('div');
  wiz.className = 'modal-backdrop';
  wiz.id = 'sensorWizard';
  wiz.setAttribute('role','dialog');
  wiz.setAttribute('aria-label','Add sensor');
  wiz.innerHTML = `
    <div class="modal">
      <div class="modal-head">
        <div class="modal-title">${ICON('plus-circle')} Add sensor</div>
        <button class="btn-mini" id="wizClose" aria-label="Close">${ICON('x')}</button>
      </div>
      <div class="modal-body">
        <div class="wiz-steps">
          <div class="wiz-dot active"></div>
          <div class="wiz-dot"></div>
          <div class="wiz-dot"></div>
          <div class="wiz-dot"></div>
        </div>

        <!-- Step 1: Type -->
        <div class="wiz-step active" data-step="1">
          <div class="label" style="margin-bottom:8px">Sensor type</div>
          <div class="wiz-type-grid">
            <div class="wiz-type-card selected" data-type="bme280">${ICON('thermometer')}<div class="wiz-type-name">BME280</div><div class="wiz-type-meta">I2C · T/H/P</div></div>
            <div class="wiz-type-card" data-type="bme688">${ICON('thermometer-sun')}<div class="wiz-type-name">BME688</div><div class="wiz-type-meta">I2C · 4-in-1</div></div>
            <div class="wiz-type-card" data-type="sds011">${ICON('cloud-fog')}<div class="wiz-type-name">SDS011</div><div class="wiz-type-meta">UART · PM2.5/10</div></div>
            <div class="wiz-type-card" data-type="pms5003">${ICON('cloud-fog')}<div class="wiz-type-name">PMS5003</div><div class="wiz-type-meta">UART · PM1/2.5/10</div></div>
            <div class="wiz-type-card" data-type="sgp30">${ICON('wind')}<div class="wiz-type-name">SGP30</div><div class="wiz-type-meta">I2C · TVOC/eCO₂</div></div>
            <div class="wiz-type-card" data-type="ens160">${ICON('wind')}<div class="wiz-type-name">ENS160</div><div class="wiz-type-meta">I2C · AQI/TVOC</div></div>
            <div class="wiz-type-card" data-type="scd4x">${ICON('leaf')}<div class="wiz-type-name">SCD4x</div><div class="wiz-type-meta">I2C · CO₂</div></div>
            <div class="wiz-type-card" data-type="bh1750">${ICON('sun')}<div class="wiz-type-name">BH1750</div><div class="wiz-type-meta">I2C · Lux</div></div>
            <div class="wiz-type-card" data-type="veml7700">${ICON('sun')}<div class="wiz-type-name">VEML7700</div><div class="wiz-type-meta">I2C · Lux</div></div>
            <div class="wiz-type-card" data-type="yfs201">${ICON('droplets')}<div class="wiz-type-name">YF-S201</div><div class="wiz-type-meta">Pulse · Flow</div></div>
            <div class="wiz-type-card" data-type="rain">${ICON('cloud-rain')}<div class="wiz-type-name">Rain</div><div class="wiz-type-meta">Pulse</div></div>
            <div class="wiz-type-card" data-type="wind">${ICON('wind')}<div class="wiz-type-name">Wind</div><div class="wiz-type-meta">Pulse</div></div>
          </div>
        </div>

        <!-- Step 2: ID + Zone -->
        <div class="wiz-step" data-step="2">
          <div class="form-grid">
            <div class="field"><label for="wiz-id">Sensor ID</label><input id="wiz-id" class="input mono" value="bme280_kitchen"/><div class="hint">Used in API responses and MQTT topics</div></div>
            <div class="field"><label for="wiz-zone">Zone</label>
              <select id="wiz-zone" class="input"><option>indoor</option><option>outdoor</option><option>utility</option><option>+ new zone…</option></select>
            </div>
            <div class="field" style="grid-column:span 2"><label for="wiz-name">Display name</label><input id="wiz-name" class="input" value="BME280 · Kitchen"/></div>
          </div>
        </div>

        <!-- Step 3: Interface -->
        <div class="wiz-step" data-step="3">
          <div class="form-grid">
            <div class="field"><label for="wiz-iface">Interface</label>
              <select id="wiz-iface" class="input"><option>I2C</option><option>UART</option><option>SPI</option></select>
            </div>
            <div class="field"><label for="wiz-addr">Address</label><input id="wiz-addr" class="input mono" value="0x76"/></div>
            <div class="field"><label for="wiz-sda">SDA pin</label><input id="wiz-sda" class="input mono" value="6"/></div>
            <div class="field"><label for="wiz-scl">SCL pin</label><input id="wiz-scl" class="input mono" value="7"/></div>
            <div class="field"><label for="wiz-int">Read interval (ms)</label><input id="wiz-int" class="input mono" type="number" value="10000"/></div>
          </div>
          <div style="margin-top:14px;padding:10px;background:var(--panel-2);border:1px solid var(--border);border-radius:6px;display:flex;gap:10px;align-items:center">
            ${ICON('zap')}<div><div style="font-size:12px;font-weight:600">I2C scan</div><div style="font-size:11px;color:var(--text-3)">Detected addresses: <span class="mono">0x76, 0x58, 0x62</span></div></div>
            <button class="btn sm" style="margin-left:auto">Rescan</button>
          </div>
        </div>

        <!-- Step 4: Review -->
        <div class="wiz-step" data-step="4">
          <div class="label" style="margin-bottom:8px">Review configuration</div>
          <pre style="background:var(--panel-2);border:1px solid var(--border);border-radius:6px;padding:14px;font-family:var(--mono);font-size:11.5px;line-height:1.7;overflow:auto"><span style="color:var(--text-3)">// platform_config.json — new sensor entry</span>
{
  "id":         "bme280_kitchen",
  "type":       "bme280",
  "zone":       "indoor",
  "enabled":    true,
  "interface":  "i2c",
  "sda":        6,
  "scl":        7,
  "address":    118,
  "read_interval_ms": 10000
}</pre>
          <p style="font-size:12px;color:var(--text-3);margin-top:10px">Saving will reload the platform pipeline. Existing sensors are not affected.</p>
        </div>
      </div>
      <div class="modal-foot">
        <button class="btn ghost" id="wizPrev">${ICON('arrow-left')} Back</button>
        <span class="mono" style="color:var(--text-3);font-size:11px" id="wizStepLabel">Step 1 of 4 · Type</span>
        <button class="btn primary" id="wizNext">Next ${ICON('arrow-right')}</button>
      </div>
    </div>
  `;
  document.body.appendChild(wiz);
  re();

  let wizStep = 1;
  const wizLabels = ['Type','ID & Zone','Interface','Review'];
  function updateWiz() {
    wiz.querySelectorAll('.wiz-step').forEach(s => s.classList.toggle('active', +s.dataset.step === wizStep));
    wiz.querySelectorAll('.wiz-dot').forEach((d,i) => {
      d.classList.toggle('done', i+1 < wizStep);
      d.classList.toggle('active', i+1 === wizStep);
    });
    document.getElementById('wizStepLabel').textContent = `Step ${wizStep} of 4 · ${wizLabels[wizStep-1]}`;
    document.getElementById('wizPrev').style.visibility = wizStep === 1 ? 'hidden' : 'visible';
    const next = document.getElementById('wizNext');
    next.innerHTML = wizStep === 4 ? `${ICON('check')} Save & reload` : `Next ${ICON('arrow-right')}`;
    re();
  }
  function openWiz() { wiz.classList.add('visible'); wizStep = 1; updateWiz(); }
  function closeWiz() { wiz.classList.remove('visible'); }
  wiz.querySelector('#wizClose').onclick = closeWiz;
  wiz.onclick = e => { if (e.target === wiz) closeWiz(); };
  wiz.querySelector('#wizPrev').onclick = () => { if (wizStep > 1) { wizStep--; updateWiz(); } };
  wiz.querySelector('#wizNext').onclick = () => {
    if (wizStep < 4) { wizStep++; updateWiz(); }
    else {
      closeWiz();
      if (window.showToast) showToast('Sensor saved', 'bme280_kitchen added · pipeline reloading', 'ok', 4000);
    }
  };
  wiz.querySelectorAll('.wiz-type-card').forEach(c => {
    c.onclick = () => {
      wiz.querySelectorAll('.wiz-type-card').forEach(x => x.classList.remove('selected'));
      c.classList.add('selected');
    };
  });

  // Bind triggers
  document.getElementById('ovAddSensor')?.addEventListener('click', openWiz);
  // Bind "Add sensor" on existing Sensors page header
  document.querySelectorAll('.page[data-page="sensors"] .btn').forEach(b => {
    if (b.textContent.trim().toLowerCase().includes('add')) b.onclick = openWiz;
  });
  document.querySelectorAll('.page[data-page="corelogic"] .btn').forEach(b => {
    if (b.textContent.trim().toLowerCase() === 'add') b.onclick = openWiz;
  });

  // ─────────────────────────────────────────────
  // Jump-to links (Overview "View all →")
  // ─────────────────────────────────────────────
  document.querySelectorAll('[data-jump]').forEach(a => {
    a.onclick = () => {
      const target = a.dataset.jump;
      document.querySelectorAll('.nav-item, .bnav').forEach(n => {
        const active = n.dataset.page === target;
        n.classList.toggle('active', active);
        if (active) n.setAttribute('aria-current','page'); else n.removeAttribute('aria-current');
      });
      document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === target));
    };
  });

  // ─────────────────────────────────────────────
  // Mode-aware default page routing
  //   If user is on a hidden page after mode switch, jump to default
  // ─────────────────────────────────────────────
  function routeToDefault() {
    const m = root.dataset.mode;
    const active = document.querySelector('.page.active');
    if (active) {
      const allowed = (active.getAttribute('data-mode-show') || '').split(' ').filter(Boolean);
      if (allowed.length && !allowed.includes(m)) {
        // jump to default for mode
        const def = m === 'legacy' ? 'dashboard' : 'overview';
        document.querySelectorAll('.nav-item, .bnav').forEach(n => {
          const a = n.dataset.page === def;
          n.classList.toggle('active', a);
          if (a) n.setAttribute('aria-current','page'); else n.removeAttribute('aria-current');
        });
        document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === def));
      }
    }
  }

  // For continuous/hybrid: default to Overview, not Dashboard
  if (mode !== 'legacy') {
    document.querySelectorAll('.nav-item, .bnav').forEach(n => {
      const a = n.dataset.page === 'overview';
      n.classList.toggle('active', a);
      if (a) n.setAttribute('aria-current','page'); else n.removeAttribute('aria-current');
    });
    document.querySelectorAll('.page').forEach(p => {
      p.classList.toggle('active', p.dataset.page === 'overview');
    });
  }

  // ─────────────────────────────────────────────
  // Add keyboard shortcuts for new pages
  // ─────────────────────────────────────────────
  if (window.addEventListener) {
    // Repurpose existing shortcut handler — just expose new pages via the same nav clicks
    // Add visual hints
    const kbMap = { overview:['G','O'], alerts:['G','A'], health:['G','H'] };
    Object.entries(kbMap).forEach(([page, keys]) => {
      const item = document.querySelector(`.nav-item[data-page="${page}"]`);
      if (!item || item.querySelector('.kbd')) return;
      const kbd = document.createElement('span');
      kbd.className = 'kbd';
      kbd.setAttribute('aria-hidden','true');
      keys.forEach(k => { const s = document.createElement('span'); s.className = 'key'; s.textContent = k; kbd.appendChild(s); });
      item.appendChild(kbd);
    });

    let gPrefix = false;
    document.addEventListener('keydown', e => {
      if (['INPUT','TEXTAREA','SELECT'].includes(e.target.tagName)) return;
      if (e.key.toUpperCase() === 'G' && !gPrefix) { gPrefix = true; setTimeout(() => gPrefix = false, 1000); return; }
      if (gPrefix) {
        gPrefix = false;
        const k = e.key.toUpperCase();
        const page = { O:'overview', A:'alerts', H:'health' }[k];
        if (page) {
          document.querySelectorAll('.nav-item, .bnav').forEach(n => {
            const a = n.dataset.page === page;
            n.classList.toggle('active', a);
            if (a) n.setAttribute('aria-current','page'); else n.removeAttribute('aria-current');
          });
          document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === page));
          if (window.showToast) showToast(`Navigated to ${page}`, `G → ${k}`, 'info', 1500);
        }
      }
    });
  }

  re();
})();
