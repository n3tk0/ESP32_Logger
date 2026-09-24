"""drive_modules_page.py — click the Settings › Modules page in Chromium.

Written after a report that the page showed no modules while /api/modules,
opened in a browser tab, plainly returned them. The page had no driver and
the mock had no /api/modules route at all, so nothing here exercised it.

The two things worth asserting are the two the report turned on:

  every module renders — including one whose `status` is its own domain
  object with no `text` field (forecast answers {provider, tempC, summary}).
  That row must fall through to the client-side heuristic; it must not print
  "undefined" and it must not take the other rows down with it.

  a failure says WHICH failure. Every way of not loading used to collapse
  into one "Could not reach /api/modules" with nothing on the console, so a
  device that answered fine but shipped an unparseable body looked exactly
  like one that was off the network. They are opposite problems and they
  have opposite fixes. A body that r.json() rejects is not hypothetical: an
  ESPAsyncWebServer stream that runs out of heap is cut short mid-document,
  and a browser tab shows that as perfectly fine text because it prints the
  bytes instead of parsing them.

    python3 tests/web/mock_device.py 8765 &
    python3 tests/web/drive_modules_page.py
"""
import json
import os
import re
import sys
import threading
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from playwright.sync_api import sync_playwright

PORT = os.environ.get("MOCK_PORT", "8765")
BASE = "http://127.0.0.1:" + PORT
fails = []
console = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails.append(what)


# ── A proxy that can corrupt one response ───────────────────────────────────
# Everything is forwarded to the real mock untouched except /api/modules,
# which is served from MODE. That keeps the corruption to the one response
# under test instead of teaching the shared mock to lie.
MODE = {"kind": "pass"}


