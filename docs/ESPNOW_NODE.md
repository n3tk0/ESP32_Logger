# The battery node, over ESP-NOW

A second kind of sensor node: a XIAO ESP32-C3 with a BME280 and a 21700 cell,
deep sleeping between measurements and reporting over ESP-NOW instead of WiFi.

The existing ESP8266 node in [`../node/`](../node/README.md) is untouched by any
of this. It stays awake, posts JSON to `POST /api/ingest`, and is not a battery
design. This is the battery design, and it is a separate device, a separate
firmware and a separate ingest path.

**Status.** Both ends are implemented. The collector is behind
`FEATURE_ESPNOW_INGEST`: the wire format (`src/espnow/EspNowProto.h`), the
signature (`src/espnow/EspNowAuth.h`), the battery model
(`src/power/BatteryModel.h`), the node bookkeeping (`src/espnow/NodeTable.h`)
and the radio (`src/espnow/EspNowIngest.cpp`). The node firmware is
[`node_espnow/`](../node_espnow/README.md), a separate PlatformIO project that
compiles three of those headers so the two cannot drift apart. The dashboard
warning badge is not written yet.

Nothing has been run on hardware. Everything below that describes behaviour on
a board is a design statement, not an observation, and the two places most
likely to need revising once one exists are marked where they appear.

---

## 1. Why the node has to heal itself

The failure this design has to survive is the router moving to another channel.
It happens on its own — most consumer access points re-pick a channel on reboot,
after a firmware update, or when their automatic channel selection decides the
band got noisy.

When it happens, the collector follows: it is a station, it reconnects, and it
lands on the new channel. The node does not follow, because it was asleep. It
wakes, transmits on the old channel, and nobody hears it.

The obvious fix is to have the collector notice the silence and do something
about it. **It cannot, and the reason is worth writing down so it does not get
re-proposed.** To reach a node sitting on channel 6, the collector has to be on
channel 6 — and it cannot be, because it is a station associated to an access
point on channel 11 and moving off that channel drops the link. It could hop
briefly, but the node is awake for roughly a third of a second per minute and at
an unknown offset, so "briefly" means holding the wrong channel for a full wake
interval. That is a minute with no WiFi, no web interface and no exporters,
repeated until the node happens to be listening.

So the collector cannot heal the link. What it *can* do is notice, and that is
worth having on its own account:

* it knows each node's expected interval from provisioning, so silence past
  three intervals means something is wrong;
* it can say so — a node marked offline on the web interface and on the Kindle
  dashboard, rather than a stale reading that looks current.

That is the collector's half of the job. The healing is the node's.

## 2. The node's half: an acknowledgement it stays awake for

After transmitting, the node holds the radio in receive for a short window and
waits for an `AckMsg` from the collector.

```
wake → read sensors → send DATA → listen (≤ 30 ms, exits early on arrival) → sleep
```

Three things ride on that reply, and each pays for the window on its own:

**Time.** The XIAO ESP32-C3 has no 32 kHz crystal fitted, so deep sleep is timed
by the internal RC oscillator, which drifts by percent, not by parts per
million. The node has no NTP either — it never associates to the access point.
The `epoch` in the ACK is its only source of wall-clock time, and it needs one
to timestamp readings it had to buffer.

**Configuration.** `intervalS` lets the wake period be changed from the
collector's web interface, without walking to a node that may be behind a wall
or on a roof.

**Liveness.** This is the part that heals the channel. The radio's own send
callback reports whether the frame was acknowledged at the MAC layer, which
catches a wrong channel; the application-level ACK catches strictly more, and
its absence is what the node acts on.

A correction to an earlier version of this document, because it matters for
what the node has to do on its own. A collector that was reflashed and lost its
peer table **cannot tell the node so.** ESP-NOW decrypts an incoming frame only
from a peer it already holds the key for, so the node's reports are dropped by
the radio before any code on the collector runs. `EN_ACK_REDISCOVER` exists and
is correct, but it is reachable only in the narrow case where the peer entry
survived and the node record did not.

Recovery from a reflashed collector is therefore the node's, by the same route
as everything else: enough unanswered wakes, and it runs the pairing sweep
again.

