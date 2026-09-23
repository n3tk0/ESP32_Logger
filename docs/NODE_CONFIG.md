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
   `""` together with `"<field>_set": false` means "clear it" — the only way
   to remove an optional secret (basic-auth password, the passphrase of a
   network that became open); it is also exactly what echoing a GET back
   sends for a secret that was never set, so an unedited echo changes nothing.
   A document written *with* secrets carries the `_set` flags too, so the
   collector sending `"basic_pass": "", "basic_pass_set": false` means "none",
   not "keep yours".
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

Decoding details (`src/nodecfg/NodeConfigJson.h`, shared by every side):

- `sensors`, when present, **replaces** the whole list. An entry that keeps its
  position and its `type` keeps the fields it does not mention; any other
  entry starts from that type's defaults for the chip. Changing a pulse
  entry's `mode` also resets `per_pulse` and `debounce_us` to that mode's
  defaults unless the document gives them.
- A value of the wrong type, or outside its field's storage range (a pin of
  262, an `interval_s` of 70000, a 17-character `name`), is an error with the
  same `{field, reason}` shape as a validation error — never wrapped or
  truncated. Decoding is all-or-nothing.
- `net` is read only on a WiFi node and `link`/`batt` only on an ESP-NOW
  node; sent to the other kind they are ignored like unknown keys. `sleep` is
  written only for an ESP-NOW node.
