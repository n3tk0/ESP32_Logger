"""mock_device.py — serve www/ with the device's API stubbed.

Exists so the real SPA can be opened in a real browser without a real ESP32.
The pages are the one part of this project with no other way to be checked:
they are not compiled, so nothing catches a page that fetches a field the
firmware does not send, or a button wired to a handler that was never
registered in core.js's allowlist.

The stub answers what the Nodes (ESP-NOW + WiFi remote, merged in redesign
1a), sensors, and e-ink pages need. Everything else the SPA polls on boot
gets an empty object, so the page under test is not competing with a wall of
failed requests — and the routes it does NOT serve (a plain download link
like /export_settings) 404 by design; the driver ignores those.

    python3 tests/web/mock_device.py 8765
"""
import json, threading, http.server, socketserver, urllib.parse, os, sys

import pathlib
ROOT = str(pathlib.Path(__file__).resolve().parent.parent.parent / "www")

STATUS = {
    "pairing": False,
    "offline": 1,
    "warn": True,
    "nodes": [
        {"id": "outdoor", "node_id": 1, "interval": 60, "frames": 1440, "dropped": 2,
         "offline": False, "mac": "24:6F:28:01:02:03", "age_s": 42, "seen": True,
         "rssi": None, "mv": 3874, "percent": 62, "days": 237, "warn": False,
         "skew_s": 3},
        {"id": "balcony", "node_id": 2, "interval": 300, "frames": 88, "dropped": 0,
         "offline": True, "mac": "24:6F:28:0A:0B:0C", "age_s": 5400, "seen": True,
         "rssi": None, "mv": 3560, "percent": 8, "days": None, "warn": True,
         # Well past ESPNOW_SKEW_WARN_S, and negative — the node is AHEAD of
         # the collector, the direction the obvious unsigned subtraction in the
         # firmware would have turned into four billion.
         "skew_s": -184},
        {"id": "espnow-03", "node_id": 3, "interval": 60, "frames": 0, "dropped": 0,
         "offline": True, "mac": "24:6F:28:AA:BB:CC", "age_s": 0, "seen": False,
         "rssi": None, "mv": None, "percent": None, "days": None, "warn": False,
         # Never reported, so there is no measurement — which is not zero.
         "skew_s": None},
    ],
    "stats": {"frames": 1528, "malformed": 0, "unknown_node": 3, "replayed": 1,
              "ring_full": 0, "history_collapsed": 0, "history_no_clock": 2,
              "acks": 1531,
              "discover_seen": 5, "discover_bad_sig": 1, "paired": 3},
}

# GET /api/modules — the module manager's index, shaped like a real device's.
# Two kinds of `status` on purpose, because the page has to survive both:
# wifi/ota/time answer with the {text,tone} chip the UI renders directly,
# while forecast answers with its own domain object and NO `text` field at
# all — that row has to fall through to the client-side heuristic rather than
# printing "undefined" or taking the whole list down with it.
MODULES = [
    {"id": "wifi", "name": "Wi-Fi", "enabled": True, "hasUI": True,
     "description": "Station/AP connection, credentials and static-IP settings.",
     "status": {"text": "Internet · 192.168.1.214 · -19 dBm", "tone": "ok"}},
    {"id": "ota", "name": "OTA update", "enabled": True, "hasUI": True,
     "description": "Firmware updates and A/B rollback.",
     "status": {"tone": "ok", "text": "app1"}},
    {"id": "time", "name": "Time", "enabled": True, "hasUI": True,
     "description": "NTP sync, timezone and DST.",
     "status": {"text": "synced", "tone": "ok"}},
    {"id": "usbcdc", "name": "USB CDC", "enabled": False, "hasUI": False,
     "description": "USB serial-on-boot.",
     "status": {"tone": "ok", "text": "on · GPIO 18,19 locked"}},
    {"id": "forecast", "name": "Weather forecast", "enabled": True, "hasUI": True,
     "description": "Short forecast from Open-Meteo or OpenWeatherMap",
     "status": {"provider": "open-meteo", "valid": True, "failures": 0,
                "tempC": 29.2, "summary": "Променливо"}},
]