### What the window costs

Receive on an ESP32-C3 draws around 85 mA. A fixed 30 ms window at one wake per
minute would be 1.0 mAh/day against a budget of about 9.7 — a tenth of the
battery for something that normally completes in single-digit milliseconds.

So the window is a **ceiling, not a duration**: the node leaves receive the
moment the frame arrives. The collector replies straight from its receive
callback with no filesystem or network work in between, so the usual turnaround
is a few milliseconds and the average cost lands near 0.3 mAh/day. The 30 ms
only gets spent on the wakes where the reply never comes — which are exactly the
wakes that are about to trigger a rescan anyway.

### And the rescan itself

Three consecutive wakes with no reply, and the node does a passive scan for its
access point, takes the channel it finds, stores it, and carries on.

Two bounds on that, in opposite directions:

* **At most once an hour.** A collector that is simply switched off would
  otherwise make the node scan every single minute, and a scan is 1.5–2 s of
  radio — an order of magnitude more than a normal wake. The hourly ceiling
  turns a dead collector from a battery emergency into a rounding error.
* **Immediately on the first eligible failure.** The ceiling is a rate limit,
  not a schedule. A channel move at 14:03 is recovered at the next wake, not at
  15:00, because the hour is measured from the last scan and not from a clock.
  Losing an hour of readings to a router reboot would be the wrong trade.

The node stores **both the BSSID and the SSID** of the access point. The BSSID
is exact; the SSID is the fallback, because a mesh or a repeater changes the
BSSID under you while the SSID stays put.

## 3. Provisioning

The shared key (LMK, 16 bytes) is set on both sides at build time, which is what
removes the chicken-and-egg problem of exchanging a key over a link that needs
the key:

```
-DFEATURE_ESPNOW_INGEST -DESPNOW_LMK='"16-byte-secret!!"'
```

**One key for every node, not one per node.** A per-node key would be better —
one compromised node would then not be every node — but it needs somewhere to
store eight of them and a UI to enter them, and this feature has neither yet.
For one to three nodes on a home network that is the right trade; it is written
here so it is a decision rather than an oversight.

```
Node with no stored channel:
    for ch in 1..13:  broadcast DISCOVER{mac, nodeId, nonce, HMAC}, wait ~120 ms

Collector, only while a pairing window is open:
    verify the HMAC against the configured keys
    add the peer with its LMK
    reply, unicast and encrypted:  WELCOME{nodeId, channel, ssid, bssid, interval, epoch}

Node: store it all in NVS and sleep.
```

**WELCOME is broadcast and signed, not unicast and encrypted, and that is
forced.** ESP-NOW decrypts an incoming frame only from a peer already added
with the key — so for the node to receive an encrypted WELCOME it would have to
have added the collector as a peer already, which means knowing the collector's
MAC address, which is what the WELCOME is for. No ordering resolves that.

So the reply goes out in the clear to the broadcast address, carries the MAC it
is meant for, and is authenticated exactly as DISCOVER is. A node ignores one
addressed to somebody else, and one whose tag does not verify — and the target
field is inside the signed region, so a valid WELCOME cannot be retargeted at a
different node by anyone in range.

What that discloses is an SSID and a BSSID, both of which the access point
broadcasts continuously anyway, plus a node number and a wake interval.

Both signatures are produced and checked by `src/espnow/EspNowAuth.h`, which
both firmwares compile. A signature is the kind of thing two implementations
get subtly different — the covered region, the truncation length — and the
failure mode is a node that pairs with nothing while each side looks correct on
its own.

`DISCOVER` is broadcast, and broadcast cannot be encrypted, so it is signed
instead — the first 8 bytes of HMAC-SHA256 over the frame. That proves the
sender holds the key, which is what stops a stranger's node from being adopted
by being carried past the house during a pairing window. It does not make the
frame private: it is readable in the clear and leaks a MAC address. The pairing
window being short and deliberately opened is the rest of the defence.

**How the window is opened today: by power-cycling the collector.** A collector
that has no nodes provisioned listens for two minutes after it comes up
(`ESPNOW_BOOT_PAIRING_S`), and that is the entire provisioning interface for
now. `espnowBeginPairing()` exists for a button in the web interface to call;
the button is not built. Once a node is paired the window never opens again on
its own, so a second node needs that button — or a collector with its node file
removed.