- `rev`/`local` are read only when the receiver asks for them (a node applying
  the collector's config, the collector reading a report), and
  `transport`/`hw`/`fw` only by the collector reading a report — never from a
  page POST.
- ESP-NOW node page only: `"lmk": ""` + `"lmk_set"` on GET; POST `lmk` with
  exactly 16 characters sets the key, `""` keeps it, `""` + `"lmk_set": false`
  returns to the compiled key. Never on the radio, never at the collector.

### 1.1 Sensors

| `type` | Fields | Metrics (name, unit) | Count | Sleep-safe |
|---|---|---|---|---|
| `bmx280` | `addr` (0x76/0x77, 0 = probe both) | temperature C, humidity %, pressure hPa, pressure_sea hPa | 4 | yes |
| `bme688` | `addr` | the above + gas_resistance Ohm | 5 | yes |
| `ds18b20` | `pin`, `count` (1–8), `metric` (≤10 chars, default `probe_temp`) | probe_temp, probe_temp_1, … C | count | yes |
| `bh1750` | `addr` (0x23/0x5C) | lux lx | 1 | yes |
| `sds011` | `rx`, `tx` | pm25, pm10 ug/m3 | 2 | **no** |
| `pulse` | `pin`, `mode` (`rain`/`flow`), `per_pulse`, `debounce_us` | rain_rate mm/h + rain_total mm, or flow_rate L/min + flow_total L | 2 | **no** |

Units are the ones the collector's own sensor plugins and the WiFi node
already publish (gas_resistance is `Ohm`, as `BME688Sensor` and node/ report
it — not kOhm), so a remote reading is indistinguishable from a wired one.

A ds18b20 entry's probe N publishes `<metric>` for N = 0 and `<metric>_N`
after that. With two ds18b20 entries every resulting name must still be
unique (see §1.2), so give each bus its own `metric`.

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

Also rejected — rules the list above implied but did not spell out, added
when the validator was written:

- two metrics with the same name anywhere in the config (two ds18b20 entries
  both called `probe_temp`, a ds18b20 `metric` of `humidity` beside a bmx280,
  a bus called `pool_1` beside a two-probe bus called `pool`) — the collector
  keys readings by (node, metric), so the second would silently overwrite the
  first;
- `pulse.pin == 16` on ESP8266 (no interrupt, same as `sds011.rx`);
- a pin that is not on the chip (ESP8266 > 16, ESP32-C3 > 21);
- ESP-NOW: `batt.pin` counts as a used pin, and must be ADC1 (GPIO 0–4; ADC2
  belongs to the radio);
- field values: bmx280/bme688 `addr` 0x76/0x77/0, bh1750 `addr` 0x23/0x5C,
  ds18b20 `count` 1–8 and `metric` of `[a-z0-9_]` starting with a letter,
  pulse `per_pulse > 0` and `debounce_us` ≤ 1 000 000, `board` a known board
  of the chip, `altitude_m` −500..9000;
- ESP-NOW: `batt.divider` 1..20, `batt.trim` 0.5..1.5, `link.ack_window_ms`
  5..1000, `link.rescan_fails` ≥ 1;
- WiFi: `net.ssid` and `net.host` required, `net.port` ≥ 1, `net.pass` and
  `net.next.pass` empty (open network) or at least 8 characters;
- `lmk` (ESP-NOW page) empty or exactly 16 characters.

The first failing rule is reported, in a fixed order (identity, sensors,
metrics, pins, transport sections). `field` is at most 23 characters and
`reason` at most 47, so both fit a CFG_ACK unchanged. Field paths used:
`name`, `interval_s`, `altitude_m`, `board`, `i2c.sda`, `i2c.scl`, `sensors`
(count/budget), `sensors[N].<type|addr|pin|count|metric|rx|tx|mode|per_pulse|debounce_us>`,
`batt.pin|divider|trim`, `link.ack_window_ms|rescan_fails`,
`net.ssid|pass|host|port|token|basic_user|basic_pass|next.ssid|next.pass`, `lmk`.
A sensor that is not allowed to sleep on a sleeping ESP-NOW node is reported
at `sensors[N].type`.

Warned (accepted, the page shows it): ESP8266 GPIO 0, 2, 15 (boot straps),
1, 3 (console), 16 (no interrupt/pull-up); ESP32-C3 GPIO 2, 8, 9 (straps),
18, 19 (USB), 20, 21 (console). Warnings come back as
`"warnings": [{"field","reason"}…]` next to `"ok"`.

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

Secrets in that reply: the collector only ever learns a node's secrets as
`_set` flags (a report carries none), so its stored copy can be stale. It
therefore sends a secret's value only when it was set through the collector
(`/api/nodes/config`, or the handover's `net.next.pass`) since the node last
applied a config; every other secret goes as `""` + `"<field>_set": true` —
"keep yours" under principle 5.

If the node reports `cfg` with `local: true`, the collector stores it as
`desired` (keeping its stored secrets where the node sent `""`), bumps rev,
and replies with that `cfg` (rev only is enough: `{"cfg":{"rev":6}}` means
"your local config is now rev 6"; the node clears `local`).

The node's JSON parse buffer for the reply must hold a full config: budget
2 KB. The collector accepts an ingest body of up to 6 KB (a full buffered
batch plus `cfg`).

A cycle with nothing queued still POSTs, with `"readings": []`, so config
keeps flowing to a node whose sensors all fail; the collector accepts an
empty array (only a missing one is a 400).

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

Both packets are built and checked by `src/nodecfg/UdpDiscovery.h`
(`udpdisc::buildQuery/parseQuery/buildReply/parseReply`, HMAC passed in; the
two firmware adapters are in `UdpDiscoveryHmac.h`). The HMAC key is the token
string's bytes; with no token on either side the key is empty, which both
ends compute identically. The name field is zero padded, and a packet with
bytes after the name's first NUL is refused; a reply with port 0 is refused.

## 4. Following the collector's network (handover)

1. The collector's network page saves a new SSID/pass (after its existing
   `/api/modules/wifi/test`). Instead of switching immediately, it calls
   `POST /api/nodes/handover {"action":"start","ssid":…,"pass":…,"form":{…}}`.
   `form` is optional: every field of the `/save_network` form, keyed by its
   form name (`wifiMode`, `clientSSID`, `clientPassword`, `useStaticIP`,
   `staticIP`, `gateway`, `subnet`, `dns`, `apSSID`, …), all strings, exactly
   as that form would post them (an unchecked checkbox is absent). When the
   collector switches (step 4) it applies `form` the way `/save_network`
   would; without it, it changes only `clientSSID`/`clientPassword`. It
   exists so a static address meant for the new network is not lost. The page
   only takes this path for a NEW client SSID while at least one node exists;
   otherwise it posts `/save_network` as before. A 404 from this endpoint also
   makes it fall back to `/save_network`.
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
   `linkFindChannel()` when the stored SSID/BSSID is not on the air (or is
   still up on the very channel the node is failing on while `next_ssid` is
   heard elsewhere — the old network need not go away). On success it is
   promoted, the old SSID becomes `next_ssid`, and `local` is set. And the
   collector now answers a signed DISCOVER **from a MAC already in its node
   table even when no pairing window is open**, so a node that slept through
   the handover finds it on its hourly sweep.
7. **Rollback** (WiFi node): after applying a config that changed `net.ssid`,
   `net.pass`, `net.host`, `net.port` or `net.token`, if no POST succeeds in
   5 cycles (and discovery finds nothing), restore `/config.prev.json`,
   restart, and report `cfg_error {"rev":<the rev rolled back from>,"field":"net","reason":"rolled back: no collector on new settings"}`.

## 5. ESP-NOW node ⇄ collector (radio)

All additions are **new message types and a new ACK flag**; `ESPNOW_PROTO_VER`
stays 1. An old node ignores unknown ACK flag bits; an old collector drops
unknown types in `espnowValidate()`.

- `EN_ACK_CFG_PENDING = 1 << 1` — set in ACK while `desired.rev > applied_rev`
  and the node has not answered `desired.rev` with a rejecting CFG_ACK (a
  rejected rev stays unapplied until someone edits it; flagging it anyway
  would have a battery node download it again every wake). The node also
  defends itself: it remembers the rev it refused, stops a fetch at the first
  slice of that rev, repeats its CFG_ACK, and backs off.
- `AckMsg.intervalS` (the legacy interval push, still honoured and written
  into the node's document unless `local` is set or the same ACK flags a
  pending config) must be `desired.interval_s` once the collector holds a
  desired config for the node, or 0 — any other value would undo an applied
  config on the next wake.
- `EN_MSG_CFG_GET = 5` (node → collector, encrypted unicast):
  `magic ver type nodeId | haveRev u16 | offset u16`.
- `EN_MSG_CFG = 6` (collector → node, encrypted unicast):
  `magic ver type nodeId | rev u16 | total u16 | offset u16 | len u8 | data[≤200]`.
  `data` is a slice of the §1 document as compact JSON, without `net`.
  The node requests offset 0, then each next offset, until `offset+len == total`
  (max total 1 KB), all within the same wake. A wake that runs out of time
  resumes next wake from offset 0. The node waits at most
  max(`link.ack_window_ms`, 50 ms) per slice and 400 ms per wake, so answer
  from the receive path. A CFG_GET whose `haveRev` equals `desired.rev` means
  the node already runs it (its CFG_ACK was lost): the node treats a first
  slice with `rev == haveRev`, or a CFG with `total == 0`, as "up to date"
  and repeats its CFG_ACK.
- `EN_MSG_CFG_ACK = 7` (node → collector): `magic ver type nodeId | rev u16 |
  status u8 (0 ok, 1 rejected) | field[24] | reason[48]`.
- `EN_MSG_CFG_REPORT = 8` (node → collector): same framing as `EN_MSG_CFG`
  (`rev` = node's current rev), sent on the first wake after boot and while
  `local == true`. The collector reassembles by (nodeId, rev, total) and
  answers the last chunk with a CFG with `total == 0` and the adopted `rev`
  (meaning "your local config is now rev N"). The node waits for that answer
  only when `local` is true (same per-slice window as CFG_GET) and repeats
  the report on a later wake if it does not come — so a repeated report of
  the same local document must be answered with the rev already adopted for
  it, not adopted again. A report with `local == false` needs no answer.
  The FIRST report from a node the collector holds no config for is adopted
  with the node table's label and interval in place of the node's own `name`
  and `interval_s` (the label is the id its readings are filed under), and
  flagged to the node as a new rev when they differ.
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
  14 battery_voltage. Values are in the catalogue's units — `pressure` hPa,
  `battery_voltage` **volts** (legacy DATA carried millivolts). Whole frame
  ≤ 250 bytes; the node packs as many whole samples as fit. The collector accepts both `EN_MSG_DATA` (legacy BME node)
  and `EN_MSG_DATA2`, ACKs both the same way.

  Precisely (as implemented in `listMetrics()` / `metricNameFor()`): the
  probe_temp `index` is the probe's **ordinal across all ds18b20 entries in
  list order** — a first bus with two probes has indexes 0 and 1, a second
  bus's first probe is 2 — and the collector names it from the reported
  config as that entry's `metric`, plus `_N` for probe N ≥ 1 *within the
  entry*. Before any config has been reported, index k is named
  `probe_temp` / `probe_temp_k`. An unknown metric id is dropped (the rest of
  the sample is kept); a non-finite value is dropped. A sample carries at most
  9 values (`EN_DATA2_MAX_VALUES`: the 8-metric budget + battery_voltage) and
  may carry none — a node whose sensors all failed still sends a frame, so it
  still gets its ACK. `count` ≥ 1.

  Framing rules `espnowValidate()` enforces: a CFG / CFG_REPORT slice has
  `len` ≥ 1 and `offset + len ≤ total ≤ 1024`, except the `total == 0` reply,
  which has `offset == 0` and `len == 0`; CFG_GET's `offset` < 1024;
  CFG_ACK's `status` is 0 or 1 and both strings are NUL-terminated inside
  their arrays. Reassembly (`EnCfgAssembler`) is strictly sequential: an
  offset-0 slice starts over, anything but the next expected offset of the
  same (nodeId, rev, total) is ignored.

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
  "boards": [{"id":0,"name":"NodeMCU V2/V3","pins":{"D0":16,"D1":5,…},
              "left":["A0","RSV",…], "right":["D0","D1",…]}, …],
  "forbidden_pins": [6,7,8,9,10,11],
  "warn_pins": {"0":"boot strap, must be high at reset", …},
  "max_sensors": 8, "max_metrics": 8 }
```

`encodeCaps()` (`src/nodecfg/NodeConfigJson.h`) also writes, all optional for
the page: `"sleep_unsafe": ["sds011","pulse"]` (refused on a sleeping ESP-NOW
node), `"metric_count": {"bmx280":4,…,"ds18b20":1}` (budget cost per entry;
ds18b20's is per probe, times `count`), `"max_gpio"` (16 / 21), and per board
`"left"` / `"right"` — header pads top to bottom, non-GPIO pads such as `GND`
included; absent for "other". Board ids are 0..2 on both chips: ESP8266
NodeMCU / Wemos D1 mini / bare ESP-12; ESP32-C3 Seeed XIAO / C3 SuperMini
(labels are the printed GPIO numbers) / other. Pin tables:
`src/nodecfg/HwPins.h`.

`boards[].left` / `right` (optional) are the header's pads top to bottom as
printed, for the diagram; pads that are not in `pins` (`GND`, `3V3`, `RST`…)
are drawn as plain pads. Without them the page draws a grid of `pins`. The
page accepts any key of the selected board's `pins` (case-insensitive),
`GPIOn` or a bare number as a pin, and always shows what it resolved to.

Additions the page relies on (all optional, absent = shown as "—"):

- **ESP-NOW key.** The ESP-NOW node's §1 document carries `"lmk": ""` and
  `"lmk_set": true|false`, a write-only secret under principle 5 (`""` on
  POST = keep; exactly 16 characters = set; anything else is refused with
  `field: "lmk"`). It exists only on this page's API: never in `EN_MSG_CFG` /
  `EN_MSG_CFG_REPORT` (principle 6), never in the collector's copy.
- **`/api/status` on the ESP-NOW node** adds `"paired": bool`, `"node_id"`,
  `"ch"` (the stored link state, since ESP-NOW is off while the page runs)
  and `"batt_v"` (last battery voltage, after divider and trim — the page's
  trim helper computes `trim_applied × measured / batt_v`).
- **`/api/scan` `enc`** is the core's own number (ESP8266 `ENC_TYPE_NONE`
  = 7, ESP32 `WIFI_AUTH_OPEN` = 0) or the strings `"open"`/`"none"`; the page
  reads it by `caps.hw`.

Validation errors name the field in the §1 spelling (`name`, `interval_s`,
`i2c.sda`, `sensors[1].pin`, `sensors[1].type`, `sensors` for the count and
the budget, `net.host`, `lmk`, `batt.trim`, …); the page takes the user to
the step holding that field and shows `reason` under it, or at the top of
the step when there is no such input. The page repeats the §1.2 rules as
live hints but never disables Save on them — the node's validator is the
authority, so a page copy that drifted stricter cannot lock anyone out.
After `{"ok":true}` it polls `/api/status` until `uptime_s` shows a restart;
on the setup AP that never comes (the restart closes the AP), so after ~30 s
it says so instead.

Where it runs: WiFi node — as today (AP portal, and on the LAN behind basic
auth). ESP-NOW node — press RESET (or power it up), then hold BOOT (GPIO9)
within 2 s; BOOT held *through* reset selects the C3's ROM download mode, so
the firmware watches for it after a power-on/reset instead. In mains mode
(`sleep: false`) holding BOOT for 1 s also works. It also opens by itself on
a power-on when the node has never paired and its key is still the shipped
placeholder. WPA2 AP `esp-node-XXXX` (pass `PORTAL_AP_PASS`), 5-minute
timeout paused while a station is connected, then restart. ESP-NOW is off
while the portal runs.

## 7. Collector HTTP API (for the Nodes page)

All POSTs need the CSRF token like every other settings POST.

| Method | Path | Body / reply |
|---|---|---|
| GET | `/api/nodes/config?key=w:balcony` (or `e:3`) | `{"key","transport","desired":{…secrets blanked…},"reported":{…}|null,"applied_rev","status","error","caps"}` — `caps` as §6 for the node's `hw`, from the reported config (defaults to the transport's hw) |
| POST | `/api/nodes/config` | `{"key":"w:balcony","config":{…partial §1…}}` → validates with the shared validator → `{"ok":true,"rev":5}` or 400 `{"ok":false,"field","reason"}` |
| GET | `/api/nodes/handover` | §4 step 3 |
| POST | `/api/nodes/handover` | `{"action":"start","ssid","pass","form"?}` (§4.1) / `{"action":"switch"}` / `{"action":"cancel"}` → `{"ok":true}`, or 4xx `{"ok":false,"reason"}` |

`GET /api/espnow/status` and `GET /api/remote/status` each gain, per node,
`"cfg": {"key","rev","applied_rev","status","error"?}` so the list can show the badge
without one request per row. `error` (`{"field","reason"}`, as in the
config GET) is present only when `status == "rejected"`, so the badge can say
why. A node the collector holds no config file for has no `cfg`.

Field paths in a 400 `field` (and in `cfg_error`/`error`) are dotted, with
list indices in brackets: `name`, `interval_s`, `i2c.sda`, `sensors[1].pin`,
`net.port`, `link.ack_window_ms`, `batt.pin`. `sensors` alone means the list
as a whole (too many entries, over the metric budget). The page puts the
reason beside the field the path names, and at the top of the panel when it
names none.

The Nodes page no longer calls `/api/espnow/node`: an ESP-NOW node's `name`
and `interval_s` saved through `/api/nodes/config` must also update the label
and interval that `/api/espnow/status` reports (and the interval the ACK
carries). `GET /api/nodes/handover` answers `active:false` once finished.

A WiFi node the collector has never seen a `cfg` report from still appears
(from `/api/remote/status`); its config GET answers `"reported": null`
(`desired` may be null too), its panel says "waiting for the node to report
its settings" and editing is disabled until it does. (The collector answers
every well-formed key it holds no file for that way — `desired`,
`reported`, `status` and `error` null, `applied_rev` 0 — never with a 404.)
Such a node was not handed the next network, so it stays `pending` for the
rest of a handover unless it is offline — the automatic switch waits, and
`{"action":"switch"}` overrides.
