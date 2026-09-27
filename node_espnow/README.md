# The ESP-NOW battery node

A XIAO ESP32-C3 with a BME280 and a 21700 cell. It wakes about once a minute,
reads its sensors, sends a few dozen bytes, waits a few milliseconds for an
acknowledgement and goes back to sleep. It never joins the WiFi network.

The BME280 is the default, not the limit: the node runs the shared config
document of [`../docs/NODE_CONFIG.md`](../docs/NODE_CONFIG.md), so its sensor
list, interval, pins, battery divider and radio tuning are set on its own setup
page or from the collector's Nodes page — no rebuild. With `sleep` turned off
it becomes a mains-powered node that stays awake, which is what an SDS011 or a
rain gauge needs.

This is not the node in [`../node/`](../node/README.md). That one is an ESP8266
that stays awake and posts JSON to `POST /api/ingest`, and it is not a battery
design. This one is. Both can be used at once; they arrive by different paths
and land in the same place.

The protocol, the reasoning behind it, and the collector's half are in
[`../docs/ESPNOW_NODE.md`](../docs/ESPNOW_NODE.md). This file is about building
and wiring the node.

**Nothing here has been run on hardware.** It compiles, and the parts that
could be tested without a radio are tested on the build host. Everything else
is a design statement.

---

## Wiring

| | XIAO ESP32-C3 | note |
|---|---|---|
| BME280 SDA | D4 / GPIO6 | `i2c.sda` (default `NODE_I2C_SDA`) |
| BME280 SCL | D5 / GPIO7 | `i2c.scl` (default `NODE_I2C_SCL`) |
| BME280 VCC | 3V3 | |
| Battery divider | A0 / GPIO2 | `batt.pin` (default `NODE_BATT_PIN`) |
| Cell + | BAT pad, underside | |
| Cell − | BAT pad, underside | |

### The divider is not optional

**The XIAO ESP32-C3 has no battery sense.** The BAT pads go to the charger and
the regulator and to no GPIO at all, so without a divider the node cannot tell
you anything about its cell — which is most of what this design is for.

```
   BAT+ ──┬── 220 kΩ ──┬── 220 kΩ ── GND
          │            │
       (charger)     A0 / GPIO2
```

Two 220 kΩ resistors, permanently connected. That draws about 9.5 µA at 4.2 V —
under a percent of the daily budget at one-minute intervals, which is cheaper
than the MOSFET and the GPIO it would take to switch the divider off, and one
less thing that can fail closed. Larger resistors would draw less and would
also stop the ADC's input settling within a sample.

A full cell at 4.2 V puts 2.1 V on the pin, comfortably inside the roughly
2.5 V the C3's ADC reaches at 11 dB attenuation.

### Trim it once

The node reads through `analogReadMilliVolts()`, which applies the calibration
burned into the chip at the factory — the raw ADC counts are out by up to 10 %
and would make the whole battery estimate fiction. What that does not correct
is the resistors, which are 1 % at best.

So: measure the cell with a meter, read what the node reports, and set the
ratio as `batt.trim` — the setup page's Battery step has a helper that does the
division from its live reading. It takes a minute and it is the difference
between a remaining-life figure that means something and one that is
confidently wrong.

## Settings

Every value in `src/node_config.h` marked *default* only seeds the config
document on a node that has none. After that the document in NVS is what
runs, and it changes in two ways:

- **On the node's own page** (below). A save there sets `local: true`; the node
  reports the document on its next contact and the collector adopts it as the
  new desired config. Local edits win.
- **From the collector.** When the collector holds a newer config for this
  node, its ACK says so and the node pulls it in the same wake, validates it
  with the same code the collector used, applies it and says whether it took
  it (or which field it refused and why). A refused config changes nothing.

The collector's older interval push (the ACK's interval field) still works
and is written into the document.

### The setup page

Press **RESET** (or plug the node in), then **hold BOOT** within two seconds.
Not the other way round: BOOT held *through* reset is the C3's ROM download
mode, and the firmware never runs. A node that has never been paired and has
only the placeholder key opens the page on its own at power-on.

