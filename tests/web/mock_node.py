"""mock_node.py — a sensor node's setup API, for driving node_portal/ in a browser.

Implements docs/NODE_CONFIG.md §6 for either node:

    python3 tests/web/mock_node.py 8790                   # WiFi node (ESP8266, NodeMCU)
    python3 tests/web/mock_node.py 8790 --transport espnow  # ESP-NOW node (XIAO ESP32-C3)
    python3 tests/web/mock_node.py 8790 --sources         # build the page from node_portal/
                                                          # instead of serving the header

`GET /?transport=wifi|espnow` resets the mock to that node's factory state
before serving the page, so one mock serves both drivers' runs.

By default `/` is the page EXACTLY as the firmware links it: the gzip bytes
out of src/nodecfg/NodePortalPage.h, sent with Content-Encoding: gzip. A
driver that passes against that has tested what ships.
(tools/check_node_portal_css.py is what guarantees the header is current.)

Behaviour worth knowing when reading a driver:
  * GET never returns a secret: `pass`, `token`, `basic_pass`, `lmk` are ""
    plus `<field>_set`. A POSTed "" keeps the stored value; "" with
    `<field>_set: false` clears it.
  * POST /api/config validates a few of the shared rules the real node
    enforces (forbidden pin, pin used twice, metric budget, name, interval,
    sleep-unsafe sensor, LMK length) and answers 400
    {"ok":false,"field","reason"} with the contract's field spelling.
  * A good POST "restarts" the node: /api/status answers 503 for
    RESTART_S seconds, then comes back with a fresh uptime.
  * Test hooks, not part of §6:  GET /__last  (the last POSTed body),
    GET /__state (the stored config, secrets included).
"""
import copy
import gzip
import json
import pathlib
import re
import sys
import threading
import time
import http.server
import socketserver
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))
RESTART_S = 3.0

WARN_8266 = {
    "0": "boot strap: must be HIGH at reset (an I2C pull-up satisfies it)",
    "1": "UART0 TX — the serial log comes out here",
    "2": "boot strap: must be HIGH at reset (an I2C pull-up satisfies it)",
    "3": "UART0 RX — the serial console",
    "15": "boot strap: must be LOW at reset — a pull-up here stops the board booting",
    "16": "no interrupt and no internal pull-up (deep-sleep wake pin)",
}
D_8266 = {"D0": 16, "D1": 5, "D2": 4, "D3": 0, "D4": 2, "D5": 14, "D6": 12, "D7": 13, "D8": 15,
          "RX": 3, "TX": 1, "SD0": 7, "SD1": 8, "SD2": 9, "SD3": 10, "CMD": 11, "CLK": 6}
CAPS_WIFI = {
    "transport": "wifi", "hw": "esp8266",
    "sensor_types": ["bmx280", "bme688", "ds18b20", "bh1750", "sds011", "pulse"],
    "boards": [
        {"id": 0, "name": "NodeMCU V2/V3", "pins": D_8266,
         "left": "A0,RSV,RSV,SD3,SD2,SD1,CMD,SD0,CLK,GND,3V3,EN,RST,GND,VIN".split(","),
         "right": "D0,D1,D2,D3,D4,3V3,GND,D5,D6,D7,D8,RX,TX,GND,3V3".split(",")},
        {"id": 1, "name": "Wemos D1 mini", "pins": D_8266,
         "left": "RST,A0,D0,D5,D6,D7,D8,3V3".split(","),
         "right": "TX,RX,D1,D2,D3,D4,GND,5V".split(",")},
        {"id": 2, "name": "Bare ESP-12 / other",
         "pins": {"GPIO%d" % g: g for g in (16, 14, 12, 13, 15, 2, 0, 4, 5, 3, 1)}},
    ],
    "forbidden_pins": [6, 7, 8, 9, 10, 11],
    "warn_pins": WARN_8266,
    "max_sensors": 8, "max_metrics": 8,
}
XIAO = {"D0": 2, "D1": 3, "D2": 4, "D3": 5, "D4": 6, "D5": 7, "D6": 21, "D7": 20,
        "D8": 8, "D9": 9, "D10": 10, "A0": 2, "A1": 3, "A2": 4}
CAPS_ESPNOW = {
    "transport": "espnow", "hw": "esp32c3",
    "sensor_types": ["bmx280", "bme688", "ds18b20", "bh1750", "sds011", "pulse"],
    "boards": [
        {"id": 0, "name": "XIAO ESP32-C3", "pins": XIAO,
         "left": "D0,D1,D2,D3,D4,D5,D6".split(","),
         "right": "5V,GND,3V3,D10,D9,D8,D7".split(",")},
        {"id": 1, "name": "C3 SuperMini",
         "pins": {str(g): g for g in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21)},
         "left": "5V,GND,3V3,4,3,2,1,0".split(","),
         "right": "5,6,7,8,9,10,20,21".split(",")},
        {"id": 2, "name": "Other ESP32-C3",
         "pins": {"GPIO%d" % g: g for g in list(range(0, 11)) + [18, 19, 20, 21]}},
    ],
    "forbidden_pins": [12, 13, 14, 15, 16, 17],
    "warn_pins": {
        "2": "boot strap: keep it floating or high at reset",
        "8": "boot strap: must be HIGH at reset",
        "9": "boot strap (BOOT button): must be HIGH at reset",
        "18": "USB D- — the USB serial/JTAG uses it",
        "19": "USB D+ — the USB serial/JTAG uses it",
        "20": "UART0 RX — the serial console",
        "21": "UART0 TX — the serial log comes out here",
    },
    "max_sensors": 8, "max_metrics": 8,
}
CAPS = {"wifi": CAPS_WIFI, "espnow": CAPS_ESPNOW}

