# Kindle FBInk Dashboard

Direct framebuffer weather dashboard for jailbroken Kindle devices.
Fetches data from an ESP32_Logger collector and renders with FBInk.

## Requirements

- Jailbroken Kindle (7th Gen or Paperwhite 4)
- [FBInk](https://github.com/NiLuJe/FBInk) installed
- ESP32_Logger collector on the same network

## Installation

**The folder has to go in `/mnt/us/extensions/`, and the name of the folder is
up to you.** KUAL builds its menu by scanning the subdirectories of
`/mnt/us/extensions` for a `menu.json`, and it looks nowhere else — an earlier
version of this file said to install to `/mnt/us/dashboard/`, which is why the
entry never appeared in the launcher.

1. Copy the whole `kindle/` folder to `/mnt/us/extensions/esp32dash/`:
   ```
   ssh root@kindle-ip mkdir -p /mnt/us/extensions/esp32dash
   scp -r kindle/* root@kindle-ip:/mnt/us/extensions/esp32dash/
   ```
   Over USB instead: mount the Kindle and copy `kindle/` into
   `extensions/`, renaming it `esp32dash`.

   The script finds its own layout, icons and fonts relative to wherever it is
   installed, so the folder name and location are free — this one only has to
   be under `extensions/` for KUAL to see it.

2. Tell it where the collector is — **from the Kindle**, no editing and no
   keyboard: KUAL → ESP32 Dashboard → Settings → **Find collector**. It probes
   every address on the Kindle's own subnet for a dashboard payload and saves
   the one that answers. If more than one does, **Next collector** steps to the
   following one; the screen says which is current.

   Over USB or SSH instead: put the address in `dash.conf` (it is created from
   `dash.conf.default` on first run).

3. Ensure FBInk is installed and in PATH.

4. On the ESP32 WebUI, go to Settings → E-ink Dashboard and set the
   FBInk resolution to match your Kindle.

5. Restart KUAL (leave the launcher and open it again). It reads the extension
   list once at startup, so a folder added while it is open does not appear.

## Usage

**From KUAL:** open the launcher → **ESP32 Dashboard** → *Start Dashboard*.
*Stop Dashboard* ends it and restores the screensaver.

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
| **Find collector** | Scans this subnet for a host serving `/kindle/data` and saves it |
| **Next collector** | Steps to the next address that scan found |
| **Refresh: normal** | clock 1 min · data 5 · chart 15 · forecast 30 · full 60 |
| **Refresh: fast** | clock 1 · data 2 · chart 5 · forecast 15 · full 30 |
| **Refresh: battery saver** | clock 5 · data 15 · chart 30 · forecast 60 · full 120 |
| **Reset settings** | Back to `dash.conf.default` |

From a shell the same thing, one key at a time:

```bash
sh settings.sh show
sh settings.sh set HOST 192.168.1.50
sh settings.sh set FORECAST_EVERY 60
sh settings.sh profile saver
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
