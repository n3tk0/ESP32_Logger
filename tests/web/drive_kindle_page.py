"""drive_kindle_page.py — click the e-ink dashboard settings page in Chromium.

Same reasoning as drive_espnow_page.py: the page is not compiled, so nothing
else here would notice a select that never reflects what the device holds, a
checkbox whose bit does not match src/core/Config.h, or a button wired to a
handler core.js never registered.

The assertion that matters most is the BITMASKS. Weight and visibility travel
as two integers whose bits are defined in the firmware and repeated in
www/js/kindle.js, and a page that reads them back correctly while WRITING the
neighbouring bit looks perfect and quietly toggles the wrong setting. So the
driver sets a mask, reads it back off the wire, and compares against the value
the firmware's own constants say it should be.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_kindle_page.py
"""
import os
import sys
from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT
URL = BASE + "/#settings_kindle"
fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


# What mock_device.py starts with — not the defaults, on purpose: a page that
# renders correctly only when every value is zero has never had its selects
# proven against anything.
START = dict(face="4", clock="3", time="2", date="1", press="1", dec="0")

with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page()
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1200)

    check(pg.locator("#page-settings_kindle").count() == 1,
          "the page partial was fetched and injected")

    # Every select shows what the device holds, not its own first option.
    for el, want in (("kd-face", START["face"]), ("kd-clock", START["clock"]),
                     ("kd-time", START["time"]), ("kd-date", START["date"]),
                     ("kd-press", START["press"]), ("kd-dec", START["dec"])):
        got = pg.input_value("#" + el)
        check(got == want, f"#{el} reflects the device ({got!r})")

    # The two checkbox groups are built by kindle.js from its own copy of the
    # firmware's bit values, so their COUNT is the first thing to check: a
    # missing row means a setting nobody can reach.
    check(pg.locator("#kd-bold input[type=checkbox]").count() == 9,
          "every weight zone has a checkbox")
    check(pg.locator("#kd-show input[type=checkbox]").count() == 8,
          "every block has a checkbox")

    # bold = 0x0009 in the mock: outdoor temperature (0x1) and clock (0x8).
    check(pg.is_checked("#kd-b-1") and pg.is_checked("#kd-b-8"),
          "the set weight bits come back checked")
    check(not pg.is_checked("#kd-b-2"), "an unset weight bit comes back clear")
    # show = 0xFF minus the week strip (0x40).
    check(not pg.is_checked("#kd-s-64"), "the one cleared block comes back clear")
    check(pg.is_checked("#kd-s-32"), "a set block comes back checked")

    # The custom-face field is hidden unless the custom face is chosen, and the
    # date hint is hidden when the dated clock is: a visible control that does
    # nothing is a question the page asks and then ignores the answer to.
    check(not pg.is_visible("#kd-face-custom-row"),
          "the custom font field is hidden for a named face")
    pg.select_option("#kd-face", "6")
    pg.wait_for_timeout(200)
    check(pg.is_visible("#kd-face-custom-row"),
          "choosing Custom reveals the font field")
    pg.select_option("#kd-face", "0")
    pg.wait_for_timeout(200)
    check(not pg.is_visible("#kd-face-custom-row"),
          "choosing a named face hides it again")

    check(not pg.is_visible("#kd-date-hint"),
          "the dated clock does not tell you the date is unused")
    pg.select_option("#kd-clock", "0")
    pg.wait_for_timeout(200)
    check(pg.is_visible("#kd-date-hint"),
          "another clock style says where the date format applies")

    # The page states the build-time width rather than offering it as a knob.
    intro = pg.locator("#kd-pagew").inner_text()
    check("600" in intro, f"the layout width is stated: {intro.strip()[:60]!r}")

    # ── Save, and read the wire ─────────────────────────────────────────────
    # Bits flipped in both directions so a handler that only ever ORs, or one
    # that writes a constant, cannot pass.
    pg.select_option("#kd-face", "2")       # Palatino
    pg.select_option("#kd-clock", "1")      # boxed
    pg.select_option("#kd-time", "0")       # 24 h
    pg.select_option("#kd-press", "0")      # hPa
    pg.select_option("#kd-dec", "1")
    pg.uncheck("#kd-b-1")                   # clear outdoor temperature
    pg.check("#kd-b-4")                     # set pressure
    pg.check("#kd-s-64")                    # put the week strip back
    pg.uncheck("#kd-s-2")                   # and drop the pressure block
    pg.click('[data-click="kindleSave"]')
    pg.wait_for_timeout(900)

    msg = pg.locator("#kd-msg").inner_text()
    check("next repaint" in msg, f"the page confirms it: {msg.strip()[:48]!r}")

    got = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(got["face"] == 2, f"the face round-trips (got {got['face']})")
    check(got["clock_style"] == 1, f"the clock style round-trips (got {got['clock_style']})")
    check(got["pressure_unit"] == 0, "the pressure unit round-trips")
    check(got["decimals"] == 1, "the decimal count round-trips")
    # 0x0009 - 0x0001 + 0x0004 = 0x000C
    check(got["bold"] == 0x000C, f"the weight mask is exactly right (got {got['bold']:#06x})")
    # (0xFF - 0x40) + 0x40 - 0x02 = 0xFD
    check(got["show"] == 0x00FD, f"the visibility mask is exactly right (got {got['show']:#06x})")

    # Restore, with the confirm dialog accepted.
    pg.on("dialog", lambda d: d.accept())
    pg.click('[data-click="kindleDefaults"]')
    pg.wait_for_timeout(900)
    got = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(got["face"] == 0 and got["bold"] == 0 and got["show"] == 0xFF and
          got["clock_style"] == 0 and got["decimals"] == 1,
          "restoring puts back the built-in design")
    check(pg.input_value("#kd-face") == "0", "and the form re-renders as restored")

    shot = os.environ.get("SCREENSHOT")
    if shot:
        pg.screenshot(path=shot, full_page=True)
    b.close()

# Resource 404s are the mock's gap, not the page's — see drive_espnow_page.py.
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