The node table is persisted to `/espnow_nodes.bin` on LittleFS: written whole
on a provisioning change and once a day, never per frame. The daily write is
what keeps the battery history across a reboot; without it every power cut
would cost five days of history before `battery_days` could be answered again.

## 4. The wire format

Defined once, in `src/espnow/EspNowProto.h`, and compiled by both sides — a
change there is a change to both or it does not build. Layout is asserted at
compile time and re-checked on the host by
[`tests/host/test_espnow_proto.cpp`](../tests/host/test_espnow_proto.cpp); the
decisions made about an arriving frame live in `src/espnow/NodeTable.h` and are
tested by [`tests/host/test_espnow_nodetable.cpp`](../tests/host/test_espnow_nodetable.cpp).

Two wraps are handled there that are easy to miss and fail exactly once, months
in, on a device nobody is watching: the sequence number wraps after 65,536
frames — about six weeks at one a minute — and `millis()` wraps after 49 days.
Comparing either numerically would silence a node or mark every node offline.

| message | direction | bytes | carries |
|---|---|---|---|
| `DataMsg`     | node → collector | 24 (1 sample) … 192 (15) | sequence, flags, node epoch, samples |
| `AckMsg`      | collector → node | 14 | echoed sequence, channel, epoch, interval |
| `DiscoverMsg` | node → broadcast | 22 | MAC, nonce, truncated HMAC |
| `WelcomeMsg`  | collector → node | 51 | node id, channel, SSID, BSSID, interval |

It is binary rather than JSON because ESP-NOW carries at most 250 bytes per
frame and that is a MAC-layer limit, not a buffer. The JSON the ESP8266 node
posts measures 237 bytes for a BME280 plus battery — inside the cap with 13
bytes to spare, until an ingest token is added and it becomes 264 and does not
fit at all.

The saving is not the point. Fitting fifteen samples in one frame is the point:
that is what lets a node that could not reach the collector keep its readings in
RTC memory and send them as one burst when the link comes back, with `dt_s` on
each sample giving it an honest timestamp. *(The buffering itself is not
implemented yet; the format has room for it so that adding it later does not
mean a protocol version bump.)*

Every field that can be missing has a reserved value meaning "not measured",
mapped to NaN on the way out. A BMP280 has no humidity sensor, and reporting
that as `0 %RH` would be a reading the collector could not tell from a real one.

## 5. Battery

The node sends **millivolts and nothing else**. It has no history — it deep
sleeps, and its RAM does not survive — and giving it one would mean writing
flash daily on a device whose entire purpose is not to spend energy. The
collector concludes; see `src/power/BatteryModel.h`.

Voltage is read through a resistor divider, permanently connected. At 2×220 kΩ
that is about 9.5 µA, which is under a percent of the budget at one-minute
intervals and buys not having a MOSFET and a GPIO to switch it.

Three figures come out, and they become readings like any other:

| metric | note |
|---|---|
| `battery_voltage` | as sent |
| `battery_percent` | piecewise-linear lithium curve, 0 % at the LDO's floor |
| `battery_days` | linear fit over a fortnight of daily minima, or absent |

`battery_days` and not `battery_days_left`, because `SensorReading::metric` is
`char[16]` and a 17-character name would be stored truncated, silently, and
never match anything again — which is exactly how `humidity_ambient` shipped
broken. `tools/check_metric_names.py` exists because of that one.

**0 % is 3.4 V, not the cell's 3.0 V.** The node runs from a 3.3 V LDO, and an
LDO cannot regulate once its input reaches its dropout. The roughly 600 mAh the
cell still holds below that point is unreachable, and counting it would
overstate the remaining life by weeks.

**The daily minimum, not the average.** Terminal voltage moves with load and
with temperature, and a node on an unheated balcony swings several tenths of a
volt for reasons that have nothing to do with charge. One figure per day — the
lowest, measured under the radio's load — takes most of that out.

