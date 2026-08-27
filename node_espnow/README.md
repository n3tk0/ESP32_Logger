# The ESP-NOW battery node

A XIAO ESP32-C3 with a BME280 and a 21700 cell. It wakes about once a minute,
reads the sensor, sends twenty-four bytes, waits a few milliseconds for an
acknowledgement and goes back to sleep. It never joins the WiFi network.

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
| BME280 SDA | D4 / GPIO6 | `NODE_I2C_SDA` |
| BME280 SCL | D5 / GPIO7 | `NODE_I2C_SCL` |
| BME280 VCC | 3V3 | |
| Battery divider | A0 / GPIO2 | `NODE_BATT_PIN` |
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
ratio as `NODE_BATT_TRIM`. It takes a minute and it is the difference between a
remaining-life figure that means something and one that is confidently wrong.

## Building

```
cd node_espnow
pio run -e xiao_esp32c3          # the real thing
pio run -e xiao_esp32c3_bench    # stays awake, keeps the serial console
pio run -e xiao_esp32c3 -t upload
```

**Set the key first, on both sides.** `ESPNOW_LMK` in `platformio.ini` here and
`ESPNOW_LMK` in the collector's build must be the same sixteen bytes. It
encrypts the link and it authorises this node to be adopted. If they differ,
nothing pairs and nothing decrypts, and neither end will say anything more
useful than "bad signature".

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

## What it does when things go wrong

| what happened | what the node does |
|---|---|
| one frame lost | nothing; it is a shared band |
| three wakes with no answer | passive scan for the access point, take its channel |
| access point not on the air | sweep for a collector again — at most once an hour |
| collector reflashed | the same sweep. It cannot tell you it forgot: an encrypted frame from a peer it no longer holds is dropped by the radio before any code runs |
| collector switched off | keep buffering, scan at most once an hour |

Readings that could not be delivered are held in RTC memory — up to fifteen,
about a quarter of an hour at the default interval — and sent as one burst when
the link comes back, each with the time it was actually taken. That is what the
clock in the acknowledgement is for. When the queue is full the **oldest** is
dropped: losing the start of an outage is better than losing the end of it.

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

At that scale the cell's own self-discharge stops being negligible — around 2 %
a month is roughly a quarter of the capacity over fourteen months. Expect
**10–11 months** at one-minute intervals and about seven at thirty seconds.

4312 mAh and not the 4900 on the label: the node runs from a 3.3 V regulator
that cannot regulate below its dropout, so the charge left in the cell under
about 3.4 V is unreachable.

Every figure above is arithmetic. None of it has been measured.

## What is shared with the collector, and why it matters

Three headers, reached through `-I..`, not copied:

| | |
|---|---|
| `src/espnow/EspNowProto.h` | the wire format |
| `src/espnow/EspNowAuth.h` | the DISCOVER and WELCOME signature |
| `src/drivers/BME280_Mini.h` | the same compensation maths on both ends |

Sharing them is what stops the two firmwares drifting apart. A signature in
particular is the kind of thing two implementations get subtly different — the
covered region, the truncation length — and the failure mode is a node that
pairs with nothing while both sides look correct in isolation.

CI builds this project on every change for exactly that reason.
