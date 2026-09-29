"""drive_sensor_chart.py — the Sensors page's cards and chart in a real browser.

TWO REPORTS
-----------
1. The page's own Refresh button left the sensor cards one per row, full
   width, until the browser reloaded the page. Zone grouping watched the grid
   with a MutationObserver that disconnected for 500 ms after each change.
   Refresh clears the grid, then fills it when /api/sensors answers: when the
   answer came back inside those 500 ms, the fill was never seen, and the
   cards sat ungrouped in a grid that grouping had set to display:block.

2. The Sensor chart showed only the last few minutes. /api/data serves the
   in-memory ring, and one request sees its newest 300 readings across every
   sensor. The history is in the data log (one TIMER row per aggregation
   interval, a column per metric), which nothing read for the chart.

This serves a data log with a day of rows, an archived file from yesterday
and one from last month (which must not be read), and checks both.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_sensor_chart.py
"""
import json
import os
import sys
import time
from datetime import datetime, timedelta, timezone

from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT
URL  = BASE + "/#sensors"

fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


NOW = int(time.time())
INTERVAL = 60
# The device keeps UTC+2 all year; the browser is in New York. Rows are the
# device's wall time, so reading them in the browser's zone puts them 6-7 h
# off and a 1h range finds nothing.
DEV_TZ = timezone(timedelta(hours=2))


def dev(ts):
    return datetime.fromtimestamp(ts, DEV_TZ)

SENSORS = {"sensors": [
    {"id": "env_indoor", "type": "bme280", "name": "Indoor", "status": "ok",
     "metrics": ["temperature", "humidity"],
     "last_values": {"temperature": {"v": 21.5, "u": "C", "ts": NOW},
                     "humidity": {"v": 50, "u": "%", "ts": NOW}}},
    {"id": "outdoor", "type": "remote", "name": "Outdoor", "status": "ok",
     "metrics": ["temperature"],
     "last_values": {"temperature": {"v": 12.0, "u": "C", "ts": NOW}}},
]}

CFG = {
    "hardware": {"storageType": 0},
    "network": {"timezone": 2, "dstRule": 2},
    "logger": {"csvLoggingEnabled": True, "aggregationIntervalSec": INTERVAL},
    "datalog": {"dateFormat": 1, "timeFormat": 0, "endFormat": 0,
                "currentFile": "/logs/dev_datalog.txt",
                # Two columns with the same label: only their position tells
                # them apart.
                "sensorCols": {"auto": True, "cols": [
                    {"s": "env_indoor", "m": "temperature", "l": "Temp"},
                    # Logged as the highest of each interval: the chart
                    # groups its buckets the same way.
                    {"s": "env_indoor", "m": "humidity", "l": "env_indoor_humidity",
                     "a": "max"},
                    {"s": "outdoor", "m": "temperature", "l": "Temp"}]}},
    "platform": {"mode": "continuous", "sensors": [
        {"id": "env_indoor", "zone": "indoor"},
        {"id": "outdoor", "zone": "outdoor"}]},
}

HEADER = "Date|Start|End|Trigger|Temp|env_indoor_humidity[max]|Temp"
# Yesterday's file, from before the humidity column was switched to max:
# its header carries no mode, so its humidity rows are averages.
HEADER_AVG = "Date|Start|End|Trigger|Temp|env_indoor_humidity|Temp"


def row(ts, t):
    lt = dev(ts)
    end = dev(ts + INTERVAL)
    return "%s|%s|%s|TIMER|%s|55|%s" % (
        lt.strftime("%d/%m/%Y"), lt.strftime("%H:%M:%S"), end.strftime("%H:%M:%S"),
        ("%.2f" % t).rstrip("0").rstrip("."), "10.5")


def log(frm, to, t, header=HEADER):
    lines = [header]
    ts = frm - frm % INTERVAL
    while ts < to:
        lines.append(row(ts, t))
        ts += INTERVAL
    # A water event in the same file: no sensor values, not a TIMER row.
    lt = dev(to - 3000)
    lines.append("%s|%s|45s|FF_BTN" % (lt.strftime("%d/%m/%Y"), lt.strftime("%H:%M:%S")))
    return "\n".join(lines) + "\n"


# Active file: the last 20 hours. Yesterday's archive: the 10 hours before.
# Last month's archive would be 30.00 everywhere — read, it shows up.
ACTIVE = log(NOW - 20 * 3600, NOW - 600, 21.0)
ARCH = log(NOW - 30 * 3600, NOW - 20 * 3600, 20.0, HEADER_AVG)
lm = dev(NOW - 40 * 86400)
OLD = log(NOW - 41 * 86400, NOW - 40 * 86400, 30.0)
yday = dev(NOW - 20 * 3600).strftime("%Y-%m-%d")
FILES = {
    "/logs/dev_datalog.txt": ACTIVE,
    "/logs/dev_datalog_%s.txt" % yday: ARCH,
    "/logs/dev_datalog_%s.txt" % lm.strftime("%Y-%m"): OLD,
    "/logs/other.txt": "unrelated\n",
}
downloads = []
listings = []
# Yesterday's archive answers 503 the first time (the FS was busy): that
# must not be remembered as an empty file.
FAIL_ONCE = {"/logs/dev_datalog_%s.txt" % yday}

