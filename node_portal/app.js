// ============================================================================
// node_portal/app.js — the node's own setup page (docs/NODE_CONFIG.md §6)
//
// One page for both node firmwares. Everything is driven by GET /api/config:
// `config.transport` picks the steps, `caps` picks the boards, the pins and
// the sensor types. Plain ES5, no frameworks, no network beyond the node.
//
// RULES CARRIED OVER FROM node/src/ConfigPortal.cpp, where each was learned
// the hard way (node/README.md "Setup portal"):
//   * Pin fields take the silkscreen OR the GPIO number. D6 is GPIO12 while
//     GPIO6 is the flash clock; typing "6" for the pad marked D6 used to put
//     I2C on the flash bus and brick the node on every boot. So every pin
//     field says what it resolved to, and a forbidden pin is refused outright
//     with a "did you mean D6?" when that is the likely slip.
//   * Secrets are write-only. The API never returns them; a field left empty
//     means "keep the saved one" and says so in its placeholder. Clearing one
//     is a deliberate act (the Clear button, or for the WiFi password picking
//     an open network or another SSID) and is sent as "" + <key>_set:false.
//   * SSIDs come from the air: they are escaped, and never put in markup
//     attributes — the scan list refers to them by index.
//
// THE PAGE'S CHECKS ARE HINTS, THE NODE'S ARE THE LAW. The rules below
// repeat src/nodecfg/NodeConfigValidate.h so problems show while typing, but
// Save is never disabled by them: if the two copies ever drift, a page that
// is stricter than the node would lock someone out of a valid config. The
// node answers {"ok":false,"field","reason"} and the page takes the user to
// that field.
// ============================================================================
(function () {
"use strict";

// ── i18n ────────────────────────────────────────────────────────────────────
var L = {
en: {
  title: "Node setup", stepOf: "Step {n} of {m}",
  s_net: "Network", s_coll: "Collector", s_board: "Board & I2C", s_sens: "Sensors",
  s_batt: "Battery", s_node: "This node", s_review: "Review & save",
  back: "Back", next: "Next", save: "Save & restart",
  loadFail: "Could not read the node's settings ({e}).", reload: "Reload",
  keep: "saved — leave empty to keep", notSet: "not set", show: "Show",
  ssid: "Network name (SSID)", pass: "Password", scan: "Scan", scanning: "Scanning…",
  scanNone: "No networks found. Move closer, or type the name.",
  scanHint: "Tap a network to fill in its name.", open: "open",
  nextNet: "Next network",
  nextNetHint: "Set by the collector for a planned network change; tried when this one fails.",
  host: "Collector address", hostHint: "IP or hostname of the logger, e.g. 192.168.1.50",
  port: "Port", token: "Ingest token", tokenHint: "The collector's INGEST_TOKEN, if it has one.",
  bUser: "Basic auth user", bPass: "Basic auth password",
  bHint: "The collector's Basic Auth pair, if it uses one. It also guards this page on your LAN.",
  lmk: "ESP-NOW key",
  lmkHint: "16 characters, the collector's key. Typed only here; never sent over the radio.",
  link: "Link", paired: "Paired", pairedAs: "node #{n}", notPaired: "not paired",
  channel: "Channel", collector: "Collector", lastOk: "Last delivery", never: "never", ago: "{t} ago",
  reachable: "reachable", unreachable: "unreachable", unknown: "unknown",
  linkHint: "As of before setup mode (ESP-NOW is off while this page is open). To pair, open the pairing window on the collector, then save here.",
  board: "Board", boardHint: "Changes the diagram and the labels you can type.",
  sda: "I2C SDA", scl: "I2C SCL",
  i2cHint: "Shared by every I2C sensor. Type the board label (D6) or the GPIO number (12).",
  i2cUnused: "Unused until you add an I2C sensor.",
  lgUse: "used here", lgWarn: "caution", lgBad: "never",
  pinEmpty: "Enter a pin.", pinBad: "\"{v}\" is not a pin on this chip.",
  pinFlash: "GPIO{g} is the flash bus. Refused.",
  pinMean: " Did you mean {l} (GPIO{g})?", pinDup: "Also used by {w}.",
  metrics: "Metrics", budget: "{n} / {m} metrics", sensCount: "{n} / {m} sensors",
  noSensors: "No sensors yet. Add one below.", add: "Add", remove: "Remove",
  needAwake: "(needs sleep off)",
  sleepUnsafe: "SDS011 and the pulse counter need the node awake. Turn off Deep sleep under \"This node\" to use them.",
  addr: "I2C address", probe: "probe both", pin: "Data pin", count: "Probes on this pin",
  metricName: "Metric name", metricHint: "≤10 characters; more probes add _1, _2 …",
  rx: "RX pin (sensor TX)", tx: "TX pin (sensor RX)", mode: "Mode", rain: "Rain gauge", flow: "Flow meter",
  perPulse: "Per pulse", ppRain: "mm of rain per bucket tip", ppFlow: "litres per pulse",
  debounce: "Debounce", us: "microseconds",
  seaNote: "pressure_sea counts toward the budget even at altitude 0.",
  t_bmx280: "BME280 / BMP280", t_bme688: "BME688", t_ds18b20: "DS18B20 (1-Wire)",
  t_bh1750: "BH1750 (light)", t_sds011: "SDS011 (dust)", t_pulse: "Pulse counter",
  battPin: "Battery pin", battPinHint: "ADC pin at the divider's midpoint (A0 on the XIAO).",
  divider: "Divider ratio", dividerHint: "2.0 for two equal resistors.",
  trim: "Trim", trimHint: "Correction after the divider. 1.0 = none.",
  calib: "Calibrate", measured: "I measured (V)", reported: "Node reports (V)",
  calibHint: "Measure the battery with a multimeter; compare with the node.",
  useTrim: "Use trim {v}", calibNeed: "Enter both voltages.",
  name: "Name",
  nameHintW: "The node id the collector files readings under. ≤16 of A-Z a-z 0-9 _ -",
  nameHintE: "The label shown on the collector. ≤16 of A-Z a-z 0-9 _ -",
  interval: "Interval (s)", intervalHint: "10–65535 s.",
  alt: "Altitude (m)", altHint: "0 = do not publish pressure_sea.",
  sleep: "Deep sleep",
  sleepHint: "On for batteries. Off = mains powered, always awake.",
  fw: "Firmware", localEdits: "local edits",
  fwRun: "Running", fwGo: "Upload & restart",
  fwUp: "Uploading… {p}%", fwChk: "Checking…", fwOk: "Updated, restarting…",
  fwFail: "Upload failed: {e}",
  fwOld: "This firmware cannot update itself from the page.",
  fwKeep: " The running firmware is unchanged.",
  fe_not_node_image: "Not a firmware for this node.", fe_too_big: "Too big for this node.",
  fe_write_failed: "Could not write it ({d}).",
  issues: "The node will refuse this configuration:", noIssues: "Looks good. Saving restarts the node.",
  secNew: "new", secKeep: "saved", secClr: "cleared",
  clr: "Clear saved", clrUndo: "Keep saved", clrPh: "will be cleared — type one to set it",
  saving: "Saving…", saved: "Saved, restarting…", waiting: "Waiting for the node to come back…",
  isBack: "The node is back (up {t}).",
  apGone: "The node restarted and closed its setup network, as expected: {what} Rejoin your usual WiFi.",
  apGoneW: "it is joining \"{ssid}\" and will report to {host}.",
  apGoneE: "it is back on the radio and will look for the collector.",
  apNote: "On the node's own network, this page closes when it restarts.",
  close: "Close", saveFail: "Save failed: {e}", refused: "The node refused this: {r}",
  e_name: "1–16 of A-Z a-z 0-9 _ -",
  e_interval: "A whole number from 10 to 65535.", e_port: "1–65535.",
  e_host: "Enter the collector's address.", e_ssid: "Enter the network name.",
  e_lmk: "The key must be exactly 16 characters.",
  e_budget: "{n} metrics; the collector takes at most {m}.",
  e_many: "At most {m} sensors.",
  e_both: "BME280 and BME688 publish the same metrics; use one.",
  e_dupType: "Only one {t}.", e_sleep: "{t} needs the node awake — turn off Deep sleep.",
  e_rx16: "GPIO16 has no interrupt; the SDS011's RX needs one.",
  e_count: "1 to 8 probes.", e_metric: "Up to 10 characters.", e_pos: "Must be greater than 0."
},
bg: {
  title: "Настройка на възела", stepOf: "Стъпка {n} от {m}",
  s_net: "Мрежа", s_coll: "Колектор", s_board: "Платка и I2C", s_sens: "Сензори",
  s_batt: "Батерия", s_node: "Този възел", s_review: "Преглед и запис",
  back: "Назад", next: "Напред", save: "Запис и рестарт",
  loadFail: "Настройките на възела не могат да бъдат прочетени ({e}).", reload: "Презареди",
  keep: "запазена — празно я запазва", notSet: "не е зададена", show: "Покажи",
  ssid: "Име на мрежата (SSID)", pass: "Парола", scan: "Търси", scanning: "Търсене…",
  scanNone: "Няма открити мрежи. Приближете се или въведете името.",
  scanHint: "Докоснете мрежа, за да попълните името ѝ.", open: "отворена",
  nextNet: "Следваща мрежа",
  nextNetHint: "Зададена от колектора за планирана смяна; опитва се, ако текущата не работи.",
  host: "Адрес на колектора", hostHint: "IP адрес или име на логера, напр. 192.168.1.50",
  port: "Порт", token: "Токен за приемане", tokenHint: "INGEST_TOKEN на колектора, ако има такъв.",
  bUser: "Потребител (Basic auth)", bPass: "Парола (Basic auth)",
  bHint: "Basic Auth данните на колектора, ако ги използва. Пазят и тази страница в локалната мрежа.",
  lmk: "ESP-NOW ключ",
  lmkHint: "16 символа, ключът на колектора. Въвежда се само тук; не се предава по радиото.",
  link: "Връзка", paired: "Сдвоен", pairedAs: "възел №{n}", notPaired: "не е сдвоен",
  channel: "Канал", collector: "Колектор", lastOk: "Последна доставка", never: "никога", ago: "преди {t}",
  reachable: "достъпен", unreachable: "недостъпен", unknown: "неизвестно",
  linkHint: "Състоянието отпреди режима на настройка (ESP-NOW е изключен, докато страницата е отворена). За сдвояване отворете прозореца на колектора и запазете тук.",
  board: "Платка", boardHint: "Променя диаграмата и етикетите, които можете да въвеждате.",
  sda: "I2C SDA", scl: "I2C SCL",
  i2cHint: "Обща за всички I2C сензори. Въведете етикета (D6) или номера на GPIO (12).",
  i2cUnused: "Не се използва, докато не добавите I2C сензор.",
  lgUse: "използван", lgWarn: "внимание", lgBad: "забранен",
  pinEmpty: "Въведете пин.", pinBad: "„{v}“ не е пин на този чип.",
  pinFlash: "GPIO{g} е шината на флаш паметта. Отказано.",
  pinMean: " Имахте предвид {l} (GPIO{g})?", pinDup: "Вече се използва от {w}.",
  metrics: "Метрики", budget: "{n} / {m} метрики", sensCount: "{n} / {m} сензора",
  noSensors: "Все още няма сензори. Добавете отдолу.", add: "Добави", remove: "Премахни",
  needAwake: "(изисква без сън)",
  sleepUnsafe: "SDS011 и броячът на импулси изискват буден възел. Изключете „Дълбок сън“ в „Този възел“.",
  addr: "I2C адрес", probe: "провери двата", pin: "Пин за данни", count: "Сонди на този пин",
  metricName: "Име на метриката", metricHint: "≤10 символа; още сонди добавят _1, _2 …",
  rx: "RX пин (TX на сензора)", tx: "TX пин (RX на сензора)", mode: "Режим", rain: "Дъждомер", flow: "Разходомер",
  perPulse: "На импулс", ppRain: "мм дъжд на едно преобръщане", ppFlow: "литри на импулс",
  debounce: "Потискане на трептене", us: "микросекунди",
  seaNote: "pressure_sea се брои в лимита и при височина 0.",
  t_bmx280: "BME280 / BMP280", t_bme688: "BME688", t_ds18b20: "DS18B20 (1-Wire)",
  t_bh1750: "BH1750 (светлина)", t_sds011: "SDS011 (прах)", t_pulse: "Брояч на импулси",
  battPin: "Пин за батерията", battPinHint: "ADC пинът в средата на делителя (A0 на XIAO).",
  divider: "Коефициент на делителя", dividerHint: "2.0 при два еднакви резистора.",
  trim: "Корекция", trimHint: "Корекция след делителя. 1.0 = без.",
  calib: "Калибриране", measured: "Измерих (V)", reported: "Възелът отчита (V)",
  calibHint: "Измерете батерията с мултицет; сравнете с възела.",
  useTrim: "Използвай корекция {v}", calibNeed: "Въведете двете напрежения.",
  name: "Име",
  nameHintW: "Идентификаторът, под който колекторът записва данните. ≤16 от A-Z a-z 0-9 _ -",
  nameHintE: "Етикетът, показван на колектора. ≤16 от A-Z a-z 0-9 _ -",
  interval: "Интервал (s)", intervalHint: "10–65535 s.",
  alt: "Надморска височина (m)", altHint: "0 = без pressure_sea.",
  sleep: "Дълбок сън",
  sleepHint: "Включено при батерии. Изключено = мрежово захранване, винаги буден.",
  fw: "Фърмуер", localEdits: "локални промени",
  fwRun: "Работещ", fwGo: "Качи и рестартирай",
  fwUp: "Качване… {p}%", fwChk: "Проверка…", fwOk: "Обновено, рестартиране…",
  fwFail: "Качването не успя: {e}",
  fwOld: "Този фърмуер не може да се обнови от страницата.",
  fwKeep: " Работещият фърмуер не е променен.",
  fe_not_node_image: "Не е фърмуер за този възел.", fe_too_big: "Твърде голям за този възел.",
  fe_write_failed: "Записът не успя ({d}).",
  issues: "Възелът ще откаже тази конфигурация:", noIssues: "Изглежда наред. Записът рестартира възела.",
  secNew: "нова", secKeep: "запазена", secClr: "изтрита",
  clr: "Изтрий запазената", clrUndo: "Запази я", clrPh: "ще бъде изтрита — въведете нова, за да я зададете",
  saving: "Запис…", saved: "Запазено, рестартиране…", waiting: "Изчакване възелът да се върне…",
  isBack: "Възелът е отново на линия (работи от {t}).",
  apGone: "Възелът се рестартира и затвори мрежата за настройка, както се очаква: {what} Върнете се към обичайната WiFi мрежа.",
  apGoneW: "свързва се към „{ssid}“ и ще изпраща към {host}.",
  apGoneE: "отново е на радиото и ще търси колектора.",
  apNote: "В мрежата на възела тази страница се затваря при рестарта.",
  close: "Затвори", saveFail: "Записът не успя: {e}", refused: "Възелът отказа: {r}",
  e_name: "1–16 от A-Z a-z 0-9 _ -",
  e_interval: "Цяло число от 10 до 65535.", e_port: "1–65535.",
  e_host: "Въведете адреса на колектора.", e_ssid: "Въведете името на мрежата.",
  e_lmk: "Ключът трябва да е точно 16 символа.",
  e_budget: "{n} метрики; колекторът приема най-много {m}.",
  e_many: "Най-много {m} сензора.",
  e_both: "BME280 и BME688 публикуват едни и същи метрики; изберете един.",
  e_dupType: "Само един {t}.", e_sleep: "{t} изисква възелът да е буден — изключете „Дълбок сън“.",
  e_rx16: "GPIO16 няма прекъсване; RX на SDS011 се нуждае от такова.",
  e_count: "От 1 до 8 сонди.", e_metric: "До 10 символа.", e_pos: "Трябва да е по-голямо от 0."
}
};
var lang = "en";
try { lang = localStorage.getItem("np-lang") || ""; } catch (e) { lang = ""; }
if (!L[lang]) lang = /^bg/i.test(navigator.language || "") ? "bg" : "en";

function t(k, v) {
  var s = L[lang][k];
  if (s === undefined) s = L.en[k];
  if (s === undefined) return k;
  if (v) for (var n in v) s = s.split("{" + n + "}").join(String(v[n]));
  return s;
}
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
  });
}
function $(id) { return document.getElementById(id); }

