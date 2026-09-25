"""drive_node_portal.py — walk the node's own setup page in Chromium, both transports.

The page (node_portal/, shipped as src/nodecfg/NodePortalPage.h) is what
someone standing next to a node, on a phone joined to its AP, uses to set it
up. Nothing compiles it, and the node that serves it is the one thing that
cannot be configured when it is broken — so it is driven here, at phone
width, against tests/web/mock_node.py, which serves the committed header's
bytes: what passes here is what ships.

What it proves, in the order a person meets it:
  * secrets never reach the page, and an untouched secret is POSTed as ""
    ("keep"), not as a placeholder or a stale value;
  * the WiFi scan lists what is on the air once per SSID, renders a hostile
    SSID as text, and tapping one fills the field;
  * a pin field takes "D6" or "12", says which GPIO that is, refuses the flash
    bus with a "did you mean D6?" for the GPIO6 slip, and follows the board;
  * sensors add and remove, the metric budget meter follows, and a
    sleep-unsafe sensor cannot be added to a sleeping ESP-NOW node;
  * the node's own refusal ({"ok":false,"field","reason"}) lands next to the
    field it names, on the step that holds it;
  * a good save says "Saved, restarting…" and notices the node come back;
  * no console errors, and no horizontal scroll at 360 px on any step.

    python3 tests/web/mock_node.py 8790 &
    MOCK_PORT=8790 python3 tests/web/drive_node_portal.py

CHROMIUM_PATH points at an existing Chromium; SCREENSHOT_DIR, when set,
collects a light (WiFi) and a dark (ESP-NOW) screenshot at phone width.
"""
import json
import os
import re
import sys
import urllib.request
from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8790")
BASE = "http://127.0.0.1:" + PORT
SHOTS = os.environ.get("SCREENSHOT_DIR")
fails = []
console = []

# The page does these on purpose: a refused save is an HTTP 400, and a node
# that is restarting answers 503 (or nothing). Chromium logs both as
# "Failed to load resource"; anything else is a real error.
EXPECTED = re.compile(r"Failed to load resource: the server responded with a status of (400|503)")


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def mock(path):
    with urllib.request.urlopen(BASE + path, timeout=5) as r:
        return json.loads(r.read())


def step_name(pg):
    return pg.locator(".step-name").inner_text().lower()


def no_hscroll(pg, where):
    w = pg.evaluate("document.documentElement.scrollWidth")
    check(w <= 360, "no horizontal scroll at 360 px on %s (%d px)" % (where, w))


def nxt(pg):
    pg.click("[data-a=next]")
    pg.wait_for_timeout(150)


def fld(pg, path):
    return pg.locator('[data-f="%s"]' % path)


def hint(pg, path):
    return pg.locator('[data-ph="%s"]' % path).inner_text()


def ferr(pg, path):
    return pg.locator('[data-fe="%s"]' % path).inner_text()


def attach(pg):
    pg.on("console", lambda m: console.append((m.type, m.text)) if m.type == "error" else None)
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))


def wait_saved(pg):
    pg.wait_for_selector("#saved", timeout=5000)
    check("saved, restarting" in pg.locator("#ov").inner_text().lower(), "a good save says it is restarting")
    pg.wait_for_selector("#back", timeout=20000)
    check(pg.locator("#back").is_visible(), "and notices the node come back")


