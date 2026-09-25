"""drive_pin_pages.py — the Collector's pin fields, in a real browser.

WHAT IS UNDER TEST
------------------
Every page of the Collector that takes a GPIO now does it the way the node's
own setup page does (www/js/pins.js): a pin is typed as the board prints it
("D9") or as a GPIO ("12"), the line under the field says what it resolved
to, a boot strap or the console UART is a yellow warning rather than a
refusal, and only what no wiring fixes — the flash bus, a GPIO the chip does
not have, a pin something else already uses — is red and stops the save.

The mock's board profiles are read from src/core/BoardProfiles.cpp and the
drawings from www/boards.json, so this runs against the lists the firmware
really has. The active board is the Seeed XIAO ESP32-C3:
D0=2 D1=3 D2=4 D3=5 D4=6 D5=7 D6=21 D7=20 D8=8 D9=9 D10=10.

Four pages, in the order a device meets them:
  1. first-run wizard  — the build's board preselected, labels, save payload
  2. Hardware          — labels shown back, a clash with a sensor's SDA
  3. sensor editor     — a strap pin saved with allow_unsafe_pins, a clash
  4. add-sensor wizard — a flash pin stops Next, a strap pin sets the flag,
                         a UART sensor keeps the baud it was given
  5. Hardware again    — with /api/board-profiles failing, a typed pin is
                         still what gets saved, and the next load retries

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_pin_pages.py
"""
import json
import os
import sys

from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT

fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def hint(pg, key):
    el = pg.locator('[data-pin-hint="%s"]' % key).first
    return (el.get_attribute("class") or "", el.inner_text())


def mock_hw(pg):
    return json.loads(pg.request.get(BASE + "/__mock/hw").text())


