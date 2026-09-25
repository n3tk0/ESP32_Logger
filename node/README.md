# Sensor node (ESP8266)

A satellite board that reads one or more environment sensors and POSTs the
values to an ESP32_Logger collector. No data storage, no display, and — outside
its setup page — no server and no listening port. Everything that looks at
this node's data looks at the collector.

Which sensors it reads, on which pins, how often and where it sends them is
its **config**: set on the node's own setup page, or from the collector's
Nodes page, which hands it to the node in the reply to its next POST. The
contract both sides implement is [`docs/NODE_CONFIG.md`](../docs/NODE_CONFIG.md);
this file is about the ESP8266 end of it.

## Why this is not a fork of the collector firmware

The collector is built on FreeRTOS tasks, an async web server, a ring buffer
and a sensor-plugin registry. None of that survives the move to an ESP8266
with 80 KB of RAM and no RTOS, and none of it is needed to read a few sensors
and make one HTTP request.

What *is* shared is the part that has to agree between the devices:

- the sensor drivers in `../src/drivers/`, included unmodified — same
  compensation maths on both ends, one place to fix it;
- the config model, its JSON, its validator and the metric catalogue in
  `../src/nodecfg/` — so the node, its page and the collector cannot disagree
  about what a setting means or whether it is allowed;
- the runtime sensor layer in `../node_common/NodeSensors.cpp`, which the
  ESP-NOW node (`../node_espnow/`) builds too.

Metric names and units match the collector's own plugins exactly —
`temperature`/`C`, `humidity`/`%`, `pressure`/`hPa` — so a remote reading and
a wired one are the same series shape downstream.

## Hardware

NodeMCU V3 (CH340) plus whichever sensors you wire to it. The default pins:

| Breakout | NodeMCU V3 | GPIO | Sensor |
|----------|-----------|------|--------|
| VCC      | 3V3       | —    | all    |
| GND      | GND       | —    | all    |
| SDA      | D2        | 4    | BMx280 / BME688 / BH1750 |
| SCL      | D1        | 5    | BMx280 / BME688 / BH1750 |
| DQ       | D6        | 12   | DS18B20 (+ 4.7 kΩ to 3V3) |
| signal   | D2        | 4    | rain gauge / hall flow sensor |
| TXD      | D5        | 14   | SDS011 (sensor's TXD → node's RX) |
| RXD      | D7        | 13   | SDS011 (only needed to command it) |

These are only **defaults** — every pin is part of the config and can be
changed per device. Note the pulse input and I2C SDA share GPIO4 by default:
there is no assignment that avoids every clash on eleven usable GPIOs, and a
config with both is refused until one of them moves (the page says which).

A BMP280 has no humidity sensor; the node detects which chip is fitted and
omits the humidity metric. With `addr` 0 both I2C addresses (0x76 and 0x77)
are probed, so a breakout that shipped with SDO strapped the other way works
untouched; a set address is tried first and the other one after it.

## Sensors are chosen at runtime

Every driver is compiled into every build; which ones run is the `sensors`
list of the config (at most 8 entries). Add, remove or re-pin a sensor on the
setup page or from the collector, and the node restarts into the new list —
no reflash.

| `type` | Interface | Metrics | Count |
|---|---|---|---|
| `bmx280` | I2C | `temperature`, `humidity`, `pressure`, `pressure_sea` | 4 |
| `bme688` | I2C | the above plus `gas_resistance` | 5 |
| `ds18b20` | 1-Wire | `probe_temp`, `probe_temp_1`, … (one entry per bus pin) | `count`, 1–8 |
| `bh1750` | I2C | `lux` | 1 |
| `sds011` | UART | `pm25`, `pm10` | 2 |
| `pulse` | GPIO interrupt | `rain_rate`+`rain_total`, or `flow_rate`+`flow_total` | 2 |

The BMx280, BME688 and DS18B20 drivers are the collector's own, included
unmodified from `../src/drivers/`. BH1750, SDS011 and the pulse counter have
no shared driver to reuse — their device logic lives in the collector's
plugins, entangled with `ISensor` — so they are implemented directly in
`../node_common/NodeSensors.cpp`, matching the collector's frame parsing,
scaling and metric names.

A sensor that is listed but not answering is retried every cycle, and its
metrics are simply absent until it does — the others still post. The boot log
and `/api/status` say what answered:

```
sensors: bmx280@0x76 ok, ds18b20x2@GPIO12 ok, pulse@GPIO4 rain
```

### The 8-metric ceiling

The collector drains a remote node through the ordinary plugin path, and
`SensorManager` hands every plugin a fixed array of **8 readings per tick**. A
node publishing more is not an error anywhere on the collector — the surplus
is simply not copied, silently.

So the validator (`../src/nodecfg/NodeConfigValidate.h`) counts what a config
would publish and **refuses** one over 8, on the page and on the collector
alike. `pressure_sea` counts even while the altitude is 0, so setting an
altitude later cannot push an accepted config over. If you need more than 8,
split the sensors across two nodes with different names — the collector treats
each as its own series.

### Two constraints worth knowing

**`bmx280` and `bme688` are mutually exclusive**, and a config with both is
refused. They do the same job and publish the same metric names, and the
collector's ingest table is keyed by `(node, metric)` — the second to post
would silently overwrite the first. The same rule refuses any two metrics with
the same name anywhere in the config.

**DS18B20 publishes under `probe_temp`, not `temperature`,** for the same
reason: alongside a BMx280 the two would collide. Each ds18b20 entry has a
`metric` field; on a DS18B20-only node nothing collides and you may prefer
`temperature`, so the series matches a wired DS18B20 elsewhere. Two buses need
two different names. Keep it at 10 characters or fewer, lower case —
`SensorReading::metric` is 16 bytes and the multi-probe `_1` suffix needs the
room.

The node now waits out the DS18B20 conversion (up to 750 ms at 12 bits) before
reading, as the collector's plugin does. It used to read straight after
asking, which returned the *previous* conversion — and 85 °C, the power-on
value, on the first cycle after boot.

### Pulse input: rain or water

One counter serves both, because a tipping-bucket reed switch and a hall-
effect flow sensor differ only in scale and in what the numbers are called.

| `mode` | Metrics | Typical `per_pulse` |
|---|---|---|
| `rain` | `rain_rate` mm/h, `rain_total` mm | 0.2794 mm per tip (0.011″ bucket) |
| `flow` | `flow_rate` L/min, `flow_total` L | 0.00222 L per pulse (YF-S201, ~450/L) |

Debounce defaults differ on purpose: **10 ms for rain, 0 for flow**, and
switching the mode resets both `per_pulse` and `debounce_us` to that mode's
defaults unless you give them. A reed switch bounces for a few milliseconds
and tips at most a few times a second, so 10 ms is generous. A hall flow
sensor legitimately produces hundreds of pulses a second, and any debounce
large enough to help a reed switch would silently cap the reading.

The rate is the **average over the interval just ended**, not extrapolated
from the gap between the last two pulses. For a node posting once a minute
the average is the honest number; extrapolation would report a downpour from
one tip that happened to land near the deadline.

`*_total` accumulates since boot and **resets on reboot** — the node has no
persistent counter, and a config change that restarts it resets it too. Trend
the rate; treat the total as a since-power-on figure.

GPIO16 cannot be a pulse input: it has no interrupt. The validator refuses it.

### SDS011 notes

The ESP8266's only hardware UART is the console, so the sensor is read over
SoftwareSerial at 9600 baud. The RX pin must be interrupt-capable — **GPIO16
will not work**, and is refused. The node only listens: the SDS011 streams a
frame a second by default, and the newest complete, checksum-valid frame is
what gets posted.

Its laser and fan have a rated life of about 8000 hours of continuous running.
This firmware does not duty-cycle it; for a permanent installation you would
want to.

### Build-time defaults

`src/node_config.h` and its `NODE_SENSOR_*` block no longer choose what is
compiled in. They **seed the config on a blank filesystem**: the WiFi
credentials, collector address, token, node name, interval, altitude, I2C pins
and the default sensor list (BMx280 only, out of the box), with each entry's
pins and options from the same file. After the first boot the node's own
config wins, and a reflash with different flags changes nothing on a node that
already has one — erase the filesystem to start over from them.

The seed is not validated before it runs, so the header still warns at compile
time when the default list would publish more than 8 metrics, and enabling
both `NODE_SENSOR_BMX280` and `NODE_SENSOR_BME688` is still a compile error.

`-U` will not turn the default sensor off. The toggles use
`#ifndef`/`#define`, so a `-U` on the command line is undone by the header —
and PlatformIO emits every `-D` before any `-U` anyway. Comment the block out
in `node_config.h` instead, exactly as the collector's `setup.h` works.