# ─────────────────────────────────────────────────────────────────────────────
def wifi(b):
    print("\n── WiFi node (ESP8266, NodeMCU), light, 360 px")
    ctx = b.new_context(viewport={"width": 360, "height": 740}, color_scheme="light", locale="en-US")
    pg = ctx.new_page()
    attach(pg)
    pg.goto(BASE + "/?transport=wifi", wait_until="networkidle")
    pg.wait_for_selector("#steps")

    html = pg.content()
    check("hunter22" not in html and "s3cret-token" not in html, "no stored secret is anywhere in the page")
    pw = pg.locator('[data-f="net.pass"]')
    check(pw.get_attribute("type") == "password" and pw.input_value() == "", "the passphrase field is empty and masked")
    check("leave empty to keep" in (pw.get_attribute("placeholder") or ""), "and says leaving it empty keeps the saved one")
    check(pg.locator(".steps button").count() == 6, "WiFi has six steps")
    check("network" in step_name(pg), "and starts at Network (%r)" % step_name(pg))
    check("home-5g" in pg.locator("#app").inner_text(), "the collector's next network is shown read-only")
    no_hscroll(pg, "Network")

    # ── scan
    pg.click("[data-a=scan]")
    pg.wait_for_selector("#nets", timeout=8000)
    rows = pg.locator("#nets button")
    names = [rows.nth(i).locator(".n").inner_text() for i in range(rows.count())]
    check(names.count("home") == 1, "an SSID heard from two APs is one row (%r)" % names)
    check("" not in names and len(names) == 4, "a hidden network is not listed (%r)" % names)
    check(names[0] == "home", "strongest first")
    check(any(n.startswith("<img") for n in names), "a hostile SSID is shown as text")
    check(pg.evaluate("window.__pwned === undefined"), "and is never parsed as markup")
    check("open" in rows.nth(names.index("cafe guest")).inner_text(), "an open network says so")
    rows.nth(names.index("home-5g")).click()
    check(fld(pg, "net.ssid").input_value() == "home-5g", "tapping a network fills the SSID")
    fld(pg, "net.ssid").fill("home")
    SHOTS and pg.screenshot(path=os.path.join(SHOTS, "node_portal_wifi_light_network.png"), full_page=True)

    # ── collector
    nxt(pg)
    check("collector" in step_name(pg), "step 2 is Collector")
    check(fld(pg, "net.host").input_value() == "192.168.1.50", "the collector address is filled in")
    check(fld(pg, "net.token").input_value() == "", "the token field is empty")
    fld(pg, "net.port").fill("8080")
    no_hscroll(pg, "Collector")

    # ── board & I2C: the D6/GPIO6 lesson
    nxt(pg)
    check("board" in step_name(pg), "step 3 is Board & I2C")
    check(fld(pg, "i2c.sda").input_value() == "D2", "a stored GPIO4 shows as its silkscreen, D2")
    check("GPIO4" in hint(pg, "i2c.sda"), "and says which GPIO that is")
    fld(pg, "i2c.sda").fill("6")
    h = hint(pg, "i2c.sda")
    check("flash" in h.lower() and "D6" in h and "GPIO12" in h,
          "typing 6 is refused as the flash bus, with 'did you mean D6 (GPIO12)?' (%r)" % h)
    check("bad" in fld(pg, "i2c.sda").get_attribute("class"), "and the field turns red")
    check(pg.locator("#diag .header .chip.err").count() == 6, "the diagram marks the flash pins")
    fld(pg, "i2c.sda").fill("d6")
    check(hint(pg, "i2c.sda").startswith("GPIO12"), "'d6' resolves to GPIO12 (%r)" % hint(pg, "i2c.sda"))
    fld(pg, "i2c.sda").fill("GPIO0")
    check("strap" in hint(pg, "i2c.sda") and "warn" in pg.locator('[data-ph="i2c.sda"]').get_attribute("class"),
          "a strap pin is a warning, not a refusal")
    fld(pg, "i2c.sda").fill("D1")
    check("also used" in hint(pg, "i2c.sda").lower(), "a pin used twice is flagged (%r)" % hint(pg, "i2c.sda"))
    fld(pg, "i2c.sda").fill("D2")
    before = pg.locator("#diag .header .chip").count()
    pg.click("[data-a=board][data-i='1']")
    check(pg.locator("#diag .header .chip").count() != before, "choosing the D1 mini redraws the header")
    check(pg.locator("#diag .header .chip.use").count() == 2, "and badges the two I2C pins on it")
    no_hscroll(pg, "Board & I2C")

    # ── sensors
    nxt(pg)
    check("sensors" in step_name(pg), "step 4 is Sensors")
    bud = lambda: pg.locator("#k-bud").inner_text()
    check(bud() == "4 / 8 metrics", "the budget meter counts the BME280 as 4 (%r)" % bud())
    pg.select_option("#addtype", "ds18b20")
    pg.click("[data-a=add]")
    check(pg.locator(".srow").count() == 2, "a DS18B20 row was added")
    check(bud() == "5 / 8 metrics", "one probe is one metric (%r)" % bud())
    fld(pg, "sensors[1].pin").fill("D5")
    check(hint(pg, "sensors[1].pin").startswith("GPIO14"), "its pin accepts D5")
    fld(pg, "sensors[1].count").fill("5")
    check(bud() == "9 / 8 metrics", "five probes blow the budget (%r)" % bud())
    check("at most 8" in ferr(pg, "sensors"), "and the page says so")
    fld(pg, "sensors[1].count").fill("1")
    pg.select_option("#addtype", "bh1750")
    pg.click("[data-a=add]")
    check(bud() == "6 / 8 metrics", "a BH1750 adds one (%r)" % bud())
    pg.select_option("#addtype", "pulse")
    pg.click("[data-a=add]")
    check(pg.locator('[data-f="sensors[3].mode"]').count() == 1, "a pulse counter can be added to a WiFi node")
    pg.locator('[data-row="2"] [data-a=rm]').click()
    check(pg.locator(".srow").count() == 3, "removing a row removes it")
    check(fld(pg, "sensors[2].mode").count() == 1, "and the rows below move up")
    pg.locator('[data-row="2"] [data-a=rm]').click()
    check(bud() == "5 / 8 metrics", "the meter follows removals (%r)" % bud())
    no_hscroll(pg, "Sensors")

    # ── a refusal from the node, on a field the page also flags
    fld(pg, "sensors[1].pin").fill("7")
    check("flash" in hint(pg, "sensors[1].pin").lower(), "GPIO7 is flagged on the row")
    nxt(pg)
    check("this node" in step_name(pg), "step 5 is This node")
    fld(pg, "name").fill("balcony-2")
    fld(pg, "interval_s").fill("120")
    no_hscroll(pg, "This node")
    nxt(pg)
    check("review" in step_name(pg), "step 6 is Review")
    check(pg.locator("#issues").count() == 1 and "flash" in pg.locator("#issues").inner_text().lower(),
          "Review lists the problem before saving")
    pg.click("#save")
    pg.wait_for_selector('[data-fe="sensors[1].pin"]:not(:empty)', timeout=5000)
    check("sensors" in step_name(pg), "the refusal takes the user back to Sensors (%r)" % step_name(pg))
    e = ferr(pg, "sensors[1].pin")
    check("refused" in e.lower() and "GPIO7" in e, "and shows the node's reason under that pin (%r)" % e)
    check(pg.locator("#ov").is_hidden(), "the save overlay is gone")

    # ── a second rule: the budget, refused by the node
    fld(pg, "sensors[1].pin").fill("D5")
    fld(pg, "sensors[1].count").fill("6")
    pg.click("[data-a=go][data-i='5']")
    pg.click("#save")
    pg.wait_for_function("document.querySelector('[data-fe=\"sensors\"]') && "
                         "document.querySelector('[data-fe=\"sensors\"]').textContent.indexOf('refused') >= 0",
                         timeout=5000)
    check("sensors" in step_name(pg), "an over-budget refusal lands on Sensors too")
    fld(pg, "sensors[1].count").fill("1")

    # ── save for real
    pg.click("[data-a=go][data-i='5']")
    check("looks good" in pg.locator("#app").inner_text().lower(), "Review is clean")
    SHOTS and pg.screenshot(path=os.path.join(SHOTS, "node_portal_wifi_light_review.png"), full_page=True)
    pg.click("#save")
    wait_saved(pg)
    body = mock("/__last")
    check(body["net"]["pass"] == "" and body["net"]["token"] == "" and body["net"]["basic_pass"] == "",
          "untouched secrets are sent as \"\" (keep)")
    check(not any(k.endswith("_set") for k in body["net"]), "no *_set flag is sent back")
    check("next" not in body["net"], "net.next is the collector's, not sent by the page")
    check(body["net"]["port"] == 8080 and body["name"] == "balcony-2" and body["interval_s"] == 120,
          "edited values are sent as numbers and strings")
    check(body["board"] == 1 and body["i2c"] == {"sda": 4, "scl": 5}, "board and I2C as GPIO numbers (%r)" % body["i2c"])
    check(body["sensors"][1] == {"type": "ds18b20", "pin": 14, "count": 1, "metric": "probe_temp"},
          "the DS18B20 row (%r)" % body["sensors"][1])
    st = mock("/__state")
    check(st["net"]["pass"] == "hunter22" and st["local"] is True, "the mock kept the passphrase and marked it local")
    ctx.close()


