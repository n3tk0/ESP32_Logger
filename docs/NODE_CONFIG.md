# Node configuration: the contract

This file is the single source of truth for how a sensor node's settings are
shaped, stored, edited on the node itself, and edited from the collector —
for both the WiFi node (`node/`, ESP8266) and the ESP-NOW node
(`node_espnow/`, XIAO ESP32-C3). Every piece of code that touches node
configuration implements a part of this document; when they disagree, this
document wins and the code is the bug.

## 0. Principles

1. **The node always starts the conversation.** The WiFi node learns about new
   config in the reply to its own POST; the ESP-NOW node learns about it from a
   flag in the ACK to its own DATA frame. The collector never connects to a node.
2. **Desired vs applied.** The collector holds a *desired* config per node with
   a revision number `rev` (uint16, starts at 1, 0 = never synced). The node
   holds its *applied* config and the `rev` it came from. The UI shows
   `applied` / `pending (applied N → desired M)` / `rejected: <reason>`.
3. **The node validates everything it is sent**, with the same shared code the
   collector and the node page use (`src/nodecfg/`). A rejected config is
   reported with a reason and the node keeps running its previous config.
4. **Local edits win.** A change saved on the node's own page sets
   `local: true`. On next contact the node reports its full config; the
   collector adopts it as the new desired config with `rev = desired.rev + 1`,
   and sends that rev back so the node clears `local`.
5. **Secrets are write-only.** WiFi passphrase, ingest token and basic-auth
   password are never returned by any GET. A GET returns `""` plus a
   `<field>_set: true|false`; a POST with `""` means "keep the stored one".
   The collector does send secrets *to the WiFi node* in the ingest reply
   (see §3) — that is the one place they travel.
6. **The ESP-NOW key (LMK) never travels over the radio.** It can be typed on
   the ESP-NOW node's local page only.
7. **Intervals are seconds everywhere** (`interval_s`, 10..65535).

## 1. The config document (JSON, canonical)

```jsonc
{
  "rev": 4,                 // rev this config came from; 0 = never synced
  "local": false,           // edited on the node page, not yet adopted
  "transport": "wifi",      // "wifi" | "espnow" — read-only, reported by node
  "hw": "esp8266",          // "esp8266" | "esp32c3" — read-only
  "fw": "2026.09.1",        // read-only
  "name": "balcony",        // ≤16 chars, [A-Za-z0-9_-]. WiFi: the ingest node id.
                            // ESP-NOW: the label shown on the collector.
  "interval_s": 60,
  "altitude_m": 0.0,        // 0 = do not publish pressure_sea
  "board": 0,               // UI only: esp8266 0=NodeMCU 1=D1 mini 2=ESP-12;
                            //          esp32c3 0=XIAO C3 1=C3 SuperMini 2=other
  "sleep": true,            // ESP-NOW only: deep sleep between wakes.
                            // false = mains powered, stays awake.
  "i2c": { "sda": 4, "scl": 5 },       // shared by every I2C sensor
  "sensors": [ /* see §1.1, at most 8 entries */ ],

  "net": {                  // WiFi node only
    "ssid": "home", "pass": "", "pass_set": true,
    "host": "192.168.1.50", "port": 80,
    "token": "", "token_set": true,
    "basic_user": "", "basic_pass": "", "basic_pass_set": false,
    "next": { "ssid": "", "pass": "", "pass_set": false }   // see §4
  },

  "link": {                 // ESP-NOW node only
    "ack_window_ms": 30, "rescan_fails": 3, "rescan_min_s": 3600,
    "next_ssid": ""         // see §4
  },
  "batt": { "pin": 2, "divider": 2.0, "trim": 1.0 }   // ESP-NOW node only
}
```

Unknown keys are ignored on read (forward compatibility). Missing keys keep
the node's current value (a partial document is a valid edit).

### 1.1 Sensors

| `type` | Fields | Metrics (name, unit) | Count | Sleep-safe |
|---|---|---|---|---|
| `bmx280` | `addr` (0x76/0x77, 0 = probe both) | temperature C, humidity %, pressure hPa, pressure_sea hPa | 4 | yes |
| `bme688` | `addr` | the above + gas_resistance kOhm | 5 | yes |
| `ds18b20` | `pin`, `count` (1–8), `metric` (≤10 chars, default `probe_temp`) | probe_temp, probe_temp_1, … C | count | yes |
| `bh1750` | `addr` (0x23/0x5C) | lux lx | 1 | yes |
| `sds011` | `rx`, `tx` | pm25, pm10 ug/m3 | 2 | **no** |
| `pulse` | `pin`, `mode` (`rain`/`flow`), `per_pulse`, `debounce_us` | rain_rate mm/h + rain_total mm, or flow_rate L/min + flow_total L | 2 | **no** |