# GET /api/remote/status — the WiFi-remote half of the merged Nodes page
# (redesign 1a). No node/interval/battery fields here at all: these nodes are
# configured on their own captive portal, not from this collector, so the
# page has nothing to write back for them — read-only by construction, not
# by an editor that happens not to be wired up.
REMOTE_STATUS = {
    "nodes": [
        {"id": "greenhouse", "online": True, "age_ms": 47000,
         "metrics": [{"metric": "temperature", "value": 21.4, "unit": "°C"},
                     {"metric": "humidity", "value": 62, "unit": "%"}]},
        # Never reported: age_ms is None, which is NOT zero — same nulls
        # discipline as the ESP-NOW side's rssi/days.
        {"id": "shed-wifi", "online": False, "age_ms": None, "metrics": []},
    ],
}

# GET /api/platform_config — what the sensors page loads and rewrites.
# Deliberately mixed: a wired sensor and a remote one, because the two take
# completely different halves of the editor (pins versus a node id) and a page
# that only ever renders the wired case has never proved it can do the other.
PLATFORM = {
    "version": 1,
    "mode": "legacy",
    "sensors": [
        {"id": "env_indoor", "type": "bme280", "enabled": True,
         "interface": "i2c", "sda": 4, "scl": 0, "address": 119,
         "read_interval_ms": 10000},
        {"id": "balcony", "type": "remote", "enabled": True,
         "interface": "http", "node": "outside", "read_interval_ms": 30000},
    ],
}

# The e-ink dashboard's appearance, as GET /api/kindle/config returns it.
# Deliberately NOT the defaults: a page that renders correctly only when every
# value is zero is a page whose select boxes have never been proven to reflect
# what the device holds.
# GET /api/kindle/slots — what is in each of the eleven places, and the layout
# vocabulary the firmware defines. Deliberately NOT the defaults: a page that
# renders only when every place holds a plain temperature is a page whose
# dropdowns have never been proven to reflect what the device holds.
# What the collector's metric table renders a caption as — see kdSlotCaption()
# in src/web/KindleSlotStore.cpp. Only the metrics this fixture uses.
SHOWN = {
    "temperature": "TEMP", "humidity": "HUM", "pressure": "PRESS",
    "dew_point": "DEW", "aqi": "AQI", "pm25": "PM2.5", "co2": "CO2",
}

KINDLE_SLOTS = {
    "zones": {
        "hero": {"sensor": "balcony", "metric": "temperature", "label": "НАВЪН",
                 "shown": "НАВЪН", "flags": 7, "decimals": 255, "ink": 0},
        # A place the reader has pushed back into the mid grey.
        "big":  {"sensor": "balcony", "metric": "pressure", "label": "",
                 "shown": "PRESS", "flags": 2, "decimals": 255, "ink": 2},
        "g1":   {"sensor": "balcony", "metric": "pm25", "label": "",
                 "shown": "PM2.5", "flags": 2, "decimals": 255, "ink": 0},
        # Places the reader left empty. The editor must draw the card and mark
        # each one, not skip it: the layout is fixed places, not a list.
        "g2":   {"sensor": "", "metric": "", "label": "",
                 "shown": "", "flags": 2, "decimals": 255, "ink": 0},
        "g3":   {"sensor": "", "metric": "", "label": "",
                 "shown": "", "flags": 2, "decimals": 255, "ink": 0},
        # A sensor that is no longer configured — the editor must keep it
        # rather than silently reassigning the reader's layout.
        "g4":   {"sensor": "shed", "metric": "aqi", "label": "",
                 "shown": "AQI", "flags": 2, "decimals": 255, "ink": 1},
        "g5":   {"sensor": "", "metric": "", "label": "",
                 "shown": "", "flags": 2, "decimals": 255, "ink": 0},
        "g6":   {"sensor": "", "metric": "", "label": "",
                 "shown": "", "flags": 2, "decimals": 255, "ink": 0},
        "in1":  {"sensor": "livingroom", "metric": "temperature", "label": "",
                 "shown": "TEMP", "flags": 6, "decimals": 1, "ink": 0},
        "in2":  {"sensor": "livingroom", "metric": "humidity", "label": "",
                 "shown": "HUM", "flags": 2, "decimals": 255, "ink": 1},
        # The third indoor place empty, which is how a row of two is asked for.
        "in3":  {"sensor": "", "metric": "", "label": "",
                 "shown": "", "flags": 2, "decimals": 255, "ink": 0},
    },
    "order": [
        {"key": "hero", "group": "outdoor", "role": "hero"},
        {"key": "big",  "group": "outdoor", "role": "big"},
        {"key": "g1",   "group": "outdoor", "role": "grid"},
        {"key": "g2",   "group": "outdoor", "role": "grid"},
        {"key": "g3",   "group": "outdoor", "role": "grid"},
        {"key": "g4",   "group": "outdoor", "role": "grid"},
        {"key": "g5",   "group": "outdoor", "role": "grid"},
        {"key": "g6",   "group": "outdoor", "role": "grid"},
        {"key": "in1",  "group": "indoor",  "role": "indoor"},
        {"key": "in2",  "group": "indoor",  "role": "indoor"},
        {"key": "in3",  "group": "indoor",  "role": "indoor"},
    ],
    "inks": [{"id": 0, "css": "#000"}, {"id": 1, "css": "#444"},
             {"id": 2, "css": "#777"}, {"id": 3, "css": "#aaa"}],
    "grid_cols": 3,
    # A heading the reader has overridden, and one they have not.
    "group_out": "БАЛКОН", "group_out_set": "БАЛКОН",
    "group_in": "ВЪТРЕ",   "group_in_set": "",
    "flag_bold": 1, "flag_unit": 2, "flag_age": 4, "flag_trend": 8,
    "auto_decimals": 255,
}