# ─────────────────────────────────────────────────────────────────────────────
def wifi_clear(b):
    print("\n── WiFi node: clearing a saved secret")
    ctx = b.new_context(viewport={"width": 360, "height": 740}, color_scheme="light", locale="en-US")
    pg = ctx.new_page()
    attach(pg)

    # An open network: the saved passphrase must go, or the ESP8266 never joins.
    pg.goto(BASE + "/?transport=wifi", wait_until="networkidle")
    pg.wait_for_selector("#steps")
    pg.click("[data-a=scan]")
    pg.wait_for_selector("#nets", timeout=8000)
    rows = pg.locator("#nets button")
    names = [rows.nth(i).locator(".n").inner_text() for i in range(rows.count())]
    rows.nth(names.index("cafe guest")).click()
    check("will be cleared" in (fld(pg, "net.pass").get_attribute("placeholder") or ""),
          "picking an open network says the saved passphrase will be cleared")
    pg.click("[data-a=go][data-i='5']")
    check("cleared" in pg.locator(".kv").inner_text(), "and Review says so")
    pg.click("#save")
    wait_saved(pg)
    body = mock("/__last")
    check(body["net"]["ssid"] == "cafe guest" and body["net"]["pass"] == "" and body["net"].get("pass_set") is False,
          "an open network is sent as pass \"\" + pass_set:false (%r)" % body["net"])
    check("token_set" not in body["net"] and "basic_pass_set" not in body["net"], "the other secrets are kept")
    st = mock("/__state")
    check(st["net"]["pass"] == "" and st["net"]["token"] == "s3cret-token", "the mock cleared only the passphrase")

    # The token, cleared on purpose; typing another SSID and back changes nothing else.
    pg.goto(BASE + "/?transport=wifi", wait_until="networkidle")
    pg.wait_for_selector("#steps")
    ph = lambda: fld(pg, "net.pass").get_attribute("placeholder") or ""
    fld(pg, "net.ssid").fill("elsewhere")
    check("will be cleared" in ph(), "another SSID with no passphrase typed clears the saved one")
    fld(pg, "net.ssid").fill("home")
    check("leave empty to keep" in ph(), "back to the saved SSID keeps it")
    nxt(pg)
    clr = pg.locator('[data-a=clr][data-i="net.token"]')
    check(clr.count() == 1, "a saved token offers Clear")
    check(pg.locator('[data-a=clr][data-i="net.basic_pass"]').count() == 0, "an unset one does not")
    clr.click()
    check("will be cleared" in (fld(pg, "net.token").get_attribute("placeholder") or ""), "Clear marks the token")
    pg.locator('[data-a=clr][data-i="net.token"]').click()
    check("leave empty to keep" in (fld(pg, "net.token").get_attribute("placeholder") or ""), "and pressing it again undoes that")
    pg.locator('[data-a=clr][data-i="net.token"]').click()
    pg.click("[data-a=go][data-i='5']")
    pg.click("#save")
    wait_saved(pg)
    body = mock("/__last")
    check(body["net"].get("token_set") is False and "pass_set" not in body["net"],
          "a cleared token is sent as token_set:false, the passphrase kept (%r)" % body["net"])
    st = mock("/__state")
    check(st["net"]["token"] == "" and st["net"]["pass"] == "hunter22", "the mock cleared only the token")
    ctx.close()