`pressure_sea` is only published when `altitude_m != 0` but is always counted
in the budget. The ESP-NOW node additionally always publishes
`battery_voltage` (not counted: the collector derives battery_percent and
battery_days itself).

### 1.2 Validation (shared, `src/nodecfg/NodeConfigValidate.h`)

Rejected, with a machine-readable `field` and a human `reason`:

- more than 8 sensor entries, or a total metric count > 8 (the collector's
  `MAX_METRICS_PER_TICK`);
- `bmx280` and `bme688` together (same metric names);
- two entries of the same `type` except `ds18b20` (one entry per bus pin);
- a pin used twice (I2C pair counts only if an I2C sensor is present);
- a forbidden pin — ESP8266: GPIO 6–11 (flash); ESP32-C3: GPIO 12–17 (flash);
- `sds011` or `pulse` when `transport == "espnow"` and `sleep == true`;
- `sds011.rx == 16` on ESP8266 (no interrupt);
- `interval_s` outside 10..65535; `name` empty/too long/bad characters;
- `ds18b20.metric` longer than 10 chars.

Warned (accepted, the page shows it): ESP8266 GPIO 0, 2, 15 (boot straps),
1, 3 (console), 16 (no interrupt/pull-up); ESP32-C3 GPIO 2, 8, 9 (straps),
18, 19 (USB), 20, 21 (console).

## 2. Where it is stored

| Who | Where |
|---|---|
| WiFi node | `/config.json` on LittleFS, the §1 document **with** secrets; previous copy in `/config.prev.json` (for rollback, §4). Old flat-format files (`ssid`, `intervalMs`, `i2cSda`, …) are migrated on first load; `NODE_SENSOR_*` build flags only seed the default sensor list on a blank filesystem. |
| ESP-NOW node | NVS namespace `cfg`, key `doc` = the §1 document (no secrets, no `net`); key `lmk` = the 16-byte key if set on the page (else the compiled one). Link state (`id`, `ch`, MAC, BSSID, SSID) stays where it is. |
| Collector | `/nodes/w_<name>.json` and `/nodes/e_<nodeId>.json` on LittleFS: `{ "desired": {…§1 with secrets…}, "applied_rev": 3, "reported": {…§1 without secrets…}, "status": "applied"\|"pending"\|"rejected", "error": {"field":"…","reason":"…"}, "seen": <epoch> }`. Secrets live only in `desired`. |

## 3. WiFi node ⇄ collector (over `/api/ingest`)

Request body gains optional fields (an old collector ignores them):

```jsonc
{ "node": "balcony", "readings": [ … ],
  "cfg_rev": 4,                          // always sent
  "cfg": { …§1, no secrets… },          // sent when local==true, or on the first POST after boot
  "cfg_error": { "rev": 5, "field": "sensors[1].pin", "reason": "GPIO6 is the flash bus" } // once, after rejecting
}
```

Reply gains, only when `desired.rev > cfg_rev` and no `cfg_error` for that rev:

```jsonc
{ "ok": true, "accepted": 12, …,
  "cfg": { …§1 WITH secrets, "rev": 5 … } }
```

The node applies it (validate → save with `/config.prev.json` backup →
restart if network/sensor fields changed, else apply live) and reports
`cfg_rev: 5` on the next POST. The collector marks `applied` when it sees
`cfg_rev == desired.rev`, `rejected` on `cfg_error`.

If the node reports `cfg` with `local: true`, the collector stores it as
`desired` (keeping its stored secrets where the node sent `""`), bumps rev,
and replies with that `cfg` (rev only is enough: `{"cfg":{"rev":6}}` means
"your local config is now rev 6"; the node clears `local`).

The node's JSON parse buffer for the reply must hold a full config: budget
2 KB.

### 3.1 Collector discovery (UDP, port 47810)

When the configured `host` does not answer (3 consecutive failed POSTs) or
right after a network switch (§4), the WiFi node broadcasts:

```
"ESPL?" (5) | nonce (8 random bytes) | name (16, zero padded) | tag (8)
tag = first 8 bytes of HMAC-SHA256(ingest token, all preceding bytes)
```

