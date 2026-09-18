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
all, by construction (configured on their own captive portal, not this one) —
so the driver checks they render as dashes rather than invented zeros too.

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

    # ── Rename via the savebar, not an immediate per-row save ────────────────
    # This is the one real behaviour change from the page this replaces: the
    # old page saved a rename the moment its own button was clicked; the
    # merged page tracks it as dirty and saves through one savebar, same as
    # every other redesigned settings page now does.
    row(pg, "en:1").click()
    pg.wait_for_timeout(350)
    check(pg.is_visible("#nd-label-1"), "opening a row reveals its rename field")
    check(not pg.is_visible("#nd-savebar"), "nothing is dirty yet")
    pg.fill("#nd-label-1", "garden")
    pg.wait_for_timeout(250)
    check(pg.is_visible("#nd-savebar"), "editing a field brings the savebar up")
    dirty = pg.locator("#nd-dirty").inner_text()
    check("1" in dirty, "and it counts one changed node (%r)" % dirty)

    # AN UNSAVED EDIT SURVIVES A RE-RENDER. #nd-rows is replaced wholesale on
    # every collapse, filter and search, and only the open row has inputs at
    # all — so a draft read back out of the DOM was lost the moment the row
    # closed, while the bar stayed up and Save then wrote nothing. Collapse,
    # filter and search between the edit and the Save so the row is rebuilt
    # several times over.
    row(pg, "en:1").click()          # collapse
    pg.wait_for_timeout(250)
    check(pg.is_visible("#nd-savebar"), "the bar survives collapsing the edited row")
    pg.click('#nd-filter button[data-args=\'["espnow"]\']')
    pg.wait_for_timeout(200)
    pg.fill("#nd-search", "out")
    pg.wait_for_timeout(200)
    pg.fill("#nd-search", "")
    pg.click('#nd-filter button[data-args=\'["all"]\']')
    pg.wait_for_timeout(200)
    row(pg, "en:1").click()          # re-open
    pg.wait_for_timeout(300)
    check(pg.input_value("#nd-label-1") == "garden",
          "and the edit itself survives, rather than reverting to the device's value (%r)"
          % pg.input_value("#nd-label-1"))
    dirty = pg.locator("#nd-dirty").inner_text()
    check("1" in dirty, "with the bar still counting exactly one node (%r)" % dirty)

    pg.click('[data-click="nodesSave"]')
    pg.wait_for_timeout(900)
    check("garden" in pg.locator("#nd-rows").inner_text().lower(),
          "a rename round-trips and re-renders")
    check(not pg.is_visible("#nd-savebar"), "and the bar goes away once saved")

    # It reached the DEVICE, not just the list: a Save that re-rendered from
    # its own drafts would look identical here without having sent anything.
    saved = pg.evaluate(
        "fetch('/api/espnow/status').then(function(r){return r.json()})")
    names = [n["id"] for n in saved.get("nodes", [])]
    check("garden" in names, "and the device holds the new name (%r)" % names)

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
