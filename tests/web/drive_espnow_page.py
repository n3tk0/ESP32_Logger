"""drive_espnow_page.py — click the ESP-NOW settings page in a real browser.

The page is not compiled, so nothing else in this repository would notice if it
fetched a field the firmware does not send, referenced an icon that is not in
the sheet, or wired a button to a handler core.js never registered. This opens
it in Chromium against tests/web/mock_device.py and asserts on what is painted.

The assertions that matter are the two NULLS. /api/espnow/status sends null for
an unavailable RSSI (IDF 4.4 gives the receive callback no signal information)
and for a remaining life the battery model refuses to estimate. Both must read
as a dash. A zero in either would be a number somebody believes.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_espnow_page.py
"""
import sys
from playwright.sync_api import sync_playwright

import os
PORT = os.environ.get("MOCK_PORT", "8765")
URL = "http://127.0.0.1:" + PORT + "/#settings_espnow"
fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


with sync_playwright() as p:
    # Honour an explicit browser path when one is set (this sandbox ships
    # Chromium at a fixed location and blocks the download), otherwise let
    # Playwright find its own.
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page()
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1200)

    sec = pg.locator("#page-settings_espnow")
    check(sec.count() == 1, "the page partial was fetched and injected")

    # LOWERCASED. .badge carries text-transform:uppercase, so inner_text()
    # returns what is PAINTED, not what the DOM holds. The first version of
    # this file compared case-sensitively and reported five failures against a
    # page that was rendering perfectly.
    body = pg.locator("#en-nodes").inner_text().lower()
    check("outdoor" in body, "the first node is listed")
    check("balcony" in body, "the second node is listed")
    check("62%" in body, "a battery percentage is shown")
    check("237 d" in body, "a remaining-life figure is shown")

    # The two nulls must read as a dash, not as 0.
    check("rssi: —" in body, "an unavailable RSSI shows a dash")
    check("left: —" in body, "an undeterminable remaining life shows a dash")
    check("0 dBm" not in body, "a missing RSSI is never printed as 0 dBm")
    check("left: 0 d" not in body, "a null day count is never printed as 0 d")

    # Clock drift. Three cases, and the page has to keep them apart. The badge
    # reads as the node's clock relative to the collector's, so a node that is
    # behind reads negative — the API's own sign is the other way round, being
    # "how far ahead of the node are we".
    #   skew_s 3     the node is three seconds behind: "-3s"
    #   skew_s -184  the node is three minutes AHEAD: "+3m". A negative on the
    #                wire, and the sign the firmware's uint32 subtraction used
    #                to lose to a wrap.
    #   skew_s null  never measured, which is NOT zero. A page that printed
    #                "0s" here would be claiming a perfectly synchronised node.
    check("clock: -3s" in body, "a node behind the collector reads as negative")
    check("clock: +3m" in body, "a node ahead of the collector reads as positive")
    check("clock: —" in body, "an unmeasured drift shows a dash")
    check("clock: 0s" not in body, "an unmeasured drift is never printed as 0s")

    # And the threshold is coloured: 184 s is past ESPNOW_SKEW_WARN_S, which is
    # also the drift that writes a line to /error_log.txt.
    warn_texts = pg.locator("#en-nodes .badge.warn").all_inner_texts()
    check(any("3m" in x.lower() for x in warn_texts),
          "a drift past the warning threshold is coloured as one")

    # A node that has never reported.
    check("never" in body, "a node that has never reported says so")

    online = pg.locator("#en-nodes .badge.ok").count()
    offline = pg.locator("#en-nodes .badge.err").count()
    check(online >= 1 and offline >= 1, f"online/offline pills render ({online} ok, {offline} err)")

    # Icons were swapped for real SVG rather than left as empty spans.
    svgs = pg.locator("#en-nodes svg").count()
    check(svgs > 0, f"icons were substituted ({svgs} svg)")

    warn_badges = pg.locator("#en-nodes .badge.err").all_inner_texts()
    check(any("8%" in x for x in warn_badges),
          "a battery under the warning threshold is coloured as one")

    # Counters
    stats = pg.locator("#en-stats").inner_text()
    check("1528" in stats, "the accepted-frame counter is shown")
    check("bad pairing signature" in stats, "the failure counters are labelled")
    # The two backfill counters are separate on purpose — an overflowing queue
    # and a burst with no clock ask for opposite fixes — so the page has to
    # show both rather than one number covering them.
    check("backfill dropped" in stats and "backfill with no clock" in stats,
          "the two backfill causes are shown apart")

    # Pairing button end to end: click, POST, badge flips.
    check(pg.locator("#en-pair-state").inner_text().strip().lower() == "closed", "pairing starts closed")
    pg.click('[data-click="espnowPair"]')
    pg.wait_for_timeout(900)
    check(pg.locator("#en-pair-state").inner_text().strip().lower() == "open", "clicking opens the window")
    msg = pg.locator("#en-msg").inner_text()
    check("Pairing window open" in msg, f"the page confirms it: {msg.strip()[:48]!r}")

    # Rename, and confirm it round-trips through the API and re-renders.
    pg.fill("#en-label-1", "garden")
    pg.click('[data-click="espnowSaveNode"]')
    pg.wait_for_timeout(900)
    check("garden" in pg.locator("#en-nodes").inner_text().lower(), "a rename round-trips and re-renders")

    # Forget, with the confirm dialog accepted.
    pg.on("dialog", lambda d: d.accept())
    pg.click('[data-click="espnowForget"] >> nth=1')
    pg.wait_for_timeout(900)
    check("balcony" not in pg.locator("#en-nodes").inner_text().lower(), "forgetting removes the node")

    shot = os.environ.get("SCREENSHOT")
    if shot:
        pg.screenshot(path=shot, full_page=True)
    b.close()

# The mock does not serve every route the SPA touches on boot (/export_settings
# is a plain download link in the settings hub). Those 404s are the harness's
# gap, not the page's, so only real script errors count.
errs = [c for c in console
        if c[0] == "pageerror"
        or (c[0] == "error" and "404" not in c[1] and "Failed to load resource" not in c[1])]
print()
for t, m in errs[:8]:
    print(f"  console {t}: {m[:140]}")
check(not errs, f"no console errors ({len(errs)})")

print()
print(("FAIL: %d" % len(fails)) if fails else "OK: the page works against the API contract")
sys.exit(1 if fails else 0)
