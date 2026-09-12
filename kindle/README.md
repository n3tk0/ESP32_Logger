# Kindle FBInk Dashboard

Direct framebuffer weather dashboard for jailbroken Kindle devices.
Fetches data from an ESP32_Logger collector and renders with FBInk.

## Requirements

- Jailbroken Kindle (7th Gen or Paperwhite 4)
- [FBInk](https://github.com/NiLuJe/FBInk) installed
- ESP32_Logger collector on the same network

## Installation

Three routes. **The first one is the reason the other two are listed second**:
every failure of this extension reported so far has been an installation
failure rather than a code one — a checkout on Windows that rewrote every line
ending so `busybox ash` could not run a single script, a folder copied into a
place KUAL does not look, an FBInk that was never installed. All three look
identical on the device, because KUAL discards whatever a menu entry prints:
the menu closes, the home screen comes back, nothing is said.

A built package removes the question. The bytes that leave the build are the
bytes that reach the reader.

### 1 · The MRPI package — one file, one button

Needs [MRPI](https://wiki.mobileread.com/wiki/MobileRead_Package_Installer),
which most jailbreak walkthroughs install alongside KUAL.

1. Download `Update_esp32dash_<version>_install.bin` from this repository's
   **Releases** page.
2. Plug the Kindle in and drop the file into `mrpackages` at the root of the
   USB volume (create the folder if it is not there).
3. Eject the Kindle, then **KUAL → Helper → Install MR Packages**.

It unpacks into `/mnt/us/extensions/esp32dash/` and **keeps `dash.conf` and
the collector scan list** if they are already there, so an update never sends
you back to the Find collector screen. What it did is appended to
`extensions/esp32dash/kual.log`, which the USB cable can read.

The package is an OTA V2 update signed with the jailbreak key a hacked Kindle
already trusts — the same mechanism every MobileRead hack package uses. It is
built by `.github/workflows/build-kindle-package.yml`, which also takes the
package apart again and compares it with what went in.

### 2 · The zip — for a reader without MRPI

`esp32dash-kindle-<version>.zip`, from the same Releases page. Unpack it at the
root of the USB volume: it contains `extensions/esp32dash/` and an empty
`mrpackages/`, so it lands in the right place by construction. A zip stores
bytes — no unpacker on any platform rewrites a line ending.

### 3 · From a checkout — for development

```
ssh root@kindle-ip mkdir -p /mnt/us/extensions/esp32dash
scp -r kindle/* root@kindle-ip:/mnt/us/extensions/esp32dash/
```

Over USB instead: copy `kindle/` into `extensions/`, renaming it `esp32dash`.
**Check your line endings if you do this from Windows** — or let `kual.sh` do
it, which strips carriage returns out of the folder every time a menu entry
runs.

`tools/mk_kindle_package.sh` builds both artifacts locally; it needs
[KindleTool](https://github.com/NiLuJe/KindleTool) for the `.bin`, and
`--stage-only` skips that and produces just the zip.

The folder name is free — KUAL scans the subdirectories of
`/mnt/us/extensions` for a `menu.json` and every path in ours is relative — but
`esp32dash` is what the documentation and the packages use.

### After installing, whichever route

1. **Tell it where the collector is** — from the Kindle, no editing and no
   keyboard: KUAL → ESP32 Dashboard → Settings → **Find collector**. It probes
   every address on the Kindle's own subnet for a dashboard payload and saves
   the one that answers. If more than one does, **Next collector** steps to the
   following one; the screen says which is current.

   Over USB or SSH instead: put the address in `dash.conf` (it is created from
   `dash.conf.default` on first run).

2. **Make sure FBInk is findable.** It is not part of the Kindle firmware and
   it is not shipped here; it is the whole output of this dashboard, and KUAL
   hands an extension a `PATH` that does not include most of the places people
   keep binaries. On a jailbroken Kindle it is usually already at
   `/mnt/us/libkh/bin/fbink`, where the jailbreak hotfix puts it — that is the
   first place the launcher looks. Any of these also works:

   ```
   /mnt/us/extensions/esp32dash/bin/fbink     ← beside the scripts
   /mnt/us/bin/fbink
   /mnt/us/fbink/fbink
   /mnt/us/extensions/fbink/bin/fbink
   ```

   If it is missing, the extension says so on the panel and in `kual.log`
   instead of starting a dashboard that can never draw anything.

3. On the ESP32 WebUI, go to Settings → E-ink Dashboard and set the
   FBInk resolution to match your Kindle.

4. Restart KUAL (leave the launcher and open it again). It reads the extension
   list once at startup, so a folder added while it is open does not appear.

## Usage

**From KUAL:** open the launcher → **ESP32 Dashboard** → *Start Dashboard*.
*Stop Dashboard* ends it and restores the screensaver.

**From the panel itself:** tap the screen. A bar appears along the bottom —
**Refresh · Awake/Sleep · More · Exit** — and *More* opens **Find · Next · Battery ·
Info · Back**, which is the settings menu with no launcher involved. *Exit*
asks before it acts. Everything on both bars is reachable without a keyboard,
a cable or KUAL, which matters because `GUI_STOP=1` takes KUAL away.

**And if the panel is asleep, press the power button.** It wakes, repaints, and
puts the bar up for two minutes. See *[Battery](#battery)*.

**From a shell**, if you prefer:

```bash
ssh root@kindle-ip
sh /mnt/us/extensions/esp32dash/start.sh
sh /mnt/us/extensions/esp32dash/stop.sh
```

Note the `sh`. `/mnt/us` is a FAT filesystem with no execute bit to set, so
`./start.sh` may or may not run depending on how the firmware mounted it —
which is also why `menu.json` invokes `/bin/sh` rather than the scripts
directly.

## Settings

Everything adjustable lives in **`dash.conf`**, beside the script. It is
created from `dash.conf.default` the first time the dashboard runs, and only
`dash.conf` is ever written — so copying a newer version of the extension over
an older one cannot overwrite what you set.

KUAL → ESP32 Dashboard → **Settings** edits it on the device:

| Entry | What it does |
|---|---|
| **Show settings** | Paints the current values on the screen |
| **Info: what is going on** | FBInk, the collector and whether it answers, the touchscreen, the wake alarm, the battery — the page that used to need a USB cable |
| **Find collector** | Scans this subnet for a host serving `/kindle/data` and saves it |
| **Next collector** | Steps to the next address that scan found |
| **Refresh: normal** | clock 1 min · data 5 · chart 15 · forecast 30 · full 60 |
| **Refresh: fast** | clock 1 · data 2 · chart 5 · forecast 15 · full 30 |
| **Refresh: battery saver** | clock 5 · data 15 · chart 30 · forecast 60 · full 120 |
| **Battery →** | normal · radio off between updates · sleep between updates · **deep sleep, rare updates** |
| **Screen →** | draw over the reader or take the screen · tap menu on/off · power button opens the menu on/off · footer status on/off |
| **Quiet hours →** | 22:00–07:00, or off |
| **Reset settings** | Back to `dash.conf.default` |

From a shell the same thing, one key at a time:

```bash
sh settings.sh show
sh settings.sh diag
sh settings.sh set HOST 192.168.1.50
sh settings.sh set FORECAST_EVERY 60
sh settings.sh profile saver
sh settings.sh power days
sh settings.sh quiet night
```

A value that would break the loop is refused rather than written — an interval
of `0`, a word where a number belongs, an address carrying shell
metacharacters. **No restart is needed**: a running dashboard re-reads
`dash.conf` every minute and repaints as soon as it sees a change, so the
settings screen you were just looking at gives way to the page again by itself.

| Setting | Default | Meaning |
|---|---|---|
| `HOST` | `192.168.1.50` | Collector address; `http://` is added if you leave it off |
| `FETCH_TIMEOUT` | `10` | Seconds to wait for the collector |
| `CLOCK_EVERY` | `1` | Minutes between clock updates |
| `DATA_EVERY` | `5` | Minutes between sensor updates |
| `GRAPH_EVERY` | `15` | Minutes between chart updates |
| `FORECAST_EVERY` | `30` | Minutes between forecast updates |
| `FULL_EVERY` | `60` | Minutes between whole-screen refreshes |
| `CLOCK_FLASH_EVERY` | `1` | Flash the clock zone every N clock updates (0 = never) |
| `SENSOR_FLASH_EVERY` | `0` | Flash the readings zone every N sensor updates (0 = never) |
| `POWER` | `awake` | `awake`, `wifi` (radio off between fetches) or `suspend` (sleeps to RAM as well) |
| `TOUCH` | `1` | Tap the screen for the menu |
| `WAKE_MENU` | `1` | A press of the power button wakes the panel and opens the menu |
| `WAKE_HOLD` | `120` | Seconds it then stays awake and listening; every tap pushes this out |
| `MENU_ACT` | `refresh\|wake\|settings\|quit` | What the bar's buttons do — 2 to 5 of `refresh`, `wake`, `settings`, `hide`, `quit` |
| `MENU_LBL` | `Refresh\|Awake/Sleep\|More\|Exit` | What they are called. The sleep button may carry both directions with a slash; the bar draws the half that says where the next tap goes. Fewer labels than buttons and the built-in names are used instead |
| `QUIET_FROM`, `QUIET_TO` | `0`, `0` | Hours between which nothing flashes. Equal = off |
| `QUIET_EVERY` | `15` | Minutes between clock updates during those hours |
| `STATUS` | `1` | Draw this Kindle's battery and power mode at the end of the footer |
| `AUTO_FIND` | `1` | Look for the collector once if the very first fetch fails |

`Find collector` writes the addresses that answered to `collectors`, beside
`dash.conf` — not under `/tmp`, which Stop deletes and a reboot clears — so
**Next collector** still works the next time you come back to it.

### Battery

A ten-year-old Kindle looping this script with an associated radio draws 60–80
mA — a day and a half on a cell that is probably down to 600–900 mAh. Most
minutes on this panel need none of it: the clock comes from the reader's own
clock.

| Battery menu entry | What it does |
|---|---|
| **normal** | nothing is touched |
| **radio off between updates** | the radio is up only around a fetch. Cannot fail in a way the panel does not already handle |
| **sleep between updates** | and the wait is a real suspend to RAM with an RTC alarm |
| **deep sleep, rare updates** | the above, **and** the intervals that make it worth having: clock 15 min, data 30, chart 60, forecast 60, full 240 |

**The last two are not the same setting, and the difference is the point.**
Sleeping between updates with `CLOCK_EVERY=1` suspends and comes back sixty
times an hour — each one a resume, a draw and a flashing refresh of the clock.
The saving is in *not waking up*, so the panel sleeps straight through the
minutes with no tier due in them, and it is the intervals that decide how long
that is.

**Waking it up.** A suspended Kindle wakes from the **power button** — not from
the touchscreen, whose controller has no power while the CPU is down, which is
the same reason a sleeping Kindle does not wake when you touch its screen. A
short press repaints the page and puts the tap bar up for `WAKE_HOLD` seconds;
every tap pushes that out again, so there is time to work through the settings
bar. Then it goes back to sleep by itself.

Tap **Awake** on that bar and the sleeping stops until you tap it again — which
is the way to hold the panel up while you change something, from the bar or
from KUAL. It writes `POWER` to `dash.conf`, so the menu and the panel agree
about it afterwards.

`TOUCH=0` with `WAKE_MENU=1` is worth knowing about for a panel on a wall:
nothing reads the touchscreen while nobody is there, and the button summons a
menu when somebody is.

**Quiet hours** (Settings → Quiet hours) stop the flashing overnight and slow
the clock to `QUIET_EVERY`, which with a real suspend turns the night into one
long sleep instead of sixty short ones. Leaving them spends one full flashing
refresh, which is where the night's ghosting goes.

### When the collector cannot be reached

The readings block is replaced by the address it tried, how to change it, and
**the time it last worked**, and the clock keeps running underneath. The chart
and the forecast are left alone: a page with yesterday's chart and a reason on
it beats a blank one. Every data tier retries, and the first success draws the
whole page again.

**One failed fetch is not enough to say so.** A single wifi hiccup should not
make the page flinch when the numbers are five minutes old and the next tier
will almost certainly work, so it takes two failures in a row. What it will not
do any more is keep repainting the last readings indefinitely with nothing to
say they are old — a dead collector and a calm afternoon used to look
identical.

**A reading the collector stops sending is forgotten**, rather than left on
screen at whatever it last was: a sensor whose node goes flat takes its value
off the panel with it.

**And after a reboot there is still a page.** The last payload is kept beside
`dash.conf` as `last.txt` — written once an hour, not once a fetch — so a
reader that has just been switched on comes up with the chart, the forecast and
the week strip it last had, in the language the collector set, with the offline
notice over the readings and the time they were from. `/tmp` is a ramdisk;
without it a cold start with the collector also down is a blank screen.

The forecast interval is the Kindle's **redraw** cadence. The collector fetches
from the weather API on its own schedule — WebUI → Settings → Forecast → *Fetch
interval*, 10–360 min — so setting this one shorter than that just redraws the
same numbers.

## It does not appear in KUAL

In order of how often each one is the answer:

1. **Wrong folder.** It must be `/mnt/us/extensions/<anything>/`, with
   `menu.json` directly inside it — not in a subfolder.
2. **KUAL was already open.** It enumerates extensions at startup; leave it
   and reopen.
3. **CRLF line endings.** Copying through a Windows editor can rewrite
   `menu.json`, and KUAL's parser rejects it silently, listing nothing. The
   repository pins these files to LF; check with `file menu.json` if in doubt.
4. **Truncated copy.** `menu.json` must be valid JSON — an interrupted `scp`
   leaves a file that parses as nothing.

## It is in KUAL, but pressing an entry does nothing

The menu closes and the Kindle goes back to the home screen — which is what
`exitmenu` asks for — and then nothing happens: no dashboard, no settings page,
no error. KUAL discards whatever a menu entry prints, so all of the causes
below used to look identical from the sofa.

**They do not any more. Read `kual.log`.** Every menu entry runs `kual.sh`,
which writes what it did, what it ran, and the exit code to `kual.log` *beside
the scripts* — on `/mnt/us`, the volume that appears when you plug the Kindle
into a computer. No shell, no network and no SSH needed:

```
extensions/esp32dash/kual.log
```

A launch that worked looks like this:

```
2026-09-05 18:31:02 --- start (dir /mnt/us/extensions/esp32dash)
2026-09-05 18:31:02 run: Start Dashboard (sh /mnt/us/extensions/esp32dash/start.sh)
2026-09-05 18:31:02 Dashboard started (PID 1234). Log: /tmp/dash.log
2026-09-05 18:31:02 exit 0
```

What the log will usually say instead, in order of how often each is the
answer:

1. **`FBInk is not installed, or not in PATH`.** FBInk is not part of the
   Kindle firmware and it is not part of this extension — it is a separate
   binary you install once, and KUAL hands an extension a `PATH` that does not
   include most of the places people put it. Without it *every* draw fails
   silently: the dashboard starts, keeps its schedule, and the panel never
   changes, which from across the room is the same thing as nothing having
   started. `kual.sh` looks in **`/mnt/us/libkh/bin`** first — the jailbreak
   hotfix installs FBInk there, so on most jailbroken Kindles the binary is
   already present and merely unreachable from the `PATH` a menu entry is
   given — then in `bin/` beside itself, `/mnt/us/bin`, `/mnt/us/fbink` and
   `/mnt/us/extensions/fbink/bin` before giving up. It also prints the message
   on the panel with `eips`, which *is* in the firmware.

   `update_dash.sh` now refuses to start without it rather than running blind.

2. **Windows line endings.** `busybox ash` cannot run a script whose lines end
   in a carriage return — `then\r` is not `then` — and a layout file copied
   the same way gives every coordinate one (`GR_X=20\r`), which FBInk rejects
   as a bad argument. This is **repaired automatically**: `kual.sh` is written
   so that a CRLF copy of it still runs, and it strips the carriage returns out
   of every `.sh`, `.conf`, `.json` and `.xml` in the folder before running
   anything, logging what it fixed. The icons are BMPs and are never touched.

   If you want to do it by hand anyway:

   ```sh
   cd /mnt/us/extensions/esp32dash
   for f in *.sh *.conf *.json *.xml layout/*.conf; do sed -i 's/\r$//' "$f"; done
   ```

3. **The copy is incomplete.** `start.sh` runs `update_dash.sh` from beside
   itself; an interrupted `scp` that dropped the larger file leaves Start
   apparently doing nothing. `ls -l /mnt/us/extensions/esp32dash` should show
   `update_dash.sh` at roughly 47 KB. The log names the file that was missing.

   Installing from the MRPI package or the zip makes 2 and 3 impossible: both
   carry the tree as one archive, built and checked by CI.

The folder name is **not** one of the causes. KUAL runs a menu entry in the
directory the `menu.json` it came from lives in — which is why KOReader's own
extension can say `./bin/koreader-ext.sh` — so every entry here names its
script relatively and an extension unzipped as `esp32dash-main` works exactly
the same.

To watch one live instead, over SSH:

```sh
sh /mnt/us/extensions/esp32dash/kual.sh start
cat /mnt/us/extensions/esp32dash/kual.log
cat /tmp/dash.log
```

## The chart area is blank

The rule and the caption are there, and nothing underneath them. That is the
image, not the readings — and the dashboard now says which:

- **`No chart yet — http://…`** drawn where the chart goes means the image is
  not on the reader. The address is printed with it, so a wrong one is visible
  from across the room. It resolves itself at the next chart update
  (`GRAPH_EVERY`, 15 minutes by default) once the collector answers.
- `/tmp/dash.log` says which half failed. `chart: no image from …` is the
  network or a collector that is not there. **`chart: incomplete image (N
  bytes)`** is a transfer that was cut off partway — the ESP32 streams the BMP
  while it is also serving the web UI, and a reader on wifi is the client most
  likely to lose a connection mid-image. The half-written file is discarded and
  the chart that already works stays on the screen.
- **A chart with a grid and no line in it** is the other thing entirely: the
  image arrived, and the collector had no history to draw. That is a sensor
  question — check the sensor named as outdoor/indoor on the E-ink Dashboard
  settings page — not a Kindle one.

## Custom fonts

Text is drawn with `fbink -t regular=FILE`, so a font is a FILE and the name
has to resolve to one. Drop any `.ttf` into `fonts/` beside the script to
override the system's.

The search order is `fonts/` first, then the Kindle's own `/usr/java/lib/fonts`,
and within each: `Bookerly-Regular.ttf`, `Caecilia_LT_65_Medium.ttf`,
`Helvetica_LT_65_Medium.ttf`, `Futura_LT_Book.ttf` — and their bold cousins.
A device whose firmware ships none of them gets a message on stderr saying so,
rather than a page that silently comes up blank: a font that does not resolve
means every string is undrawn.

## Refresh strategy

E-ink ghosts: a partial update leaves a faint impression of what was there
before, and the impressions accumulate. The cure is a *flashing* update — the
panel driven to black and back — which is slow and visible. So it is spent
where it buys the most and withheld where it would only annoy.

| Every | What is redrawn | Refresh |
|---|---|---|
| `CLOCK_EVERY` (1 min) | the clock, in its own rectangle | **flashing**, that rectangle only |
| `DATA_EVERY` (5 min) | the readings block | plain, that rectangle |
| `GRAPH_EVERY` (15 min) | the 24 h chart | plain, that rectangle |
| `FORECAST_EVERY` (30 min) | forecast, week strip, footer | plain, that rectangle |
| `FULL_EVERY` (60 min) | everything | **flashing**, whole screen |

Inside the quiet hours nothing flashes at all, and the clock tier stretches to
`QUIET_EVERY`.

The clock is the one region that changes every minute, so it is the one that
ghosts first — and it is small enough that flashing it is barely noticeable,
which is why it gets the treatment the rest of the screen only gets hourly.

Two mechanics matter more than the intervals:

- **Each tier clears its rectangle before drawing it.** E-ink does not erase
  what it is drawn over: `21.0` replaced by `9.8` leaves the `0` standing where
  nothing wrote.
- **Each tier draws with `fbink -b` and refreshes once at the end.** Without
  `-b` every string is its own visible repaint — twenty of them for this page,
  each leaving its own ghost.

The screen is divided into four rectangles that tile it exactly (clock,
readings, chart, forecast); `tests/kindle/drive_dash.sh` checks that they do
for both shipped panel sizes, because a gap between two of them is a strip
nothing ever repaints.

## The FBInk command line

The renderer speaks FBInk's documented interface: `-t/--truetype` for text at a
pixel position, `-k/--cls` for filled rectangles, `-g/--image` for the chart and
icons, `-s/--refresh` with a region for refreshes, `-b/--norefresh` to batch.

It did not always. Until this version it invoked `-p` for pixel coordinates (it
means `--padded`), `-M` for a partial refresh (`--halfway`, which centres text
vertically), `-R WxH` for rectangles and `-L W` for lines — neither exists,
and `-L` is `--linecountcode` — `-F` with a path (it names a *built-in* font),
and colours `GRAY10`/`GRAY14`/`GRAY15` when the palette is `GRAY1`..`GRAY9`
then `GRAYA`..`GRAYE`. Text was positioned in character cells while every
layout file is written in pixels.

Since the script sends stderr to `/dev/null`, all of that failed silently: the
page came up with the chart drawn and the text missing. `tests/kindle/drive_dash.sh`
now validates every emitted command against FBInk's own option table, which is
the only way anything here can be checked without a Kindle on the desk.