Join the WPA2 network `esp-node-XXXX` (password `PORTAL_AP_PASS`, default
`configure` — change it) and a phone shows the page. It walks through the key
and pairing status, board and I2C pins, sensors, battery, and this node's
name, interval, altitude and sleep, validates as you go, and saves. The node
restarts to apply it. The page closes by itself after five minutes with
nobody connected, and ESP-NOW is off while it is up.

The ESP-NOW key can be typed there (exactly 16 characters). It is stored on
the node only: it never goes over the radio and the collector's page cannot
show or set it. Clearing it returns the node to the compiled `ESPNOW_LMK`.

### Mains mode

`sleep: false` keeps the node awake: it reports every `interval_s` from a
delay loop instead of deep sleeping, and the sensors keep running in between.
That is what lets it carry an SDS011 (the fan needs half a minute to give a
meaningful reading) or a pulse counter (it counts in an interrupt) — the
validator refuses both on a sleeping node. Holding BOOT for a second opens
the setup page without a reset. It is for USB power: at around 25 mA awake it
would flatten the 21700 in about a week.

## Building

```
cd node_espnow
pio run -e xiao_esp32c3          # the real thing
pio run -e xiao_esp32c3_bench    # stays awake, keeps the serial console
pio run -e xiao_esp32c3 -t upload
```

**Set the key first, on both sides.** `ESPNOW_LMK` here (a build flag, or
typed on the setup page) and `ESPNOW_LMK` in the collector's build must be the
same sixteen bytes. It encrypts the link and it authorises this node to be
adopted. If they differ, nothing pairs and nothing decrypts, and neither end
will say anything more useful than "bad signature".

**It needs a collector that speaks DATA2.** Readings go out as `DATA2` frames
(metric id + value, so any sensor list fits), which a collector built before
the config document drops as an unknown type — the node would then see no
ACKs and go looking for its network every hour.

The bench build stays awake between sends so a serial console can watch a
pairing attempt or a channel rescan happen. It says so at boot, because a node
flashed with it will flatten a cell in a couple of days.

## Pairing

1. Power-cycle the collector. With no nodes provisioned it listens for two
   minutes and says `pairing open` on its serial log.
2. Power the node. It sweeps channels 1–13 broadcasting a signed request, and
   the collector answers with the channel, the access point to look for, the
   clock, and a node number.
3. The node stores all of it in NVS and reports every interval from then on.

If the node prints `nobody answered`, the window was shut. Power-cycle the
collector and try again — the node retries once per interval, not continuously,
because sweeping in a loop would empty the cell before anyone got to the
collector.

A **second** node needs the window opened again, and there is no button for
that yet. `espnowBeginPairing()` on the collector is written and waiting for
one.

## Firmware updates

From the collector (Settings → Nodes, with an SD card in it): upload a
`firmware.bin` from `.pio/build/xiao_esp32c3/` and pick the nodes. Each one
downloads it over ESP-NOW in the wakes after its next report — up to 20 s of
radio a wake, three or four wakes for a whole image — checks it, and restarts
into it. A node whose battery is under the rollout's floor (3600 mV unless
set otherwise; only when a divider is fitted) waits and says so.

A new image is on trial until it hears its first acknowledgement. Three wakes
without one and the node goes back to the firmware it came from by itself, and
tells the collector, which shows the update as failed. Nothing to walk to.

On the node's own setup page, **Firmware** uploads a `.bin` directly. Only an
ESP-NOW node image is accepted (a collector or WiFi-node image is refused
before it is written); the node restarts into it a second later.

Every image carries a `NODEFW1|espnow-c3|<NODE_FW_VERSION>|` marker, printed
at boot. Bump `NODE_FW_VERSION` in `src/node_config.h` for a release: it is
what the pages show. What decides "done" is the image id, which every build
changes. The whole protocol is [docs/NODE_OTA.md](../docs/NODE_OTA.md).

