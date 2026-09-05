"""drive_sensors_page.py — edit a REMOTE sensor in a real browser.

WHY THIS PAGE, AND WHY THIS SENSOR
----------------------------------
A satellite node pairs with the collector on one string: the node id. The
collector never contacts the node — the node POSTs to /api/ingest — so there
is no address to get wrong and nothing to probe. Get that string wrong and the
sensor sits in the list forever with no metrics, which looks exactly like a
node that is not reporting.

The editor had no field for it. The type was missing from the page's own list,
so a remote sensor rendered as the raw string "remote" with no interface line,
and its one setting could only be reached through the Advanced JSON overlay.
This drives what a person does — open the sensor, change the node id, save —
and asserts on the document that goes back to the device.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_sensors_page.py
"""
import json
import os
import sys

from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT
URL  = BASE + "/#settings_hardware"

fails = []
console = []
notfound = []

# mock_device.py deliberately does not serve every route the SPA touches on
# boot — /export_settings is a plain download link on the real device, and its
# docstring says drivers are to ignore the 404. core.js fetches it on load and
# calls .json() on the result, so the miss also surfaces as a parse error.
# Ignored here BY URL, so a 404 on anything else still fails the run.
UNSERVED = ("/export_settings",)


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
    pg.on("response", lambda r: notfound.append(r.url) if r.status == 404 else None)

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1500)

    print("The sensor list:")
    lst = pg.locator("#cl-sensors-list")
    check(lst.count() == 1, "the hardware page partial was fetched and injected")
    body = lst.inner_text()
    check("balcony" in body, "the remote sensor is listed")

    # The type list is what names a sensor everywhere in this UI. Without an
    # entry the row printed the plugin's own type string.
    check("Remote node" in body,
          "it is named, not printed as the raw type: "
          + repr([l for l in body.splitlines() if "balcony" in l or "Remote" in l][:2]))
    check("remote" not in body.replace("Remote node (HTTP ingest)", ""),
          "the raw type string is not shown anywhere in the row")

    # A wired sensor shows its pins there; a remote one has none, so the row
    # has to show the only thing that identifies it.
    check("Node:outside" in body.replace(" ", ""),
          "the row shows the node id it is paired on")
    check("SDA:4" in body.replace(" ", ""),
          "and a wired sensor still shows its pins")

    print("\nThe editor:")
    # Open the remote sensor's editor the way a person does: the pencil on
    # its row. (Clicking the row itself hits the enable checkbox, which is a
    # different button with a much worse failure mode.)
    row = pg.locator('.sensor-list-row', has_text="balcony").first
    row.locator('button[data-click="clEditSensor"]').first.click()
    pg.wait_for_timeout(800)

    node_field = pg.locator('#sensorEditForm input[name="node"]')
    check(node_field.count() == 1, "the editor has a Remote node id field")
    if node_field.count() == 1:
        check(node_field.input_value() == "outside",
              "pre-filled with what the device holds")
        # The pin fields belong to a wired sensor and must not appear here.
        check(pg.locator('#sensorEditForm input[name="sda"]').count() == 0,
              "and no I2C pin fields, which this sensor does not have")

        node_field.fill("balcony_2")

    # Save, and read back the document the page POSTed.
    posted = {}

    def capture(route):
        try:
            posted.update(json.loads(route.request.post_data or "{}"))
        except ValueError:
            pass
        route.continue_()

    pg.route("**/save_platform*", capture)

    # TWO saves, because the page has two. The editor's Save only writes the
    # sensor back into the page's copy of the config and closes the panel;
    # "Save Sensor List" is what POSTs the document to the device. Anyone who
    # presses only the first one has changed nothing on the node — which is
    # its own trap, and one this test would rather pin down than paper over.
    pg.locator('button[data-role="save"]').first.click()
    pg.wait_for_timeout(400)
    check(pg.locator('#cl-sensors-list').inner_text().replace(" ", "")
          .find("Node:balcony_2") >= 0,
          "the edited node id is back in the list before anything is sent")

    pg.locator('button[data-click="clSave"]').first.click()
    pg.wait_for_timeout(1500)

    print("\nWhat reached the device:")
    sensors = posted.get("sensors") or []
    remote = next((s for s in sensors if s.get("type") == "remote"), None)
    check(remote is not None, "the saved document still contains the sensor")
    if remote:
        check(remote.get("node") == "balcony_2",
              f"the new node id was saved (node={remote.get('node')!r})")
        # RemoteNodeSensor::init reads `node`, falling back to the sensor id.
        # An empty string would be neither — a node id nothing can ever match.
        check(remote.get("node") != "", "and never as an empty string")
        check("pin" not in remote,
              "no pin was invented for a sensor that has no pins")
        check("sda" not in remote and "scl" not in remote,
              "and no I2C pins either")

    # A page that throws while saving still looks like it saved.
    unexpected_404 = [u for u in notfound
                      if not any(x in u for x in UNSERVED)]
    check(not unexpected_404,
          f"no unexpected 404s ({len(unexpected_404)}): {unexpected_404[:3]}")

    mock_noise = not unexpected_404
    errs = []
    for kind, text in console:
        if kind not in ("error", "pageerror"):
            continue
        if mock_noise and ("404" in text or "is not valid JSON" in text):
            continue        # the unserved-route miss above, and its fallout
        errs.append((kind, text))
    check(not errs, f"no console errors ({len(errs)})")
    for kind, text in errs:
        print(f"        {kind}: {text}")

    b.close()

print()
if fails:
    print(f"FAIL: {len(fails)} check(s) failed")
    for f in fails:
        print("  - " + f)
    sys.exit(1)
print("OK: a remote sensor can be named, edited and saved")
