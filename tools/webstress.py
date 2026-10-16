#!/usr/bin/env python3
"""
webstress.py — load a Collector the way its pages and readers do, and say
whether it held.

WHY THIS EXISTS
---------------
Every heap problem on the C3 so far was found by flashing a build and
opening the Overview: an empty reply, a PANIC, a TASK_WDT reset, each one a
flash-and-wait cycle (design/c3-heap-design-1016.md). This does the opening
for you, many times over, while it watches /api/diag:

  * clients load the Overview, the Sensors page and the Kindle payload in
    parallel, the way a browser tab, a second tab and a Kindle would;
  * every 10 s it reads /api/diag: uptime (a drop is a reset), the heap low
    mark, the budget's refusals, the reply pool;
  * at the end it prints what it saw and PASS or FAIL.

PASS means: no reset, every request answered (200, or 503 the page would
ask again), and the heap low mark above --min-heap.

    python3 tools/webstress.py 192.168.100.8
    python3 tools/webstress.py 192.168.100.8 --minutes 10 --clients 3

Only the Python standard library. Run it from a machine on the same network
as the Collector; it changes nothing on the device (GET requests only).
"""
import argparse
import json
import random
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import Counter


def get(base, path, timeout=15):
    t0 = time.time()
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            body = r.read()
            return r.status, body, time.time() - t0
    except urllib.error.HTTPError as e:
        return e.code, b"", time.time() - t0
    except Exception as e:  # timeout, reset, refused
        return type(e).__name__, b"", time.time() - t0


def sensors_of(base):
    st, body, _ = get(base, "/api/sensors")
    if st != 200:
        return []
    try:
        return [(s["id"], m) for s in json.loads(body).get("sensors", [])
                for m in s.get("metrics", [])]
    except Exception:
        return []


def scenarios(base, pairs):
    now = int(time.time())
    frm = now - 3600
    sparks = ",".join("%s:%s" % p for p in pairs[:12])
    overview = ["/api/status", "/export_settings", "/api/latest", "/api/sensors",
                "/api/alerts"]
    if sparks:
        overview.append("/api/sparks?s=%s&from=%d&to=%d&limit=40" % (sparks, frm, now))
    sensors = ["/api/sensors", "/export_settings"] + [
        "/api/data?sensor=%s&metric=%s&from=%d&to=%d&agg=raw&mode=lttb&limit=300"
        % (s, m, frm, now) for s, m in pairs[:6]]
    kindle = ["/kindle/data"]
    return {"overview": overview, "sensors": sensors, "kindle": kindle}


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.codes = Counter()
        self.by_path = {}
        self.slow = 0

    def add(self, path, code, secs):
        key = path.split("?")[0]
        with self.lock:
            self.codes[code] += 1
            self.by_path.setdefault(key, Counter())[code] += 1
            if secs > 5:
                self.slow += 1


def client(base, stop, stats, pairs, parallel):
    while not stop.is_set():
        name, paths = random.choice(list(scenarios(base, pairs).items()))
        # A page asks a few at a time (www/js/core.js FETCH_MAX).
        for i in range(0, len(paths), parallel):
            batch = paths[i:i + parallel]
            ts = [threading.Thread(target=lambda p=p: stats.add(p, *get(base, p)[::2]))
                  for p in batch]
            for t in ts: t.start()
            for t in ts: t.join()
            if stop.is_set():
                return
        time.sleep(random.uniform(0.5, 3.0))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("host")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--clients", type=int, default=3)
    ap.add_argument("--parallel", type=int, default=3)
    ap.add_argument("--min-heap", type=int, default=4096,
                    help="the heap low mark must stay above this (bytes)")
    a = ap.parse_args()
    base = a.host if a.host.startswith("http") else "http://" + a.host

    st, body, _ = get(base, "/api/diag")
    if st != 200:
        print("cannot read %s/api/diag (%s)" % (base, st))
        return 2
    d0 = json.loads(body)
    pairs = sensors_of(base)
    print("device up %ss, free heap %s, low mark %s, %d series" % (
        d0.get("uptime"), d0.get("free_heap"), d0.get("min_free_heap"), len(pairs)))

    stats, stop = Stats(), threading.Event()
    threads = [threading.Thread(target=client, args=(base, stop, stats, pairs, a.parallel),
                                daemon=True) for _ in range(a.clients)]
    for t in threads: t.start()

    end = time.time() + a.minutes * 60
    resets, last_up, low, d = 0, d0.get("uptime", 0), d0.get("min_free_heap", 1 << 30), d0
    while time.time() < end:
        time.sleep(10)
        st, body, _ = get(base, "/api/diag")
        if st != 200:
            print("  %s  /api/diag: %s" % (time.strftime("%H:%M:%S"), st))
            continue
        d = json.loads(body)
        up = d.get("uptime", 0)
        if up < last_up:
            resets += 1
            print("  %s  RESET (uptime %s -> %s)" % (time.strftime("%H:%M:%S"), last_up, up))
        last_up = up
        low = min(low, d.get("min_free_heap", low))
        b, pool = d.get("budget", {}), d.get("reply_pool", {})
        print("  %s  up %5ss  free %6s  low %6s  refused %s  tls %s/%s forced  pool min %s  %s" % (
            time.strftime("%H:%M:%S"), up, d.get("free_heap"), d.get("min_free_heap"),
            b.get("web_refused", "-"), b.get("heavy_runs", "-"), b.get("heavy_forced", "-"),
            pool.get("min_free", "-"), dict(stats.codes)))
    stop.set()
    for t in threads: t.join(timeout=20)

    total = sum(stats.codes.values())
    bad = {k: v for k, v in stats.codes.items() if k not in (200, 503)}
    print("\nrequests: %d  %s" % (total, dict(stats.codes)))
    for path, c in sorted(stats.by_path.items()):
        print("  %-22s %s" % (path, dict(c)))
    print("slower than 5 s: %d" % stats.slow)
    print("resets: %d   heap low mark: %s" % (resets, low))
    fails = []
    if resets:
        fails.append("the device reset %d time(s)" % resets)
    if bad:
        fails.append("requests not answered 200/503: %s" % bad)
    if low < a.min_heap:
        fails.append("heap low mark %s under %s" % (low, a.min_heap))
    if (d.get("counters") or {}).get("resets", 0) > (d0.get("counters") or {}).get("resets", 0):
        fails.append("the device counted a reset")
    print("FAIL: " + "; ".join(fails) if fails else "PASS")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