// ── icons (inline SVG — no icon font, no network) ───────────────────────────
var IC = {
  wifi: '<path d="M5 12.5a11 11 0 0 1 14 0M1.5 9a16 16 0 0 1 21 0M8.5 16a6 6 0 0 1 7 0"/><circle cx="12" cy="20" r="1"/>',
  server: '<rect x="2" y="3" width="20" height="7" rx="2"/><rect x="2" y="14" width="20" height="7" rx="2"/><path d="M6 6.5h.01M6 17.5h.01"/>',
  cpu: '<rect x="5" y="5" width="14" height="14" rx="2"/><rect x="9" y="9" width="6" height="6"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/>',
  therm: '<path d="M14 4v10.5a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z"/>',
  batt: '<rect x="2" y="7" width="17" height="10" rx="2"/><path d="M22 11v2M6 10v4M10 10v4"/>',
  tag: '<path d="M12 2H2v10l9.3 9.3a1 1 0 0 0 1.4 0l8.6-8.6a1 1 0 0 0 0-1.4Z"/><circle cx="7" cy="7" r="1.5"/>',
  check: '<path d="M20 6 9 17l-5-5"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  trash: '<path d="M3 6h18M8 6V4h8v2M19 6l-1 14H6L5 6"/>',
  scan: '<path d="M21 12a9 9 0 1 1-3-6.7L21 8"/><path d="M21 3v5h-5"/>',
  eye: '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/>',
  lock: '<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>'
};
function ic(n) { return '<svg class="i" viewBox="0 0 24 24" aria-hidden="true">' + IC[n] + "</svg>"; }