### How much it costs

Measured on `nodemcuv2` with every driver, the page and the config exchange in:
RAM 41 448 of 81 920 bytes (50.6 %), flash 461 283 of 1 044 464 bytes
(44.2 %). Flash is not the constraint; RAM is watched — see
`src/NodeCfgTables.h` and `src/NodeLog.h` for the two things that keep it
down on a part whose string constants live in RAM.

### Adding another sensor type

A new type is a contract change first: a `type` in `docs/NODE_CONFIG.md`
§1.1, its fields in `../src/nodecfg/NodeConfig.h` and the codec, its metrics
in `../src/nodecfg/MetricCatalog.h`, its rules in the validator. The driver
then goes in `../node_common/NodeSensors.cpp` — one `begin` function and one
case in `readEntry()` — and both nodes have it.

Note that `DS18B20_Mini.h` needs a small shim on this part: it guards its
1-Wire timing with FreeRTOS `portDISABLE_INTERRUPTS`, which the ESP8266 core
does not define. `NodeSensors.cpp` maps it onto `noInterrupts()` rather than
changing the shared driver. The critical sections are one bit each (~70 µs),
short enough not to disturb WiFi — a whole-frame lock would not be.

## Setup

1. **Enable ingest on the collector.** Build its firmware with:

   ```
   -DFEATURE_REMOTE_NODES -DINGEST_TOKEN='"pick-a-token"'
   ```

2. **Add a remote sensor** to the collector's `platform_config.json`:

   ```json
   {
     "id": "outdoor",
     "type": "remote",
     "enabled": true,
     "node": "balcony",
     "stale_after_ms": 600000,
     "read_interval_ms": 30000
   }
   ```

   `node` must match the node's name. `stale_after_ms` should be a
   comfortable multiple of the node's posting interval: at 60 s posts and a
   600 s window, nine missed posts are tolerated before the readings start
   being marked as errored. Too tight and one dropped packet flags the
   station as failed; too loose and a dead node keeps publishing a plausible
   frozen value.

3. **Configure the node.** Flash it, then use the setup page (below) — or
   seed the values at build time, which still works for a first boot. Either
   edit `src/node_config.h` (and keep it out of git) or override from
   `platformio.ini`:

   ```ini
   build_flags =
       -I..
       -DWIFI_SSID='"my-network"'
       -DWIFI_PASS='"my-password"'
       -DCOLLECTOR_HOST='"192.168.1.50"'
       -DINGEST_TOKEN='"pick-a-token"'
       -DNODE_ID='"balcony"'
       -DALTITUDE_M=350.0f
   ```

   Give the collector a DHCP reservation on your router. The node takes an IP
   address rather than an mDNS name — resolving one would need a second
   library. If the address does change, the node finds the collector again on
   its own (see *Finding the collector again* below), but a reservation means
   it never has to.

   If the collector was built with `WEB_BASIC_AUTH_ENABLED`, also set
   `COLLECTOR_BASIC_USER` and `COLLECTOR_BASIC_PASS`.

4. **Build and flash:**

   ```bash
   cd node
   pio run -t upload
   pio device monitor
   ```

   **Use that monitor, not the Arduino IDE's.** Two reasons, and the first
   catches people every time:

   - The ESP8266's boot ROM prints at **74880 baud** and this firmware prints
     at **115200**. Whichever one the terminal is set to, the other arrives as
     mojibake — so a monitor showing nothing but garbage is the normal
     appearance of a working board, not a broken one. Everything after the
     `ESP32_Logger sensor node` banner is readable at 115200.
   - `platformio.ini` sets `monitor_filters = esp8266_exception_decoder`,
     which turns a crash dump into a stack trace with function names. Without
     it a crash is a page of hex addresses, which is most of the reason a
     crashing node looks like a silent one.

   This is a PlatformIO project, not a sketch: `src/main.cpp` is not a `.ino`,
   and the shared code is reached through `-I..`. Opening the folder in the
   Arduino IDE will not build it.

## The setup page

The config lives in `/config.json` on the node's LittleFS — the
`docs/NODE_CONFIG.md` §1 document, secrets included — with the copy from
before the last change in `/config.prev.json`. Every save writes a temp file
and renames it, so a brownout mid-save leaves the old config intact. A
`/config.json` from before this format (flat `ssid`, `intervalMs`, `i2cSda`,
…) is migrated on first boot: its settings and pins are kept, and the sensors
it never recorded come from the `NODE_SENSOR_*` defaults.

