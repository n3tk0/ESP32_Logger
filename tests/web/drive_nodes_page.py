"""drive_nodes_page.py — click the unified Nodes settings page in Chromium.

Same reasoning as drive_kindle_page.py: the page is not compiled, so nothing
else here would notice a page that fetches a field the firmware does not
send, references an icon that is not in the sheet, or wires a button to a
handler core.js never registered.

This replaces drive_espnow_page.py. Redesign 1a merged the former Battery
nodes (ESP-NOW) and Remote WiFi nodes pages into one list, transport shown as
a column — #settings_espnow / #settings_remote are gone with no redirect, so
there is nothing left at the old URL for a driver to open.

The assertions that matter most are still the NULLS, carried over unchanged
from the page this replaces:
  rssi   unavailable on Arduino core 2.x — IDF 4.4 gives the receive
         callback no signal information at all.
  days   null whenever the battery model refuses to answer: too little
         history, a flat trace, or a slope inside the noise. Redesign 1a
         moved this from an always-visible "left: N d" badge to the battery
         badge's title (a table with six columns per node has no room for a
         seventh just for this) — still never printed as "0 d".
  skew_s null until the node has reported with both clocks set. Zero is a
         real and good answer here, so it must not be conflated with "never
         measured".
WiFi remote nodes carry none of this — they have no MAC/battery/RSSI/clock at
all, by construction — so the driver checks they render as dashes rather than
invented zeros too.

Then each node's own settings (docs/NODE_CONFIG.md §7), for both transports:
read only when a row opens, edited through the one savebar, drafts that
survive the row closing, a save that sends only what changed, and a refusal
from the collector's validator shown beside the field it names. And the
network handover (§4): a new WiFi network saved on the Network page is handed
to the nodes first, with a banner that follows them, on both pages.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_nodes_page.py
"""
import os
import sys
from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
URL = "http://127.0.0.1:" + PORT + "/#settings_nodes"
fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


def row(pg, key):
    """The .node-row for a given merge key, e.g. row(pg, 'en:1') for the
    ESP-NOW node with node_id 1, or row(pg, 'rn:greenhouse') for a WiFi node."""
    return pg.locator('.node-row[data-args=\'["%s"]\']' % key)


def cols(pg, key):
    """The five .node-row-col cells in column order: transport, battery,
    seen, rssi, clock."""
    return row(pg, key).locator(".node-row-col")