// ── sensor catalogue (§1.1) ─────────────────────────────────────────────────
// n: metrics it adds to the budget (ds18b20: its `count`). i2c: shares the
// bus. awake: false when it cannot run on a node that deep-sleeps.
var TYPES = {
  bmx280: { n: 4, i2c: 1, awake: 0, m: "temperature, humidity, pressure, pressure_sea" },
  bme688: { n: 5, i2c: 1, awake: 0, m: "temperature, humidity, pressure, pressure_sea, gas_resistance" },
  ds18b20: { n: 0, i2c: 0, awake: 0, m: "probe_temp" },
  bh1750: { n: 1, i2c: 1, awake: 0, m: "lux" },
  sds011: { n: 2, i2c: 0, awake: 1, m: "pm25, pm10" },
  pulse: { n: 2, i2c: 0, awake: 1, m: "" }
};
var PIN_KEYS = { ds18b20: ["pin"], sds011: ["rx", "tx"], pulse: ["pin"] };
function metricsOf(s) {
  if (s.type === "ds18b20") return Math.max(0, +s.count || 0);
  return TYPES[s.type] ? TYPES[s.type].n : 0;
}
function newSensor(type) {
  switch (type) {
    case "ds18b20": return { type: type, pin: null, count: 1, metric: "probe_temp" };
    case "bh1750": return { type: type, addr: 0x23 };
    case "sds011": return { type: type, rx: null, tx: null };
    case "pulse": return { type: type, pin: null, mode: "rain", per_pulse: 0.2794, debounce_us: 5000 };
    default: return { type: type, addr: 0 };
  }
}

// ── state ───────────────────────────────────────────────────────────────────
// Whole-line comments here, not trailing ones: the build strips only those,
// and the page is held to a gzip budget (tools/build_node_portal.py).
var S = {
  // config as read from the node (the applied one)
  cfg: null,
  caps: null,
  // last /api/status
  st: null,
  // the edit copy — what Save sends
  ed: null,
  // pin fields: what was typed, keyed by field path
  raw: {},
  step: 0,
  steps: [],
  // {field, reason} from the last refused POST
  srv: null,
  // null | "running" | [nets]
  scan: null,
  showPw: {},
  // secret field path -> 1: the user asked to clear the saved one
  clr: {},
  // SSID last picked from the scan if it was open, else null
  openPick: null
};

function clone(o) { return JSON.parse(JSON.stringify(o)); }
function isW() { return S.cfg.transport !== "espnow"; }
function hw() { return S.caps.hw || S.cfg.hw || (isW() ? "esp8266" : "esp32c3"); }
function maxGpio() { return hw() === "esp8266" ? 16 : 21; }

// Field paths are the contract's own spelling ("sensors[1].pin", "net.host")
// so an error the node names lands on the input that holds it.
function parts(f) { return f.replace(/\[(\d+)\]/g, ".$1").split("."); }
function getP(o, f) {
  var p = parts(f);
  for (var i = 0; i < p.length && o != null; i++) o = o[p[i]];
  return o;
}
function setP(o, f, v) {
  var p = parts(f);
  for (var i = 0; i < p.length - 1; i++) {
    if (o[p[i]] == null || typeof o[p[i]] !== "object") o[p[i]] = {};
    o = o[p[i]];
  }
  o[p[p.length - 1]] = v;
}

// ── pins ────────────────────────────────────────────────────────────────────
function boardDef() {
  var b = S.caps.boards || [];
  for (var i = 0; i < b.length; i++) if (b[i].id === S.ed.board) return b[i];
  return b[0] || { id: 0, name: "", pins: {} };
}
function labelFor(g) {
  var p = boardDef().pins || {};
  for (var k in p) if (p[k] === g && k !== String(g) && !/^GPIO/i.test(k)) return k;
  return "";
}
// "D6", "d6", "GPIO12", "12" → 12. -1 = not a pin on this chip. null = empty.
function resolvePin(txt) {
  var v = String(txt == null ? "" : txt).replace(/\s+/g, "").toUpperCase();
  if (!v) return null;
  var p = boardDef().pins || {};
  for (var k in p) if (k.toUpperCase() === v) return p[k];
  v = v.replace(/^GPIO/, "");
  if (!/^\d{1,2}$/.test(v)) return -1;
  var n = parseInt(v, 10);
  return n <= maxGpio() ? n : -1;
}
function pinText(g) {
  if (g == null || g < 0) return "";
  return labelFor(g) || String(g);
}
function forbidden(g) { return (S.caps.forbidden_pins || []).indexOf(g) >= 0; }
function warnWhy(g) { var w = S.caps.warn_pins || {}; return w[String(g)] || ""; }

function hasI2C() {
  var s = S.ed.sensors || [];
  for (var i = 0; i < s.length; i++) if (TYPES[s[i].type] && TYPES[s[i].type].i2c) return true;
  return false;
}
// Every pin the config claims, as [{f, g, who}] — the basis for both the
// duplicate check and the "used here" chips on the header diagram.
function pinUses() {
  var u = [], s = S.ed.sensors || [];
  if (hasI2C()) {
    u.push({ f: "i2c.sda", g: S.ed.i2c.sda, who: "I2C SDA" });
    u.push({ f: "i2c.scl", g: S.ed.i2c.scl, who: "I2C SCL" });
  }
  for (var i = 0; i < s.length; i++) {
    var ks = PIN_KEYS[s[i].type] || [];
    for (var j = 0; j < ks.length; j++) {
      u.push({ f: "sensors[" + i + "]." + ks[j], g: s[i][ks[j]], who: t("t_" + s[i].type) + " " + ks[j].toUpperCase() });
    }
  }
  if (!isW() && S.ed.batt) u.push({ f: "batt.pin", g: S.ed.batt.pin, who: t("battPin") });
  return u;
}
// What a pin field's hint line says, and in which colour.
function pinState(f) {
  var raw = S.raw[f], g = getP(S.ed, f);
  if (raw === undefined) raw = pinText(g);
  if (g == null) return { c: "err", m: t("pinEmpty") };
  if (g < 0) return { c: "err", m: t("pinBad", { v: raw }) };
  var lab = labelFor(g), head = "GPIO" + g + (lab ? " · " + lab : "");
  if (forbidden(g)) {
    var m = head + " — " + t("pinFlash", { g: g });
    // The D6-vs-GPIO6 slip: a bare number that is forbidden as a GPIO but is
    // also a D-number printed on this board.
    var bp = boardDef().pins || {};
    if (/^\d+$/.test(String(raw).trim()) && bp["D" + g] != null && !forbidden(bp["D" + g])) {
      m += t("pinMean", { l: "D" + g, g: bp["D" + g] });
    }
    return { c: "err", m: m };
  }
  var u = pinUses(), dup = [];
  for (var i = 0; i < u.length; i++) if (u[i].f !== f && u[i].g === g) dup.push(u[i].who);
  if (dup.length && usesPin(f)) return { c: "err", m: head + " — " + t("pinDup", { w: dup.join(", ") }) };
  var w = warnWhy(g);
  if (w) return { c: "warn", m: head + " — " + w };
  return { c: "ok", m: head };
}
function usesPin(f) {
  var u = pinUses();
  for (var i = 0; i < u.length; i++) if (u[i].f === f) return true;
  return false;
}

