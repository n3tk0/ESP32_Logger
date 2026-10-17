"""drive_overview_sparks.py — the Overview's sparklines come in one request.

Every metric card on the Overview draws an hour's sparkline. Each card made
its own /api/data request: nine on a two-sensor Collector, the larger part of
what an Overview load asked a C3 for, each with its own connection and reply
(design/c3-heap-design-1016.md). They now share one /api/sparks request.

This loads the Overview against the mock and checks that the sparklines are
drawn, from one /api/sparks request, and that a series the device leaves out
(a name it will not echo) is asked for with /api/data on its own and drawn
too, without breaking the others.

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
    pg.emulate_media(reduced_motion="no-preference")   # the sweep is skipped otherwise
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))
    # Every sweep clip the page ever inserts, counted as it goes in: the deck
    # can be drawn again after the sweep, and then shows the line whole.
    pg.add_init_script("""window.__sweeps = 0;
        new MutationObserver(function (ms) { ms.forEach(function (m) {
            m.addedNodes.forEach(function (n) {
                if (n.querySelectorAll) window.__sweeps += n.querySelectorAll('clipPath').length +
                    (n.nodeName === 'clipPath' ? 1 : 0);
            }); }); }).observe(document, { childList: true, subtree: true });""")

    pg.route("**/api/latest*", lambda r: r.fulfill(
        status=200, content_type="application/json", body=json.dumps(LATEST)))

    def api_sparks(route):
        q = parse_qs(urlparse(route.request.url).query)
        sparks.append(q.get("s", [""])[0])
        series = []
        for item in q.get("s", [""])[0].split(","):
            sensor, _, metric = item.rpartition(":")
            if metric == "pressure":
                continue                      # left out: asked for on its own
            series.append({"sensor": sensor, "metric": metric,
                           "v": [20 + (i % 5) * 0.5 for i in range(40)]})
        route.fulfill(status=200, content_type="application/json",
                      body=json.dumps({"from": NOW - 3600, "to": NOW, "series": series}))
    pg.route("**/api/sparks*", api_sparks)

    def api_data(route):
        datas.append(route.request.url)
        route.fulfill(status=200, content_type="application/json",
                      body=json.dumps({"count": 40, "data": [
                          {"ts": NOW - 3600 + i * 90, "v": 1010 + (i % 4)} for i in range(40)]}))
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
    swept = pg.evaluate("() => window.__sweeps")
    print("  sparks requests:", sparks)
    print("  drawn:", drawn)

    check(len(sparks) >= 1, "the sparklines were asked for with /api/sparks")
    check(len(sparks) == 1, "in one request (%d made)" % len(sparks))
    ragged = pg.evaluate("""() => { const by = {};
        [...document.querySelectorAll('#overview-deck > .deck-slot:not([hidden])')].forEach(s => {
            (by[s.style.top] = by[s.style.top] || []).push(s.offsetHeight); });
        return Object.values(by).filter(h => Math.max(...h) - Math.min(...h) > 1); }""")
    check(not ragged, "Overview cards in a row are one height (%s)" % ragged[:2])
    asked_data = sorted(parse_qs(urlparse(u).query).get("metric", [""])[0] for u in datas)
    check(asked_data == ["pressure"] * sum(1 for k in drawn if k.endswith("::pressure")),
          "/api/data only for the series left out (%s)" % asked_data)
    asked = sparks[0].split(",") if sparks else []
    check(len(asked) == len(set(asked)), "each series is asked for once")
    check(any(v for v in drawn.values()), "the sparklines are drawn")
    check(all(v for k, v in drawn.items() if not k.endswith("::pressure")),
          "every series the device answered is drawn")
    check(drawn.get("livingroom::pressure") is True,
          "a series left out of the answer is drawn from /api/data")
    check(swept >= 1, "a sparkline drawn for the first time sweeps in (%d)" % swept)
    errs = [(k, t) for k, t in console if k in ("error", "pageerror")]
    check(not errs, "no page errors (%s)" % errs[:3])

    # Auto-refresh, chosen while customising: never under 5 s, off by default.
    latest_off = []
    pg.unroute("**/api/latest*")
    pg.route("**/api/latest*", lambda r: (latest_off.append(time.time()), r.fulfill(
        status=200, content_type="application/json", body=json.dumps(LATEST)))[1])
    pg.wait_for_timeout(6500)
    check(len(latest_off) == 0, "off by default: no rounds on its own (%d)" % len(latest_off))
    pg.click(".deck-customise-btn")
    opts = pg.evaluate("""() => [...document.querySelectorAll('#ovAutoRefresh option')].map(o => +o.value)""")
    check(opts and min(o for o in opts if o) >= 5, "offered in Customise, none under 5 s (%s)" % opts)

    pg2 = b.new_page(viewport={"width": 1400, "height": 1000})
    # A stored 2 s (set by hand) is held to 5 s.
    pg2.add_init_script("try{localStorage.setItem('esp32logger.overview.autoRefresh','2')}catch(e){}")
    seen = []
    pg2.route("**/api/latest*", lambda r: (seen.append(time.time()), r.fulfill(
        status=200, content_type="application/json", body=json.dumps(LATEST)))[1])
    pg2.route("**/api/sparks*", api_sparks)
    pg2.route("**/api/data*", api_data)
    pg2.goto(URL, wait_until="networkidle")
    pg2.wait_for_timeout(11500)
    gaps = [round(b_ - a_, 1) for a_, b_ in zip(seen, seen[1:])]
    check(len(seen) >= 2, "the readings are read again on their own (%d)" % len(seen))
    check(all(g >= 4.8 for g in gaps), "never sooner than 5 s apart (%s)" % gaps)

    # The Sensors page: the same setting, and its sparks across the card.
    pg3 = b.new_page(viewport={"width": 1400, "height": 1000})
    pg3.on("pageerror", lambda e: console.append(("pageerror", str(e))))
    pg3.add_init_script("try{localStorage.setItem('esp32logger.overview.autoRefresh','5')}catch(e){}")
    pg3.add_init_script("""window.__emptied = 0;
        document.addEventListener('DOMContentLoaded', function () {
            var g = document.getElementById('sensors-grid'); if (!g) return;
            new MutationObserver(function () { if (!g.children.length) window.__emptied++; })
                .observe(g, { childList: true }); });""")
    s_seen, s_data = [], []

    def s_sensors(route):
        # The mock's sensors with an hour's spark on each: one moving, one flat.
        d = route.fetch().json()
        for i, sn in enumerate(d.get("sensors") or []):
            # The first: a moving hour with one reading far below the rest.
            sn["spark"] = [5 if k == 12 else 20 + (k % 7) * 0.4 for k in range(24)] if i == 0 else [1012] * 24
            if sn.get("id") == "livingroom":   # six cards: two even rows, not 4+2
                sn["metrics"] = list(sn.get("metrics") or []) + ["gas_resistance", "tvoc"]
        route.fulfill(status=200, content_type="application/json", body=json.dumps(d))

    def s_series(route):
        s_data.append(time.time())
        pts = [{"t": NOW - 3600 + k * 120, "v": 40 + (k % 5)} for k in range(30)]
        route.fulfill(status=200, content_type="application/json",
                      body=json.dumps({"ok": True, "data": pts}))
    pg3.route("**/api/sensors*", s_sensors)
    pg3.route("**/api/data*", s_series)
    pg3.on("request", lambda r: s_seen.append(time.time()) if "/api/sensors" in r.url else None)
    pg3.goto(BASE + "/#sensors", wait_until="networkidle")
    pg3.wait_for_selector("#sensors-grid .sensor svg.s-spark polyline", timeout=15000)
    geo = pg3.evaluate("""() => [...document.querySelectorAll('#sensors-grid .sensor')]
        .filter(c => c.querySelector('svg.s-spark polyline')).map(c => {
            const r = c.getBoundingClientRect(), sv = c.querySelector('svg.s-spark').getBoundingClientRect(),
                  f = c.querySelector('.s-foot > span').getBoundingClientRect();
            return { dl: sv.left - r.left, dr: r.right - sv.right, db: r.bottom - sv.bottom,
                     footIn: f.top >= sv.top && f.bottom <= sv.bottom && f.left >= sv.left }; })""")
    check(geo and all(g["dl"] <= 1.5 and g["dr"] <= 1.5 and g["db"] <= 1.5 for g in geo),
          "Sensors sparks run edge to edge and down to the card's bottom (%s)" % geo[:2])
    check(geo and all(g["footIn"] for g in geo), "the age sits inside the graph's area")
    first = pg3.evaluate("""() => { const p = document.querySelector('#sensors-grid svg.s-spark polyline');
        const ys = p.getAttribute('points').split(' ').map(q => +q.split(',')[1]);
        const card = p.closest('.sensor');
        return { ys: ys, scale: card.querySelector('.s-scale').textContent }; }""")
    body = sorted(first["ys"])[1:]          # all but the far reading
    check(max(body) - min(body) >= 10,
          "one far reading does not press the rest flat (%.1f of 19)" % (max(body) - min(body)))
    check(first["scale"].startswith("\u2193") and "\u2191" in first["scale"] and "5" in first["scale"],
          "the card gives the graph's true low and high (%r)" % first["scale"])
    rows = pg3.evaluate("""() => { const g = document.querySelector('#sensors-grid .s-group[data-sid="livingroom"] .s-group-cards');
        const by = {}; [...g.children].forEach(c => { by[c.offsetTop] = (by[c.offsetTop] || 0) + 1; });
        return Object.values(by); }""")
    check(len(rows) >= 2 and max(rows) - min(rows) <= 1, "six cards sit in even rows (%s)" % rows)
    head = pg3.evaluate("""() => document.querySelector('#sensors-grid .s-group[data-sid="livingroom"] .s-group-head').innerText""")
    check(head.startswith("livingroom") and "bme688" not in head, "the heading is the id, then what it is (%r)" % head)
    data_first = len(s_data)
    pg3.wait_for_timeout(11500)
    rounds = [t for t in s_seen]
    s_gaps = [round(b_ - a_, 1) for a_, b_ in zip(rounds, rounds[1:])]
    check(len(rounds) >= 3, "Sensors reads again on the Overview's setting (%d)" % len(rounds))
    check(all(g >= 4.8 for g in s_gaps), "Sensors rounds are 5 s apart or more (%s)" % s_gaps)
    check(len(s_data) == data_first,
          "the other metrics' sparks are not asked for again within a minute (%d -> %d)" % (data_first, len(s_data)))
    check(pg3.evaluate("window.__emptied") <= 1, "a round redraws in place, the grid is never emptied")
    check(pg3.locator("#sensors-grid svg.s-spark polyline").count() >= 1, "the sparks are still drawn")
    flat = pg3.evaluate("""() => { const s = [...document.querySelectorAll('#sensors-grid svg.s-spark polyline')]
        .map(p => p.getAttribute('points')).find(t => new Set(t.split(' ').map(q => q.split(',')[1])).size === 1);
        return s ? +s.split(' ')[0].split(',')[1] : null; }""")
    check(flat is not None and 8 <= flat <= 18, "a flat hour is a line across the middle (y=%s)" % flat)
    # Off the page, its cards are left as they are (a round would replace them).
    pg3.evaluate("document.querySelector('#sensors-grid .sensor').dataset.mark = '1'")
    pg3.evaluate("location.hash = '#overview'")
    pg3.wait_for_timeout(11000)
    check(pg3.evaluate("!!document.querySelector('#sensors-grid .sensor[data-mark]')"),
          "and stops once another page is shown")
    b.close()

print("FAIL" if fails else "PASS", "(%d failed)" % len(fails))
sys.exit(1 if fails else 0)