FACTORY = {
    "wifi": {
        "rev": 4, "local": False, "transport": "wifi", "hw": "esp8266", "fw": "2026.09.1",
        "name": "balcony", "interval_s": 60, "altitude_m": 0.0, "board": 0,
        "i2c": {"sda": 4, "scl": 5},
        "sensors": [{"type": "bmx280", "addr": 0x76}],
        "net": {"ssid": "home", "pass": "hunter22", "host": "192.168.1.50", "port": 80,
                "token": "s3cret-token", "basic_user": "", "basic_pass": "",
                "next": {"ssid": "home-5g", "pass": "newpass99"}},
    },
    "espnow": {
        "rev": 2, "local": False, "transport": "espnow", "hw": "esp32c3", "fw": "2026.09.1",
        "name": "garden", "interval_s": 300, "altitude_m": 0.0, "board": 0, "sleep": True,
        "i2c": {"sda": 6, "scl": 7},
        "sensors": [{"type": "bmx280", "addr": 0}],
        "link": {"ack_window_ms": 30, "rescan_fails": 3, "rescan_min_s": 3600, "next_ssid": "home-5g"},
        "batt": {"pin": 2, "divider": 2.0, "trim": 1.0},
        "lmk": "0123456789abcdef",
    },
}
SECRETS = {"wifi": ["net.pass", "net.token", "net.basic_pass", "net.next.pass"], "espnow": ["lmk"]}

NETS = [
    {"ssid": "home", "rssi": -48, "ch": 6, "enc": 3},
    {"ssid": "home", "rssi": -71, "ch": 11, "enc": 3},     # a second AP, same SSID: one row
    {"ssid": "home-5g", "rssi": -60, "ch": 1, "enc": 3},
    {"ssid": "cafe guest", "rssi": -82, "ch": 1, "enc": 7},  # ENC_TYPE_NONE on the ESP8266
    {"ssid": "", "rssi": -80, "ch": 3, "enc": 3},            # hidden: never listed
    # An SSID is attacker-controlled text off the air. It must render as text.
    {"ssid": "<img src=x onerror=window.__pwned=1>", "rssi": -90, "ch": 9, "enc": 3},
]

METRICS = {"bmx280": 4, "bme688": 5, "bh1750": 1, "sds011": 2, "pulse": 2}
PIN_KEYS = {"ds18b20": ["pin"], "sds011": ["rx", "tx"], "pulse": ["pin"]}