// ── validation (a copy of the node's rules — see the header comment) ───────
function issues() {
  var out = [], e = S.ed, c = S.caps, s = e.sensors || [], i;
  function add(f, m) { out.push({ f: f, m: m }); }
  if (isW()) {
    if (!e.net.ssid) add("net.ssid", t("e_ssid"));
    if (!e.net.host) add("net.host", t("e_host"));
    if (!(e.net.port >= 1 && e.net.port <= 65535 && e.net.port % 1 === 0)) add("net.port", t("e_port"));
  } else if (e.lmk && e.lmk.length !== 16) add("lmk", t("e_lmk"));
  var u = pinUses();
  for (i = 0; i < u.length; i++) {
    var ps = pinState(u[i].f);
    if (ps.c === "err") add(u[i].f, ps.m);
  }
  var max = c.max_sensors || 8, mm = c.max_metrics || 8, n = 0, seen = {};
  if (s.length > max) add("sensors", t("e_many", { m: max }));
  for (i = 0; i < s.length; i++) {
    var ty = s[i].type, f = "sensors[" + i + "]";
    n += metricsOf(s[i]);
    if (ty !== "ds18b20" && seen[ty]) add(f + ".type", t("e_dupType", { t: t("t_" + ty) }));
    seen[ty] = 1;
    if (!isW() && e.sleep && TYPES[ty] && TYPES[ty].awake) add(f + ".type", t("e_sleep", { t: t("t_" + ty) }));
    if (ty === "ds18b20") {
      if (!(s[i].count >= 1 && s[i].count <= 8 && s[i].count % 1 === 0)) add(f + ".count", t("e_count"));
      if ((s[i].metric || "").length > 10) add(f + ".metric", t("e_metric"));
    }
    if (ty === "sds011" && hw() === "esp8266" && s[i].rx === 16) add(f + ".rx", t("e_rx16"));
    if (ty === "pulse") {
      if (!(s[i].per_pulse > 0)) add(f + ".per_pulse", t("e_pos"));
      if (!(s[i].debounce_us >= 0)) add(f + ".debounce_us", t("e_pos"));
    }
  }
  if (seen.bmx280 && seen.bme688) add("sensors", t("e_both"));
  if (n > mm) add("sensors", t("e_budget", { n: n, m: mm }));
  if (!/^[A-Za-z0-9_-]{1,16}$/.test(e.name || "")) add("name", t("e_name"));
  if (!(e.interval_s >= 10 && e.interval_s <= 65535 && e.interval_s % 1 === 0)) add("interval_s", t("e_interval"));
  if (!isW() && e.batt) {
    if (!(e.batt.divider > 0)) add("batt.divider", t("e_pos"));
    if (!(e.batt.trim > 0)) add("batt.trim", t("e_pos"));
  }
  return out;
}
function stepOfField(f) {
  var k = parts(f)[0], id;
  if (k === "net") id = /^net\.(ssid|pass)/.test(f) ? "net" : "coll";
  else if (k === "lmk") id = "coll";
  else if (k === "i2c" || k === "board") id = "board";
  else if (k === "sensors") id = "sens";
  else if (k === "batt") id = "batt";
  else if (k === "name" || k === "interval_s" || k === "altitude_m" || k === "sleep") id = "node";
  else id = "review";
  var i = S.steps.indexOf(id);
  return i < 0 ? S.steps.length - 1 : i;
}

// ── markup helpers ──────────────────────────────────────────────────────────
var uid = 0;
// kind: s (text) · n (number) · i (integer) · p (pin) · pw (secret)
function fld(label, f, kind, o) {
  o = o || {};
  var id = "f" + (++uid), v = getP(S.ed, f), h = "";
  if (kind === "p") v = S.raw[f] !== undefined ? S.raw[f] : pinText(v);
  var at = ' id="' + id + '" class="input" data-f="' + f + '" data-k="' + kind + '"';
  if (o.max) at += ' maxlength="' + o.max + '"';
  if (o.ph) at += ' placeholder="' + esc(o.ph) + '"';
  if (kind === "n" || kind === "i") at += ' type="number" inputmode="decimal" step="' + (o.step || (kind === "i" ? 1 : "any")) + '"';
  else if (kind === "p") at += ' autocapitalize="characters" autocomplete="off" spellcheck="false"';
  else if (kind !== "pw") at += ' autocapitalize="off" autocomplete="off" spellcheck="false"';
  var inp;
  if (kind === "pw") {
    // Never a stored value: the API does not return one, and the field
    // starts empty on every render unless the user typed something.
    at += ' type="' + (S.showPw[f] ? "text" : "password") + '" autocomplete="new-password" autocapitalize="off" spellcheck="false"';
    inp = '<div class="pw"><input' + at + ' value="' + esc(v || "") + '"><button type="button" data-a="pw" data-i="' +
      f + '" aria-label="' + esc(t("show")) + '">' + ic("eye") + "</button></div>";
    // "" keeps the saved secret, so clearing one needs its own control.
    if (o.clr && secretSet(f) && !autoClear(f)) {
      inp += '<button type="button" class="btn sm" style="margin-top:6px" data-a="clr" data-i="' + f + '">' +
        esc(t(S.clr[f] ? "clrUndo" : "clr")) + "</button>";
    }
  } else if (o.opts) {
    inp = "<select" + at + ">";
    for (var i = 0; i < o.opts.length; i++) {
      var op = o.opts[i];
      inp += '<option value="' + esc(op[0]) + '"' + (String(op[0]) === String(v) ? " selected" : "") + (op[2] ? " disabled" : "") + ">" + esc(op[1]) + "</option>";
    }
    inp += "</select>";
  } else {
    inp = "<input" + at + ' value="' + esc(v == null ? "" : v) + '">';
  }
  if (kind === "p") h = '<div class="hint" data-ph="' + f + '"></div>';
  else if (o.hint) h = '<div class="hint">' + esc(o.hint) + "</div>";
  return '<div class="field"><label for="' + id + '">' + esc(label) + "</label>" + inp + h +
    '<div class="ferr" data-fe="' + f + '"></div></div>';
}
function card(icon, title, body, right) {
  return '<section class="card"><div class="card-head"><div class="card-title">' + ic(icon) + " " + esc(title) +
    "</div>" + (right || "") + '</div><div class="card-body">' + body + "</div></section>";
}
function secretPh(set, f) { return f && willClear(f) ? t("clrPh") : set ? t("keep") : t("notSet"); }
// "net.pass" -> S.cfg.net.pass_set: does the node have one saved?
function secretSet(f) {
  var p = f.split("."), o = S.cfg;
  for (var i = 0; i < p.length - 1; i++) o = (o || {})[p[i]];
  return !!(o && o[p[p.length - 1] + "_set"]);
}
// The saved WiFi password belongs to the saved SSID: another network, or one
// picked from the scan as open, must not inherit it (an ESP8266 given a
// passphrase for an open network does not connect, and the node would fall
// back to its AP).
function autoClear(f) {
  return f === "net.pass" && (S.ed.net.ssid !== S.cfg.net.ssid || S.ed.net.ssid === S.openPick);
}
// Sent as "" + <key>_set:false: only when nothing new was typed and there is
// a saved one to drop.
function willClear(f) { return !getP(S.ed, f) && secretSet(f) && !!(S.clr[f] || autoClear(f)); }
function fmtDur(s) {
  if (s == null || s < 0) return "—";
  if (s < 90) return Math.round(s) + " s";
  if (s < 5400) return Math.round(s / 60) + " min";
  if (s < 172800) return Math.round(s / 3600) + " h";
  return Math.round(s / 86400) + " d";
}
function kpi(l, v, id) { return '<div class="kpi"><div class="kpi-l">' + esc(l) + '</div><div class="kpi-v"' + (id ? ' id="' + id + '"' : "") + ">" + v + "</div></div>"; }

// ── steps ───────────────────────────────────────────────────────────────────
var R = {};

R.net = function () {
  var n = S.ed.net, h = "", sc = "";
  h += fld(t("ssid"), "net.ssid", "s", { max: 32 });
  h += fld(t("pass"), "net.pass", "pw", { max: 64, ph: secretPh(S.cfg.net.pass_set, "net.pass"), clr: 1 });
  if (S.scan === "running") sc = '<p class="hint" style="display:flex;gap:8px;align-items:center"><span class="spin"></span>' + esc(t("scanning")) + "</p>";
  else if (S.scan && !S.scan.length) sc = '<p class="hint">' + esc(t("scanNone")) + "</p>";
  else if (S.scan) {
    sc = '<p class="hint">' + esc(t("scanHint")) + '</p><div class="nets" id="nets">';
    for (var i = 0; i < S.scan.length; i++) {
      var x = S.scan[i];
      sc += '<button type="button" data-a="net" data-i="' + i + '">' + ic(x.open ? "wifi" : "lock") + '<span class="n">' + esc(x.ssid) +
        '</span><span class="m">' + (x.open ? esc(t("open")) + " · " : "") + "ch " + esc(x.ch) + " · " + esc(x.rssi) + " dBm</span></button>";
    }
    sc += "</div>";
  }
  h += sc;
  var nx = n.next && n.next.ssid;
  if (nx) h += '<div class="field"><span class="field-label">' + esc(t("nextNet")) + '</span><div class="mono">' + esc(nx) + '</div><div class="hint">' + esc(t("nextNetHint")) + "</div></div>";
  return card("wifi", t("s_net"), h, '<button type="button" class="btn sm" data-a="scan"' + (S.scan === "running" ? " disabled" : "") + ">" + ic("scan") + " " + esc(t("scan")) + "</button>");
};

function collBadge() {
  var c = S.st && S.st.collector;
  if (!c) return '<span id="cb"></span>';
  return '<span id="cb" class="badge ' + (c === "reachable" ? "ok" : c === "unreachable" ? "err" : "dim") + '">' + esc(t(c)) + "</span>";
}

