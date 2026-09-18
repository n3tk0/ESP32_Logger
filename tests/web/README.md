# Browser tests

The web pages are the one part of this project with no other way to be checked.
They are not compiled, so nothing else here would notice a page that fetches a
field the firmware does not send, references an icon that is not in the sheet,
or wires a button to a handler `core.js` never registered in its allowlist.
Every one of those opens a window that looks fine and does nothing.

So: serve `www/` with the device's API stubbed, open it in a real browser, and
assert on what is painted.

```
python3 tests/web/mock_device.py 8765 &
python3 tests/web/drive_nodes_page.py
python3 tests/web/drive_kindle_page.py
python3 tests/web/drive_sensors_page.py
```

| | |
|---|---|
| [`mock_device.py`](mock_device.py) | serves `www/` and stubs the endpoints these pages use |
| [`drive_nodes_page.py`](drive_nodes_page.py) | drives `#settings_nodes` (ESP-NOW + WiFi remote, merged) in Chromium |
| [`drive_kindle_page.py`](drive_kindle_page.py) | drives `#settings_kindle` in Chromium |
| [`drive_sensors_page.py`](drive_sensors_page.py) | drives the Sensors page's remote-sensor editor in Chromium |

`drive_nodes_page.py` replaces the former `drive_espnow_page.py`: redesign 1a
merged the Battery nodes (ESP-NOW) and Remote WiFi nodes pages into one list,
transport shown as a column, with `#settings_espnow`/`#settings_remote`
retired and no redirect — so there is nothing left at the old URL to drive.

Needs `pip install playwright` and a browser. `CHROMIUM_PATH` points the driver
at an existing Chromium when one is already installed; without it Playwright
uses its own (`playwright install chromium`). `MOCK_PORT` and `SCREENSHOT` are
the other two knobs.

## What it actually checks

Rendering, and then behaviour: clicking the pairing button and watching the
badge flip, renaming a node and watching the change round-trip through the API
and re-render, forgetting one and watching it leave the list.

On the Nodes page the assertions that matter most are the two **nulls**,
carried over unchanged from the ESP-NOW-only page this replaced.
`/api/espnow/status` sends `null` for an unavailable RSSI — Arduino core 2.x
hands the receive callback no signal information at all — and for a remaining
life the battery model refuses to estimate. Both have to read as a dash. A zero
in either would be a number somebody believes. (Redesign 1a moved the
remaining-life estimate from an always-visible badge to the battery badge's
`title` — a six-column table has no room for a seventh column just for it —
so the driver checks the attribute instead of painted text, but the same
null-is-not-zero rule applies.)

On the e-ink settings page it is the **bitmasks**. Weight and visibility travel
as two integers whose bits are defined in `src/core/Config.h` and repeated in
`www/js/kindle.js`, and a page that reads a mask back correctly while writing
the neighbouring bit looks perfect and silently toggles the wrong setting. So
the driver sets bits in both directions, reads the mask back off the wire, and
compares it against the exact value the firmware's constants say it should be.
The mock also starts from a non-default configuration on purpose: a page that
renders correctly only when every value is zero has never had its select boxes
proven against anything.

## A note on the first run of this file

It reported seven failures against a page that was rendering perfectly. Five
were case: `.badge` carries `text-transform: uppercase`, so `inner_text()`
returns what is **painted** rather than what the DOM holds, and the assertions
compared against the DOM's casing. Two were 404s from routes the mock does not
serve — a plain download link in the settings hub — counted as page errors.

Both are fixed, and the episode is written down because it is the failure mode
a browser test has that a unit test does not: it can be wrong about the thing it
is looking at while the thing itself is fine, and a wall of red is exactly as
misleading as a wall of green.