The page is the same one the ESP-NOW node serves (`../node_portal/`, built
into `../src/nodecfg/NodePortalPage.h`), sent gzip-compressed straight out of
flash, and driven by a small JSON API: `GET /api/config`, `POST /api/config`,
`GET /api/scan`, `GET /api/status` (§6).

To reach it without a cable: join the node's `esp-node-XXXX` network
(password `configure` by default — change `PORTAL_AP_PASS`). A phone normally
pops the "sign in to network" prompt straight into the page; otherwise open
`http://192.168.4.1`.

**If the page will not load**, in the order these actually bite:

- **Type `http://` explicitly.** A browser that has seen HTTPS for a bare
  `192.168.4.1` will retry it as `https://`, and the node serves plain HTTP
  only. There is no certificate on an 80 MHz part.
- **Turn mobile data off.** The AP has no internet. Android and iOS both
  notice and quietly send everything over cellular instead, which looks
  exactly like a device that is not answering. The "stay connected?" prompt
  is the one to accept.
- **Check the window is still open.** The portal only runs without a time
  limit when the node has nothing to fall back to. If a network was compiled
  in or previously saved, it runs for five minutes after a failed round of
  association attempts and then goes back to retrying — see the table below.
  The clock is paused while somebody is connected to the AP, so it will not
  close mid-form, but it can close before you join.

The page is a wizard: **Network** (SSID and passphrase; **Scan** lists what is
on the air) → **Collector** (address, port, ingest token, Basic Auth, and what
the node last heard from it) → **Board & I2C** → **Sensors** (add, remove and
edit entries, with the metric budget counted as you go) → **This node** (name,
interval, altitude) → **Review & Save**. Saving validates the whole config on
the node — the page's own hints never block Save, the node's validator is the
authority — then writes it and restarts. A refusal names the field and the
page takes you to it.

A save on the page marks the config **local**: the next POST reports it to the
collector, which adopts it as its own copy, so the Nodes page and the node
agree again.

### The pin fields, and the mistake they used to allow

**Type either form: `D6` or `12`.** The page says which pin that resolved to,
and the board diagram badges it on the header.

This is the whole reason the pin fields look the way they do. The config holds
GPIO numbers, the board is printed with D-numbers, and the two disagree
exactly where it hurts: **D6 is GPIO12, while GPIO6 is the SPI flash clock**.
The setup form used to accept any number from 0 to 16 and write it to flash,
so a BMP280 wired to the pads marked D6/D5 and entered as `6` and `5` put I2C
on the flash bus. The node then came up as

```
wdt reset
load 0x4010f000, len 3424, room 16
~ld
<garbage, forever>
```

on every boot, because the setting had been saved. Now:

- **GPIO6-11 are refused outright** — by the validator, wherever the config
  comes from, and by the sensor layer again at bring-up. No wiring makes the
  flash bus work, so there is no "are you sure" for it either.
- **The silkscreen is accepted as input**, so nothing has to be translated by
  hand.
- **The awkward-but-usable pins are warned about, not banned**: GPIO0/GPIO2
  (strap, must be high at reset), GPIO15 (strap, must be low), GPIO1/GPIO3
  (the serial console) and GPIO16 (no interrupt, no pull-up). An I2C pull-up
  holding a strap pin high is how the vendor boards wire their own sensors.

The board selector changes the diagram, not the behaviour — the ESP8266's pins
are the same on all of them. The D-numbers are identical on the NodeMCU and
the D1 mini; the "bare ESP-12" option simply has no silkscreen to promise.

The pin tables are `../src/nodecfg/HwPins.h`, shared with the collector and
the ESP-NOW node. `src/NodePins.h` and `tests/host/test_node_pins.cpp` keep
the ESP8266 facts pinned down on the build host — that D6 is 12, that the
flash bus is refused, and that nothing merely awkward is refused with it.

**Secrets are write-only.** The WiFi passphrase, the ingest token and the
Basic Auth password come back **empty** from every GET, with a `_set` flag
saying whether one is stored; an empty field on save means "keep the saved
one". The stored values are never rendered into the page — putting them in
the page source would expose them to anyone who reaches it.

### When the portal opens — and when it closes

This is the part that matters for a node on a wall:

| Situation | Portal behaviour |
|---|---|
| No usable config (no network or no collector address) | Runs until configured. There is nothing else the node could be doing. |
| **FLASH** button held through reset | Runs until configured. The deliberate "let me in" path. |
| Config saved but WiFi keeps failing | Runs for **5 minutes**, then closes and retries — the saved network, or the collector's next one (below). Repeats. |

That third row is the important one. A portal that stayed up whenever WiFi was
down would turn a router reboot at 3 am into a node still parked in AP mode
the next afternoon, having missed a night of readings waiting for someone who
was asleep. Time-boxing it means the node self-heals when the router comes
back, while still being reachable in that window if the credentials genuinely
changed.

Each round is three association attempts of up to 20 s before the portal is
offered — one failure is usually a transient the next attempt clears, and
tearing the radio down to raise an AP costs a posting interval.

A network scan widens the radio from AP to AP+STA, and the ESP8266 has one
radio: a station that associates drags the AP onto its channel and drops the
phone standing in the portal. The node puts it back itself 20 s after a scan,
whether or not the page came back for the results.

### Security

The AP is WPA2, not open. It only exists while the node cannot reach its
network, but an open AP in that window would let anyone in range repoint the
node at their own collector. **Change `PORTAL_AP_PASS` from the default.**

Outside the portal the node runs no server and listens on no port — **unless**
a Basic Auth user and password are set. Then, and only then, the same page
and API are also served on the LAN for as long as the node is up, every route
behind those credentials. Leaving them empty is what the log means by:

```
[portal] background server NOT started: set a basic-auth user and password
         in the setup page to enable configuration over the LAN
```

One pair, two jobs: it guards that LAN page, and it is what the node sends to
the collector when the collector was built with `WEB_BASIC_AUTH_ENABLED`. If
your collector uses Basic Auth, these have to be the collector's credentials —
you do not get to pick a different password for the page.

## Configured from the collector

The collector never connects to the node. Every POST to `/api/ingest` carries
`cfg_rev` — the revision of the config the node is running — and, on the first
POST after boot and while a local edit is waiting to be adopted, the config
itself (without secrets). When the collector holds a newer revision for this
node, its reply carries that config, secrets included (the one place they
travel). The node then:

1. **validates** it with the same validator the page and the collector use.
   A refused config is reported once, as `cfg_error` with the field and the
   reason, and the node keeps running what it had; it never retries a
   revision it refused;
2. **backs up** the running config to `/config.prev.json` and saves the new
   one;
3. **applies** it — live for the name, interval, altitude and board;
   restarting for anything that touches the network, the I2C pins or the
   sensor list. Before a restart it hands over what the collector will take
   of its queue, since the queue is RAM.

The next POST reports the new `cfg_rev`, which is how the collector marks it
applied. A node whose sensors are all missing still POSTs every cycle (with no
readings), so a wrong sensor config can always be fixed from the collector.

### Rollback

A config from the collector that changes the WiFi network, the passphrase, the
collector's address or port, or the token decides whether the node can reach
anything at all. So it runs **on trial**: if no POST is answered within **5
cycles** (discovery included), the node restores `/config.prev.json`,
restarts on the old settings and reports `cfg_error` "rolled back: no
collector on new settings". The trial survives a restart (`/sync.json`); a
save on the node's own page ends it. The trial is written before the new
config, so a reset between the two writes never leaves new network settings
with no trial; a trial whose rev is not the saved config's is dropped at boot.

Five cycles are counted, not timed. When what the new settings got wrong is
the WiFi itself, each of those cycles is three association attempts and then
the setup portal's five minutes, so the rollback lands about half an hour
after the restart — later if somebody joins the portal, which holds it open.
That is deliberate: the portal is how a human fixes the same mistake sooner.

## Following the collector to a new network

When the collector's own WiFi is about to change, it first gives every node
the new network as `net.next` (a handover, `docs/NODE_CONFIG.md` §4). The node
keeps using its current network. If that fails **two cycles in a row**, it
tries `next`; once it joins, `next` becomes its network and the old one is
kept as `next`, so a cancelled switch is survivable. If both fail it
alternates between them, cycle by cycle, with the setup portal behaving as
above in between.

## Finding the collector again

After a network switch, and after **three POSTs in a row** that got no answer,
the node broadcasts a discovery query on UDP port 47810, signed with the
ingest token. A collector holding the same token answers with its HTTP port;
the node takes the answering address as its collector, saves it, and marks the
config local so the collector learns what its address is on this network. An
answer that is not signed with the token is ignored — nothing else on the LAN
can redirect the node.