R.coll = function () {
  var h = "";
  if (isW()) {
    h += '<div class="form-grid">' + fld(t("host"), "net.host", "s", { max: 64, hint: t("hostHint") }) +
      fld(t("port"), "net.port", "i", {}) + "</div>";
    h += fld(t("token"), "net.token", "pw", { max: 64, ph: secretPh(S.cfg.net.token_set, "net.token"), hint: t("tokenHint"), clr: 1 });
    h += '<div class="form-grid">' + fld(t("bUser"), "net.basic_user", "s", { max: 32 }) +
      fld(t("bPass"), "net.basic_pass", "pw", { max: 64, ph: secretPh(S.cfg.net.basic_pass_set, "net.basic_pass"), clr: 1 }) + "</div>";
    h += '<p class="hint">' + esc(t("bHint")) + "</p>";
    return card("server", t("s_coll"), h, collBadge());
  }
  h += fld(t("lmk"), "lmk", "pw", { max: 16, ph: secretPh(S.cfg.lmk_set) });
  h += '<div class="hint" id="lmkn"></div><p class="hint">' + esc(t("lmkHint")) + "</p>";
  return card("lock", t("lmk"), h) + card("wifi", t("link"), '<div id="linkbox">' + linkBox() + "</div>", collBadge());
};
function linkBox() {
  var st = S.st || {}, k = "";
  k += kpi(t("paired"), st.paired == null ? "—" : st.paired ? esc(t("pairedAs", { n: st.node_id })) : esc(t("notPaired")), "k-pair");
  k += kpi(t("channel"), st.ch ? esc(st.ch) : "—");
  k += kpi(t("collector"), st.collector ? esc(t(st.collector)) : "—");
  k += kpi(t("lastOk"), st.last_ok_s == null || st.last_ok_s < 0 ? esc(t("never")) : esc(t("ago", { t: fmtDur(st.last_ok_s) })));
  var b = '<div class="kpis">' + k + '</div><p class="hint">' + esc(t("linkHint")) + "</p>";
  var nx = S.cfg.link && S.cfg.link.next_ssid;
  if (nx) b += '<div class="field"><span class="field-label">' + esc(t("nextNet")) + '</span><div class="mono">' + esc(nx) + '</div><div class="hint">' + esc(t("nextNetHint")) + "</div></div>";
  return b;
}

// The header diagram: pads top-to-bottom as printed, coloured by what each
// pin costs and whether this config uses it.
function chip(lab) {
  var p = boardDef().pins || {}, g = p[lab];
  if (g == null && /^GPIO\d+$/i.test(lab)) g = parseInt(lab.slice(4), 10);
  if (g == null) return '<span class="chip pwr">' + esc(lab) + "</span>";
  var u = pinUses(), used = false, i;
  for (i = 0; i < u.length; i++) if (u[i].g === g) used = true;
  var c = forbidden(g) ? "err" : used ? "use" : warnWhy(g) ? "warn" : "";
  var sub = String(g) === lab || lab === "GPIO" + g ? "" : " " + g;
  return '<span class="chip ' + c + '" title="GPIO' + g + (warnWhy(g) ? " — " + esc(warnWhy(g)) : "") + '"><b>' + esc(lab) + "</b>" + sub + "</span>";
}
function diagram() {
  var b = boardDef(), h = "", i;
  if (b.left && b.right) {
    h += '<div class="header"><div class="chip-col l">';
    for (i = 0; i < b.left.length; i++) h += chip(b.left[i]);
    h += '</div><div class="body"></div><div class="chip-col">';
    for (i = 0; i < b.right.length; i++) h += chip(b.right[i]);
    h += "</div></div>";
  } else {
    h += '<div class="chips">';
    for (var k in b.pins || {}) h += chip(k);
    h += "</div>";
  }
  return h + '<div class="legend"><span class="chip use">' + esc(t("lgUse")) + '</span><span class="chip warn">' + esc(t("lgWarn")) +
    '</span><span class="chip err">' + esc(t("lgBad")) + "</span></div>";
}

R.board = function () {
  var bs = S.caps.boards || [], seg = '<div class="seg" role="group" aria-label="' + esc(t("board")) + '">';
  for (var i = 0; i < bs.length; i++) {
    seg += '<button type="button" data-a="board" data-i="' + bs[i].id + '"' + (bs[i].id === S.ed.board ? ' class="active" aria-pressed="true"' : "") + ">" + esc(bs[i].name) + "</button>";
  }
  seg += "</div>";
  var h = seg + '<p class="hint">' + esc(t("boardHint")) + '</p><div id="diag">' + diagram() + "</div>";
  var i2 = '<p class="hint">' + esc(t("i2cHint")) + "</p>" +
    (hasI2C() ? "" : '<p class="hint">' + esc(t("i2cUnused")) + "</p>") +
    '<div class="form-grid">' + fld(t("sda"), "i2c.sda", "p") + fld(t("scl"), "i2c.scl", "p") + "</div>";
  return card("cpu", t("board") + " · " + hw().toUpperCase(), h) + card("cpu", "I2C", i2);
};

function sensorRow(s, i) {
  var f = "sensors[" + i + "]", h = "", ty = s.type, ad;
  h += '<div class="srow" data-row="' + i + '"><div class="srow-h"><span class="t">' + esc(t("t_" + ty)) +
    '</span><span class="badge dim">' + metricsOf(s) + '</span><button type="button" class="btn-mini" data-a="rm" data-i="' + i +
    '" aria-label="' + esc(t("remove")) + '">' + ic("trash") + "</button></div>";
  if (ty === "bmx280" || ty === "bme688") ad = [[0, t("probe")], [0x76, "0x76"], [0x77, "0x77"]];
  if (ty === "bh1750") ad = [[0x23, "0x23"], [0x5c, "0x5C"]];
  if (ad) h += fld(t("addr"), f + ".addr", "i", { opts: ad });
  if (ty === "ds18b20") {
    h += fld(t("pin"), f + ".pin", "p");
    h += '<div class="form-grid">' + fld(t("count"), f + ".count", "i", {}) +
      fld(t("metricName"), f + ".metric", "s", { max: 10, hint: t("metricHint") }) + "</div>";
  }
  if (ty === "sds011") h += '<div class="form-grid">' + fld(t("rx"), f + ".rx", "p") + fld(t("tx"), f + ".tx", "p") + "</div>";
  if (ty === "pulse") {
    h += fld(t("mode"), f + ".mode", "s", { opts: [["rain", t("rain")], ["flow", t("flow")]] });
    h += fld(t("pin"), f + ".pin", "p");
    h += '<div class="form-grid">' + fld(t("perPulse"), f + ".per_pulse", "n", { hint: s.mode === "flow" ? t("ppFlow") : t("ppRain") }) +
      fld(t("debounce"), f + ".debounce_us", "i", { hint: t("us") }) + "</div>";
  }
  var m = ty === "pulse" ? (s.mode === "flow" ? "flow_rate, flow_total" : "rain_rate, rain_total") :
    ty === "ds18b20" ? (s.metric || "probe_temp") + (s.count > 1 ? ", _1 … _" + (s.count - 1) : "") : TYPES[ty] ? TYPES[ty].m : "";
  h += '<div class="hint mono">' + esc(m) + '</div><div class="ferr" data-fe="' + f + '.type"></div>';
  return h + "</div>";
}

R.sens = function () {
  var s = S.ed.sensors, h = '<div class="kpi"><div class="kpi-l">' + esc(t("metrics")) + '</div><div class="kpi-v" id="k-bud"></div>' +
    '<div class="bar"><span id="bud-bar"></span></div><div class="hint" id="k-cnt"></div></div><div class="ferr" data-fe="sensors"></div>';
  if (!s.length) h += '<p class="hint">' + esc(t("noSensors")) + "</p>";
  for (var i = 0; i < s.length; i++) h += sensorRow(s[i], i);
  var noWake = !isW() && S.ed.sleep, types = S.caps.sensor_types || [], o = "";
  for (i = 0; i < types.length; i++) {
    var off = noWake && TYPES[types[i]] && TYPES[types[i]].awake;
    o += '<option value="' + esc(types[i]) + '"' + (off ? " disabled" : "") + ">" + esc(t("t_" + types[i])) + (off ? " " + esc(t("needAwake")) : "") + "</option>";
  }
  h += '<div class="addbar"><select class="input" id="addtype" aria-label="' + esc(t("add")) + '">' + o + '</select><button type="button" class="btn" data-a="add">' +
    ic("plus") + " " + esc(t("add")) + "</button></div>";
  if (noWake) h += '<p class="hint">' + esc(t("sleepUnsafe")) + "</p>";
  var sea = false;
  for (i = 0; i < s.length; i++) if (s[i].type === "bmx280" || s[i].type === "bme688") sea = true;
  if (sea && !S.ed.altitude_m) h += '<p class="hint">' + esc(t("seaNote")) + "</p>";
  return card("therm", t("s_sens"), h);
};

