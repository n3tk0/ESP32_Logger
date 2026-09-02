"""mock_device.py — serve www/ with the device's API stubbed.

Exists so the real SPA can be opened in a real browser without a real ESP32.
The pages are the one part of this project with no other way to be checked:
they are not compiled, so nothing catches a page that fetches a field the
firmware does not send, or a button wired to a handler that was never
registered in core.js's allowlist.

The stub answers what the ESP-NOW and e-ink pages need. Everything else the
SPA polls on boot gets an empty object, so the page under test is not competing
with a wall of failed requests — and the routes it does NOT serve (a plain
download link like /export_settings) 404 by design; the driver ignores those.

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

# The e-ink dashboard's appearance, as GET /api/kindle/config returns it.
# Deliberately NOT the defaults: a page that renders correctly only when every
# value is zero is a page whose select boxes have never been proven to reflect
# what the device holds.
# GET /api/kindle/slots — what is in each of the eleven places, and the layout
# vocabulary the firmware defines. Deliberately NOT the defaults: a page that
# renders only when every place holds a plain temperature is a page whose
# dropdowns have never been proven to reflect what the device holds.
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
    "page_w": 600,
}


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, *a):
        pass

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
        if path == "/api/kindle/config":
            return self._json(KINDLE)
        if path == "/api/csrf-token":
            return self._json({"token": "test-token"})
        # Everything else the SPA polls on boot — answered emptily so the page
        # under test is not competing with a wall of failed requests.
        if path.startswith("/api/") or path in ("/status", "/wifi_scan_result"):
            return self._json({})
        if path == "/":
            self.path = "/index.html"
        return super().do_GET()

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
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
                      "date_format", "pressure_unit", "decimals"):
                if k in body:
                    KINDLE[k] = int(body[k][0])
            if "face_custom" in body:
                KINDLE["face_custom"] = body["face_custom"][0]
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