with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page(viewport={"width": 390, "height": 900})
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))
    # The Hardware save asks "save and restart?" first.
    pg.on("dialog", lambda d: d.accept())

    # ── 1. First-run ─────────────────────────────────────────────────────────
    print("First-run wizard:")
    pg.goto(BASE + "/firstrun.html", wait_until="networkidle")
    pg.wait_for_timeout(600)
    check(pg.locator("#profile").input_value() == "xiao_c3",
          "the board this image was built for is preselected")
    check("D10" in pg.locator("#pinMap").inner_text(),
          "and its header is drawn with the printed labels")

    pg.fill("#pin-wifiTrigger", "D9")
    cls, txt = hint(pg, "wifiTrigger")
    check("warn" in cls and "GPIO9" in txt,
          f"D9 resolves to GPIO9 and is a warning, not a refusal: {txt!r}")

    pg.fill("#pin-flowSensor", "12")
    cls, txt = hint(pg, "flowSensor")
    check("err" in cls and "flash" in txt.lower(), f"GPIO12 is the flash bus, red: {txt!r}")

    pg.fill("#pin-wakeupFF", "gpio9")
    cls, txt = hint(pg, "wakeupFF")
    check("err" in cls and "WiFi" in txt, f"a second use of GPIO9 names the first: {txt!r}")

    before = len(mock_hw(pg)["posts"])
    pg.click("#saveBtn")
    pg.wait_for_timeout(400)
    check(len(mock_hw(pg)["posts"]) == before, "a red pin stops the save before anything is sent")

    pg.fill("#pin-wakeupFF", "D1")
    pg.fill("#pin-flowSensor", "D6")
    pg.click("#saveBtn")
    pg.wait_for_timeout(500)
    posts = mock_hw(pg)["posts"]
    body = posts[-1]["body"] if len(posts) > before else {}
    pins = body.get("pins", {})
    check(pins.get("wifiTrigger") == 9 and pins.get("wakeupFF") == 3 and pins.get("flowSensor") == 21,
          f"the device is sent GPIO numbers, never labels: {pins}")
    check(pins.get("wakeupPF") == -1, "and -1 for a field left empty")

    # ── 2. Hardware ──────────────────────────────────────────────────────────
    print("\nHardware page:")
    pg.goto(BASE + "/#settings_hardware", wait_until="networkidle")
    pg.wait_for_timeout(1500)
    check(pg.locator("#pin-pinSdCS").input_value() == "D10",
          "a stored GPIO10 is shown as the board prints it (D10)")
    check(pg.locator("#pin-pinSdSCK").input_value() == "",
          "an unassigned pin (255) is an empty box, not 255")
    check("D2" in pg.locator("#hwPinMap").inner_text(), "the header is drawn")
    used = pg.eval_on_selector_all("#hwPinMap .pm-chip.use", "l => l.map(e => e.textContent)")
    check(any(u.startswith("D2") for u in used),
          f"the sensor list's SDA (GPIO4 = D2) is marked as taken: {used}")

    pg.fill("#pin-pinWifiTrigger", "D2")
    cls, txt = hint(pg, "pinWifiTrigger")
    check("err" in cls and "env_indoor" in txt,
          f"putting a button on that sensor's SDA is red, and says whose it is: {txt!r}")
    before = len(mock_hw(pg)["posts"])
    pg.locator('#hw-host button[type="submit"]').click()
    pg.wait_for_timeout(500)
    check(len(mock_hw(pg)["posts"]) == before, "and the save stops there")

    pg.fill("#pin-pinWifiTrigger", "D4")
    pg.locator('#hw-host button[type="submit"]').click()
    pg.wait_for_timeout(800)
    posts = mock_hw(pg)["posts"]
    sent = posts[-1]["body"] if len(posts) > before else {}
    check(sent.get("pinWifiTrigger") == "6", f"D4 reaches /save_hardware as 6: {sent.get('pinWifiTrigger')!r}")
    check(sent.get("pinSdSCK") == "-1", "and the empty SD SCK as -1")

    # ── 3. Sensor editor ─────────────────────────────────────────────────────
    print("\nSensor editor:")
    # A real device restarts after a Hardware save; a reload is the nearest
    # thing, and it is what re-reads the pins the editor checks against.
    pg.reload(wait_until="networkidle")
    pg.wait_for_timeout(1500)
    row = pg.locator(".sensor-list-row", has_text="env_indoor").first
    row.locator('button[data-click="clEditSensor"]').first.click()
    pg.wait_for_timeout(800)
    sda = pg.locator("#pin-s-sda")
    check(sda.count() == 1 and sda.input_value() == "D2", "SDA=4 opens as D2")
    sda.fill("D8")
    cls, txt = hint(pg, "s-sda")
    check("warn" in cls, f"D8 is a strap: yellow, allowed: {txt!r}")
    check(pg.locator("#sensor-pinwarn").is_visible(), "and the page says it will set allow_unsafe_pins")

    scl = pg.locator("#pin-s-scl")
    scl.fill("D4")               # GPIO6 — the WiFi button the Hardware save just stored
    cls, txt = hint(pg, "s-scl")
    check("err" in cls and "WiFi" in txt, f"a Hardware pin is red here too: {txt!r}")
    pg.locator('#sensorPopup button#sensorPopupSaveBtn, button[data-role="save"]').first.click()
    pg.wait_for_timeout(400)
    s0 = pg.evaluate("PCFG.sensors[0]")
    check(s0.get("scl") == 0, "the red pin was not written back")

    scl.fill("D5")
    pg.locator('#sensorPopup button#sensorPopupSaveBtn, button[data-role="save"]').first.click()
    pg.wait_for_timeout(400)
    s0 = pg.evaluate("PCFG.sensors[0]")
    check(s0.get("sda") == 8 and s0.get("scl") == 7, f"saved as GPIOs: sda={s0.get('sda')} scl={s0.get('scl')}")
    check(s0.get("allow_unsafe_pins") is True, "with allow_unsafe_pins set for the strap")

    # A sensor whose pins the form does not show (they live in advanced JSON,
    # like an HC-SR04's trig_pin/echo_pin) keeps the flag its strap needs.
    pg.evaluate("""PCFG.sensors.push({id: "sonar", type: "hcsr04", enabled: true,
        interface: "gpio", trig_pin: 2, echo_pin: 1, read_interval_ms: 10000,
        allow_unsafe_pins: true}); clRenderSensors(PCFG.sensors)""")
    pg.wait_for_timeout(300)
    row = pg.locator(".sensor-list-row", has_text="sonar").first
    row.locator('button[data-click="clEditSensor"]').first.click()
    pg.wait_for_timeout(600)
    pg.locator('#sensorPopup button#sensorPopupSaveBtn, button[data-role="save"]').first.click()
    pg.wait_for_timeout(400)
    sn = pg.evaluate("PCFG.sensors.filter(function (x) { return x.id === 'sonar'; })[0]")
    check(sn.get("trig_pin") == 2 and sn.get("allow_unsafe_pins") is True,
          f"a strap in advanced JSON (trig_pin=2) keeps allow_unsafe_pins: {sn.get('allow_unsafe_pins')!r}")

    # And the flag goes when no pin needs it any more.
    row = pg.locator(".sensor-list-row", has_text="env_indoor").first
    row.locator('button[data-click="clEditSensor"]').first.click()
    pg.wait_for_timeout(600)
    pg.locator("#pin-s-sda").fill("D2")
    pg.locator('#sensorPopup button#sensorPopupSaveBtn, button[data-role="save"]').first.click()
    pg.wait_for_timeout(400)
    s0 = pg.evaluate("PCFG.sensors[0]")
    check(s0.get("sda") == 4 and "allow_unsafe_pins" not in s0,
          f"moved off the strap, the flag is dropped: sda={s0.get('sda')} flag={s0.get('allow_unsafe_pins')!r}")

    # ── 4. Add-sensor wizard ─────────────────────────────────────────────────
    print("\nAdd-sensor wizard:")
    check(not pg.locator("#sensorPopup").is_visible(), "the editor closed on the good save")
    # The page's own "+ Add Sensor" button; clicked from script because at
    # phone width the bottom navigation bar sits over it.
    pg.evaluate("document.querySelector('button[data-click=\"clAddSensor\"]').click()")
    pg.wait_for_timeout(800)
    pg.locator("#wizNext").click()
    pg.locator("#wizNext").click()
    pg.wait_for_timeout(200)
    pg.select_option("#wiz-iface", "Pulse")
    pin = pg.locator("#pin-wiz-pin")
    pin.fill("13")
    cls, txt = hint(pg, "wiz-pin")
    check("err" in cls, f"GPIO13 is the flash bus: {txt!r}")
    pg.locator("#wizNext").click()
    pg.wait_for_timeout(200)
    check(pg.locator('.wiz-step[data-step="3"]').is_visible(), "Next stays on the pins step")
    pin.fill("D0")
    pg.locator("#wizNext").click()
    pg.wait_for_timeout(300)
    review = json.loads(pg.locator("#wiz-json").inner_text() or "{}")
    check(review.get("pin") == 2, f"D0 is GPIO2 in the review: {review.get('pin')!r}")
    check(review.get("allow_unsafe_pins") is True, "and the strap sets allow_unsafe_pins")

    # A UART sensor at 115200: the baud box is not a pin field, and reading it
    # as one saved every UART sensor at 9600.
    pg.locator("#wizPrev").click()
    pg.wait_for_timeout(200)
    pg.select_option("#wiz-iface", "UART")
    pg.locator("#pin-wiz-rx").fill("1")
    pg.locator("#pin-wiz-tx").fill("")
    pg.fill("#wiz-baud", "115200")
    pg.locator("#wizNext").click()
    pg.wait_for_timeout(300)
    review = json.loads(pg.locator("#wiz-json").inner_text() or "{}")
    check(review.get("baud") == 115200 and review.get("uart_rx") == 1,
          f"the review carries baud 115200: baud={review.get('baud')!r} rx={review.get('uart_rx')!r}")
    pg.locator("#wizNext").click()             # save (and the page reloads)
    pg.wait_for_timeout(1500)
    saved = json.loads(pg.request.get(BASE + "/api/platform_config").text()).get("sensors", [])
    uart = [x for x in saved if x.get("interface") == "uart"]
    check(uart and uart[-1].get("baud") == 115200,
          f"and /save_platform is sent baud 115200: {[x.get('baud') for x in uart]}")

    # ── 5. Hardware, board profiles unavailable ──────────────────────────────
    print("\nHardware page, board profiles unavailable:")
    pg.request.get(BASE + "/__mock/profiles?fail=1")
    pg.goto(BASE + "/#settings_hardware", wait_until="networkidle")
    pg.reload(wait_until="networkidle")
    pg.wait_for_timeout(1500)
    box = pg.locator("#pin-pinWifiTrigger")
    check(box.input_value() == "6", f"no board context: the pin shows as a GPIO: {box.input_value()!r}")
    box.fill("20")
    before = len(mock_hw(pg)["posts"])
    pg.locator('#hw-host button[type="submit"]').click()
    pg.wait_for_timeout(800)
    posts = mock_hw(pg)["posts"]
    sent = posts[-1]["body"] if len(posts) > before else {}
    check(sent.get("pinWifiTrigger") == "20",
          f"the typed pin is what is saved, not the old one: {sent.get('pinWifiTrigger')!r}")
    pg.request.get(BASE + "/__mock/profiles?fail=0")
    n = pg.evaluate("Pins.load().then(function (d) { return d.profiles.length; })")
    check(n > 0, f"the failed load was not cached: the next one has {n} profiles")

    # The 503s above are the test's own doing, not the page's.
    errs = [(k, t) for k, t in console if k in ("error", "pageerror") and "503" not in t]
    check(not errs, f"no console errors ({len(errs)})")
    for k, t in errs:
        print(f"        {k}: {t}")
    b.close()

print()
if fails:
    print(f"FAIL: {len(fails)} check(s) failed")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("OK: pins are typed as printed, warned like the node, refused only when nothing can fix them")