R.batt = function () {
  var h = fld(t("battPin"), "batt.pin", "p") + '<p class="hint">' + esc(t("battPinHint")) + "</p>" +
    '<div class="form-grid">' + fld(t("divider"), "batt.divider", "n", { hint: t("dividerHint") }) +
    fld(t("trim"), "batt.trim", "n", { hint: t("trimHint") }) + "</div>";
  var bv = S.st && S.st.batt_v != null ? S.st.batt_v : "";
  var c = '<p class="hint">' + esc(t("calibHint")) + '</p><div class="form-grid"><div class="field"><label for="cal-x">' + esc(t("measured")) +
    '</label><input class="input" id="cal-x" type="number" inputmode="decimal" step="any"></div><div class="field"><label for="cal-y">' + esc(t("reported")) +
    '</label><input class="input" id="cal-y" type="number" inputmode="decimal" step="any" value="' + esc(bv) + '"></div></div>' +
    '<button type="button" class="btn" id="cal-go" data-a="trim" disabled>' + esc(t("calibNeed")) + "</button>";
  return card("batt", t("s_batt"), h) + card("batt", t("calib"), c);
};

R.node = function () {
  var h = fld(t("name"), "name", "s", { max: 16, hint: isW() ? t("nameHintW") : t("nameHintE") }) +
    '<div class="form-grid">' + fld(t("interval"), "interval_s", "i", { hint: t("intervalHint") }) +
    fld(t("alt"), "altitude_m", "n", { hint: t("altHint") }) + "</div>";
  if (!isW()) {
    h += '<label class="toggle-row"><span>' + esc(t("sleep")) + '</span><span class="switch"><input type="checkbox" data-f="sleep" data-k="b"' +
      (S.ed.sleep ? " checked" : "") + "><span></span></span></label><p class=\"hint\">" + esc(t("sleepHint")) +
      '</p><div class="ferr" data-fe="sleep"></div>';
  }
  return card("tag", t("s_node"), h) + fwCard();
};

// docs/NODE_OTA.md §5. Transport-neutral on purpose: both nodes answer
// POST /update the same way, and the node, not the page, decides whether the
// file is an image for it.
function fwCard() {
  return card("cpu", t("fw"), '<p class="hint" id="fwv">' + esc(t("fwRun") + ": " + (S.cfg.fw || "—")) +
    '</p>' + '<input id="fwf" type="file" accept=".bin" aria-label="' + esc(t("fw")) + '"><div id="fwm"></div>' +
    '<button type="button" class="btn" data-a="fw">' + esc(t("fwGo")) + "</button>");
}

function secretLine(v, set, f) { return v ? t("secNew") : f && willClear(f) ? t("secClr") : set ? t("secKeep") : t("notSet"); }
R.review = function () {
  var e = S.ed, kv = [], i, h = "";
  if (isW()) {
    kv.push([t("ssid"), e.net.ssid], [t("pass"), secretLine(e.net.pass, S.cfg.net.pass_set, "net.pass")],
      [t("host"), e.net.host + ":" + e.net.port], [t("token"), secretLine(e.net.token, S.cfg.net.token_set, "net.token")],
      [t("bUser"), e.net.basic_user || "—"], [t("bPass"), secretLine(e.net.basic_pass, S.cfg.net.basic_pass_set, "net.basic_pass")]);
  } else kv.push([t("lmk"), secretLine(e.lmk, S.cfg.lmk_set)]);
  kv.push([t("board"), boardDef().name]);
  if (hasI2C()) kv.push(["I2C", "SDA " + pinText(e.i2c.sda) + " · SCL " + pinText(e.i2c.scl)]);
  for (i = 0; i < e.sensors.length; i++) {
    var s = e.sensors[i], ks = PIN_KEYS[s.type] || [], d = [];
    for (var j = 0; j < ks.length; j++) d.push(ks[j] + " " + pinText(s[ks[j]]));
    if (s.addr != null) d.push(s.addr ? "0x" + s.addr.toString(16) : t("probe"));
    kv.push([t("t_" + s.type), d.join(" · ")]);
  }
  if (!isW()) kv.push([t("s_batt"), pinText(e.batt.pin) + " · ×" + e.batt.divider + " · " + t("trim") + " " + e.batt.trim]);
  kv.push([t("name"), e.name], [t("interval"), e.interval_s], [t("alt"), e.altitude_m]);
  if (!isW()) kv.push([t("sleep"), e.sleep ? "✓" : "—"]);
  var dl = '<dl class="kv">';
  for (i = 0; i < kv.length; i++) dl += "<dt>" + esc(kv[i][0]) + "</dt><dd>" + esc(kv[i][1]) + "</dd>";
  dl += "</dl>";
  var is = issues();
  if (S.srv && stepOfField(S.srv.field) === S.steps.length - 1) {
    h += '<div class="alert err">' + esc(t("refused", { r: S.srv.reason })) + "</div>";
  }
  if (is.length) {
    h += '<div class="alert err" id="issues">' + esc(t("issues")) + "<ul>";
    for (i = 0; i < is.length; i++) h += '<li><a href="#" data-a="go" data-i="' + stepOfField(is[i].f) + '">' + esc(t("s_" + S.steps[stepOfField(is[i].f)])) + "</a>: " + esc(is[i].m) + "</li>";
    h += "</ul></div>";
  } else h += '<div class="alert ok">' + esc(t("noIssues")) + "</div>";
  return card("check", t("s_review"), h + dl);
};

// ── render ──────────────────────────────────────────────────────────────────
function head() {
  var c = S.cfg, b = '<span class="badge acc">' + (isW() ? "WiFi" : "ESP-NOW") + '</span><span class="badge dim">' + esc(hw()) + "</span>";
  if (c.fw) b += '<span class="badge dim">' + esc(c.fw) + "</span>";
  b += c.local ? '<span class="badge warn">' + esc(t("localEdits")) + "</span>" : c.rev ? '<span class="badge dim">rev ' + esc(c.rev) + "</span>" : "";
  var lg = '<div class="seg" role="group" aria-label="Language"><button type="button" data-a="lang" data-i="en"' + (lang === "en" ? ' class="active"' : "") +
    '>EN</button><button type="button" data-a="lang" data-i="bg"' + (lang === "bg" ? ' class="active"' : "") + ">БГ</button></div>";
  return '<header class="page-head"><div><h1 class="page-title">' + ic("cpu") + " " + esc(c.name || t("title")) + '</h1><div class="page-sub">' + b + "</div></div>" + lg + "</header>";
}
function render() {
  uid = 0;
  document.documentElement.lang = lang;
  document.title = t("title") + (S.cfg.name ? " · " + S.cfg.name : "");
  var h = head() + '<div class="steps" id="steps">';
  for (var i = 0; i < S.steps.length; i++) {
    h += '<button type="button" data-a="go" data-i="' + i + '" aria-label="' + esc(t("s_" + S.steps[i])) + '"' + (i === S.step ? ' aria-current="step"' : "") + ">" + (i + 1) + "</button>";
  }
  h += '</div><div class="step-name">' + esc(t("stepOf", { n: S.step + 1, m: S.steps.length })) + " · " + esc(t("s_" + S.steps[S.step])) + "</div>";
  h += '<div id="srv"></div>' + R[S.steps[S.step]]();
  $("app").innerHTML = h;
  var last = S.step === S.steps.length - 1;
  var nav = $("nav");
  nav.hidden = false;
  nav.innerHTML = "<div>" + (S.step > 0 ? '<button type="button" class="btn" data-a="back">' + esc(t("back")) + "</button>" : "") +
    (last ? '<button type="button" class="btn primary" data-a="save" id="save">' + ic("check") + " " + esc(t("save")) + "</button>"
      : '<button type="button" class="btn primary" data-a="next">' + esc(t("next")) + "</button>") + "</div>";
  live();
}