## Pairing with the collector

The node POSTs to `/api/ingest`, and the only thing that pairs the two is the
**node name** — compared exactly, up to 16 characters. The node's IP is not
entered anywhere on the collector.

On the collector, add a sensor of type **`remote`** whose `node` field is that
string (leave `node` out and the sensor's own id is used instead):

```json
{ "id": "balcony", "type": "remote", "enabled": true,
  "interface": "http", "node": "balcony", "read_interval_ms": 30000 }
```

Its serial log says what it is listening for:

```
[balcony.remote] listening for node "balcony" (stale after 600000 ms)
```

A remote sensor has **no metrics until the first POST arrives**, and the
collector's dashboard draws one card per (sensor, metric) pair — so a
correctly configured node that has not reported yet shows up in the sensor
list and nowhere else. That is not a failure; it is one posting interval.

Renaming a node makes it a new node to the collector: the old name's history
stays under the old name.

## Altitude and pressure

Station pressure falls about 12 Pa per metre near sea level, so a node 300 m
up reads roughly 35 hPa below what a forecast quotes. Set the altitude and the
node publishes both:

- `pressure` — what this box actually experiences. This is the one to trend;
  a falling barometer means the same thing at any altitude.
- `pressure_sea` — the same reading normalised to sea level, which is what
  compares against a forecast or a neighbour's station.

Leave the altitude at 0 and only `pressure` is sent.

## What the node does on failure

| Situation | Behaviour |
|-----------|-----------|
| Sensor missing at boot | Retries the probe on every post cycle — a cold breakout that fails its first probe recovers without a power cycle |
| WiFi down | Three association attempts of up to 20 s, then the setup portal for 5 minutes, then back to retrying. Those five minutes are counted only while nobody is joined to the AP — a connected station means a human is mid-configuration, and the window used to close under them. With a handover `next` network set, it is tried after two failed cycles |
| Config lost or corrupt | Falls back to `/config.prev.json`, then to the compiled-in defaults; if those are incomplete, the portal comes up and waits |
| Collector unreachable | Keeps the readings and hands them over when it comes back — see below. After three failed POSTs it looks for the collector on the LAN |
| Collector's config refused | Keeps running the old one; reports the field and the reason once |
| Collector's network change reaches nothing | Rolls back after 5 cycles and says so |
| Wrong token | Collector answers 401; the message is printed on the serial monitor. The readings are kept, so fixing the token recovers the gap as well as the future |

## What it keeps while the collector is away

The node holds **192 readings** — an hour for a three-metric node at the
default one-minute interval, twenty-four minutes for an eight-metric one — in
a ring in RAM (`src/Backlog.h`). That is about 3 KB, and it is the whole cost.

The sensors are read **before the network is even looked at**, so the cycle
where the router is down is not the cycle whose reading is lost. Each entry
remembers `millis()`, not a date: this board has no clock, and what it can
always say honestly is *how long ago*. The POST turns that into `dt_s` per
reading and the collector, which does have NTP, turns it back into a timestamp.

Nothing leaves the ring until the collector says what it did with it. The reply
carries `accepted` — how many readings from the front of the batch it
consumed — and the node drops exactly that many and keeps the rest, in order.
So a 200 that took nothing (the collector's own history queue is full and
draining) costs no data: the batch is simply offered again next cycle. Up to
four batches of 48 go per cycle, which lets a node catch up several times
faster than it accumulates without looking like a flood.

When the outage outruns the buffer, the **oldest** readings go, and the count
is printed. Losing the start of an outage beats losing the end of it: the
recent hours are the ones the dashboard draws. On the collector's side the
same rule applies, so a reading does not survive one queue to be dropped by
the other's opposite opinion.

The ring is RAM, so a **reboot of the node** still loses what it was holding —
including the restart a config change causes, after the node has handed over
whatever the collector would take. It is the collector's chart that survives a
restart (`/trend.bin`), not the node's queue.

## Power

This firmware stays awake and posts on a timer, which suits a mains-powered
NodeMCU V3. It is not a battery design: the board's regulator and USB-serial
chip draw more idle than the ESP8266 does, so deep sleep would not buy much
without different hardware. (The config's `sleep` field is the ESP-NOW node's;
it means nothing here.)
