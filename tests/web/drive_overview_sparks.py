"""drive_overview_sparks.py — the Overview's sparklines come in one request.

Every metric card on the Overview draws an hour's sparkline. Each card made
its own /api/data request: nine on a two-sensor Collector, the larger part of
what an Overview load asked a C3 for, each with its own connection and reply
(design/c3-heap-design-1016.md). They now share one /api/sparks request.

This loads the Overview against the mock and checks that the sparklines are
drawn, from one /api/sparks request and no /api/data request, and that a
series the device leaves out is drawn as nothing rather than breaking the
others.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_overview_sparks.py
"""
import json
import os
import sys
import time
from urllib.parse import urlparse, parse_qs

from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT
URL  = BASE + "/#overview"

fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


NOW = int(time.time())
LATEST = {"ok": True, "ts": 100, "items": [
    {"id": "livingroom", "type": "bme688", "metric": m, "value": v, "unit": u, "ts": NOW, "q": 0}
    for m, v, u in (("temperature", 22.5, "C"), ("humidity", 48, "%"),
                    ("pressure", 1012, "hPa"), ("aqi", 40, ""))
] + [
    {"id": "balcony", "type": "bmp280", "metric": m, "value": v, "unit": u, "ts": NOW, "q": 0}
    for m, v, u in (("temperature", 12.0, "C"), ("pressure", 1010, "hPa"))
]}

sparks = []
datas = []

with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page(viewport={"width": 1400, "height": 1000})
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))

    pg.route("**/api/latest*", lambda r: r.fulfill(
        status=200, content_type="application/json", body=json.dumps(LATEST)))

    def api_sparks(route):
        q = parse_qs(urlparse(route.request.url).query)
        sparks.append(q.get("s", [""])[0])
        series = []
        for item in q.get("s", [""])[0].split(","):
            sensor, _, metric = item.rpartition(":")
            if metric == "pressure":
                continue                      # left out: drawn as nothing
            series.append({"sensor": sensor, "metric": metric,
                           "v": [20 + (i % 5) * 0.5 for i in range(40)]})
        route.fulfill(status=200, content_type="application/json",
                      body=json.dumps({"from": NOW - 3600, "to": NOW, "series": series}))
    pg.route("**/api/sparks*", api_sparks)

    def api_data(route):
        datas.append(route.request.url)
        route.fulfill(status=200, content_type="application/json",
                      body=json.dumps({"count": 0, "data": []}))
    pg.route("**/api/data*", api_data)

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1500)

    drawn = pg.evaluate("""() => {
        const out = {};
        document.querySelectorAll('svg[data-drawn-for]').forEach(s => {
            out[s.dataset.drawnFor] = !!s.querySelector('polyline');
        });
        return out;
    }""")
    print("  sparks requests:", sparks)
    print("  drawn:", drawn)

    check(len(sparks) >= 1, "the sparklines were asked for with /api/sparks")
    check(len(sparks) == 1, "in one request (%d made)" % len(sparks))
    check(not datas, "and no /api/data request (%d made)" % len(datas))
    asked = sparks[0].split(",") if sparks else []
    check(len(asked) == len(set(asked)), "each series is asked for once")
    check(any(v for v in drawn.values()), "the sparklines are drawn")
    check(all(v for k, v in drawn.items() if not k.endswith("::pressure")),
          "every series the device answered is drawn")
    check("livingroom::pressure" in drawn and not drawn["livingroom::pressure"],
          "a series left out of the answer is drawn as nothing")
    errs = [(k, t) for k, t in console if k in ("error", "pageerror")]
    check(not errs, "no page errors (%s)" % errs[:3])
    b.close()

print("FAIL" if fails else "PASS", "(%d failed)" % len(fails))
sys.exit(1 if fails else 0)
