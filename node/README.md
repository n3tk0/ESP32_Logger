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
| SDA      | D2        | 4    | BMx280 / BME688 / BH1750 |
| SCL      | D1        | 5    | BMx280 / BME688 / BH1750 |
| DQ       | D6        | 12   | DS18B20 (+ 4.7 kΩ to 3V3) |
| signal   | D7        | 13   | rain gauge / hall flow sensor |
| TXD      | D5        | 14   | SDS011 (sensor's TXD → node's RX) |

These are only the **defaults** — the setup portal lets you change them per
device, and shows only the pins whose sensor is in the build.

A BMP280 has no humidity sensor; the node detects which chip is fitted and
omits the humidity metric. Both I2C addresses (0x76 and 0x77) are probed, so a
breakout that shipped with SDO strapped the other way works untouched.

## Choosing sensors at build time

Same idea as the collector's `setup.h`: only what you enable is compiled in.
Edit the `NODE_SENSOR_*` block in `src/node_config.h`.

| Toggle | Interface | Metrics | Count |
|---|---|---|---|
| `NODE_SENSOR_BMX280` (default) | I2C | `temperature`, `humidity`, `pressure`, `pressure_sea` | 4 |
| `NODE_SENSOR_BME688` | I2C | the above plus `gas_resistance` | 5 |
| `NODE_SENSOR_DS18B20` | 1-Wire | `probe_temp`, `probe_temp_1`, … | 1–8 |
| `NODE_SENSOR_BH1750` | I2C | `lux` | 1 |
| `NODE_SENSOR_SDS011` | UART | `pm25`, `pm10` | 2 |
| `NODE_SENSOR_PULSE` | GPIO interrupt | `rain_rate`+`rain_total`, or `flow_rate`+`flow_total` | 2 |

The BMx280, BME688 and DS18B20 drivers are the collector's own, included
unmodified from `../src/drivers/`, so the compensation maths cannot drift
between a wired sensor and a remote one. BH1750, SDS011 and the pulse counter
have no shared driver to reuse — their device logic lives in the collector's
plugins, entangled with `ISensor` — so they are implemented directly in
`sensors.cpp`, matching the collector's frame parsing, scaling and metric
names.

### The 8-metric ceiling

The collector drains a remote node through the ordinary plugin path, and
`SensorManager` hands every plugin a fixed array of **8 readings per tick**.
A node publishing more is not an error anywhere — the surplus is simply not
copied, silently.

So the build counts what your selection emits and warns when it exceeds 8:

```
warning: #warning "This sensor set emits more than 8 metrics; the collector
copies only the first 8 per tick and drops the rest silently."
```

Set `NODE_DS18B20_EXPECTED` if you run more than one probe, so the count is
right. If you need more than 8, split the sensors across two nodes with
different `NODE_ID`s — the collector treats each as its own series.

### Pulse input: rain or water

One counter serves both, because a tipping-bucket reed switch and a hall-
effect flow sensor differ only in scale and in what the numbers are called.

```ini
-DNODE_SENSOR_PULSE
-DPULSE_MODE_RAIN=1              ; 0 for flow
-DPULSE_UNITS_PER_PULSE=0.2794f  ; mm per tip, or litres per pulse
-DPULSE_PIN=13
```

| Mode | Metrics | Typical scale |
|---|---|---|
| rain (`=1`) | `rain_rate` mm/h, `rain_total` mm | 0.2794 mm per tip (0.011″ bucket) |
| flow (`=0`) | `flow_rate` L/min, `flow_total` L | 0.00222 L per pulse (YF-S201, ~450/L) |

Debounce defaults differ on purpose: **10 ms for rain, 0 for flow**. A reed
switch bounces for a few milliseconds and tips at most a few times a second,
so 10 ms is generous. A hall flow sensor legitimately produces hundreds of
pulses a second, and any debounce large enough to help a reed switch would
silently cap the reading.

The rate is the **average over the interval just ended**, not extrapolated
from the gap between the last two pulses. For a node posting once a minute
the average is the honest number; extrapolation would report a downpour from
one tip that happened to land near the deadline.

`*_total` accumulates since boot and **resets on reboot** — the node has no
persistent counter. Trend the rate; treat the total as a since-power-on figure.

### SDS011 notes

The ESP8266's only hardware UART is the console, so the sensor is read over
SoftwareSerial at 9600 baud. The RX pin must be interrupt-capable — **GPIO16
will not work**. The node only listens: the SDS011 streams a frame a second by
default, and the newest complete, checksum-valid frame is what gets posted.

Its laser and fan have a rated life of about 8000 hours of continuous running.
This firmware does not duty-cycle it; for a permanent installation you would
want to.

Measured cost on `nodemcuv2` (1 044 464 bytes available):

