"""mock_device.py — serve www/ with the device's API stubbed.

Exists so the real SPA can be opened in a real browser without a real ESP32.
The pages are the one part of this project with no other way to be checked:
they are not compiled, so nothing catches a page that fetches a field the
firmware does not send, or a button wired to a handler that was never
registered in core.js's allowlist.

The stub answers only what the ESP-NOW page needs. Everything else the SPA
polls on boot gets an empty object, so the page under test is not competing
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
         "rssi": None, "mv": 3874, "percent": 62, "days": 237, "warn": False},
        {"id": "balcony", "node_id": 2, "interval": 300, "frames": 88, "dropped": 0,
         "offline": True, "mac": "24:6F:28:0A:0B:0C", "age_s": 5400, "seen": True,
         "rssi": None, "mv": 3560, "percent": 8, "days": None, "warn": True},
        {"id": "espnow-03", "node_id": 3, "interval": 60, "frames": 0, "dropped": 0,
         "offline": True, "mac": "24:6F:28:AA:BB:CC", "age_s": 0, "seen": False,
         "rssi": None, "mv": None, "percent": None, "days": None, "warn": False},
    ],
    "stats": {"frames": 1528, "malformed": 0, "unknown_node": 3, "replayed": 1,
              "ring_full": 0, "history_collapsed": 0, "acks": 1531,
              "discover_seen": 5, "discover_bad_sig": 1, "paired": 3},
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
        if path == "/api/espnow/status":
            return self._json(STATUS)
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
