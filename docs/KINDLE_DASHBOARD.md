# Kindle dashboard (`GET /kindle`)

A weather panel for a 6" e-ink reader on the local network: outdoor and indoor
temperature from your own sensors, humidity and pressure, a 24-hour trend, and
a short forecast for the part a sensor cannot know.

Point the Kindle's experimental browser at `http://<collector-ip>/kindle`.

## Enabling it

```ini
build_flags =
    ${env.build_flags}
    -DFEATURE_KINDLE_DASHBOARD
    -DKINDLE_OUTDOOR_SENSOR='"outdoor"'
    -DKINDLE_INDOOR_SENSOR='"indoor"'
    -DKINDLE_REFRESH_SEC=300
    ; optional, adds the forecast section
    -DMODULE_FORECAST_ENABLED
```

The two sensor names are instance ids from `platform_config.json`. Typically
`outdoor` is a remote node (see [`node/`](../node/README.md)) and `indoor` is a
locally wired BME280.

## What the target browser can't do

The Paperwhite 3 (model PQ94WIF, 2015) runs a WebKit build from around 2012.
No `fetch`, no `Promise`, no ES6, no flexbox, no CSS grid. So the page:

- is rendered server-side and ships **zero JavaScript**;
- lays out with tables and blocks, because those work;
- draws the trend chart as **inline SVG path data** — no canvas, no charting
  library, nothing to execute;
- refreshes with `<meta http-equiv="refresh">`, not a timer.

The panel is 1072×1448 at 300 ppi, but the browser reports roughly **600×800
CSS pixels** at devicePixelRatio 2, and that is what the layout targets. Newer
readers have more room and get more margin.

## The top of the page

There is no masthead. The place name never changed and the date is carried by
the week strip at the foot, so the row was two lines of furniture standing above
the only two numbers the page exists to show. The hero row is the masthead.

The right half is split about two-to-one:

- **the clock**, at 96 px — an e-ink panel on a shelf is read from across a
  room, and the time used to be 14 px of grey in a corner;
- **inside temperature and humidity** below a hairline.

The inside 24-hour range went with the masthead. The dashed line on the chart
already carries it, and it was the least-read figure on the page.

### Temperature and humidity share a line

`8.4° / 71%`, on one baseline, humidity at half the size. They are one
measurement of one parcel of air at one instant, and a line break between them
was putting a paragraph boundary through a single reading. The same pairing
runs inside, at 40 px and 30 px.

The slash forces a width check: `-12.4° / 100%` is the widest this line can
get. A reading of four or more glyphs already drops to the smaller `.big4`
face, and `.big4 .hum-o` brings the pair down with it — without that rule the
humidity runs off the column in a hard frost. It is measured in the browser at
the device's viewport, not estimated.

### Pressure has its own size

34 px, on its own line, with the tendency underneath unchanged. The absolute
figure is the one number on the page a reader compares against memory rather
than against the page, and at 15 px it was set as a footnote to the humidity.

The two columns are different shapes, so their numerals are aligned by a fixed
box height on both (`.big`, `.clock`) plus a 12 px nudge on the clock, which has
no label above it to push it down. `.clock`'s height also sets where the divider
falls — two thirds down the cell the left column sizes. Getting this by eye is
what made an earlier version look ragged; it is measured in the browser instead.

### Language

```ini
-DKINDLE_LANG_BG    ; Bulgarian; omit for English
```

A compile-time switch, so a single-language build pays nothing for the other —
the unused literal is discarded. The weekday names in the week strip come from
tables in `DashboardStrings.h` rather than from `strftime`: the C locale would
give English names whatever the build language, and newlib on this part has no
`bg_BG` to switch to.

Cyrillic depends on the reader's fallback font. The page declares UTF-8 and
names the device's serif faces first, but Bookerly's Cyrillic coverage varies
by firmware — if a Bulgarian build shows boxes, that is the font, not the
encoding.

### The greys

The palette is `#000 #444 #777 #aaa #d8d8d8 #fff` plus two panel washes.

An earlier version of this page was pure black and white, on the reasoning
that "16-level e-ink dithers mid-greys into visible noise". That was
over-cautious and made the page poorer. The panel has **16 real grey levels**;
the dithering worth avoiding comes from gradients and from tones too close
together, not from flat, well-separated fills. Spaced this far apart, each
tone lands on its own level and renders solid.

The min/max band was originally hatched for the same wrong reason. With a flat
wash doing the job, the hatch was texture over texture — two bands that nearly
touch read as one muddy mass — so it is gone.

The two chart lines are still told apart by **dash pattern as well as** shade,
because redundant coding costs nothing and survives a panel with its contrast
turned down.

### Refresh cadence

Every page load repaints the whole panel — it flashes, and it costs battery.
There is no partial update available to a web page. `KINDLE_REFRESH_SEC`
defaults to 300, which is current enough for a device on a shelf without
strobing. Below about 60 s the reader spends more time flashing than showing.

## The 24-hour trend needs its own storage

This is the part worth understanding before you wonder why a new feature
appeared alongside the page.

`webRingBuf` holds a **fixed byte budget** of raw readings — about 227 entries
on a C3. A build emitting ~19 metrics every 10 s fills that in roughly **two
minutes**. Even the 4 MB PSRAM ring on an S3 reaches about eight hours, and
the FS-backed history that would cover the rest is still stubbed out.

So a 24-hour trend cannot be a query over existing storage. `TrendRing` keeps
a fixed grid of hourly aggregates instead:

