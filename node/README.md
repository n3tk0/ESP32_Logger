# Sensor node (ESP8266)

A satellite board that reads one or more environment sensors and POSTs the
values to an ESP32_Logger collector. No data storage, no display, and — outside the setup
portal — no server and no listening port. Everything that looks at this node's
data looks at the collector.

## Why this is not a fork of the collector firmware

The collector is built on FreeRTOS tasks, an async web server, LittleFS, a
ring buffer and a sensor-plugin registry. None of that survives the move to an
ESP8266 with 80 KB of RAM and no RTOS, and none of it is needed to read one
I2C sensor and make one HTTP request.

What *is* shared is the part that has to agree between the two devices: the
sensor drivers in `../src/drivers/`, included unmodified. Same compensation
maths on both ends, one place to fix it.

Metric names and units match the collector's own plugins exactly —
`temperature`/`C`, `humidity`/`%`, `pressure`/`hPa` — so a remote reading and
a wired one are the same series shape downstream.

## Hardware

NodeMCU V3 (CH340) plus whichever sensors you selected at build time.

| Breakout | NodeMCU V3 | GPIO | Sensor |
|----------|-----------|------|--------|
| VCC      | 3V3       | —    | all    |
| GND      | GND       | —    | all    |
| SDA      | D2        | 4    | BMx280 / BME688 |
| SCL      | D1        | 5    | BMx280 / BME688 |
| DQ       | D6        | 12   | DS18B20 (+ 4.7 kΩ to 3V3) |

These are only the **defaults** — the setup portal lets you change them per
device, and shows only the pins whose sensor is in the build.

A BMP280 has no humidity sensor; the node detects which chip is fitted and
omits the humidity metric. Both I2C addresses (0x76 and 0x77) are probed, so a
breakout that shipped with SDO strapped the other way works untouched.

## Choosing sensors at build time

Same idea as the collector's `setup.h`: only what you enable is compiled in.
Edit the `NODE_SENSOR_*` block in `src/node_config.h`.

| Toggle | Driver | Metrics |
|---|---|---|
| `NODE_SENSOR_BMX280` (default) | `BME280_Mini` | `temperature`, `humidity`, `pressure`, `pressure_sea` |
| `NODE_SENSOR_BME688` | `BME688_Mini` | the above plus `gas_resistance` |
| `NODE_SENSOR_DS18B20` | `DS18B20_Mini` | `probe_temp`, `probe_temp_1`, … (up to 8 on one pin) |

All three drivers are the collector's own, included unmodified from
`../src/drivers/`, so the compensation maths cannot drift between a wired
sensor and a remote one.

Measured cost on `nodemcuv2`:

| Build | Flash |
|---|---|
| DS18B20 only | 365 947 (35.0 %) |
| BMx280 only | 373 071 (35.7 %) |
| BMx280 + DS18B20 | 375 335 (35.9 %) |
| BME688 + DS18B20 | 375 543 (36.0 %) |

### Two constraints worth knowing

**BMx280 and BME688 are mutually exclusive** and enabling both is a compile
error. They do the same job and publish the same metric names, and the
collector's ingest table is keyed by `(node, metric)` — the second to post
would silently overwrite the first.

**DS18B20 publishes under `probe_temp`, not `temperature`,** for the same
reason: alongside a BMx280 the two would collide. On a DS18B20-only node
nothing collides and you may prefer the plain name so the series matches a
wired DS18B20 elsewhere:

```ini
-DNODE_DS18B20_METRIC='"temperature"'
```

Keep that name at 10 characters or fewer — `SensorReading::metric` is 16 bytes
and the multi-probe `_1` suffix needs the room.

### Turning the default off

`-U` will not do it. The toggles use `#ifndef`/`#define`, so a `-U` on the
command line is undone by the header — and PlatformIO emits every `-D` before
any `-U` anyway. Comment the block out in `node_config.h` instead, exactly as
the collector's `setup.h` works.

### Adding a fourth sensor

Everything sensor-specific lives in `src/sensors.cpp` behind three functions,
so main.cpp does not grow an `#ifdef` per driver. A new sensor is one
self-contained edit there, a toggle in `node_config.h`, and — if it needs a
pin — a field in `NodeSettings` plus a `row()` in the portal's guarded block.

Note that `DS18B20_Mini.h` needs a small shim on this part: it guards its
1-Wire timing with FreeRTOS `portDISABLE_INTERRUPTS`, which the ESP8266 core
does not define. `sensors.cpp` maps it onto `noInterrupts()` rather than
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

   `node` must match the node's `NODE_ID`. `stale_after_ms` should be a
   comfortable multiple of the node's posting interval: at 60 s posts and a
   600 s window, nine missed posts are tolerated before the readings start
   being marked as errored. Too tight and one dropped packet flags the
   station as failed; too loose and a dead node keeps publishing a plausible
   frozen value.