class Node:
    def __init__(self, transport):
        self.lock = threading.Lock()
        self.reset(transport)

    def reset(self, transport):
        self.transport = transport
        self.cfg = copy.deepcopy(FACTORY[transport])
        self.boot = time.time() - 812
        self.down_until = 0.0
        self.scan_calls = 0
        self.last_post = None

    # ── views ────────────────────────────────────────────────────────────
    def public(self):
        c = copy.deepcopy(self.cfg)
        for path in SECRETS[self.transport]:
            parts = path.split(".")
            o = c
            for p in parts[:-1]:
                o = o[p]
            o[parts[-1] + "_set"] = bool(o.get(parts[-1]))
            o[parts[-1]] = ""
        return c

    def status(self):
        s = {"uptime_s": int(time.time() - self.boot), "rssi": -61, "last_ok_s": 14,
             "ip": "192.168.4.1", "mac": "5C:CF:7F:01:02:03", "rev": self.cfg["rev"],
             "local": self.cfg["local"], "collector": "reachable"}
        if self.transport == "espnow":
            s.update({"mac": "34:85:18:0A:0B:0C", "paired": True, "node_id": 3, "ch": 6,
                      "batt_v": 3.87, "rssi": None, "collector": "unknown"})
        return s

    # ── the validator (a few of §1.2's rules) ────────────────────────────
    def validate(self, c):
        caps = CAPS[self.transport]
        forb = set(caps["forbidden_pins"])
        name = c.get("name", "")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,16}", name or ""):
            return "name", "1-16 characters, [A-Za-z0-9_-]"
        iv = c.get("interval_s")
        if not isinstance(iv, int) or not 10 <= iv <= 65535:
            return "interval_s", "interval_s must be 10..65535"
        sensors = c.get("sensors", [])
        if len(sensors) > caps["max_sensors"]:
            return "sensors", "more than 8 sensors"
        uses = []
        if any(s.get("type") in ("bmx280", "bme688", "bh1750") for s in sensors):
            uses += [("i2c.sda", c["i2c"]["sda"]), ("i2c.scl", c["i2c"]["scl"])]
        total = 0
        for i, s in enumerate(sensors):
            t = s.get("type")
            if t not in caps["sensor_types"]:
                return "sensors[%d].type" % i, "unknown sensor type"
            if t == "ds18b20":
                # The node refuses a count that is not a whole number of probes
                # (NodeConfigValidate.h); a string or null must be a 400 here
                # too, not a TypeError that drops the connection unanswered.
                k = s.get("count", 1)
                if isinstance(k, bool) or not isinstance(k, int) or not 1 <= k <= 8:
                    return "sensors[%d].count" % i, "must be 1..8 probes"
                total += k
            else:
                total += METRICS[t]
            if self.transport == "espnow" and c.get("sleep") and t in ("sds011", "pulse"):
                return "sensors[%d].type" % i, "%s needs the node awake (sleep is on)" % t
            for k in PIN_KEYS.get(t, []):
                uses.append(("sensors[%d].%s" % (i, k), s.get(k)))
        if self.transport == "espnow":
            uses.append(("batt.pin", c["batt"]["pin"]))
        seen = {}
        for f, g in uses:
            if not isinstance(g, int) or g < 0:
                return f, "not a pin"
            if g in forb:
                return f, "GPIO%d is the SPI flash bus" % g
            if g in seen:
                return f, "GPIO%d is already used by %s" % (g, seen[g])
            seen[g] = f
        if total > caps["max_metrics"]:
            return "sensors", "%d metrics, the collector takes at most %d" % (total, caps["max_metrics"])
        if self.transport == "espnow" and c.get("lmk") and len(c["lmk"]) != 16:
            return "lmk", "the key must be exactly 16 characters"
        return None

    def apply(self, body):
        merged = copy.deepcopy(self.cfg)

        def merge(dst, src):
            for k, v in src.items():
                if isinstance(v, dict) and isinstance(dst.get(k), dict):
                    merge(dst[k], v)
                else:
                    dst[k] = v
        # Read-only keys are the node's to say, not the page's.
        body = {k: v for k, v in body.items() if k not in ("rev", "transport", "hw", "fw", "local")}
        merge(merged, body)
        for path in SECRETS[self.transport]:       # "" = keep, "" + _set:false = clear
            parts = path.split(".")
            new, old = merged, self.cfg
            for p in parts[:-1]:
                new, old = new.get(p, {}), old.get(p, {})
            if new.get(parts[-1], "") == "":
                clear = new.get(parts[-1] + "_set") is False
                new[parts[-1]] = "" if clear else old.get(parts[-1], "")
            new.pop(parts[-1] + "_set", None)
        err = self.validate(merged)
        if err:
            return err
        merged["local"] = True
        self.cfg = merged
        now = time.time()
        self.down_until = now + RESTART_S
        self.boot = self.down_until
        return None


NODE = None
PAGE = b""


def load_page(sources):
    if sources:
        import build_node_portal
        return gzip.compress(build_node_portal.build_html(), mtime=0)
    import build_node_portal
    gz = build_node_portal.committed_payload()
    if not gz:
        sys.exit("mock_node: src/nodecfg/NodePortalPage.h missing — run tools/build_node_portal.py")
    return gz


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json", extra=None):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def down(self):
        return time.time() < NODE.down_until

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        with NODE.lock:
            if u.path == "/":
                if "transport" in q and q["transport"][0] in CAPS:
                    NODE.reset(q["transport"][0])
                return self.send(200, PAGE, "text/html; charset=utf-8", {"Content-Encoding": "gzip"})
            if self.down():
                return self.send(503, {"error": "restarting"})
            if u.path == "/api/config":
                return self.send(200, {"config": NODE.public(), "caps": CAPS[NODE.transport]})
            if u.path == "/api/status":
                return self.send(200, NODE.status())
            if u.path == "/api/scan":
                NODE.scan_calls += 1
                if NODE.scan_calls % 2 == 1:
                    return self.send(200, {"state": "running"})
                return self.send(200, {"state": "done", "nets": NETS})
            if u.path == "/__last":
                return self.send(200, NODE.last_post)
            if u.path == "/__state":
                return self.send(200, NODE.cfg)
        return self.send(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n)
        with NODE.lock:
            if self.down():
                return self.send(503, {"error": "restarting"})
            if self.path != "/api/config":
                return self.send(404, {"error": "not found"})
            try:
                body = json.loads(raw)
                assert isinstance(body, dict)
            except Exception:
                return self.send(400, {"ok": False, "field": "", "reason": "not a JSON object"})
            NODE.last_post = body
            err = NODE.apply(body)
            if err:
                return self.send(400, {"ok": False, "field": err[0], "reason": err[1]})
            return self.send(200, {"ok": True})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global NODE, PAGE
    args = sys.argv[1:]
    port = int(args[0]) if args and args[0].isdigit() else 8790
    transport = "wifi"
    if "--transport" in args:
        transport = args[args.index("--transport") + 1]
    NODE = Node(transport)
    PAGE = load_page("--sources" in args)
    print("mock node (%s) on http://127.0.0.1:%d/" % (transport, port), flush=True)
    Server(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    main()