# The ring: the last two minutes of readings, the newest at 22.
RING = [{"ts": NOW - 120 + 10 * i, "v": 22, "metric": "temperature", "unit": "C"}
        for i in range(12)]

with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    ctx = b.new_context(viewport={"width": 1400, "height": 1000},
                        timezone_id="America/New_York")
    pg = ctx.new_page()
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))

    def fulfil_json(route, obj):
        route.fulfill(status=200, content_type="application/json", body=json.dumps(obj))

    pg.route("**/export_settings", lambda r: fulfil_json(r, CFG))
    pg.route("**/api/sensors", lambda r: fulfil_json(r, SENSORS))
    pg.route("**/api/filelist*", lambda r: (listings.append(r.request.url), fulfil_json(r, {
        "currentFile": "/logs/dev_datalog.txt",
        "files": [{"name": k.rsplit("/", 1)[1], "path": k, "isDir": False,
                   "size": len(v)} for k, v in FILES.items()]})))

    def download(route):
        from urllib.parse import urlparse, parse_qs
        f = parse_qs(urlparse(route.request.url).query).get("file", [""])[0]
        downloads.append(f)
        if f in FAIL_ONCE:
            FAIL_ONCE.discard(f)
            route.fulfill(status=503, body="busy")
        elif f in FILES:
            route.fulfill(status=200, content_type="application/octet-stream", body=FILES[f])
        else:
            route.fulfill(status=404, body="not found")
    pg.route("**/download*", download)

    def api_data(route):
        from urllib.parse import urlparse, parse_qs
        q = parse_qs(urlparse(route.request.url).query)
        data = RING if q.get("sensor", [""])[0] == "env_indoor" else []
        fulfil_json(route, {"agg": "raw", "mode": "raw", "count": len(data), "data": data})
    pg.route("**/api/data*", api_data)

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1500)

    def layout():
        return pg.evaluate("""() => {
            const g = document.getElementById('sensors-grid');
            const cards = [...g.querySelectorAll('.sensor')];
            return {
              zones: g.querySelectorAll('.zone-section').length,
              loose: g.querySelectorAll(':scope > .sensor').length,
              cards: cards.length,
              gridW: g.getBoundingClientRect().width,
              maxW: Math.max(0, ...cards.map(c => c.getBoundingClientRect().width)),
            };
        }""")

    print("Cards on first load:")
    l0 = layout()
    check(l0["cards"] == 3, f"one card per metric ({l0['cards']})")
    check(l0["zones"] == 2, f"grouped into the two zones ({l0['zones']})")

    print("\nCards after the page's Refresh button:")
    for i in range(3):
        pg.locator('#page-sensors button[data-click="sensorsLoad"]').click()
        pg.wait_for_timeout(700)
        l1 = layout()
        check(l1["cards"] == 3 and l1["zones"] == 2 and l1["loose"] == 0,
              f"refresh {i + 1}: still grouped ({l1})")
        check(l1["maxW"] < l1["gridW"] / 2,
              f"refresh {i + 1}: cards are tiles, not full-width rows "
              f"({l1['maxW']:.0f} px of {l1['gridW']:.0f})")

    print("\nThe sensor chart:")
    # How a bucket is combined is the data log column's own choice
    # (Settings → Data log), not a control of the chart's.
    check(pg.locator("#sc-mode").count() == 0, "the chart has no aggregation menu of its own")
    pg.select_option("#sc-sensor", "env_indoor")
    pg.wait_for_timeout(500)
    pg.select_option("#sc-metric", "temperature")
    pg.select_option("#sc-range", "86400")
    pg.select_option("#sc-agg", "1h")
    pg.wait_for_timeout(2000)
    pts = pg.locator("#sc-pts").inner_text()
    msg = pg.locator("#sc-msg").inner_text()
    print("  points:", pts, "| min:", pg.locator("#sc-min").inner_text(),
          "| max:", pg.locator("#sc-max").inner_text(), "|", msg)
    check(pts.isdigit() and int(pts) >= 23,
          f"a day of hourly points, not only the ring's minutes ({pts})")
    check(pg.locator("#sc-min").inner_text().startswith("20"),
          "yesterday's archived file is read (its rows are 20.0)")
    # The last hour's bucket averages the log's 21.0 rows with the ring's 22.
    cur = float(pg.locator("#sc-current").inner_text().split()[0])
    check(21 < cur <= 22, f"the ring's live readings follow the log ({cur})")
    check(not pg.locator("#sc-max").inner_text().startswith("30"),
          "last month's file is left alone")
    check(not any("%s.txt" % lm.strftime("%Y-%m") in d for d in downloads),
          "and not even downloaded")
    check("/logs/other.txt" not in downloads, "an unrelated log is not read")
    arch = "/logs/dev_datalog_%s.txt" % yday
    check(downloads.count(arch) >= 2,
          f"a failed download is tried again, not kept as empty ({downloads.count(arch)}x)")
    check(downloads.count("/logs/dev_datalog.txt") == 1,
          f"the active file is downloaded once across the loads "
          f"({downloads.count('/logs/dev_datalog.txt')}x)")
    check(listings and all("dir=%2Flogs" in u and "recursive" not in u for u in listings),
          f"only the log's folder is listed ({listings[:1]})")
    check(pg.locator("#sensorChart .u-over").count() == 1, "the chart is drawn")

    print("\nThe last hour, in the device's zone, not the browser's:")
    pg.select_option("#sc-range", "3600")
    pg.select_option("#sc-agg", "5m")
    pg.wait_for_timeout(1500)
    pts = pg.locator("#sc-pts").inner_text()
    check(pts.isdigit() and int(pts) >= 11,
          f"the log's rows of the last hour are in range ({pts})")
    check(pg.locator("#sc-min").inner_text().startswith("21"),
          "and they are the log's 21.0, next to the ring's 22")
    pg.select_option("#sc-range", "86400")
    pg.select_option("#sc-agg", "1h")
    pg.wait_for_timeout(1500)

    msg = pg.locator("#sc-msg").inner_text()
    check("average" in msg, f"an averaged column says so ({msg!r})")

    print("\nA column logged as the highest of each interval:")
    pg.select_option("#sc-metric", "humidity")
    pg.wait_for_timeout(1500)
    msg = pg.locator("#sc-msg").inner_text()
    check("highest" in msg, f"the chart combines it the same way ({msg!r})")
    # Every bucket the log fills is 55 whatever the mode, so which way a
    # bucket is combined is checked on one that holds two values — not on the
    # chart's last hour, which holds the log's rows and the ring's readings
    # only when the page is loaded late enough in the hour (it was not, once,
    # in CI).
    got = pg.evaluate("""() => ["max", "min", "avg", "last", "sum"].map(m =>
        _scAggregate([{ts: 0, v: 55}, {ts: 10, v: 22}], 3600, m).pts.map(p => p.v))""")
    check(got == [[55], [22], [38.5], [22], [77]],
          f"a bucket is combined by the column's mode ({got})")
    cur = pg.locator("#sc-max").inner_text()
    check(cur.startswith("55"), f"and the column's highs are what is drawn ({cur})")
    # Yesterday's file says its humidity rows were averages: they are not
    # drawn as if they were highs, and the chart says so.
    pts = pg.locator("#sc-pts").inner_text()
    check(pts.isdigit() and int(pts) <= 21,
          f"only the rows logged as highs are drawn ({pts} points; the archive's 10 h are not)")
    check("another aggregation" in msg, f"and the chart says why ({msg!r})")
    pg.select_option("#sc-metric", "temperature")
    pg.wait_for_timeout(1500)

    print("\nA sum column: the log's totals alone; any other mode joins the ring:")
    # The ring's single readings are not totals of an interval: a sum column
    # is drawn from its rows.
    j = pg.evaluate("""() => _scJoin(
        [{ts: 1030, v: 3.0}, {ts: 1090, v: 1.0}],
        [{ts: 1110, v: 0.2}, {ts: 1125, v: 0.2}], "sum")""")
    check([x["v"] for x in j] == [3.0, 1.0], f"a sum column is its rows ({j})")
    j = pg.evaluate("""() => _scJoin(
        [{ts: 1030, v: 20}], [{ts: 1000, v: 21}, {ts: 1100, v: 22}], "max")""")
    check([x["v"] for x in j] == [21, 22],
          f"any other mode: the ring's readings from where the ring starts ({j})")

    print("\nA second column with the same label, told apart by position:")
    pg.select_option("#sc-sensor", "outdoor")
    pg.wait_for_timeout(500)
    pg.select_option("#sc-metric", "temperature")
    pg.wait_for_timeout(1500)
    pts = pg.locator("#sc-pts").inner_text()
    check(pts.isdigit() and int(pts) >= 23,
          f"history of a sensor with nothing in the ring ({pts})")
    check(pg.locator("#sc-current").inner_text().startswith("10.5"),
          "its values come from the log's column")

    n_before = len(downloads)
    pg.select_option("#sc-agg", "5m")
    pg.wait_for_timeout(1500)
    check(len(downloads) == n_before, "unchanged files are not downloaded again")
    pts = pg.locator("#sc-pts").inner_text()
    check(pts.isdigit() and int(pts) <= 250, f"at most 250 points drawn ({pts})")

    errors = [c for c in console if c[0] in ("error", "pageerror")
              and "404" not in c[1] and "Failed to load resource" not in c[1]]
    check(not errors, f"no page errors ({errors[:3]})")
    b.close()

print()
if fails:
    print(f"{len(fails)} check(s) failed")
    sys.exit(1)
print("all checks passed")