**The model refuses to answer more often than people expect.** Fewer than five
days of history, a flat trace, a rising one, or a slope inside the noise floor
all return "unknown", and the dashboard prints a dash. A dash is a true
statement; *412 days* extrapolated from four readings two millivolts apart is
not, and it is worse than the dash because a reader believes it. This is why the
warning fires on the percentage **or** the day count, either alone: a node whose
history was lost still has to warn before it dies.

## 6. What this costs the collector

### Where a reading goes

Straight into `RemoteIngest`, the same mailbox `POST /api/ingest` writes to.
`RemoteNodeSensor` drains it on the ordinary sensor tick, so an ESP-NOW reading
gets the same calibration, the same outlier filters, the same ring buffer, the
same exporters and the same dashboard as a wired BME280. Nothing downstream
knows one arrived over a radio.

That reuse is why `FEATURE_ESPNOW_INGEST` implies `FEATURE_REMOTE_NODES`: the
mailbox and the plugin that drains it are where the readings live. It also
brings `POST /api/ingest` along, which is refused without a token.

### Which task does what

The receive callback runs on the WiFi task, where ESP-IDF's own documentation
says lengthy work is a mistake. So it validates the frame, sends the ACK, parks
the rest, and returns. Sequence numbers, the HMAC, the battery history, the
filesystem and the handoff to `RemoteIngest` all happen in `espnowIngestTick()`
from `loop()`.

**The ACK is the deliberate exception** — it is sent from the callback, because
the node is holding its radio in receive waiting for it and every millisecond
of that wait is battery. If that turns out to misbehave from inside the callback
on real hardware, the fix is a small high-priority task fed by the same ring:
the node tolerates tens of milliseconds, so a task hop is affordable. **This is
the one thing in the implementation most likely to need revisiting on a board.**

### Modem sleep

Forced off when this feature is compiled in — the sketch overrides the stored
config value rather than trusting it. `WIFI_PS_MIN_MODEM` breaks ESP-NOW
unicast, measured, while leaving broadcast working: pairing would succeed and
then no reading would ever arrive, with nothing in any log to say why. A
mains-powered collector indoors loses nothing by it.

### Flash

Measured on `xiao_esp32c3`, `firmware.bin`, against `app0` = 1,507,328 bytes:

| build | bytes | delta |
|---|---|---|
| baseline | 1,333,408 | — |
| `+FEATURE_REMOTE_NODES` | 1,339,984 | +6,576 |
| `+FEATURE_ESPNOW_INGEST` (implies the above) | 1,353,216 | **+19,808 total** |
| every optional feature on | 1,410,432 | 93.6 % of `app0` |

So ESP-NOW itself, its pairing and the ingest path add **13,232 bytes** on top
of the remote-node feature it reuses. About 8 KB of that is the ESP-NOW stack —
cheap because the WiFi stack underneath it is already linked — and the rest is
this code plus the mbedTLS HMAC it calls.

The everything-on build leaves 96,896 bytes of headroom. That is comfortable
but no longer generous, and it is the number to watch when the next feature
lands.

## 7. Expected life

| | one minute | thirty seconds |
|---|---|---|
| wake (~350 ms at ~60 mA average) | 8.4 mAh/day | 16.8 mAh/day |
| ACK window, early exit, ~8 ms average | 0.3 | 0.6 |
| deep sleep (~44 µA — the board, not the chip) | 1.06 | 1.06 |
| divider | 0.23 | 0.23 |
| **total** | **~10.0 mAh/day** | **~18.7 mAh/day** |
| from 4312 mAh usable | ~430 days | ~230 days |

At that scale the cell's own self-discharge stops being negligible: around 2 %
a month over fourteen months is roughly a quarter of the capacity. Expect
**10–11 months** at one-minute intervals and about seven at thirty seconds.

Every figure above is a calculation, not a measurement. Nothing here has been
run on hardware yet.

## 8. What is left

- The warning badge on the Kindle dashboard and in the web interface.
- A pairing button, so a second node does not need the collector power-cycled.
- Per-node keys instead of one shared key.
- Signal strength: `EspNowNode::rssi` stays 0 on Arduino core 2.x, because IDF
  4.4 hands the receive callback no signal information. The core-3 branch that
  reads it is written and compiled only by the probe environment.