// Everything derived from the edit copy that can change while typing, without
// re-rendering the inputs (which would steal the focus from the phone's keyboard).
function live() {
  var is = issues(), bad = {}, i, els;
  for (i = 0; i < is.length; i++) bad[stepOfField(is[i].f)] = 1;
  if (S.srv) bad[stepOfField(S.srv.field)] = 1;
  // A step turns red once it has been left with a problem in it — not while
  // it is still being filled in for the first time.
  els = document.querySelectorAll("#steps button");
  for (i = 0; i < els.length; i++) {
    els[i].className = (i === S.step ? "active" : i < S.step ? "done" : "") +
      (bad[i] && S.left[i] && i < S.steps.length - 1 ? " bad" : "");
  }
  els = document.querySelectorAll("[data-ph]");
  for (i = 0; i < els.length; i++) {
    var f = els[i].getAttribute("data-ph"), ps = pinState(f), inp = document.querySelector('[data-f="' + f + '"]');
    var unused = f.indexOf("i2c.") === 0 && !hasI2C();
    els[i].className = "hint " + (unused ? "" : ps.c);
    els[i].textContent = ps.m;
    if (inp) inp.className = "input" + (unused ? "" : ps.c === "err" ? " bad" : ps.c === "warn" ? " warn" : "");
  }
  // Field errors: the node's own refusal wins over the page's guess.
  var fe = {};
  for (i = 0; i < is.length; i++) if (!/\.(sda|scl|pin|rx|tx)$/.test(is[i].f) && fe[is[i].f] === undefined) fe[is[i].f] = is[i].m;
  if (S.srv) fe[S.srv.field] = t("refused", { r: S.srv.reason });
  els = document.querySelectorAll("[data-fe]");
  for (i = 0; i < els.length; i++) {
    var k = els[i].getAttribute("data-fe");
    // "Enter the network name" waits until the field was typed in or the
    // step was left once; nobody needs to be told off on arrival.
    els[i].textContent = fe[k] && (S.srv && S.srv.field === k || S.touched[k] || S.left[S.step]) ? fe[k] : "";
  }
  var bud = $("k-bud");
  if (bud) {
    var n = 0, mm = S.caps.max_metrics || 8, ms = S.caps.max_sensors || 8;
    for (i = 0; i < S.ed.sensors.length; i++) n += metricsOf(S.ed.sensors[i]);
    bud.textContent = t("budget", { n: n, m: mm });
    bud.style.color = n > mm ? "var(--err)" : n === mm ? "var(--warn)" : "";
    $("k-cnt").textContent = t("sensCount", { n: S.ed.sensors.length, m: ms });
    var bar = $("bud-bar");
    bar.style.width = Math.min(100, 100 * n / mm) + "%";
    bar.className = n > mm ? "err" : n === mm ? "warn" : "";
    var e2 = document.querySelector('[data-fe="sensors"]');
    for (i = 0; i < is.length; i++) if (is[i].f === "sensors" && !(S.srv && S.srv.field === "sensors")) { e2.textContent = is[i].m; break; }
    els = document.querySelectorAll("[data-row]");
    for (i = 0; i < els.length; i++) {
      var rb = false, pre = "sensors[" + els[i].getAttribute("data-row") + "]";
      for (var j = 0; j < is.length; j++) if (is[j].f.indexOf(pre + ".") === 0) rb = true;
      els[i].className = "srow" + (rb ? " bad" : "");
      var fe2 = els[i].querySelector('[data-fe="' + pre + '.type"]');
      for (j = 0; j < is.length; j++) if (is[j].f === pre + ".type" && !(S.srv && S.srv.field === is[j].f)) { fe2.textContent = is[j].m; break; }
    }
  }
  if ($("diag")) $("diag").innerHTML = diagram();
  if ($("lmkn")) $("lmkn").textContent = S.ed.lmk ? S.ed.lmk.length + " / 16" : "";
  // Editing the SSID changes what an empty password means (autoClear).
  var pp = document.querySelector('[data-f="net.pass"]');
  if (pp) pp.placeholder = secretPh(S.cfg.net.pass_set, "net.pass");
  if ($("cal-go")) calib();
}

function calib() {
  var x = parseFloat($("cal-x").value), y = parseFloat($("cal-y").value), b = $("cal-go");
  var base = S.cfg.batt && S.cfg.batt.trim > 0 ? S.cfg.batt.trim : 1;
  // The node's reading already has the APPLIED trim in it, not the one being
  // edited, so the correction multiplies that.
  if (x > 0 && y > 0) {
    S.newTrim = Math.round(base * x / y * 1000) / 1000;
    b.disabled = false;
    b.textContent = t("useTrim", { v: S.newTrim });
  } else {
    b.disabled = true;
    b.textContent = t("calibNeed");
  }
}

// ── events ──────────────────────────────────────────────────────────────────
function onInput(ev) {
  var el = ev.target, f = el.getAttribute("data-f"), k = el.getAttribute("data-k");
  if (el.id === "cal-x" || el.id === "cal-y") { calib(); return; }
  if (!f) return;
  var v = el.value;
  if (k === "b") v = el.checked;
  else if (k === "i" || k === "n") v = v === "" ? null : Number(v);
  else if (k === "p") { S.raw[f] = v; v = resolvePin(v); }
  setP(S.ed, f, v);
  S.touched[f] = 1;
  if (S.srv && S.srv.field === f) S.srv = null;
  // Changes that add or remove fields re-render; typing does not.
  if (ev.type === "change" && (el.tagName === "SELECT" || k === "b")) render(); else live();
}
function stepTo(i) {
  if (i < 0 || i >= S.steps.length) return;
  S.left[S.step] = 1;
  S.step = i;
  if (S.steps[i] === "coll" && !isW()) loadStatus();
  render();
  window.scrollTo(0, 0);
}
function onClick(ev) {
  var el = ev.target.closest ? ev.target.closest("[data-a]") : null;
  if (!el || el.disabled) return;
  var a = el.getAttribute("data-a"), i = el.getAttribute("data-i");
  if (el.tagName === "A") ev.preventDefault();
  if (a === "next") stepTo(S.step + 1);
  else if (a === "back") stepTo(S.step - 1);
  else if (a === "go") stepTo(+i);
  else if (a === "lang") {
    lang = i;
    try { localStorage.setItem("np-lang", i); } catch (e) { /* private mode */ }
    render();
  } else if (a === "board") {
    // Re-resolve what was typed against the new board's silkscreen.
    S.ed.board = +i;
    for (var f in S.raw) setP(S.ed, f, resolvePin(S.raw[f]));
    render();
  } else if (a === "add") {
    S.ed.sensors.push(newSensor($("addtype").value));
    render();
  } else if (a === "rm") {
    S.ed.sensors.splice(+i, 1);
    // Pin text is keyed by row index: shift the rows below up by one.
    var nr = {};
    for (var k in S.raw) {
      var m = /^sensors\[(\d+)\](.*)$/.exec(k);
      if (!m) nr[k] = S.raw[k];
      else if (+m[1] < +i) nr[k] = S.raw[k];
      else if (+m[1] > +i) nr["sensors[" + (m[1] - 1) + "]" + m[2]] = S.raw[k];
    }
    S.raw = nr;
    if (S.srv && /^sensors/.test(S.srv.field)) S.srv = null;
    render();
  } else if (a === "pw") {
    S.showPw[i] = !S.showPw[i];
    var inp = document.querySelector('input[data-f="' + i + '"]');
    if (inp) inp.type = S.showPw[i] ? "text" : "password";
  } else if (a === "clr") {
    if (S.clr[i]) delete S.clr[i]; else S.clr[i] = 1;
    render();
  } else if (a === "scan") scan();
  else if (a === "net") {
    var n = S.scan[+i];
    S.ed.net.ssid = n.ssid;
    // An open network has no password: drop the saved one, even when the
    // open network has the saved SSID (autoClear).
    S.openPick = n.open ? n.ssid : null;
    render();
    var p = document.querySelector('[data-f="net.pass"]');
    if (p && !n.open) p.focus();
  } else if (a === "trim") {
    S.ed.batt.trim = S.newTrim;
    render();
  } else if (a === "save") save();
  else if (a === "fw") fwUp();
  else if (a === "close") $("ov").hidden = true;
}

// ── network ─────────────────────────────────────────────────────────────────
function req(method, url, body, cb, tmo) {
  var x = new XMLHttpRequest();
  x.open(method, url, true);
  x.timeout = tmo || 8000;
  x.onload = function () {
    var j = null;
    try { j = JSON.parse(x.responseText); } catch (e) { j = null; }
    cb(x.status, j);
  };
  x.onerror = x.ontimeout = function () { cb(0, null); };
  if (body) x.setRequestHeader("Content-Type", "application/json");
  x.send(body ? JSON.stringify(body) : null);
}

function isOpen(n) {
  if (n.enc === "open" || n.enc === "none" || n.enc === false) return true;
  if (typeof n.enc === "number") return hw() === "esp8266" ? n.enc === 7 : n.enc === 0;
  return false;
}
function scan() {
  S.scan = "running";
  render();
  var tries = 0;
  (function poll() {
    req("GET", "/api/scan", null, function (st, j) {
      if (j && j.state === "done") {
        var by = {}, out = [], ns = j.nets || [];
        for (var i = 0; i < ns.length; i++) {
          if (!ns[i].ssid) continue;
          if (!by[ns[i].ssid] || by[ns[i].ssid].rssi < ns[i].rssi) by[ns[i].ssid] = ns[i];
        }
        for (var k in by) { by[k].open = isOpen(by[k]); out.push(by[k]); }
        out.sort(function (a, b) { return b.rssi - a.rssi; });
        S.scan = out;
        if (S.steps[S.step] === "net") render();
      } else if (++tries < 20) setTimeout(poll, 1500);
      else { S.scan = []; if (S.steps[S.step] === "net") render(); }
    });
  })();
}