The collector, if the tag verifies against its INGEST_TOKEN, unicasts back:

```
"ESPL!" (5) | nonce echoed (8) | http port (uint16 LE) | tag (8, same scheme)
```

The node takes the sender IP as the new `net.host`, saves it, and marks it
`local: true` so the collector learns the change. HMAC-SHA256 comes from
`bearssl` on the ESP8266 and `mbedtls` on the collector.

## 4. Following the collector's network (handover)

1. The collector's network page saves a new SSID/pass (after its existing
   `/api/modules/wifi/test`). Instead of switching immediately, it calls
   `POST /api/nodes/handover {"action":"start","ssid":…,"pass":…}`.
2. The collector bumps `desired.rev` of every node: WiFi nodes get
   `net.next = {ssid, pass}`, ESP-NOW nodes get `link.next_ssid`.
3. `GET /api/nodes/handover` →
   `{"active":true,"ssid":"new","ready":["w:balcony"],"pending":["e:3"],"offline":["w:attic"]}`.
   A node is *ready* when its `applied_rev` ≥ the handover rev; *offline* when
   the collector's own offline rule says so.
4. The collector switches (saves its network config and restarts WiFi) when
   every non-offline node is ready, or on `{"action":"switch"}`. `{"action":"cancel"}`
   clears `next` on all nodes (another rev bump).
5. On the node: when the current network fails 2 cycles in a row and
   `next` is set, try `next`. On success, promote `next` to current (keep the
   old one as `next`, so a cancelled switch is survivable), run UDP discovery
   (§3.1), set `local: true`. If both fail, alternate; the setup portal
   behaves as today.
6. ESP-NOW node: it never needed the password. `link.next_ssid` is tried by
   `linkFindChannel()` when the stored SSID/BSSID is not on the air. And the
   collector now answers a signed DISCOVER **from a MAC already in its node
   table even when no pairing window is open**, so a node that slept through
   the handover finds it on its hourly sweep.
7. **Rollback** (WiFi node): after applying a config that changed `net.ssid`,
   `net.pass`, `net.host`, `net.port` or `net.token`, if no POST succeeds in
   5 cycles (and discovery finds nothing), restore `/config.prev.json`,
   restart, and report `cfg_error {"reason":"rolled back: no collector on new settings"}`.

## 5. ESP-NOW node ⇄ collector (radio)

All additions are **new message types and a new ACK flag**; `ESPNOW_PROTO_VER`
stays 1. An old node ignores unknown ACK flag bits; an old collector drops
unknown types in `espnowValidate()`.

- `EN_ACK_CFG_PENDING = 1 << 1` — set in ACK while `desired.rev > applied_rev`.
- `EN_MSG_CFG_GET = 5` (node → collector, encrypted unicast):
  `magic ver type nodeId | haveRev u16 | offset u16`.
- `EN_MSG_CFG = 6` (collector → node, encrypted unicast):
  `magic ver type nodeId | rev u16 | total u16 | offset u16 | len u8 | data[≤200]`.
  `data` is a slice of the §1 document as compact JSON, without `net`.
  The node requests offset 0, then each next offset, until `offset+len == total`
  (max total 1 KB), all within the same wake. A wake that runs out of time
  resumes next wake from offset 0.
- `EN_MSG_CFG_ACK = 7` (node → collector): `magic ver type nodeId | rev u16 |
  status u8 (0 ok, 1 rejected) | field[24] | reason[48]`.