with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page()
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))
    # Every URL the page asks for: what proves a node's settings are read only
    # when its row opens, and that a move to a new network does not save.
    requests = []
    pg.on("request", lambda r: requests.append(r.url))

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1200)

    sec = pg.locator("#page-settings_nodes")
    check(sec.count() == 1, "the page partial was fetched and injected")

    # ── The merged list ──────────────────────────────────────────────────────
    body = pg.locator("#nd-rows").inner_text().lower()
    check("outdoor" in body, "an ESP-NOW node is listed")
    check("espnow-03" in body, "an ESP-NOW node that has never reported is listed")
    check("greenhouse" in body, "a WiFi node is listed")
    check("shed-wifi" in body, "a WiFi node that has never reported is listed")
    check(pg.locator(".node-row").count() == 5,
          "all five nodes are rows (%d)" % pg.locator(".node-row").count())

    # KPIs prove the merge actually merges, not just two lists side by side.
    check(pg.locator("#nd-kpi-total").inner_text().strip() == "5",
          "the total counts both transports")
    check("3 ESP-NOW" in pg.locator("#nd-kpi-total-d").inner_text() and
          "2 WiFi" in pg.locator("#nd-kpi-total-d").inner_text(),
          "and says the split (%r)" % pg.locator("#nd-kpi-total-d").inner_text())

    # ── ESP-NOW: the two nulls ──────────────────────────────────────────────
    # rssi is unavailable on Arduino core 2.x for every node in the mock — a
    # dash, never "0 dBm".
    outdoor_cols = cols(pg, "en:1").all_inner_texts()
    check(outdoor_cols[1].startswith("62%"), "a battery percentage is shown plainly (%r)" % outdoor_cols[1])
    check(outdoor_cols[3] == "—", "an unavailable RSSI shows a dash, not 0 dBm (%r)" % outdoor_cols[3])
    check("0 dBm" not in body, "a missing RSSI is never printed as 0 dBm anywhere")

    # days: moved to the battery badge's title in this redesign — still a
    # dash-equivalent (absent attribute) when the model can't estimate, never
    # a fabricated "0 d". Outdoor's battery model DOES have an answer (237 d).
    # The battery column (index 1) has its own badge — index it directly
    # rather than "the first badge in the row", which would be the transport
    # badge (index 0) instead.
    outdoor_batt_title = cols(pg, "en:1").nth(1).locator(".badge").get_attribute("title") or ""
    check("237" in outdoor_batt_title,
          "a remaining-life estimate is on the battery badge (%r)" % outdoor_batt_title)
    never_reported_batt = cols(pg, "en:3").nth(1).locator(".badge")
    check(never_reported_batt.count() == 0,
          "a node with no battery reading shows no battery badge at all")

    # Clock drift. Same three cases and sign convention as before the merge:
    #   skew_s   3    the node is 3 s behind  → "-3s"
    #   skew_s -184   the node is ~3 min AHEAD → "+3m" (past the warn threshold)
    #   skew_s  null  never measured, which is NOT zero
    # .badge carries text-transform:uppercase, so inner_text() (what is
    # PAINTED) comes back "-3S"/"+3M" even though the DOM/i18n string is
    # lowercase — lower() it before comparing, same convention the page this
    # replaces used for the same reason.
    en1_clock = cols(pg, "en:1").nth(4).inner_text().lower()
    en2_clock = cols(pg, "en:2").nth(4).inner_text().lower()
    check(en1_clock == "-3s", "a node behind the collector reads as negative (%r)" % en1_clock)
    check(en2_clock == "+3m", "a node ahead of the collector reads as positive (%r)" % en2_clock)
    check(cols(pg, "en:3").nth(4).inner_text() == "—",
          "an unmeasured drift shows a dash, never 0s")
    check("0s" not in [c.strip() for c in cols(pg, "en:1").all_inner_texts()] +
          [c.strip() for c in cols(pg, "en:2").all_inner_texts()],
          "an unmeasured drift is never printed as 0s")
    warn_badge = row(pg, "en:2").locator(".node-row-col .badge.warn", has_text="+3m")
    check(warn_badge.count() == 1, "a drift past the warning threshold is coloured as one")

    online = pg.locator("#nd-rows .badge.ok", has_text="online").count()
    offline = pg.locator("#nd-rows .badge.err", has_text="offline").count()
    check(online >= 1 and offline >= 1,
          "online/offline pills render across both transports (%d ok, %d err)" % (online, offline))

    svgs = pg.locator("#nd-rows svg").count()
    check(svgs > 0, "icons were substituted (%d svg)" % svgs)

    # ── WiFi remote: no MAC/battery/RSSI/clock, by construction ─────────────
    gh_cols = cols(pg, "rn:greenhouse").all_inner_texts()
    check(gh_cols[1] == "—", "a WiFi node shows no battery column (%r)" % gh_cols[1])
    check(gh_cols[3] == "—" and gh_cols[4] == "—",
          "and no RSSI or clock either — those don't exist for this transport")
    check(cols(pg, "rn:shed-wifi").nth(2).inner_text() == "never",
          "a WiFi node that has never reported says so")

    # ── Filters ───────────────────────────────────────────────────────────
    pg.click('#nd-filter button[data-args=\'["espnow"]\']')
    pg.wait_for_timeout(200)
    check(pg.locator(".node-row").count() == 3, "the ESP-NOW filter shows only ESP-NOW nodes")
    check(pg.locator('.node-row[data-args*="rn:"]').count() == 0,
          "and hides every WiFi row")
    pg.click('#nd-filter button[data-args=\'["wifi"]\']')
    pg.wait_for_timeout(200)
    check(pg.locator(".node-row").count() == 2, "the WiFi filter shows only WiFi nodes")
    pg.click('#nd-filter button[data-args=\'["problem"]\']')
    pg.wait_for_timeout(200)
    problem_count = pg.locator(".node-row").count()
    check(problem_count == offline,
          "the Problem filter matches the offline count (%d rows, %d offline)"
          % (problem_count, offline))
    pg.click('#nd-filter button[data-args=\'["all"]\']')
    pg.wait_for_timeout(200)
    check(pg.locator(".node-row").count() == 5, "and All brings every row back")

    # ── Search ────────────────────────────────────────────────────────────
    pg.fill("#nd-search", "green")
    pg.wait_for_timeout(200)
    check(pg.locator(".node-row").count() == 1 and "greenhouse" in pg.locator("#nd-rows").inner_text().lower(),
          "search narrows to a matching node")
    pg.fill("#nd-search", "")
    pg.wait_for_timeout(200)

    # ── Pairing ───────────────────────────────────────────────────────────
    check(pg.locator("#nd-pair-state").inner_text().strip().lower() == "closed",
          "pairing starts closed")
    pg.click('[data-click="nodesPair"]')
    pg.wait_for_timeout(900)
    check(pg.locator("#nd-pair-state").inner_text().strip().lower() == "open",
          "clicking opens the window")
    msg = pg.locator("#nd-msg").inner_text()
    check("Pairing window open" in msg, "the page confirms it: %r" % msg.strip()[:48])

    # ── Each node's settings (docs/NODE_CONFIG.md §7) ────────────────────────
    # The row badge comes from the `cfg` summary on the two status payloads,
    # so the list can say which nodes are behind without reading every
    # node's config: nothing may have asked for one yet.
    check(not [u for u in requests if "/api/nodes/config" in u],
          "no node's settings are read before its row is opened (%d reads)"
          % len([u for u in requests if "/api/nodes/config" in u]))
    b1 = row(pg, "en:1").locator(".node-row-name").inner_text().lower()
    check("applied" in b1, "a node running its desired settings says applied (%r)" % b1)
    bg = row(pg, "rn:greenhouse").locator(".node-row-name").inner_text().lower()
    check("pending rev 2 → 3" in bg, "a node behind says which rev it runs and which it is sent (%r)" % bg)
    b2 = row(pg, "en:2").locator(".node-row-name").inner_text().lower()
    check("rejected: gpio12 is the flash bus" in b2,
          "a node that refused its settings says why, in the list (%r)" % b2)
    check(row(pg, "rn:shed-wifi").locator(".node-row-name .badge").count() == 1,
          "and a node with no settings on file gets no config badge at all")

    def panel(key):
        return pg.locator('.nd-panel[data-nd-key="%s"]' % key)

    def fld(key, path):
        return panel(key).locator('[data-nd-path="%s"]' % path)

    def hint(key, path):
        return panel(key).locator('[data-nd-hint="%s"]' % path).inner_text()

    # ── ESP-NOW node ─────────────────────────────────────────────────────
    row(pg, "en:1").click()
    pg.wait_for_timeout(600)
    reads = [u for u in requests if "/api/nodes/config" in u]
    check(len(reads) == 1 and "key=e%3A1" in reads[0],
          "opening a row reads that node's settings, once (%r)" % reads)
    check(panel("e:1").is_visible(), "the open row shows the node's settings panel")
    heads = panel("e:1").locator(".nd-sec-h").all_inner_texts()
    heads = [h.strip().split("\n")[0] for h in heads]
    check(heads == ["Identity", "Timing", "Board & I²C", "Link & battery", "Sensors"],
          "an ESP-NOW node has identity, timing, board, link/battery and sensors — no network (%r)" % heads)
    check(fld("e:1", "i2c.sda").input_value() == "D4" and hint("e:1", "i2c.sda") == "GPIO6",
          "a pin shows the board's label for it, and the GPIO it is (%r, %r)"
          % (fld("e:1", "i2c.sda").input_value(), hint("e:1", "i2c.sda")))
    check("boot strap" in hint("e:1", "batt.pin"),
          "a strapping pin is warned about (%r)" % hint("e:1", "batt.pin"))
    check(fld("e:1", "sleep").is_checked(), "the sleep switch reflects the node")
    add = panel("e:1").locator("select[id$='-addtype']")
    dis = add.locator("option[disabled]").evaluate_all("els => els.map(e => e.value)")
    check("sds011" in dis and "pulse" in dis,
          "with sleep on, the sensors that need the node awake cannot be added (%r)" % dis)
    check("bmx280" in dis and "bme688" in dis,
          "nor a second BME280, nor a BME688 beside it")
    check("deep-sleeps" in panel("e:1").inner_text(), "and the panel says why")
    check(not pg.is_visible("#nd-savebar"), "nothing is dirty yet")

    fld("e:1", "name").fill("garden")
    pg.wait_for_timeout(200)
    check(pg.is_visible("#nd-savebar"), "editing a field brings the savebar up")
    check("1 node" in pg.locator("#nd-dirty").inner_text(),
          "and it counts one changed node (%r)" % pg.locator("#nd-dirty").inner_text())

    # A pin can be typed as the board's label or as a number.
    fld("e:1", "batt.pin").fill("D6")
    pg.wait_for_timeout(150)
    check(hint("e:1", "batt.pin").startswith("GPIO21"), "D6 on a XIAO resolves to GPIO21 (%r)" % hint("e:1", "batt.pin"))
    fld("e:1", "batt.pin").fill("3")
    pg.wait_for_timeout(150)
    check(hint("e:1", "batt.pin") == "GPIO3", "and a bare number is a GPIO (%r)" % hint("e:1", "batt.pin"))
    fld("e:1", "batt.pin").fill("Q9")
    pg.wait_for_timeout(150)
    check("not a pin" in hint("e:1", "batt.pin").lower(), "something that is neither is said to be (%r)" % hint("e:1", "batt.pin"))
    fld("e:1", "batt.pin").fill("D0")      # back to what the device holds
    pg.wait_for_timeout(150)

    # Add a sensor. The budget follows it.
    add.select_option("ds18b20")
    panel("e:1").locator('[data-click="nodesCfgAddSensor"]').click()
    pg.wait_for_timeout(300)
    check(panel("e:1").locator(".nd-sensor").count() == 2, "adding a sensor adds a row for it")
    check(panel("e:1").locator("[data-nd-budget]").inner_text().lower().startswith("5 / 8"),
          "and the metric budget counts it (%r)" % panel("e:1").locator("[data-nd-budget]").inner_text())
    check("pick a pin" in hint("e:1", "sensors[1].pin").lower(), "a new sensor asks for its pin")
    fld("e:1", "sensors[1].count").fill("3")
    pg.wait_for_timeout(150)
    check(panel("e:1").locator("[data-nd-budget]").inner_text().lower().startswith("7 / 8"),
          "three probes cost three metrics (%r)" % panel("e:1").locator("[data-nd-budget]").inner_text())
    # A pin the chip cannot use: flagged here, and sent anyway, so the
    # collector's own validator is what refuses it below.
    fld("e:1", "sensors[1].pin").fill("13")
    pg.wait_for_timeout(150)
    check("flash" in hint("e:1", "sensors[1].pin").lower(),
          "a flash-bus pin is marked as unusable as it is typed (%r)" % hint("e:1", "sensors[1].pin"))

    # Turning sleep off makes the awake-only sensors available.
    panel("e:1").locator("label[for=ndc-e_1-sleep]").click()
    pg.wait_for_timeout(300)
    dis2 = panel("e:1").locator("select[id$='-addtype'] option[disabled]").evaluate_all("els => els.map(e => e.value)")
    check("pulse" not in dis2 and "sds011" not in dis2,
          "with sleep off a pulse counter or particle sensor can be added (%r)" % dis2)
    panel("e:1").locator("label[for=ndc-e_1-sleep]").click()
    pg.wait_for_timeout(300)

    # ── WiFi node, opened while the ESP-NOW draft is outstanding ────────────
    row(pg, "rn:greenhouse").click()
    pg.wait_for_timeout(600)
    check(panel("w:greenhouse").is_visible(), "opening another row switches the panel to it")
    heads = [h.strip().split("\n")[0] for h in panel("w:greenhouse").locator(".nd-sec-h").all_inner_texts()]
    check("Network" in heads and "Link & battery" not in heads,
          "a WiFi node has a network section and no radio link (%r)" % heads)
    check(fld("w:greenhouse", "net.ssid").input_value() == "MonkeyNet", "its network is shown")
    pw = fld("w:greenhouse", "net.pass")
    check(pw.input_value() == "" and "stored" in (pw.get_attribute("placeholder") or ""),
          "a stored password is never shown, only said to be there (%r)" % pw.get_attribute("placeholder"))
    check(fld("w:greenhouse", "i2c.sda").input_value() == "D2", "NodeMCU labels are used for an ESP8266 (%r)"
          % fld("w:greenhouse", "i2c.sda").input_value())
    check(panel("w:greenhouse").locator("[data-nd-budget]").inner_text().lower().startswith("7 / 8"),
          "BME688 (5) + two probes = 7 of 8")
    fld("w:greenhouse", "interval_s").fill("120")
    fld("w:greenhouse", "net.pass").fill("newpass99")
    pg.wait_for_timeout(200)
    check("2 nodes" in pg.locator("#nd-dirty").inner_text(),
          "two nodes with unsaved settings are two (%r)" % pg.locator("#nd-dirty").inner_text())

    # A WiFi node that has never reported: nothing to edit, and it says why.
    row(pg, "rn:shed-wifi").click()
    pg.wait_for_timeout(600)
    wait = pg.locator(".node-row-detail").inner_text()
    check("Waiting for the node to report its settings" in wait,
          "a WiFi node that never reported its settings says it is waiting (%r)" % wait[:60])
    check(pg.locator(".node-row-detail input").count() == 0, "and offers no fields")

    # AN UNSAVED EDIT SURVIVES A RE-RENDER. #nd-rows is replaced wholesale on
    # every collapse, filter and search, and only the open row has inputs at
    # all — so a draft read back out of the DOM was lost the moment the row
    # closed, while the bar stayed up and Save then wrote nothing. Collapse,
    # filter and search between the edit and the Save so the row is rebuilt
    # several times over.
    row(pg, "rn:shed-wifi").click()          # collapse
    pg.wait_for_timeout(250)
    check(pg.is_visible("#nd-savebar"), "the bar survives collapsing the rows")
    pg.click('#nd-filter button[data-args=\'["espnow"]\']')
    pg.wait_for_timeout(200)
    pg.fill("#nd-search", "out")
    pg.wait_for_timeout(200)
    pg.fill("#nd-search", "")
    pg.click('#nd-filter button[data-args=\'["all"]\']')
    pg.wait_for_timeout(200)
    row(pg, "en:1").click()          # re-open
    pg.wait_for_timeout(300)
    check(fld("e:1", "name").input_value() == "garden",
          "and the edit itself survives, rather than reverting to the device's value (%r)"
          % fld("e:1", "name").input_value())
    check(panel("e:1").locator(".nd-sensor").count() == 2 and fld("e:1", "sensors[1].pin").input_value() == "13",
          "so does the added sensor, with what was typed into it")
    check("2 nodes" in pg.locator("#nd-dirty").inner_text(),
          "with the bar still counting exactly two nodes (%r)" % pg.locator("#nd-dirty").inner_text())

    # ── Save: one node refused, the other taken ─────────────────────────────
    pg.click('[data-click="nodesSave"]')
    pg.wait_for_timeout(1000)
    ferr = panel("e:1").locator('.nd-ferr[data-nd-err="sensors[1].pin"]')
    check(ferr.count() == 1 and "GPIO13 is the flash bus" in ferr.inner_text(),
          "the collector's refusal is shown beside the field it names (%r)"
          % (ferr.inner_text() if ferr.count() else None))
    check("garden" in pg.locator("#nd-msg").inner_text() or "GPIO13" in pg.locator("#nd-msg").inner_text(),
          "and said at the top of the page (%r)" % pg.locator("#nd-msg").inner_text()[:60])
    check(fld("e:1", "name").input_value() == "garden", "the refused node keeps its draft")
    check(pg.is_visible("#nd-savebar") and "1 node" in pg.locator("#nd-dirty").inner_text(),
          "and the bar stays up for it alone (%r)" % pg.locator("#nd-dirty").inner_text())
    mock = pg.evaluate("fetch('/__mock/nodes').then(function(r){return r.json()})")
    gh = [p for p in mock["posts"] if p["key"] == "w:greenhouse"]
    check(len(gh) == 1 and gh[0]["config"] == {"interval_s": 120, "net": {"pass": "newpass99"}},
          "the node that was taken got ONLY what changed (%r)" % (gh[0]["config"] if gh else None))
    bg = row(pg, "rn:greenhouse").locator(".node-row-name").inner_text().lower()
    check("pending rev 2 → 4" in bg, "and its badge moved on to the new rev (%r)" % bg)

    fld("e:1", "sensors[1].pin").fill("D3")
    pg.wait_for_timeout(200)
    check(panel("e:1").locator('.nd-ferr[data-nd-err="sensors[1].pin"]').count() == 0,
          "editing the field clears the refusal")
    pg.click('[data-click="nodesSave"]')
    pg.wait_for_timeout(1000)
    check("garden" in pg.locator("#nd-rows").inner_text().lower(),
          "a rename round-trips and re-renders")
    check(not pg.is_visible("#nd-savebar"), "and the bar goes away once saved")
    mock = pg.evaluate("fetch('/__mock/nodes').then(function(r){return r.json()})")
    e1 = [p for p in mock["posts"] if p["key"] == "e:1"]
    sent = e1[-1]["config"] if e1 else {}
    check(sorted(sent.keys()) == ["name", "sensors"],
          "the ESP-NOW node got its name and its sensor list, nothing else (%r)" % sorted(sent.keys()))
    check(sent.get("sensors", [{}, {}])[1:] == [{"type": "ds18b20", "pin": 5, "count": 3, "metric": "probe_temp"}],
          "with D3 sent as GPIO5 (%r)" % sent.get("sensors"))
    b1 = row(pg, "en:1").locator(".node-row-name").inner_text().lower()
    check("pending rev 4 → 5" in b1, "and it is now pending the rev it was sent (%r)" % b1)

    # It reached the DEVICE, not just the list: a Save that re-rendered from
    # its own drafts would look identical here without having sent anything.
    saved = pg.evaluate(
        "fetch('/api/espnow/status').then(function(r){return r.json()})")
    names = [n["id"] for n in saved.get("nodes", [])]
    check("garden" in names, "and the device holds the new name (%r)" % names)

    # The node's own refusal (rather than this page's) sits by its field too.
    row(pg, "en:2").click()
    pg.wait_for_timeout(600)
    rej = panel("e:2").locator('.nd-ferr[data-nd-err="sensors[1].pin"]')
    check(rej.count() == 1 and "flash bus" in rej.inner_text(),
          "a node's rejection is shown next to the field it names")
    row(pg, "en:2").click()
    pg.wait_for_timeout(250)

    # ── Forget, with the confirm dialog accepted ─────────────────────────────
    pg.on("dialog", lambda d: d.accept())
    row(pg, "en:2").click()
    pg.wait_for_timeout(350)
    pg.click('[data-click="nodesForget"]')
    pg.wait_for_timeout(900)
    check("balcony" not in pg.locator("#nd-rows").inner_text().lower(),
          "forgetting removes the node")

    # ── Radio diagnostics (ESP-NOW only) ─────────────────────────────────────
    pg.click("#nd-card-diag summary")
    pg.wait_for_timeout(300)
    stats = pg.locator("#nd-stats").inner_text()
    check("1528" in stats, "the accepted-frame counter is shown")
    check("bad pairing signature" in stats, "the failure counters are labelled")

    # ── Switching language re-renders what was built as strings ─────────────
    # The rows are assembled with I18n.t() baked in at render time, so
    # I18n.apply()'s data-i18n walk cannot reach them: without a re-render on
    # i18n:change the page sat half-translated (Bulgarian chrome, English
    # rows) until it was navigated away from and back.
    before = pg.locator("#nd-rows").inner_text().lower()
    check("online" in before, "the rows start in English")
    pg.click("#langToggleBtn")
    pg.wait_for_timeout(500)
    after = pg.locator("#nd-rows").inner_text().lower()
    check("онлайн" in after or "офлайн" in after,
          "switching language re-renders the generated rows (%r)" % after[:60])
    check(pg.locator("#nd-kpi-total-d").inner_text() != "",
          "and the KPI line survives the switch")
    pg.click("#langToggleBtn")       # back to English for any later assertions
    pg.wait_for_timeout(500)
    check("online" in pg.locator("#nd-rows").inner_text().lower(),
          "and switching back returns them to English")

    # ── Global chrome that is built once and cached ─────────────────────────
    # The command palette and the quick-settings drawer each generate their
    # markup on first open and then hold onto the node for the life of the
    # page, and the palette's item registry is a module-level literal
    # evaluated at <script defer> time.  Both used to keep whatever language
    # they were first opened in however often the page was toggled after.
    # These live on index.html, not on the Nodes page — they are checked here
    # because this is the driver that already drives the language toggle.
    def palette_text():
        pg.keyboard.press("Control+k")
        pg.wait_for_timeout(300)
        txt = pg.locator("#cmdPalette").inner_text()
        ph = pg.locator("#cmdPalette .cmd-input").get_attribute("placeholder")
        pg.keyboard.press("Escape")
        pg.wait_for_timeout(200)
        return txt + " | " + (ph or "")

    en_pal = palette_text()
    check("PAGE" in en_pal.upper() and "Search pages" in en_pal,
          "the command palette opens in English")
    pg.click("#langToggleBtn")
    pg.wait_for_timeout(400)
    bg_pal = palette_text()
    check("СТРАНИЦА" in bg_pal.upper(),
          "its group headings follow the switch (%r)" % bg_pal[:40])
    check("Търси страници" in bg_pal, "and so does its placeholder")

    # Quick settings, opened for the first time here, while still Bulgarian.
    pg.keyboard.press(",")
    pg.wait_for_timeout(300)
    check("Бързи настройки" in pg.locator("#quickSettingsPanel").inner_text(),
          "the quick-settings drawer opens in Bulgarian")
    pg.keyboard.press("Escape")
    pg.wait_for_timeout(200)
    pg.click("#langToggleBtn")       # back to English
    pg.wait_for_timeout(400)
    pg.keyboard.press(",")
    pg.wait_for_timeout(300)
    qs = pg.locator("#quickSettingsPanel").inner_text()
    check("Quick settings" in qs,
          "and a drawer already built follows the switch back (%r)" % qs[:40])
    pg.keyboard.press("Escape")
    pg.wait_for_timeout(200)

    # ── Moving the nodes to a new network (docs/NODE_CONFIG.md §4) ───────────
    # The Network page reads what the collector is joined to from
    # /export_settings, which the mock leaves unserved (a plain download on
    # the device). Answered here, for this page only.
    pg.route("**/export_settings", lambda r: r.fulfill(
        status=200, content_type="application/json",
        body='{"network":{"wifiMode":1,"clientSSID":"MonkeyNet","clientPassword":"old",'
             '"useStaticIP":false,"apSSID":"WaterLogger"}}'))
    pg.evaluate("location.hash = 'settings_network'")
    pg.wait_for_timeout(1200)
    check(pg.input_value("#net-cSSID") == "MonkeyNet", "the Network page shows the current network")
    check(not pg.is_visible("#net-handover"), "no banner while nothing is moving")
    pg.fill("#net-cSSID", "NewNet")
    pg.fill("#net-cPass", "newpass123")
    before = len([u for u in requests if "/save_network" in u])
    pg.click('#page-settings_network button[type="submit"]')
    pg.wait_for_timeout(1500)
    check("Connected" in pg.locator("#net-testResult").inner_text() or "✓" in pg.locator("#net-testResult").inner_text(),
          "a new network is tested before the nodes are told about it (%r)" % pg.locator("#net-testResult").inner_text())
    check(len([u for u in requests if "/save_network" in u]) == before,
          "and the collector does NOT switch at once while it has nodes")
    ho = pg.locator("#net-handover")
    check(ho.is_visible(), "a banner follows the move instead")
    title = ho.locator(".nd-ho-title").inner_text() if ho.locator(".nd-ho-title").count() else ""
    check("Moving nodes to NewNet: 0 of 2 ready" in title,
          "it counts the online nodes that have the new network (%r)" % title)
    check("espnow-03" in ho.inner_text().lower() and "shed-wifi" in ho.inner_text().lower(),
          "the offline nodes are listed by name, apart")
    mock = pg.evaluate("fetch('/__mock/nodes').then(function(r){return r.json()})")
    form = (mock["handover"] or {}).get("form") or {}
    check(mock["handover"].get("ssid") == "NewNet" and mock["handover"].get("pass") == "newpass123",
          "the collector was handed the new network")
    check(form.get("clientSSID") == "NewNet" and "useStaticIP" not in form and form.get("wifiMode") == "1",
          "with the rest of the form, to apply when it switches (%r)" % sorted(form.keys()))
    pg.wait_for_timeout(4500)          # one poll: one node confirms
    title = ho.locator(".nd-ho-title").inner_text()
    check("1 of 2 ready" in title, "the banner fills in as nodes confirm (%r)" % title)
    check("garden" in ho.locator(".nd-ho-list").first.inner_text().lower(),
          "and says which (%r)" % ho.locator(".nd-ho-list").first.inner_text())

    # The same banner on the Nodes page, and the WiFi node's `next`.
    pg.evaluate("location.hash = 'settings_nodes'")
    pg.wait_for_timeout(1200)
    check(pg.is_visible("#nd-handover") and "Moving nodes to NewNet" in pg.locator("#nd-handover").inner_text(),
          "the Nodes page shows the move too")
    row(pg, "rn:greenhouse").click()
    pg.wait_for_timeout(700)
    check("Next network: NewNet" in panel("w:greenhouse").inner_text(),
          "a WiFi node's panel shows the network it has been handed")
    pg.click('#nd-handover [data-click="nodesHoCancel"]')
    pg.wait_for_timeout(700)
    check(not pg.is_visible("#nd-handover"), "Cancel takes the banner down")
    check("Cancelled" in pg.locator("#nd-msg").inner_text(), "and says the nodes stay put")

    # Again, and this time switch without waiting for the last node. The same
    # credentials already passed, so they are not tested a second time.
    pg.evaluate("location.hash = 'settings_network'")
    pg.wait_for_timeout(1200)
    pg.fill("#net-cSSID", "NewNet")
    pg.fill("#net-cPass", "newpass123")
    tests = len([u for u in requests if "/api/modules/wifi/test" in u])
    pg.click('#page-settings_network button[type="submit"]')
    pg.wait_for_timeout(1200)
    check(len([u for u in requests if "/api/modules/wifi/test" in u]) == tests,
          "credentials that already passed are not tested again")
    check(pg.is_visible("#net-handover"), "the banner is back")
    pg.click('#net-handover [data-click="nodesHoSwitch"]')    # confirm() accepted
    pg.wait_for_timeout(800)
    check("Switching to NewNet" in pg.locator("#net-handover").inner_text(),
          "Switch now says the collector is on its way (%r)" % pg.locator("#net-handover").inner_text()[:50])
    mock = pg.evaluate("fetch('/__mock/nodes').then(function(r){return r.json()})")
    check(mock["handover"].get("switched") is True, "and the collector was told to switch")

    # A node list that cannot be fetched is not "no nodes": nothing is saved,
    # and the page says why.
    pg.route("**/api/espnow/status", lambda r: r.fulfill(status=200, content_type="application/json", body='{"nodes":[]}'))
    pg.route("**/api/remote/status", lambda r: r.fulfill(status=500, body="busy"))
    pg.fill("#net-cSSID", "FourthNet")
    before = len([u for u in requests if "/save_network" in u])
    pg.click('#page-settings_network button[type="submit"]')
    pg.wait_for_timeout(1200)
    check(len([u for u in requests if "/save_network" in u]) == before,
          "a failed node count does not save the network without a handover")
    check("could not check which nodes" in pg.locator("#net-msg").inner_text(),
          "and says why (%r)" % pg.locator("#net-msg").inner_text()[:60])
    pg.unroute("**/api/remote/status")

    # No nodes at all: exactly the old behaviour — save and restart.
    pg.route("**/api/remote/status", lambda r: r.fulfill(status=200, content_type="application/json", body='{"nodes":[]}'))
    pg.fill("#net-cSSID", "ThirdNet")
    before = len([u for u in requests if "/save_network" in u])
    pg.click('#page-settings_network button[type="submit"]')
    pg.wait_for_timeout(1200)
    check(len([u for u in requests if "/save_network" in u]) == before + 1,
          "with no nodes to move, a new network is saved as it always was")
    pg.unroute("**/api/espnow/status")
    pg.unroute("**/api/remote/status")

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
