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

### Why it is grayscale, and why the two lines differ by dash

16-level grayscale dithers mid-greys into visible noise at this size. Two
temperature lines on one chart are told apart by **dash pattern**, not shade,
because shade does not survive dithering. The only grey on the page is the
chart's gridline, chosen light enough to read as a rule rather than a line.

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
    "lat": 42.6977,
    "lon": 23.3219,
    "interval_min": 30
  }
}
```

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

## TLS

The forecast client uses `setInsecure()`, consistent with `HttpExporter` and
for the same reason: no CA bundle is shipped yet. On a LAN device fetching a
public forecast this is an accepted risk — the payload is not secret, and the
worst a MITM achieves is a wrong temperature on a bookshelf. It would not be
acceptable for anything carrying credentials, which is why the ingest path
never reaches outward.