# ─────────────────────────────────────────────────────────────────────────────
def espnow(b):
    print("\n── ESP-NOW node (XIAO ESP32-C3), dark, 360 px, Bulgarian browser")
    ctx = b.new_context(viewport={"width": 360, "height": 740}, color_scheme="dark", locale="bg-BG")
    pg = ctx.new_page()
    attach(pg)
    pg.goto(BASE + "/?transport=espnow", wait_until="networkidle")
    pg.wait_for_selector("#steps")

    check(pg.locator("[data-a=next]").inner_text().strip() == "Напред", "a Bulgarian browser gets Bulgarian")
    check(pg.evaluate("document.documentElement.lang") == "bg", "and <html lang> says so")
    bg = pg.evaluate("getComputedStyle(document.body).backgroundColor")
    check(bg == "rgb(11, 14, 18)", "the dark theme follows the system (%s)" % bg)
    SHOTS and pg.screenshot(path=os.path.join(SHOTS, "node_portal_espnow_dark_bg.png"), full_page=True)
    pg.click("[data-a=lang][data-i=en]")
    check(pg.locator("[data-a=next]").inner_text().strip() == "Next", "the language toggle switches to English")

    check(pg.locator(".steps button").count() == 6, "ESP-NOW has six steps")
    check("collector" in step_name(pg), "and starts at Collector (%r)" % step_name(pg))
    check(fld(pg, "net.ssid").count() == 0, "with no WiFi network fields")
    lmk = fld(pg, "lmk")
    check(lmk.input_value() == "" and "0123456789abcdef" not in pg.content(), "the key is never rendered")
    check("leave empty to keep" in (lmk.get_attribute("placeholder") or ""), "and says empty keeps it")
    pg.wait_for_function("document.getElementById('linkbox') && document.getElementById('linkbox').textContent.indexOf('#3') >= 0",
                         timeout=5000)
    check("node #3" in pg.locator("#linkbox").inner_text(), "pairing status comes from /api/status")
    check("home-5g" in pg.locator("#linkbox").inner_text(), "the next network is shown read-only")
    lmk.fill("short")
    check("16" in ferr(pg, "lmk"), "a short key is flagged (%r)" % ferr(pg, "lmk"))
    lmk.fill("fedcba9876543210")
    check(ferr(pg, "lmk") == "", "a 16-character key is accepted")
    no_hscroll(pg, "Collector (ESP-NOW)")

    nxt(pg)
    check(fld(pg, "i2c.sda").input_value() == "D4", "XIAO: GPIO6 shows as D4")
    fld(pg, "i2c.scl").fill("12")
    check("flash" in hint(pg, "i2c.scl").lower(), "GPIO12 is the flash bus on the C3")
    fld(pg, "i2c.scl").fill("D5")
    check(hint(pg, "i2c.scl").startswith("GPIO7"), "D5 is GPIO7 on the XIAO")
    no_hscroll(pg, "Board (ESP-NOW)")

    nxt(pg)
    opt = pg.locator("#addtype option[value=pulse]")
    check(opt.get_attribute("disabled") is not None, "a sleeping node cannot add a pulse counter")
    check(pg.locator("#addtype option[value=sds011]").get_attribute("disabled") is not None, "nor an SDS011")
    check("need the node awake" in pg.locator("#app").inner_text(), "and the page says why")
    no_hscroll(pg, "Sensors (ESP-NOW)")

    nxt(pg)
    check("battery" in step_name(pg), "ESP-NOW has a Battery step")
    check(pg.locator("#cal-y").input_value() == "3.87", "the reported voltage is prefilled from /api/status")
    pg.fill("#cal-x", "4.0")
    check(pg.locator("#cal-go").inner_text().strip() == "Use trim 1.034", "4.0 V measured / 3.87 V reported → trim 1.034")
    pg.click("#cal-go")
    check(fld(pg, "batt.trim").input_value() == "1.034", "and applying it fills the trim field")
    no_hscroll(pg, "Battery")

    nxt(pg)
    sw = pg.locator('[data-f="sleep"]')
    check(sw.is_checked(), "deep sleep is on")
    pg.locator(".toggle-row").click()
    check(not sw.is_checked(), "and can be turned off")
    no_hscroll(pg, "This node (ESP-NOW)")
    pg.click("[data-a=go][data-i='2']")
    check(pg.locator("#addtype option[value=pulse]").get_attribute("disabled") is None, "awake, the pulse counter can be added")
    pg.select_option("#addtype", "pulse")
    pg.click("[data-a=add]")
    fld(pg, "sensors[1].pin").fill("D2")
    pg.select_option('[data-f="sensors[1].mode"]', "flow")
    check("flow_rate" in pg.locator('[data-row="1"]').inner_text(), "flow mode names its metrics")
    SHOTS and pg.screenshot(path=os.path.join(SHOTS, "node_portal_espnow_dark_sensors.png"), full_page=True)

    # Sleep back on with a pulse counter: the node refuses the row's type.
    pg.click("[data-a=go][data-i='4']")
    pg.locator(".toggle-row").click()
    pg.click("[data-a=go][data-i='5']")
    pg.click("#save")
    pg.wait_for_selector('[data-fe="sensors[1].type"]:not(:empty)', timeout=5000)
    check("sensors" in step_name(pg), "a sleep-unsafe refusal lands on Sensors")
    check("bad" in pg.locator('[data-row="1"]').get_attribute("class"), "and marks the row")
    pg.click("[data-a=go][data-i='4']")
    pg.locator(".toggle-row").click()

    pg.click("[data-a=go][data-i='5']")
    check("looks good" in pg.locator("#app").inner_text().lower(), "Review is clean")
    pg.click("#save")
    wait_saved(pg)
    body = mock("/__last")
    check(body.get("lmk") == "fedcba9876543210", "the typed key is sent")
    check(body.get("sleep") is False, "sleep off is sent")
    check(body["batt"] == {"pin": 2, "divider": 2, "trim": 1.034}, "battery settings (%r)" % body["batt"])
    check(body["sensors"][1]["pin"] == 4 and body["sensors"][1]["mode"] == "flow", "the pulse row (%r)" % body["sensors"][1])
    check("net" not in body and "link" not in body, "no WiFi or link fields from an ESP-NOW page")
    ctx.close()


with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    try:
        wifi(b)
        wifi_clear(b)
        espnow(b)
    except Exception as e:  # a timeout is a failure with a story, not a crash
        fails.append("driver stopped: %s" % str(e).splitlines()[0])
        print("  FAIL driver stopped: %s" % e)
    b.close()

real = [c for c in console if not EXPECTED.search(c[1])]
check(not real, "no console errors (%r)" % real[:5])
print("\n%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