3. **Configure the node.** Flash it, then use the setup portal (below) — or
   bake the values in at build time, which is still supported. Either edit
   `src/node_config.h` (and keep it out of git) or override from
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
   library, and the reservation is the more reliable fix anyway.

   If the collector was built with `WEB_BASIC_AUTH_ENABLED`, also set
   `COLLECTOR_BASIC_USER` and `COLLECTOR_BASIC_PASS`.

4. **Build and flash:**

   ```bash
   cd node
   pio run -t upload
   pio device monitor
   ```

## Setup portal

Settings live in `/config.json` on the node's LittleFS. The values in
`node_config.h` are no longer the configuration — they are the **defaults**
that seed it on a blank filesystem, so building with `-D` flags works exactly
as it did before the portal existed.

To configure without a cable: join the node's `esp-node-XXXX` network
(password `configure` by default — change `PORTAL_AP_PASS`). A phone normally
pops the "sign in to network" prompt straight into the form; otherwise open
`http://192.168.4.1`.

The form covers WiFi, collector address and port, ingest token, optional Basic
Auth, **sensor pins**, node id, post interval and altitude. Saving writes the
config and restarts.

The pin fields shown depend on what is in the build: an I2C pair when a
BMx280/BME688 is compiled in, a 1-Wire pin when DS18B20 is. Offering a control
for a driver that is not present would be a control that does nothing, which
is worse than no control. GPIO numbers outside 0-16 are rejected and keep the
previous value.

Password fields come back **empty** and mean "keep the saved one". The stored
passphrase is never rendered into the page — putting it in the page source
would expose it to anyone who reaches the portal.

### When the portal opens — and when it closes

This is the part that matters for a node on a wall:

| Situation | Portal behaviour |
|---|---|
| No usable config saved | Runs until configured. There is nothing else the node could be doing. |
| **FLASH** button held through reset | Runs until configured. The deliberate "let me in" path. |
| Config saved but WiFi keeps failing | Runs for **5 minutes**, then closes and retries the saved network. Repeats. |

That third row is the important one. A portal that stayed up whenever WiFi was
down would turn a router reboot at 3 am into a node still parked in AP mode
the next afternoon, having missed a night of readings waiting for someone who
was asleep. Time-boxing it means the node self-heals when the router comes
back, while still being reachable in that window if the credentials genuinely
changed.

Two clean association failures are required before the portal is offered — one
is usually a transient the next cycle clears, and tearing the radio down to
raise an AP costs a posting interval.

### Security

The AP is WPA2, not open. It only exists while the node cannot reach its
network, but an open AP in that window would let anyone in range repoint the
node at their own collector. **Change `PORTAL_AP_PASS` from the default.**

Outside the portal the node runs no server and listens on no port.

## Altitude and pressure

Station pressure falls about 12 Pa per metre near sea level, so a node 300 m
up reads roughly 35 hPa below what a forecast quotes. Set `ALTITUDE_M` and the
node publishes both:

- `pressure` — what this box actually experiences. This is the one to trend;
  a falling barometer means the same thing at any altitude.
- `pressure_sea` — the same reading normalised to sea level, which is what
  compares against a forecast or a neighbour's station.

Leave `ALTITUDE_M` at 0 and only `pressure` is sent.

## What the node does on failure

| Situation | Behaviour |
|-----------|-----------|
| Sensor missing at boot | Retries the probe on every post cycle — a cold breakout that fails its first probe recovers without a power cycle |
| WiFi down | Gives up the association attempt after 20 s, powers the radio down, retries next cycle. After two consecutive failures it offers the setup portal for 5 minutes, then goes back to retrying |
| Config lost or corrupt | Falls back to the compiled-in defaults; if those are incomplete, the portal comes up and waits |
| Collector unreachable | Logs the error and drops that sample; there is no local buffer |
| Wrong token | Collector answers 401; the message is printed on the serial monitor |

There is deliberately no local buffering. A gap in outdoor temperature is
visible and self-explanatory on the dashboard; a node replaying an hour of
stale samples after a reconnect is neither.

## Power

This firmware stays awake and posts on a timer, which suits a mains-powered
NodeMCU V3. It is not a battery design: the board's regulator and USB-serial
chip draw more idle than the ESP8266 does, so deep sleep would not buy much
without different hardware.