```
4 series × 24 hours × 12 bytes ≈ 1.2 KB of RAM, constant
```

That is affordable on every target including the C3, which is what makes the
headline feature work on the board you probably have rather than only on the
one with PSRAM.

It stores **min / max / mean per hour**, not raw samples. A 6" panel cannot
resolve more than a couple of hundred horizontal pixels of line anyway, and
min/max is what makes an overnight frost visible — a mean-only trend hides
exactly the excursion you want to see.

Hours with no reading **break the line** rather than interpolating across the
gap. A flat line through a four-hour outage reads as "it was steady", which is
a lie the chart should not tell.

Vertical rules every three hours give the eye something to count against. The
hour labels are six-hourly — closer together they crowd — so between two of
them there was nothing to carry a point on the curve down to. They are drawn
first, so the band and the lines cover them, and lighter than the horizontals
because they are scaffolding rather than data. `#d5d5d5` rather than something
fainter: the panel quantises to 16 levels and a near-white rule rounds away to
nothing.

The grid fills as readings arrive: expect a partial chart for the first day
after a reboot, and the section says so rather than drawing an empty box.

## Forecast

Weather&Radar (WetterOnline) has **no publicly documented free API** — it is a
B2B product behind a commercial agreement. Two providers that do publish one
are implemented:

| Provider | `provider` | Key | Notes |
|---|---|---|---|
| Open-Meteo | `open-meteo` | none | No account, no quota worth counting. The default. |
| OpenWeatherMap | `owm` | required | Free tier ~1000 calls/day. |

Configure via the module UI or `modules.json`:

```json
{
  "forecast": {
    "enabled": true,
    "provider": "open-meteo",
    "outlook": "hourly",
    "lat": 42.6977,
    "lon": 23.3219,
    "interval_min": 30
  }
}
```

### The three outlook columns

`outlook` chooses what the right-hand side of the forecast row steps through:

| Value | Columns | Shows |
|---|---|---|
| `hourly` (default) | +3 h, +6 h, +9 h | temperature at that hour |
| `daily` | tomorrow, +2, +3 days | that day's high / low |

An hour has no range to report, so hourly columns print one figure — printing
`11° / 11°` would imply a precision the slot does not have.

**Open-Meteo** serves both from one request. `forecast_hours` anchors the
hourly array on the current hour rather than local midnight, which is what
makes indices 3/6/9 mean +3/+6/+9 h without any date arithmetic on the device.

**OpenWeatherMap's free tier does not have a daily endpoint**, and splits what
Open-Meteo returns in one response across two: `/weather` for current
conditions, `/forecast` for the 3-hourly list. So the OWM path makes two
requests back to back — together about 12 s worst case against ExportTask's 30 s
watchdog, which is why the per-request timeout is 6 s and redirect following is
off.

In `daily` mode the OWM days are **aggregated from that same 3-hourly list**:
high and low per local day, with the condition taken from the slot nearest
midday. Nearest-midday rather than first-of-day on purpose — an early-hours
shower should not make an otherwise sunny day render as rain. `cnt=24` bounds
how much JSON lands in heap.

If the outlook request fails the current conditions are still shown: three
empty columns are a smaller loss than a blank forecast block.

`interval_min` is clamped to 10–360. A forecast does not change faster than
that, and the floor is what keeps a misconfigured device off a provider's
rate limit.

### The temperature on the dashboard is yours, not the forecast's

Deliberately. The headline figures are what your BME280s measured; the
forecast contributes only the sky. A forecast's "current temperature" is an
interpolation from a station that may be 20 km away, and putting it beside a
real reading invites trusting the wrong one.

### A stale forecast is kept, not blanked

A failed fetch does not invalidate the cache. A three-hour-old forecast is
still broadly right, and blanking the panel because one HTTPS request timed
out trades useful for nothing. The age is printed, so you can judge.

### On OpenWeatherMap's high/low

On the free current-weather endpoint, `temp_min`/`temp_max` are the spread
across nearby stations **at this moment**, not today's high and low. They are
shown as-is rather than relabelled — inventing a daily range the API did not
supply would be worse than a narrow one. Open-Meteo's daily fields are the
real thing, which is one more reason it is the default.

## Week strip

The foot of the page carries the current week with today inverted, under a
month heading set like the two section headings above it. It answers the
question a static panel on a shelf is otherwise bad at: what day is it.

The day numbers alone say which day but not which month, which is what the
masthead used to answer. A week can straddle two months, and then one name is
wrong about half the row, so both are named — `НОЕМВРИ – ДЕКЕМВРИ`. They are
read off Monday and Sunday rather than off today, because today may be either
side of the boundary.

Monday-first. `tm_wday` counts from Sunday, so the column index is
`(wday + 6) % 7`; getting that backwards misplaces today on Sundays only,
which is the sort of bug that survives a casual look. The days either side are
walked on the epoch rather than on `tm_mday`, so month and year ends are
correct for free.

Today is marked by inverting the cell rather than outlining it: a filled block
is the one mark that stays unambiguous after e-ink dithering, where a thin
ring can read as a smudge.

## TLS

The forecast client uses `setInsecure()`, consistent with `HttpExporter` and
for the same reason: no CA bundle is shipped yet. On a LAN device fetching a
public forecast this is an accepted risk — the payload is not secret, and the
worst a MITM achieves is a wrong temperature on a bookshelf. It would not be
acceptable for anything carrying credentials, which is why the ingest path
never reaches outward.
