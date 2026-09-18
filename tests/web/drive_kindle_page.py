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

The second is ONE SAVE. There used to be two, over two working copies, with
nothing on screen saying which covered what — so editing a place and pressing
the Save at the foot of the page stored nothing and reported success. The
driver now edits both halves and presses one button, then reads both endpoints.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_kindle_page.py
"""
import os
import sys
from playwright.sync_api import sync_playwright

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT
URL = BASE + "/#settings_kindle"
fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


# Redesign 3a (panel-led/tabbed) split the inspector into Zones / Whole page /
# Reader tabs; only one is visible at a time, so any interaction with a field
# outside the active one needs a switch first. The panel itself and the
# savebar are NOT tab-gated (they sit outside .kd-pane-form entirely), so
# clicks on panel hit-targets and the Save/Discard buttons work regardless of
# which tab is showing.
def tab(pg, name):
    pg.click('#kd-tabs button[data-tab="%s"]' % name)
    pg.wait_for_timeout(150)


# What mock_device.py starts with — not the defaults, on purpose: a page that
# renders correctly only when every value is zero has never had its selects
# proven against anything.
START = dict(face="4", clock="3", time="2", date="1", press="1", dec="0",
             lang="2")

with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page(viewport={"width": 1400, "height": 1000})
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))

    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1400)

    check(pg.locator("#page-settings_kindle").count() == 1,
          "the page partial was fetched and injected")

    # Every select shows what the device holds, not its own first option.
    for el, want in (("kd-face", START["face"]), ("kd-clock", START["clock"]),
                     ("kd-time", START["time"]), ("kd-date", START["date"]),
                     ("kd-press", START["press"]), ("kd-dec", START["dec"]),
                     # The language was a build flag, so the page never had a
                     # control for it and the reader had to reflash a panel on
                     # a wall to read it in their own language.
                     ("kd-lang", START["lang"])):
        got = pg.input_value("#" + el)
        check(got == want, f"#{el} reflects the device ({got!r})")

    # ── The regions ─────────────────────────────────────────────────────────
    # One row per region of the panel, in the order the panel draws them, with
    # both bitmasks on the row itself. This replaced two anonymous checkbox
    # grids — one headed "Weight", one "What is drawn" — whose rows named the
    # same regions differently, in different orders, and never said where on
    # the panel a region was or what filled it; and then a table, whose four
    # columns still could not hold what filled a region.
    #
    # THE COUNTS ARE THE FIRST THING TO CHECK, and they are counted BY ID
    # PREFIX rather than by container: that proves every firmware bit reached
    # the page exactly once, which is the property that matters. A missing row
    # is a setting nobody can reach; a duplicated one is two checkboxes
    # fighting over the same bit.
    check(pg.locator("#kd-zones input[id^=kd-b-]").count() == 9,
          "every weight bit has a row")
    check(pg.locator("#kd-zones input[id^=kd-s-]").count() == 8,
          "every visibility bit has a row")
    ids = pg.eval_on_selector_all(
        "#kd-zones input[type=checkbox][id]", "els => els.map(e => e.id)")
    check(len(ids) == len(set(ids)), "and no bit is claimed by two rows")

    # EVERY BIT'S CHECKBOX IS IN THE DOM WHETHER ITS ROW IS OPEN OR NOT. The
    # switches sit on the row rather than inside the part that expands, which
    # is what keeps the masks collectable in one walk — a page that rendered
    # them only for the open region would save whatever the others last were.
    open_rows = pg.locator("#kd-zones .kd-zone.open").count()
    check(open_rows == 1, "exactly one region is open to begin with (%d)" % open_rows)

    rows = pg.locator("#kd-zones .kd-zone")
    check(rows.count() == 13, "a row per region, plus the two page-wide ones (%d)"
          % rows.count())
    check(pg.locator("#kd-zones .kd-zwhere").count() == 13,
          "and every row says where on the panel it is")

    # bold = 0x0009 in the mock: outdoor temperature (0x1) and clock (0x8).
    check(pg.is_checked("#kd-b-1") and pg.is_checked("#kd-b-8"),
          "the set weight bits come back checked")
    check(not pg.is_checked("#kd-b-2"), "an unset weight bit comes back clear")
    # show = 0xFF minus the week strip (0x40).
    check(not pg.is_checked("#kd-s-64"), "the one cleared block comes back clear")
    check(pg.is_checked("#kd-s-32"), "a set block comes back checked")

    # THE CONTROL THE READER JUST USED IS STILL THE ONE ON THE PAGE. The rows
    # are innerHTML, so rebuilding them on every edit destroys the switch that
    # was clicked and takes the focus out of it — a switch that cannot be
    # toggled twice without reaching for it again. Proven by tagging the
    # element and looking for the tag afterwards, because "it still looks the
    # same" is exactly what a rebuilt element does.
    pg.evaluate("document.getElementById('kd-s-32').dataset.tag = 'mine'")
    pg.click("#kd-s-32")
    pg.wait_for_timeout(300)
    check(pg.evaluate("document.getElementById('kd-s-32').dataset.tag === 'mine'"),
          "toggling a switch does not destroy the switch")
    check(pg.evaluate("document.activeElement.id") == "kd-s-32",
          "and the focus stays in it")
    pg.click("#kd-s-32")   # back to where the mask assertions below expect it
    pg.wait_for_timeout(200)

    # ── The panel, drawn from the form ──────────────────────────────────────
    # The page configures a picture, so it shows the picture: every question
    # this form asks used to be answerable only by saving, walking to the
    # reader, and waiting for it to repaint.
    check(pg.locator("#kd-panel").count() == 1, "the panel preview rendered")
    drawn = pg.locator("#kd-panel i").count()
    check(drawn > 20, "with the page's strings on it (%d)" % drawn)
    hits = pg.locator("#kd-panel .kd-hit").count()
    check(hits == 10,
          "and a hit target per region that has a place on the panel (%d)" % hits)

    # THEY MUST NOT OVERLAP. The headline's number and the value beside it
    # share a baseline on the real panel, and hit targets drawn to match that
    # left the headline's almost entirely underneath its neighbour's: a region
    # you could only open by aiming at its edge, which a test that clicked the
    # centre found and a reader would have found too.
    over = pg.evaluate("""() => {
      var b = [].map.call(document.querySelectorAll('#kd-panel .kd-hit'),
        function (e) { var r = e.getBoundingClientRect();
                       return [e.getAttribute('data-args'), r]; });
      var out = [];
      for (var i = 0; i < b.length; i++) for (var j = i + 1; j < b.length; j++) {
        var p = b[i][1], q = b[j][1];
        if (p.left < q.right && q.left < p.right &&
            p.top < q.bottom && q.top < p.bottom) out.push(b[i][0] + '/' + b[j][0]);
      }
      return out;
    }""")
    check(not over, "and no two of them overlap (%s)" % (over or "none"))

    # The week strip is switched off in the mock, so its region is hatched and
    # nothing of it is drawn.
    check(pg.locator("#kd-panel .kd-hit.off").count() >= 1,
          "a switched-off region is marked off in the preview")
    # Asserted on the strip's own cells, not on the month's name: the dated
    # clock style draws that too, so the first spelling of this check passed
    # for the wrong reason and would have failed the day the clock changed.
    check(pg.locator("#kd-panel .kd-wk.today").count() == 0,
          "and is not drawn at all")

    # THE PREVIEW IS THE NAVIGATION. Tapping a region opens it.
    pg.click("#kd-panel .kd-hit[data-args='[\"inrow\"]']")
    pg.wait_for_timeout(350)
    check(pg.locator("#kd-zrow-inrow").get_attribute("class").find("open") >= 0,
          "tapping a region in the panel opens its row")
    check(pg.locator("#kd-zrow-inrow .kd-slot").count() == 3,
          "and the region's places are in it (%d)"
          % pg.locator("#kd-zrow-inrow .kd-slot").count())

    # A region that nothing on this page fills says so, rather than opening
    # onto an empty box.
    pg.click('#kd-zrow-week .kd-zhead')
    pg.wait_for_timeout(300)
    week = pg.locator("#kd-zrow-week .kd-zbody").inner_text()
    check("date" in week.lower(), f"a region nothing here fills says why ({week[:44]!r})")

    # And the pressure block's switch is a MASTER switch for the per-place
    # arrows, which is the one relationship the old table stated wrongly.
    pg.click('#kd-zrow-tend .kd-zhead')
    pg.wait_for_timeout(300)
    tend = pg.locator("#kd-zrow-tend .kd-zbody").inner_text()
    check("master" in tend.lower(), f"the tendency switch says what it governs ({tend[:44]!r})")

    # The custom-face field is hidden unless the custom face is chosen, and the
    # date format is dimmed when the clock does not draw one: a visible control
    # that does nothing is a question the page asks and then ignores the answer
    # to.
    tab(pg, "page")
    check(not pg.is_visible("#kd-face-custom-row"),
          "the custom font field is hidden for a named face")
    pg.select_option("#kd-face", "6")
    pg.wait_for_timeout(250)
    check(pg.is_visible("#kd-face-custom-row"),
          "choosing Custom reveals the font field")
    pg.select_option("#kd-face", "0")
    pg.wait_for_timeout(250)
    check(not pg.is_visible("#kd-face-custom-row"),
          "choosing a named face hides it again")

    check(not pg.is_visible("#kd-date-hint"),
          "the dated clock does not tell you the date is unused")
    pg.select_option("#kd-clock", "0")
    pg.wait_for_timeout(250)
    check(pg.is_visible("#kd-date-hint"),
          "another clock style says where the date format applies")

    # The page states the build-time width rather than offering it as a knob.
    tab(pg, "reader")
    intro = pg.locator("#kd-pagew").inner_text()
    check("600" in intro, f"the layout width is stated: {intro.strip()[:60]!r}")

    # ── The cadence, as named choices ───────────────────────────────────────
    # The mock holds 180 s with follow off, which is none of the three presets:
    # the control that matters is the one that can show a state nobody chose
    # from its own list.
    check(pg.input_value("#kd-refresh") == "180", "the interval reflects the device")
    check(pg.is_visible("#kd-cad-custom"),
          "a cadence that matches no preset opens the fields")
    pg.click('#kd-cad button[data-args=\'["saver"]\']')
    pg.wait_for_timeout(300)
    check(pg.input_value("#kd-refresh") == "600",
          "a preset sets the interval (%s)" % pg.input_value("#kd-refresh"))
    check(pg.input_value("#kd-follow") == "0", "and what it does with new data")
    check(not pg.is_visible("#kd-cad-custom"),
          "and puts the by-hand fields away again")
    says = pg.locator("#kd-cad-says").inner_text()
    check("battery" in says.lower(), f"and says what it costs ({says[:40]!r})")

    # ── One save ────────────────────────────────────────────────────────────
    # The bar appears only once something is unsaved, and says what.
    check(pg.is_visible("#kd-savebar"), "the save bar is up once something is edited")
    dirty = pg.locator("#kd-dirty").inner_text()
    check("setting" in dirty, f"and counts what is unsaved ({dirty!r})")

    # Bits flipped in both directions so a handler that only ever ORs, or one
    # that writes a constant, cannot pass. Split across the three tabs these
    # fields now live in (redesign 3a) — same edits, same order of intent,
    # just grouped by which panel has to be active to reach them.
    tab(pg, "page")
    pg.select_option("#kd-lang", "1")       # English
    pg.select_option("#kd-face", "2")       # Palatino
    pg.select_option("#kd-clock", "1")      # boxed
    pg.select_option("#kd-time", "0")       # 24 h
    pg.select_option("#kd-press", "0")      # hPa
    pg.select_option("#kd-dec", "1")
    tab(pg, "reader")
    pg.select_option("#kd-fbink-res", "0")  # back to the smaller panel
    tab(pg, "zones")
    pg.uncheck("#kd-b-1")                   # clear outdoor temperature
    pg.check("#kd-b-4")                     # set pressure
    pg.check("#kd-s-64")                    # put the week strip back
    pg.uncheck("#kd-s-2")                   # and drop the pressure block
    pg.wait_for_timeout(250)

    # AND A PLACE, in the same save. This is the half that used to be lost:
    # the Save at the foot of the page did not cover it and said "Saved".
    # (Zones tab is already active from the checkbox edits just above, which
    # is what the hero row's select needs to be reachable.)
    pg.click("#kd-panel .kd-hit[data-args='[\"hero\"]']")
    pg.wait_for_timeout(300)
    hero_ink = pg.locator("#kd-zone-hero select").last
    hero_ink.select_option("3")
    pg.wait_for_timeout(250)
    dirty = pg.locator("#kd-dirty").inner_text()
    check("place" in dirty, f"the bar counts the place too ({dirty!r})")

    pg.click('[data-click="kindleSave"]')
    pg.wait_for_timeout(1200)

    msg = pg.locator("#kd-msg").inner_text()
    check("next repaint" in msg, f"the page confirms it: {msg.strip()[:60]!r}")
    check(not pg.is_visible("#kd-savebar"),
          "and the save bar goes away, because nothing is unsaved")

    got = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(got["face"] == 2, f"the face round-trips (got {got['face']})")
    check(got["lang"] == 1, f"the language round-trips (got {got['lang']})")
    check(got["clock_style"] == 1, f"the clock style round-trips (got {got['clock_style']})")
    check(got["pressure_unit"] == 0, "the pressure unit round-trips")
    check(got["decimals"] == 1, "the decimal count round-trips")
    check(got["refresh_sec"] == 600, f"the cadence round-trips (got {got['refresh_sec']})")
    check(got["follow_data"] == 0, "and what it does with new data")
    check(got["fbink_res_w"] == 0, f"the panel size round-trips (got {got['fbink_res_w']})")
    # 0x0009 - 0x0001 + 0x0004 = 0x000C
    check(got["bold"] == 0x000C, f"the weight mask is exactly right (got {got['bold']:#06x})")
    # (0xFF - 0x40) + 0x40 - 0x02 = 0xFD
    check(got["show"] == 0x00FD, f"the visibility mask is exactly right (got {got['show']:#06x})")

    # THE PLACES WENT WITH IT, in the same press.
    slots = pg.evaluate(
        "fetch('/api/kindle/slots').then(function(r){return r.json()})")
    check(slots["zones"]["hero"]["ink"] == 3,
          "the place edited in the same breath was saved too (got %r)"
          % slots["zones"]["hero"]["ink"])
    check("reading" in msg, f"and the message says how many places ({msg[:40]!r})")

    # ── Discard ─────────────────────────────────────────────────────────────
    # The other half of one save: a way back that does not need a reload.
    tab(pg, "page")
    pg.select_option("#kd-dec", "0")
    pg.wait_for_timeout(250)
    check(pg.is_visible("#kd-savebar"), "an edit brings the bar back")
    pg.click('[data-click="kindleDiscard"]')
    pg.wait_for_timeout(900)
    check(pg.input_value("#kd-dec") == "1",
          "discarding puts back what the device holds (%s)" % pg.input_value("#kd-dec"))
    check(not pg.is_visible("#kd-savebar"), "and the bar goes away")

    # ── The places ──────────────────────────────────────────────────────────
    # Eleven fixed places. What matters here is that the dropdowns reflect the
    # HARDWARE: a BMP280 must not offer humidity, because it cannot measure it,
    # and that is the whole reason for the feature.
    tab(pg, "zones")
    pg.click("#kd-panel .kd-hit[data-args='[\"grid\"]']")
    pg.wait_for_timeout(350)
    grid_slots = pg.locator("#kd-zrow-grid .kd-slot")
    check(grid_slots.count() == 6,
          "the grid opens onto its six places, empty ones included (%d)"
          % grid_slots.count())
    check(pg.locator("#kd-zrow-grid .badge:has-text('empty')").count() == 4,
          "with the empty ones marked")
    check("/ 11" in pg.locator("#kd-slot-count").inner_text(),
          "the count badge says how many places are filled")

    # The first grid place names "balcony", a BMP280 in the mock.
    g1 = pg.locator("#kd-zone-g1")
    metric_opts = g1.locator("select").nth(1).locator("option").all_inner_texts()
    check("temperature" in metric_opts, "a reading the sensor publishes is offered")
    check("pressure" in metric_opts, "and so is its pressure")
    check(not any("humidity" in o for o in metric_opts),
          "a BMP280 is NOT offered humidity: %r" % (metric_opts,))

    # The reader's own name for a sensor, where they gave it one — the id alone
    # is the less human of the two, and this editor used to show only the id.
    sensor_opts = g1.locator("select").nth(0).locator("option").all_inner_texts()
    check(any("Balcony" in o for o in sensor_opts),
          "the sensor's name is offered beside its id: %r" % (sensor_opts,))

    # A place naming a sensor that is no longer configured keeps its entry
    # rather than being silently reassigned to whatever happens to be first.
    g4 = pg.locator("#kd-zone-g4 select").nth(0)
    check("not configured" in g4.inner_text(),
          "an unknown sensor is kept and marked, not dropped")

    # The derived label is offered as a placeholder, so the reader can see what
    # they get without typing anything.
    ph = pg.locator("#kd-zone-g1 input[maxlength='16']").get_attribute("placeholder")
    check(ph == "PM2.5", "the derived caption is the placeholder (%r)" % ph)

    # A heading the reader overrode comes back as a value; one they did not
    # comes back as a placeholder, so the form says which is which. They are
    # edited in the region that DRAWS them, which is where they are on the page.
    pg.click("#kd-panel .kd-hit[data-args='[\"hero\"]']")
    pg.wait_for_timeout(300)
    out_head = pg.locator("#kd-zrow-hero input[maxlength='16']").nth(0)
    check(out_head.input_value() == "БАЛКОН", "an overridden heading round-trips")
    pg.click("#kd-panel .kd-hit[data-args='[\"inrow\"]']")
    pg.wait_for_timeout(300)
    in_head = pg.locator("#kd-zrow-inrow input[maxlength='16']").nth(0)
    check(in_head.input_value() == "",
          "and one left alone stays empty rather than being filled in")
    check(in_head.get_attribute("placeholder") == "ВЪТРЕ",
          "with the built-in wording as its placeholder")

    # Emptying a place is local until Save, and leaves the editor in position —
    # the place does not disappear, it becomes available.
    pg.click("#kd-panel .kd-hit[data-args='[\"grid\"]']")
    pg.wait_for_timeout(300)
    before = pg.locator("#kd-zrow-grid .badge:has-text('empty')").count()
    pg.click('#kd-zone-g1 [data-click="kindleSlotClear"]')
    pg.wait_for_timeout(350)
    check(pg.locator("#kd-zrow-grid .kd-slot").count() == 6,
          "emptying a place leaves its editor where it was")
    check(pg.locator("#kd-zrow-grid .badge:has-text('empty')").count() == before + 1,
          "and marks it empty")
    check(pg.locator("#kd-zone-g1 select").nth(0).input_value() == "",
          "with no sensor selected")

    # Changing the sensor must not leave a metric that sensor cannot report.
    pg.locator("#kd-zone-g2 select").nth(0).select_option("livingroom")
    pg.wait_for_timeout(350)
    new_metric = pg.locator("#kd-zone-g2 select").nth(1).input_value()
    check(new_metric in ("temperature", "humidity", "pressure", "aqi"),
          "changing the sensor keeps a reading it actually publishes (%r)" % new_metric)

    # ── The preview draws the numbers the DEVICE would draw ─────────────────
    # "Automatic" decimals is the metric table's own number in the firmware
    # (KD_METRIC_STYLE in src/web/KindleSlots.h), and the decimals control on
    # this page is the temperature's — the firmware applies it to temperatures
    # and nothing else. Running every reading through it drew "640.0 ppm" and
    # "71.0 %", over-stating the width of every integer reading on a page whose
    # whole question is what fits where.
    ints = pg.evaluate(
        "(function(){var z={sensor:'x',metric:'co2',flags:kdFlags.unit,"
        "decimals:kdAutoDec,label:'',ink:0};"
        "var a=kdPvValue(z)+kdPvUnit(z);"
        "z.metric='humidity'; var b=kdPvValue(z)+kdPvUnit(z);"
        "z.metric='temperature'; var c=kdPvValue(z);"
        "z.metric='pm25'; var d=kdPvValue(z)+kdPvUnit(z);"
        "return [a,b,c,d];})()")
    check(ints[0] == "640ppm", "an integer reading is drawn as one (%r)" % ints[0])
    check(ints[1] == "71%", "and so is a humidity (%r)" % ints[1])
    check(ints[3] == "12\u00b5g/m\u00b3",
          "with the unit the panel prints, not a short form (%r)" % ints[3])
    # The one metric the control does apply to, whatever it is set to here.
    want_dec = int(pg.input_value("#kd-dec") or 0)
    check(ints[2] == ("%.*f" % (want_dec, 8.4)),
          "the decimals control still rules the temperature (%r at dec=%d)"
          % (ints[2], want_dec))

    # The badge over the drawing describes the DRAWING: these coordinates are
    # the 600 px layout, and KINDLE_PAGE_W scales the firmware's sizes from
    # that same 600, so a build-time width printed here alone says the preview
    # is a width it is not.
    badge = pg.inner_text("#kd-pv-size").lower()
    check(badge.startswith("600 px wide"), "the preview says how wide IT is (%r)" % badge)

    # ── A row says what is in it ────────────────────────────────────────────
    # The metric is most of a row: the summary under the name, the value badge
    # beside it, and the Tendency arrow — which exists for pressure only, and
    # stayed on screen with its flag still set after the place was moved to a
    # metric the firmware ignores it for.
    g1 = "#kd-zone-g1"
    if pg.locator(g1).count() == 0:          # the hit target toggles
        pg.click("#kd-panel .kd-hit[data-args='[\"grid\"]']")
        pg.wait_for_timeout(400)
    pg.locator(g1 + " select").nth(0).select_option(index=1)
    pg.wait_for_timeout(300)
    mets = pg.locator(g1 + " select").nth(1)
    opts = mets.locator("option").all_inner_texts()
    if "pressure" in opts:
        mets.select_option("pressure")
        pg.wait_for_timeout(300)
        check(pg.locator(g1 + " .kd-flag", has_text="Tendency").count() == 1,
              "a pressure place offers the tendency arrow")
        mets.select_option("temperature")
        pg.wait_for_timeout(300)
        check(pg.locator(g1 + " .kd-flag", has_text="Tendency").count() == 0,
              "and moving it off pressure takes the arrow away with it")
        head = pg.inner_text(g1 + " .kd-slot-head")
        check("\u00b0" in head or "8" in head,
              "with the value badge redrawn for the new reading (%r)" % head.strip()[:40])

    # ── "By hand…" stays open once it has been chosen ───────────────────────
    # kdCadenceName() reads the VALUES, and the custom fields start at whatever
    # the last named choice left in them — so every edit anywhere on the page
    # hid the fields the reader had just opened, and typing 600 into the
    # interval passes through 60, which IS a preset.
    tab(pg, "reader")
    pg.click('[data-click="kindleCadence"][data-args=\'["balanced"]\']')
    pg.wait_for_timeout(300)
    check(not pg.is_visible("#kd-cad-custom"), "a named cadence keeps the fields shut")
    pg.click('[data-click="kindleCadence"][data-args=\'["custom"]\']')
    pg.wait_for_timeout(300)
    check(pg.is_visible("#kd-cad-custom"), "By hand opens them")
    tab(pg, "zones")
    pg.click("#kd-s-32")                      # an unrelated switch, twice
    pg.wait_for_timeout(200)
    pg.click("#kd-s-32")
    pg.wait_for_timeout(200)
    # Back to Reader to actually SEE that the fields are still open — the
    # state persisted through the zones-tab edit above, but that's only
    # verifiable while this tab is the one showing.
    tab(pg, "reader")
    check(pg.is_visible("#kd-cad-custom"),
          "and an edit somewhere else does not shut them again")
    pg.fill("#kd-refresh", "60")              # a value that matches a preset
    pg.wait_for_timeout(300)
    check(pg.is_visible("#kd-cad-custom"),
          "nor does typing a number that happens to match one")
    pg.click('[data-click="kindleCadence"][data-args=\'["balanced"]\']')
    pg.wait_for_timeout(300)
    check(not pg.is_visible("#kd-cad-custom"), "and picking a named one shuts them")

    # ── Per-place ink ───────────────────────────────────────────────────────
    # How dark a value is drawn is the reader's, per place. The selects are
    # sensor, reading, caption, decimals, ink — so ink is the last one.
    def ink_of(key):
        return pg.locator("#kd-zone-" + key + " select").last.input_value()

    tab(pg, "zones")
    pg.click("#kd-panel .kd-hit[data-args='[\"big\"]']")
    pg.wait_for_timeout(300)
    check(ink_of("big") == "2", "a place pushed into the mid grey round-trips (%s)"
          % ink_of("big"))
    pg.locator("#kd-zone-big select").last.select_option("3")
    pg.wait_for_timeout(300)
    check(ink_of("big") == "3", "choosing an ink sticks")
    check(pg.evaluate("kdZones.big.ink") == 3,
          "and reaches the working copy as a number, not a string")

    # ── Back to the built-in design ─────────────────────────────────────────
    # It fills the FORM and waits for Save, rather than writing on the spot:
    # a destructive button that acts before the reader has seen what it did is
    # a button nobody can undo.
    pg.on("dialog", lambda d: d.accept())
    tab(pg, "reader")
    pg.click('[data-click="kindleDefaults"]')
    pg.wait_for_timeout(700)
    check(pg.input_value("#kd-face") == "0", "restoring fills the form")
    check(pg.is_visible("#kd-savebar"), "and leaves it unsaved, waiting for Save")
    # AND THE ROWS, WHICH ARE HALF THE THING BEING RESTORED. They were drawn
    # before the places were replaced, so every row still described the layout
    # the button had just thrown away while the preview beside them showed the
    # new one — and touching any of those stale controls wrote its old value
    # back into the fresh layout.
    check(pg.evaluate("kdZones.g3 && kdZones.g3.sensor === ''"),
          "the built-in design leaves the last grid places empty")
    if pg.locator("#kd-zone-g3").count() == 0:
        pg.click("#kd-panel .kd-hit[data-args='[\"grid\"]']")
        pg.wait_for_timeout(400)
    check(pg.locator("#kd-zone-g3 .badge", has_text="empty").count() == 1,
          "and the rows say so, rather than still describing the old layout")
    live = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(live["face"] == 2, "nothing on the device changed until Save (%r)" % live["face"])
    pg.click('[data-click="kindleSave"]')
    pg.wait_for_timeout(1200)
    got = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(got["face"] == 0 and got["bold"] == 0 and got["show"] == 0xFF and
          got["clock_style"] == 0 and got["decimals"] == 1,
          "and then it is the built-in design")
    # Back to "as built" — not to English, which would be a choice the restore
    # button did not make on behalf of somebody who had never touched this.
    check(got["lang"] == 0, f"and the language goes back to as-built (got {got['lang']})")

    # "As built" has to say WHICH language that is, or it is a promise the page
    # cannot keep: a reader looking at the option has no way to know whether
    # leaving it there means English or Bulgarian.
    tab(pg, "page")
    as_built = pg.locator("#kd-lang option[value='0']").inner_text()
    check("English" in as_built,
          f"the as-built option names the build's language ({as_built!r})")

    # ── A LAYOUT NOBODY COULD READ IS A LAYOUT NOBODY MAY OVERWRITE ─────────
    #
    # The single Save writes both endpoints, which means a slots GET that fails
    # — older firmware with no such route, a timeout, a device that rebooted —
    # is no longer harmless: kdZones stays empty, the form shows eleven empty
    # places because it cannot tell that from a device with nothing configured,
    # and the first Save posts that over the reader's whole layout and reports
    # success. While the places had a Save of their own, this reader would
    # never have pressed it.
    before = pg.evaluate(
        "fetch('/api/kindle/slots').then(function(r){return r.json()})")
    pg.route("**/api/kindle/slots*",
             lambda route: route.abort() if route.request.method == "GET"
             else route.continue_())
    # RELOADED, not navigated: the address already carries this page's hash, so
    # goto() would be a same-document navigation and fetch nothing at all.
    pg.reload(wait_until="networkidle")
    pg.wait_for_timeout(1400)
    check(pg.evaluate("kdSlotsOk") is False, "a slots read that failed is remembered")
    check(pg.locator("#kd-zones-unread").count() == 1,
          "and the rows say they are not the device's, rather than looking empty")
    pg.unroute("**/api/kindle/slots*")
    tab(pg, "page")
    pg.select_option("#kd-face", "1")
    pg.wait_for_timeout(200)
    pg.click('[data-click="kindleSave"]')
    pg.wait_for_timeout(1400)
    after = pg.evaluate(
        "fetch('/api/kindle/slots').then(function(r){return r.json()})")
    check(after["zones"] == before["zones"],
          "one Save cannot erase the places it was never able to read")
    cfg = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(cfg["face"] == 1, "while the half it did read is still saved")
    said = pg.inner_text("#kd-msg")
    check("left exactly as they are" in said,
          "and the page says which half went (%r)" % said.strip()[:80])

    # And the other way round: the appearance written, the places refused. The
    # message has to say so — "Nothing on the reader has changed" over a device
    # that has already repainted is the same untrue save report the one-Save
    # redesign exists to end.
    # The pattern ends in a star because postWithCsrf() hangs ?csrf=... on the
    # URL: without it the POST is not matched and the abort never happens.
    pg.route("**/api/kindle/slots*",
             lambda route: route.abort() if route.request.method == "POST"
             else route.continue_())
    pg.reload(wait_until="networkidle")
    pg.wait_for_timeout(1400)
    tab(pg, "page")
    pg.select_option("#kd-face", "3")
    pg.wait_for_timeout(200)
    pg.click('[data-click="kindleSave"]')
    pg.wait_for_timeout(1400)
    said = pg.inner_text("#kd-msg")
    check("The appearance was saved" in said,
          "a half-written save says what was written (%r)" % said.strip()[:90])
    pg.unroute("**/api/kindle/slots*")

    # ── The standalone page ─────────────────────────────────────────────────
    #
    # A collector that is its own access point cannot fetch a forecast, so the
    # band goes and the readings take it. The page offers the shape as a
    # setting, and the preview has to draw the one that was chosen — a preview
    # showing the other page is worse than none, because it is believed.
    pg.reload(wait_until="networkidle")
    pg.wait_for_timeout(1400)
    check(pg.input_value("#kd-layout") == "0",
          "the page's shape starts at what the device holds (follow the collector)")
    fc_before = pg.locator("#kd-panel .kd-hit[data-args='[\"fc\"]']").count()
    tab(pg, "reader")
    pg.select_option("#kd-layout", "2")
    pg.wait_for_timeout(500)

    # The forecast is not on this page at all: no block, and nothing to tap.
    check(fc_before == 1 and
          pg.locator("#kd-panel .kd-hit[data-args='[\"fc\"]']").count() == 0,
          "choosing standalone takes the forecast off the preview")
    check(pg.locator("#kd-zrow-fc .badge", has_text="not on this page").count() == 1,
          "and says so on its row, which stays for its weight switch")

    # ...and the chart moved down into what the band used to be.
    chart = pg.evaluate(
        "(function(){var p=document.querySelector('#kd-panel').getBoundingClientRect();"
        "var c=document.querySelector('#kd-panel .kd-hit[data-args=\\'[\"chart\"]\\']')"
        ".getBoundingClientRect();"
        "return Math.round((c.top-p.top)/(p.height/800));})()")
    check(abs(chart - 406) <= 3,
          "the chart sits where the standalone layout puts it (%d, want 406)" % chart)

    # ── AND EVERY READING STILL FITS ITS COLUMN ─────────────────────────────
    # The freed band is VERTICAL. The columns are the width they always were,
    # so the type could only grow until the widest string each reading can
    # produce filled its cell — which is why the standalone sizes are a sixth
    # larger and not a third. Measured with the page's own estimator, against
    # the column widths the layout files fix.
    fits = pg.evaluate(
        "(function(){var L=kdShape(),bad=[];"
        "if (kdTw('1008',L.gridVal3)+3+kdTw('hPa',Math.round(L.gridVal3*0.42))>90)"
        " bad.push('grid-3');"
        "if (kdTw('1008',L.gridVal)+3+kdTw('hPa',Math.round(L.gridVal*0.42))>135)"
        " bad.push('grid-2');"
        "if (kdTw('21.4',L.inVal1)+kdTw('\\u00b0',Math.round(L.inVal1*0.34))>111)"
        " bad.push('indoor-1');"
        "if (kdTw('17:40',L.clockSz)>264) bad.push('clock');"
        "if (18+kdTw('8.4',L.heroSz)+kdTw('\\u00b0',Math.round(L.heroSz*0.34))+8+"
        "kdTw('/',L.bigSz)+6+kdTw('71',L.bigSz)+"
        "kdTw('%',Math.round(L.bigSz*0.42))>288) bad.push('headline');"
        "return bad;})()")
    check(not fits, "every reading still fits its column (%s)" % (fits or "all fit"))

    # The targets are a different set of rectangles on this page, and they have
    # to be disjoint here too — the reason the first set was not.
    over2 = pg.evaluate(
        "(function(){var b=[].map.call("
        "document.querySelectorAll('#kd-panel .kd-hit'),function(e){"
        "var r=e.getBoundingClientRect();"
        "return [e.getAttribute('data-args'),r];});var out=[];"
        "for(var i=0;i<b.length;i++)for(var j=i+1;j<b.length;j++){"
        "var p=b[i][1],q=b[j][1];"
        "if(p.left<q.right&&q.left<p.right&&p.top<q.bottom&&q.top<p.bottom)"
        "out.push(b[i][0]+'/'+b[j][0]);}return out;})()")
    check(not over2, "and no two standalone targets overlap (%s)" % (over2 or "none"))

    # ── AND THE PREVIEW'S COPY OF THE GEOMETRY IS THE PANEL'S ───────────────
    #
    # KD_SHAPE is a third copy of these numbers: the panel reads them from
    # kindle/layout/*.conf, the browser page gets its type sizes from the
    # firmware's stylesheet (tools/check_kindle_parity.py holds those two
    # together), and the preview draws from this table. Nothing connected the
    # third one, so a layout edit could leave the preview quietly describing
    # the page as it used to be — which is the one failure a preview cannot
    # survive, because it is believed.
    def conf_of(path):
        vals = {}
        for line in open(os.path.join(ROOT_DIR, path), encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            v = v.strip()
            if v.lstrip("-").isdigit():
                vals[k.strip()] = int(v)
        return vals

    base = conf_of("kindle/layout/600x800.conf")
    sa = dict(base, **conf_of("kindle/layout/600x800-standalone.conf"))
    KEYS = {
        "heroSz":"HERO_SZ", "bigSz":"BIG_SZ", "subY":"SUB_Y", "subSz":"SUB_SZ",
        "gridY":"GRID_Y", "rowH":"GRID_ROW_H", "gridVal":"GRID_VAL_SZ",
        "gridVal3":"GRID_VAL_SZ_3", "labSz":"GRID_LAB_SZ", "sepH":"SEP_H",
        "clockSz":"CL_SIZE", "clockH":"CL_H", "boxed":"CL_SZ_BOXED",
        "ruled":"CL_SZ_RULED", "dated":"CL_SZ_DATED", "dateSz":"CL_DATE_SZ",
        "inRuleY":"IN_RULE_Y", "inLabY":"IN_LAB_Y", "inValY":"IN_VAL_Y",
        "inVal1":"IN_VAL_SZ_1", "inVal":"IN_VAL_SZ", "chartRuleY":"RULE2_Y",
        "chartLabY":"LAB_CHART_Y", "chartY":"GR_Y", "keyY":"KEY_Y",
    }
    shapes = pg.evaluate("KD_SHAPE")
    drift = []
    for shape, conf in (("normal", base), ("sa", sa)):
        for js_key, conf_key in KEYS.items():
            if shapes[shape][js_key] != conf.get(conf_key):
                drift.append("%s.%s=%s but %s=%s" % (
                    shape, js_key, shapes[shape][js_key],
                    conf_key, conf.get(conf_key)))
    check(not drift, "the preview's geometry is the panel's, both shapes (%s)"
          % ("; ".join(drift) if drift else "all %d agree" % (2 * len(KEYS))))

    # It is a setting like any other: unsaved until Save, then read back.
    check(pg.is_visible("#kd-savebar"), "choosing a shape is an unsaved change")
    pg.click('[data-click="kindleSave"]')
    pg.wait_for_timeout(1400)
    got = pg.evaluate(
        "fetch('/api/kindle/config').then(function(r){return r.json()})")
    check(got["layout_mode"] == 2, "and it reaches the device (%r)" % got["layout_mode"])
    check(pg.input_value("#kd-layout") == "2", "and comes back on the re-read")

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