| Build | Flash |
|---|---|
| DS18B20 only | 365 947 (35.0 %) |
| BMx280 only (default) | 373 363 (35.7 %) |
| BMx280 + BH1750 | 373 831 (35.8 %) |
| BMx280 + pulse | 374 659 (35.9 %) |
| BMx280 + DS18B20 | 375 335 (35.9 %) |
| BMx280 + SDS011 | 380 139 (36.4 %) |
| everything at once | 383 139 (36.7 %) |

Flash is not the constraint here — the 8-metric ceiling is.

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
   and the shared BME280 driver is reached through `-I..`. Opening the folder
   in the Arduino IDE will not build it.

## Setup portal

Settings live in `/config.json` on the node's LittleFS. The values in
`node_config.h` are no longer the configuration — they are the **defaults**
that seed it on a blank filesystem, so building with `-D` flags works exactly
as it did before the portal existed.

To configure without a cable: join the node's `esp-node-XXXX` network
(password `configure` by default — change `PORTAL_AP_PASS`). A phone normally
pops the "sign in to network" prompt straight into the form; otherwise open
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
  limit when the node has nothing to fall back to. If an SSID was compiled in
  or previously saved, it runs for five minutes after two failed associations
  and then goes back to retrying — see the table below. The clock is paused
  while somebody is connected to the AP, so it will not close mid-form, but
  it can close before you join.

The form is a four-step wizard, in the order the answers depend on each other:

| Step | Asks for |
|---|---|
| 1 · Network | SSID and passphrase. **Scan** lists what is on the air; tap a name to fill the field. |
| 2 · Collector | The ESP32's address and port, the ingest token, and the Basic Auth pair. |
| 3 · Board & pins | Which board this is, a diagram of its header, and the sensor pins. |
| 4 · This node | Node id, post interval, altitude — and a summary of the lot. |

Saving writes the config and restarts.

### The pin fields, and the mistake they used to allow

**Type either form: `D6` or `12`.** The line under each field says which pin
that resolved to, colour-coded, and the diagram above badges it on the header.

This is the whole reason step 3 looks the way it does. The fields ask for a
GPIO number, the board is printed with D-numbers, and the two disagree exactly
where it hurts: **D6 is GPIO12, while GPIO6 is the SPI flash clock**. The form
used to accept any number from 0 to 16 and write it to flash, so a BMP280
wired to the pads marked D6/D5 and entered as `6` and `5` put I2C on the flash
bus. The node then came up as

```
wdt reset
load 0x4010f000, len 3424, room 16
~ld
<garbage, forever>
```

on every boot, because the setting had been saved. Now:

- **GPIO6-11 are refused outright** — no wiring makes the flash bus work, so
  there is no "are you sure" for it either. The save is rejected with the
  reason and the stored config is left alone.
- **The silkscreen is accepted as input**, so nothing has to be translated by
  hand.
- **The awkward-but-usable pins are warned about, not banned**: GPIO0/GPIO2
  (strap, must be high at reset), GPIO15 (strap, must be low), GPIO1/GPIO3
  (the serial console) and GPIO16 (no interrupt, no pull-up). An I2C pull-up
  holding a strap pin high is how the vendor boards wire their own sensors.

The board selector changes the diagram, not the behaviour — the ESP8266's pins
are the same on all of them. The D-numbers are identical on the NodeMCU and
the D1 mini; the "bare ESP-12" option simply has no silkscreen to promise.

The pin fields shown depend on what is in the build: an I2C pair when a
BMx280/BME688 is compiled in, a 1-Wire pin when DS18B20 is. Offering a control
for a driver that is not present would be a control that does nothing, which
is worse than no control.

The table itself is `node/src/NodePins.h`, and `tests/host/test_node_pins.cpp`
checks it on the build host — that D6 is 12, that the flash bus is refused,
and that nothing merely awkward is refused with it.

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

Outside the portal the node runs no server and listens on no port — **unless**
a Basic Auth user and password are set. Then, and only then, the same form is
also served on the LAN for as long as the node is up, behind those
credentials. Leaving them empty is what the log means by:

```
[portal] background server NOT started: set a basic-auth user and password
         in the setup portal to enable configuration over the LAN
```

One pair, two jobs: it guards that LAN form, and it is what the node sends to
the collector when the collector was built with `WEB_BASIC_AUTH_ENABLED`. If
your collector uses Basic Auth, these have to be the collector's credentials —
you do not get to pick a different password for the portal.

## Pairing with the collector

The collector never contacts the node. The node POSTs to `/api/ingest`, and
the only thing that pairs the two is the **node id string** — compared
exactly, up to 16 characters. The node's IP is not entered anywhere on the
collector.

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
| WiFi down | Gives up the association attempt after 20 s, powers the radio down, retries next cycle. After two consecutive failures it offers the setup portal for 5 minutes, then goes back to retrying. Those five minutes are counted only while nobody is joined to the AP — a connected station means a human is mid-configuration, and the window used to close under them |
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