## What it does when things go wrong

| what happened | what the node does |
|---|---|
| one frame lost | nothing; it is a shared band |
| three wakes with no answer | passive scan for the access point, take its channel |
| the collector moved to another network (`link.next_ssid`) | the scan finds the new network: switch to it, keep the old one as the fallback |
| access point not on the air | sweep for a collector again — at most once an hour |
| collector reflashed | the same sweep. It cannot tell you it forgot: an encrypted frame from a peer it no longer holds is dropped by the radio before any code runs |
| collector switched off | keep buffering; scan and sweep at most once an hour |
| collector flags a config and never sends it | give up after 400 ms, try again after 1, 3, 7 … 63 wakes |
| a new firmware never reaches the collector | three wakes, then back to the previous firmware; the collector is told |

Readings that could not be delivered are held in 1 KB of RTC memory — 35 of
them for a BME280 with battery, about half an hour at the default interval —
and sent when the link comes back, each with the time it was actually taken.
That is what the clock in the acknowledgement is for. One frame carries seven
of those beside the live reading, so a long outage drains over a few wakes,
oldest first. When the queue is full the **oldest** is dropped: losing the
start of an outage is better than losing the end of it.

The hourly limit on scanning is a **rate limit, not a schedule**. A channel
change at 14:03 is recovered at the next wake, not at 15:00. The ceiling exists
for the other case — a collector simply switched off — where scanning every
minute would cost more radio than reporting does.

## What it costs

| | one minute | thirty seconds |
|---|---|---|
| wake (~350 ms at ~60 mA average) | 8.4 mAh/day | 16.8 mAh/day |
| the acknowledgement window, early exit | 0.3 | 0.6 |
| deep sleep (~44 µA — the board, not the chip) | 1.06 | 1.06 |
| divider | 0.23 | 0.23 |
| **total** | **~10.0 mAh/day** | **~18.7 mAh/day** |
| from 4312 mAh usable | ~430 days | ~230 days |

A config change costs one extra round trip per 200 bytes of document, in the
wake after the change and never otherwise — typically four frames and a few
tens of milliseconds, about 0.001 mAh. There is no polling for configs: the
flag rides on the ACK the node already waits for.

At that scale the cell's own self-discharge stops being negligible — around 2 %
a month is roughly a quarter of the capacity over fourteen months. Expect
**10–11 months** at one-minute intervals and about seven at thirty seconds.

4312 mAh and not the 4900 on the label: the node runs from a 3.3 V regulator
that cannot regulate below its dropout, so the charge left in the cell under
about 3.4 V is unreachable.

Every figure above is arithmetic. None of it has been measured.

## What is shared with the collector, and why it matters

Headers reached through `-I..`, not copied:

| | |
|---|---|
| `src/espnow/EspNowProto.h` | the wire format |
| `src/espnow/EspNowAuth.h` | the DISCOVER and WELCOME signature |
| `src/nodecfg/` | the config document, its JSON codec, the validator, the metric catalogue, the pin tables and the setup page |
| `node_common/NodeSensors.h` | the sensor layer both node firmwares share |
| `src/drivers/BME280_Mini.h` | the same compensation maths on both ends |

`src/node_sensors_impl.cpp` compiles the shared implementation,
`node_common/NodeSensors.cpp`, into this firmware.

The logic that could be pulled away from the radio is in headers the host
tests compile: the RTC backlog (`src/Backlog.h`), the per-wake config fetch
and its backoff (`src/CfgFetch.h`), what a received config has to pass
(`src/CfgApply.h`) and the rescan decisions (`src/Rescan.h`) —
`tests/host/test_espnow_node_backlog.cpp` and
`tests/host/test_espnow_node_cfgfetch.cpp`.

Sharing them is what stops the two firmwares drifting apart. A signature in
particular is the kind of thing two implementations get subtly different — the
covered region, the truncation length — and the failure mode is a node that
pairs with nothing while both sides look correct in isolation.

CI builds this project on every change for exactly that reason.