class Proxy(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/modules" and MODE["kind"] != "pass":
            body = urllib.request.urlopen(BASE + "/api/modules").read()
            if MODE["kind"] == "truncated":
                # A REAL cut-short stream: Content-Length promises the whole
                # body, we send half and hang up. Recomputing the header
                # instead would produce a *complete* response that merely
                # isn't JSON — a different failure with a different rejection
                # (SyntaxError, not TypeError), which is how an earlier
                # version of this test passed while the page misreported the
                # case it exists for.
                return self._send(body, deliver=body[: len(body) // 2],
                                  close=True)
            if MODE["kind"] == "badbyte":
                # Complete, correctly framed, and not JSON: a raw control
                # byte inside a string. This is the SyntaxError case.
                return self._send(body.replace(b'"Wi-Fi"', b'"Wi\x07Fi"'))
            if MODE["kind"] == "http500":
                return self._send(b'{"ok":false}', code=500)
            return self._send(body)
        try:
            with urllib.request.urlopen(BASE + self.path) as r:
                return self._send(r.read(), ctype=r.headers.get("Content-Type",
                                                                "text/html"))
        except urllib.error.HTTPError as e:
            return self._send(e.read() or b"", code=e.code)

    def _send(self, raw, code=200, ctype="application/json",
              deliver=None, close=False):
        """`raw` sets Content-Length; `deliver` is what actually goes on the
        wire. They differ only for the cut-short fixture."""
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        if close:
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw if deliver is None else deliver)
        if close:
            self.close_connection = True


PROXY_PORT = int(PORT) + 100
srv = ThreadingHTTPServer(("127.0.0.1", PROXY_PORT), Proxy)
threading.Thread(target=srv.serve_forever, daemon=True).start()
URL = "http://127.0.0.1:%d/#settings_modules" % PROXY_PORT


def rows(pg):
    return pg.locator("#mod-list .mod-row")


with sync_playwright() as p:
    exe = os.environ.get("CHROMIUM_PATH")
    b = p.chromium.launch(executable_path=exe) if exe else p.chromium.launch()
    pg = b.new_page()
    pg.on("console", lambda m: console.append((m.type, m.text)))
    pg.on("pageerror", lambda e: console.append(("pageerror", str(e))))

    # ── the happy path ──────────────────────────────────────────────────────
    pg.goto(URL, wait_until="networkidle")
    pg.wait_for_timeout(1500)

    expected = json.loads(
        urllib.request.urlopen(BASE + "/api/modules").read().decode())
    check(rows(pg).count() == len(expected),
          "every module in the index gets a row (%d of %d)"
          % (rows(pg).count(), len(expected)))

    listed = rows(pg).all_inner_texts()
    blob = "\n".join(listed)
    for m in expected:
        check(m["name"] in blob, "%r is listed" % m["name"])

    # The chip the firmware supplied, rendered as-is.
    check("Internet · 192.168.1.214 · -19 dBm" in blob,
          "a firmware-supplied status chip is shown verbatim")

    # forecast's status is {provider, tempC, summary} — no `text`. The row
    # must fall back, not print the object or the word undefined.
    fc = pg.locator('.mod-row[data-mod="forecast"]')
    check(fc.count() == 1, "the module with no status.text still has a row")
    fct = fc.inner_text() if fc.count() else ""
    check("undefined" not in fct.lower() and "[object" not in fct.lower(),
          "and it falls back rather than printing undefined (%r)" % fct[-40:])
    check(fc.locator(".mod-chip").count() == 1,
          "and it still gets a status chip")

    # hasUI:false is marked, not hidden.
    usb = pg.locator('.mod-row[data-mod="usbcdc"]')
    check(usb.locator(".badge").count() == 1,
          "a status-only module is badged rather than dropped")

    # The first module is selected and its form loaded. Asserting only that
    # the pane is non-empty would pass on "Bad schema JSON." — which is what
    # it used to render, because the mock handed back a parsed array while
    # the firmware sends a JSON *string*. Assert on the fields instead.
    check(rows(pg).nth(0).get_attribute("class").find("active") != -1,
          "the first module starts selected")
    host = pg.locator("#mod-host")
    check("schema" not in host.inner_text().lower(),
          "and its schema parses (%r)" % host.inner_text()[:70])
    check(host.locator('[name="ssid"]').count() == 1,
          "and the form has the field the schema declares")
    check(host.locator('[name="ssid"]').input_value() == "MonkeyNet",
          "populated from the module's config")

    # showIf is the one schema key the form evaluates rather than renders:
    # wifi's static-IP field is hidden until useStatic is ticked.
    ip_row = host.locator('[name="ip"]')
    check(ip_row.count() == 1, "a showIf-gated field is present in the DOM")
    check(not ip_row.is_visible(), "but hidden while its condition is false")
    # A bool renders as a .switch whose real <input> is opacity:0 — the
    # visible control is its sibling <span>, so click that, the way a person
    # does. Clicking the input itself is what a person cannot do.
    host.locator('[name="useStatic"] + span').click()
    pg.wait_for_timeout(300)
    check(host.locator('[name="useStatic"]').is_checked(),
          "ticking the switch flips the checkbox behind it")
    check(ip_row.is_visible(), "and revealed once the condition holds")

    # ── the forecast's refresh button ────────────────────────────────────────
    # A panel under the form: provider, age, failures, and a button that asks
    # /kindle/forecast and then polls the module until fetchedAt moves.
    urllib.request.urlopen(BASE + "/__mock/forecast?reset=1").read()
    # The wifi form above was left dirty on purpose; leaving it asks first.
    pg.once("dialog", lambda d: d.accept())
    fc.click()
    pg.wait_for_timeout(800)
    btn = pg.locator("#fc-refresh")
    check(btn.count() == 1, "the forecast module shows a refresh button")
    panel = host.inner_text()
    check("Open-Meteo" in panel and "1 h ago" in panel,
          "and the provider and the age of the last fetch (%r)"
          % panel[-120:].replace("\n", " "))
    btn.click()
    pg.wait_for_timeout(3500)
    msg = pg.locator("#settings_modules").inner_text() if pg.locator("#settings_modules").count() else pg.inner_text("body")
    check("Forecast updated." in msg, "a refresh that lands says so")
    check("0 min ago" in host.inner_text(), "and the age row moves to the new fetch")
    btn.click()
    pg.wait_for_timeout(800)
    msg = pg.inner_text("body")
    check("Try again in 57 s" in msg, "a second press inside the minute is told to wait")

    # ── a failure names itself ──────────────────────────────────────────────
    # Each of these used to render the same sentence with an empty console.
    def reload_with(kind):
        MODE["kind"] = kind
        pg.goto("about:blank")
        pg.goto(URL, wait_until="networkidle")
        pg.wait_for_timeout(1500)
        return pg.locator("#mod-list").inner_text()

    # A cut-short stream rejects as a bare "TypeError: Failed to fetch" —
    # identical to an unplugged device, because once the body breaks there is
    # nothing left to parse. The page must not claim the request never
    # arrived: that reading sends someone off checking their wiring while the
    # firmware is what ran out of heap. It has to name both.
    txt = reload_with("truncated")
    check(rows(pg).count() == 0, "a reply cut short loads no rows")
    check("cut short" in txt,
          "and says the reply may have been cut short (%r)"
          % txt.replace("\n", " ")[:100])
    check("never reached" not in txt,
          "and does NOT claim the request never reached the device")
    check(pg.locator("#mod-retry").count() == 1, "and offers a retry")

    # A complete body that is not JSON is the narrower, separable case.
    txt = reload_with("badbyte")
    check("not JSON" in txt,
          "a complete but unparseable reply is reported as such (%r)"
          % txt.replace("\n", " ")[:100])
    check("cut short" not in txt,
          "and is told apart from a cut-short one")
    check(any(c[0] == "error" and "/api/modules" in c[1] for c in console),
          "and leaves the reason on the console")

    txt = reload_with("http500")
    check("500" in txt,
          "an HTTP error reports its status instead (%r)"
          % txt.replace("\n", " ")[:90])

    # No stray markup: the string is escaped by the caller, so it must not
    # carry tags of its own.
    check("<code>" not in txt and "&lt;" not in txt,
          "and the message is plain text, with no tags showing through")

    # Retry recovers once the device is answering again.
    MODE["kind"] = "pass"
    pg.click("#mod-retry")
    pg.wait_for_timeout(1200)
    check(rows(pg).count() == len(expected),
          "retry reloads the list once the device answers (%d)" % rows(pg).count())

    shot = os.environ.get("SCREENSHOT")
    if shot:
        pg.screenshot(path=shot, full_page=True)
    b.close()

# The failures this driver provokes on purpose are logged by design, and the
# mock does not serve every route the SPA touches on boot. Exclude those two
# and nothing else: an earlier filter here dropped anything mentioning
# "/api/modules", which on this page is very nearly every error it can emit,
# leaving only pageerror meaningfully asserted.
PROVOKED = "did not return usable JSON"           # what _getJson logs
errs = [c for c in console
        if c[0] == "pageerror"
        or (c[0] == "error"
            and PROVOKED not in c[1]
            and "could not load /api/modules" not in c[1].lower()
            and "404" not in c[1]
            and "Failed to load resource" not in c[1])]
print()
for t, m in errs[:8]:
    print(f"  console {t}: {m[:140]}")
check(not errs, f"no unexpected console errors ({len(errs)})")

print()
print(("FAIL: %d" % len(fails)) if fails else
      "OK: the module list renders, and a failure says which failure")
sys.exit(1 if fails else 0)