SENSORS = {
    "sensors": [
        {"id": "balcony", "type": "bmp280", "name": "Balcony", "enabled": True,
         "metrics": ["temperature", "pressure"]},
        {"id": "livingroom", "type": "bme688", "name": "Living room", "enabled": True,
         "metrics": ["temperature", "humidity", "pressure", "aqi"]},
    ]
}

KINDLE = {
    "face": 4,            # Helvetica
    "face_custom": "",
    "bold": 0x0009,       # outdoor temperature + clock
    "show": 0x00FF - 0x0040,   # everything except the week strip
    "clock_style": 3,     # dated
    "time_format": 2,     # 12-hour
    "date_format": 1,     # month first
    "pressure_unit": 1,   # mmHg
    "decimals": 0,
    # Bulgarian, and deliberately not "as built": the select has three entries
    # and the one that proves it reflects the device is the one that is neither
    # the default nor zero.
    "lang": 2,
    "lang_built": 1,
    "page_w": 600,
    # The cadence and the reader, which the page now offers as named choices
    # and a panel size. Deliberately NOT the balanced preset: the control that
    # matters is the one that can show a state nobody chose from its own list.
    "refresh_sec": 180,
    "follow_data": 0,
    "clock_pin_refresh": 1,
    "fbink_res_w": 1072,
    # The page's shape. 0 is "follow the collector", which is what a device
    # that has never been asked holds — and the value the driver has to be able
    # to move away from and back to.
    "layout_mode": 0,
    "outdoor_sensor": "balcony",
    "indoor_sensor": "",
}


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, *a):
        pass

    def _read_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n).decode() or "{}")
        except ValueError:
            return None

    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/kindle/slots":
            return self._json(KINDLE_SLOTS)
        if path == "/api/sensors":
            return self._json(SENSORS)
        if path == "/api/espnow/status":
            return self._json(STATUS)
        if path == "/api/remote/status":
            return self._json(REMOTE_STATUS)
        if path == "/api/kindle/config":
            return self._json(KINDLE)
        if path == "/api/csrf-token":
            return self._json({"token": "test-token"})
        if path == "/api/platform_config":
            return self._json(PLATFORM)
        # The index must be tested BEFORE the detail prefix, or "/api/modules"
        # falls into the startswith below and answers with one module.
        if path == "/api/modules":
            return self._json(MODULES)
        if path.startswith("/api/modules/"):
            mid = path[len("/api/modules/"):]
            m = next((x for x in MODULES if x["id"] == mid), None)
            if m is None:
                return self._json({"ok": False, "error": "unknown module"}, 404)
            # A detail response is the index entry plus its form; only the
            # modules the page can configure carry a schema.
            return self._json(dict(m, config={"enabled": m["enabled"]},
                                   schema=([{"key": "enabled", "type": "bool",
                                             "label": "Module enabled"}]
                                           if m["hasUI"] else [])))
        # Everything else the SPA polls on boot — answered emptily so the page
        # under test is not competing with a wall of failed requests.
        if path.startswith("/api/") or path in ("/status", "/wifi_scan_result"):
            return self._json({})
        if path == "/":
            self.path = "/index.html"
        return super().do_GET()

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        # The sensors page sends the WHOLE platform config as a JSON document,
        # unlike every form on the other pages. Read it before the form parse
        # below, which would turn it into one nonsense key.
        if path == "/api/kindle/slots":
            # THE OTHER HALF OF ONE SAVE. The page writes the appearance and
            # then the places, and reports the count this answers with — so a
            # mock that swallowed the document would let a page that never
            # sent the places look like one that did.
            doc = self._read_json()
            if doc is None or "zones" not in doc:
                return self._json({"ok": False, "error": "bad json"}, 400)
            # `shown` IS THE DEVICE'S, NOT THE FORM'S. The collector derives
            # the caption a place will render from its metric table and sends
            # it down on every read; the form does not send it back, because
            # the firmware ignores it and it is a quarter of the payload. A
            # mock that stored the posted document verbatim therefore dropped
            # it — and the page's placeholder, which is that value, came back
            # empty after the first save. Derived here, as the firmware does.
            for key, z in doc["zones"].items():
                z["shown"] = SHOWN.get(z.get("metric", ""), "")
            KINDLE_SLOTS["zones"] = doc["zones"]
            KINDLE_SLOTS["group_out_set"] = doc.get("group_out", "")
            KINDLE_SLOTS["group_in_set"] = doc.get("group_in", "")
            n = sum(1 for z in doc["zones"].values()
                    if z.get("sensor") and z.get("metric"))
            return self._json({"ok": True, "count": n})
        if path == "/save_platform":
            doc = self._read_json()
            if doc is None:
                return self._json({"ok": False, "error": "bad json"}, 400)
            PLATFORM.clear()
            PLATFORM.update(doc)
            return self._json({"ok": True})
        n = int(self.headers.get("Content-Length") or 0)
        body = urllib.parse.parse_qs(self.rfile.read(n).decode())
        if path == "/api/espnow/pair":
            STATUS["pairing"] = True
            return self._json({"ok": True, "seconds": int(body.get("seconds", ["120"])[0])})
        if path == "/api/espnow/node":
            for node in STATUS["nodes"]:
                if str(node["node_id"]) == body.get("node_id", [""])[0]:
                    if body.get("label"): node["id"] = body["label"][0]
                    if body.get("interval"): node["interval"] = int(body["interval"][0])
            return self._json({"ok": True})
        if path == "/api/kindle/config":
            # Stored back, so the driver can assert that what it set is what a
            # re-read returns — the round trip is the thing worth proving,
            # since the page rebuilds itself from the GET after every save.
            for k in ("face", "bold", "show", "clock_style", "time_format",
                      "date_format", "pressure_unit", "decimals", "lang",
                      "refresh_sec", "follow_data", "clock_pin_refresh",
                      "fbink_res_w", "layout_mode"):
                if k in body:
                    KINDLE[k] = int(body[k][0])
            for k in ("face_custom", "outdoor_sensor", "indoor_sensor"):
                if k in body:
                    KINDLE[k] = body[k][0]
            return self._json({"ok": True})
        if path == "/api/espnow/forget":
            STATUS["nodes"] = [x for x in STATUS["nodes"]
                               if str(x["node_id"]) != body.get("node_id", [""])[0]]
            return self._json({"ok": True})
        return self._json({"ok": True})

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", port), H) as httpd:
        httpd.serve_forever()