function loadStatus() {
  req("GET", "/api/status", null, function (st, j) {
    if (st === 200 && j) {
      S.st = j;
      // Only the read-only parts are redrawn, never the fields being typed in.
      if ($("linkbox")) $("linkbox").innerHTML = linkBox();
      if ($("cb")) $("cb").outerHTML = collBadge();
    }
  }, 4000);
}

function payload() {
  var e = S.ed, o = {
    name: e.name, interval_s: e.interval_s, altitude_m: e.altitude_m || 0, board: e.board,
    i2c: { sda: e.i2c.sda, scl: e.i2c.scl }, sensors: e.sensors
  };
  if (isW()) {
    // next is the collector's to set (§4); secret fields "" mean keep, and
    // "" + <key>_set:false clears (§0.5).
    o.net = { ssid: e.net.ssid, pass: e.net.pass || "", host: e.net.host, port: e.net.port,
      token: e.net.token || "", basic_user: e.net.basic_user || "", basic_pass: e.net.basic_pass || "" };
    var sk = ["pass", "token", "basic_pass"];
    for (var i = 0; i < sk.length; i++) if (willClear("net." + sk[i])) o.net[sk[i] + "_set"] = false;
  } else {
    o.sleep = !!e.sleep;
    o.batt = { pin: e.batt.pin, divider: e.batt.divider, trim: e.batt.trim };
    o.lmk = e.lmk || "";
  }
  return o;
}

function overlay(body) {
  var ov = $("ov");
  ov.innerHTML = '<div class="card" role="alertdialog" aria-live="polite"><div class="card-body">' + body + "</div></div>";
  ov.hidden = false;
}
function save() {
  var b = $("save");
  if (b) b.disabled = true;
  S.srv = null;
  var before = S.st && S.st.uptime_s, t0 = Date.now(), sent = payload();
  overlay('<p style="display:flex;gap:10px;align-items:center"><span class="spin"></span>' + esc(t("saving")) + "</p>");
  req("POST", "/api/config", sent, function (st, j) {
    if (st === 200 && j && j.ok) { restarting("saved", before, t0, sent); return; }
    $("ov").hidden = true;
    if (j && j.ok === false) {
      S.srv = { field: j.field || "", reason: j.reason || "?" };
      S.step = stepOfField(S.srv.field);
      render();
      var el = document.querySelector('[data-fe="' + S.srv.field + '"]');
      if (!el || !el.textContent) $("srv").innerHTML = '<div class="alert err">' + esc(t("refused", { r: S.srv.reason })) + "</div>";
      var inp = document.querySelector('[data-f="' + S.srv.field + '"]');
      if (inp) { inp.className += " bad"; inp.focus(); }
      else window.scrollTo(0, 0);
      return;
    }
    render();
    $("srv").innerHTML = '<div class="alert err">' + esc(t("saveFail", { e: st ? "HTTP " + st : "no reply" })) + "</div>";
  });
}
// The file goes as multipart, field "fw", with its own XHR for the progress
// bar. A node older than §5 answers 404 — or, on the ESP8266, the page itself
// with a 200 (every unknown path serves it) — and neither is a JSON verdict.
function fwUp() {
  var f = $("fwf").files[0], x = new XMLHttpRequest(), fd = new FormData();
  if (!f) { $("fwf").focus(); return; }
  var before = S.st && S.st.uptime_s, t0 = Date.now();
  function say(k, v, keep) {
    $("ov").hidden = true;
    $("fwm").innerHTML = '<div class="alert err" id="fwe">' + esc(t(k, v) + (keep ? t("fwKeep") : "")) + "</div>";
  }
  fd.append("fw", f, f.name);
  overlay('<p id="fwp">' + esc(t("fwUp", { p: 0 })) + '</p><div class="bar"><span id="fwb" style="width:0"></span></div>');
  x.upload.onprogress = function (e) {
    if (!e.lengthComputable) return;
    var p = Math.round(100 * e.loaded / e.total);
    $("fwb").style.width = p + "%";
    // The last byte sent is not the end: the node checks the marker first.
    $("fwp").textContent = p < 100 ? t("fwUp", { p: p }) : t("fwChk");
  };
  x.onload = function () {
    var j = null;
    try { j = JSON.parse(x.responseText); } catch (e) { j = null; }
    if (x.status === 200 && j && j.ok) restarting("fwOk", before, t0, null);
    else if (x.status === 404 || x.status === 200 && (!j || typeof j.ok !== "boolean")) say("fwOld");
    // The node refused it: whatever the reason, it never switched to it.
    else if (j && j.error && L.en["fe_" + j.error]) say("fe_" + j.error, { d: j.detail || "?" }, 1);
    else say("fwFail", { e: "HTTP " + x.status });
  };
  x.onerror = x.ontimeout = function () { say("fwFail", { e: "no reply" }); };
  x.open("POST", "/update", true);
  x.timeout = 300000;
  x.send(fd);
}

// The node said yes and restarts: a config save, or a firmware update.
function restarting(k, before, t0, sent) {
  overlay('<p style="display:flex;gap:10px;align-items:center;font-weight:600" id="saved"><span class="spin"></span>' + esc(t(k)) +
    '</p><p class="hint" id="wait">' + esc(t("waiting")) + '</p><p class="hint">' + esc(t("apNote")) + "</p>");
  setTimeout(function () { waitBack(before, t0, sent); }, 2500);
}

// Poll until the node answers with a fresh uptime. On the setup AP it never
// will — the restart closes the AP — so after a while say that plainly
// instead of spinning forever. `sent` null: a firmware update, which reloads
// the page once the node is back, so what is shown is the new firmware's.
function waitBack(before, t0, sent) {
  var tries = 0;
  (function poll() {
    req("GET", "/api/status", null, function (st, j) {
      var elapsed = (Date.now() - t0) / 1000;
      if (st === 200 && j && (before == null || j.uptime_s < before + elapsed - 1)) {
        if (!sent) { location.reload(); return; }
        overlay('<p class="alert ok" id="back">' + esc(t("isBack", { t: fmtDur(j.uptime_s) })) + '</p><button type="button" class="btn primary" data-a="reload" id="reload">' +
          esc(t("reload")) + "</button>");
        return;
      }
      if (++tries < 15) { setTimeout(poll, 2000); return; }
      if (!sent) { overlay('<p class="alert warn" id="gone">' + esc(t("apNote")) + "</p>"); return; }
      var what = isW() ? t("apGoneW", { ssid: sent.net.ssid, host: sent.net.host }) : t("apGoneE");
      overlay('<p class="alert warn" id="gone">' + esc(t("apGone", { what: what })) + "</p>");
    }, 3000);
  })();
}

// ── boot ────────────────────────────────────────────────────────────────────
function boot() {
  req("GET", "/api/config", null, function (st, j) {
    if (st !== 200 || !j || !j.config || !j.caps) {
      $("app").innerHTML = '<div class="alert err">' + esc(t("loadFail", { e: st ? "HTTP " + st : "no reply" })) +
        '</div><p style="margin-top:12px"><button type="button" class="btn" data-a="reload">' + esc(t("reload")) + "</button></p>";
      return;
    }
    S.cfg = j.config;
    S.caps = j.caps;
    if (!S.cfg.transport) S.cfg.transport = S.caps.transport;
    var e = clone(S.cfg);
    e.sensors = e.sensors || [];
    e.i2c = e.i2c || {};
    if (e.board == null) e.board = 0;
    if (isW()) {
      e.net = e.net || {};
      e.net.pass = ""; e.net.token = ""; e.net.basic_pass = "";
      S.cfg.net = S.cfg.net || {};
      S.steps = ["net", "coll", "board", "sens", "node", "review"];
    } else {
      e.lmk = "";
      e.batt = e.batt || { pin: 2, divider: 2, trim: 1 };
      S.steps = ["coll", "board", "sens", "batt", "node", "review"];
    }
    S.ed = e;
    render();
    loadStatus();
    setInterval(function () { if ($("ov").hidden && S.steps[S.step] === "coll") loadStatus(); }, 5000);
  });
}

S.touched = {};
S.left = {};
var app = $("app");
app.addEventListener("input", onInput);
app.addEventListener("change", onInput);
document.addEventListener("click", function (ev) {
  var el = ev.target.closest ? ev.target.closest("[data-a]") : null;
  if (el && el.getAttribute("data-a") === "reload") { location.reload(); return; }
  onClick(ev);
});
boot();
})();