- `EN_MSG_CFG_REPORT = 8` (node → collector): same framing as `EN_MSG_CFG`
  (`rev` = node's current rev), sent on the first wake after boot and while
  `local == true`. The collector reassembles by (nodeId, rev, total) and
  answers the last chunk with a CFG with `total == 0` and the adopted `rev`
  (meaning "your local config is now rev N").
- `EN_MSG_DATA2 = 9` (node → collector) — the dynamic-sensor data frame:

  ```
  header (12 bytes, same as DataMsg, type = 9, count = number of samples)
  per sample: dt_s u16 | n u8 | n × { metric u8 | index u8 | value float32 }
  ```

  Metric ids (the shared catalogue, `src/nodecfg/MetricCatalog.h`):
  1 temperature, 2 humidity, 3 pressure, 4 pressure_sea, 5 gas_resistance,
  6 lux, 7 pm25, 8 pm10, 9 rain_rate, 10 rain_total, 11 flow_rate,
  12 flow_total, 13 probe_temp (index = probe number; the name suffix `_N`
  is added for N ≥ 1, custom `metric` names map via the reported config),
  14 battery_voltage. Whole frame ≤ 250 bytes; the node packs as many whole
  samples as fit. The collector accepts both `EN_MSG_DATA` (legacy BME node)
  and `EN_MSG_DATA2`, ACKs both the same way.

## 6. The node's own page (both nodes)

Sources in `node_portal/` (`index.html`, `app.js`, `style.css`). `style.css`
is a hand-kept subset of `www/style.css` tokens and classes (`--bg`,
`--panel`, `--text`, `--accent`, …, `.card`, `.field`, `.input`, `.btn`,
`.seg`, `.badge`, `.hint`, dark theme via `prefers-color-scheme`);
`tools/check_node_portal_css.py` fails if a token value drifts from
`www/style.css`. `tools/build_node_portal.py` inlines and gzips the three
files into `src/nodecfg/NodePortalPage.h` (`NODE_PORTAL_GZ[]`, `PROGMEM`),
which is committed. Target: under 20 KB gzipped.

It is a single-page wizard driven entirely by the JSON API below; it
renders sections by `transport` and by `caps`. Steps: Network (WiFi) or
Collector (ESP-NOW: key, pairing status) → Board & I2C → Sensors (add /
remove / edit rows, live validation using `caps`) → Battery (ESP-NOW) →
This node (name, interval, altitude, sleep) → Review & Save.

HTTP API served by both nodes:

| Method | Path | Body / reply |
|---|---|---|
| GET | `/` | the page, `Content-Encoding: gzip` |
| GET | `/api/config` | `{ "config": {…§1, secrets blanked…}, "caps": {…} }` |
| POST | `/api/config` | §1 document (partial allowed) → `{"ok":true}` then restart after 500 ms, or `{"ok":false,"field":"…","reason":"…"}` (HTTP 400) |
| GET | `/api/scan` | starts a scan if none running; `{"state":"running"}` or `{"state":"done","nets":[{"ssid","rssi","ch","enc"}]}` |
| GET | `/api/status` | `{"uptime_s","rssi","last_ok_s","ip","mac","rev","local","collector":"reachable|unknown|unreachable"}` |

`caps`:
```jsonc
{ "transport": "wifi", "hw": "esp8266",
  "sensor_types": ["bmx280","bme688","ds18b20","bh1750","sds011","pulse"],
  "boards": [{"id":0,"name":"NodeMCU V2/V3","pins":{"D0":16,"D1":5,…}}, …],
  "forbidden_pins": [6,7,8,9,10,11],
  "warn_pins": {"0":"boot strap, must be high at reset", …},
  "max_sensors": 8, "max_metrics": 8 }
```

Where it runs: WiFi node — as today (AP portal, and on the LAN behind basic
auth). ESP-NOW node — hold BOOT (GPIO9) through reset, or no key/never
paired and no compiled defaults: WPA2 AP `esp-node-XXXX` (pass
`PORTAL_AP_PASS`), 5-minute timeout paused while a station is connected, then
restart. ESP-NOW is off while the portal runs.

## 7. Collector HTTP API (for the Nodes page)

All POSTs need the CSRF token like every other settings POST.

| Method | Path | Body / reply |
|---|---|---|
| GET | `/api/nodes/config?key=w:balcony` (or `e:3`) | `{"key","transport","desired":{…secrets blanked…},"reported":{…}|null,"applied_rev","status","error","caps"}` — `caps` as §6 for the node's `hw`, from the reported config (defaults to the transport's hw) |
| POST | `/api/nodes/config` | `{"key":"w:balcony","config":{…partial §1…}}` → validates with the shared validator → `{"ok":true,"rev":5}` or 400 `{"ok":false,"field","reason"}` |
| GET | `/api/nodes/handover` | §4 step 3 |
| POST | `/api/nodes/handover` | `{"action":"start","ssid","pass"}` / `{"action":"switch"}` / `{"action":"cancel"}` |

`GET /api/espnow/status` and `GET /api/remote/status` each gain, per node,
`"cfg": {"key","rev","applied_rev","status"}` so the list can show the badge
without one request per row.

A WiFi node the collector has never seen a `cfg` report from still appears
(from `/api/remote/status`); its panel says "waiting for the node to report
its settings" and editing is disabled until it does.
