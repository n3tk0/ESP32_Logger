"""mock_device.py — serve www/ with the device's API stubbed.

Exists so the real SPA can be opened in a real browser without a real ESP32.
The pages are the one part of this project with no other way to be checked:
they are not compiled, so nothing catches a page that fetches a field the
firmware does not send, or a button wired to a handler that was never
registered in core.js's allowlist.

The stub answers what the Nodes (ESP-NOW + WiFi remote, merged in redesign
1a — with each node's settings and the network handover of
docs/NODE_CONFIG.md §7), sensors, and e-ink pages need. Everything else the SPA polls on boot
gets an empty object, so the page under test is not competing with a wall of
failed requests — and the routes it does NOT serve (a plain download link
like /export_settings) 404 by design; the driver ignores those.

    python3 tests/web/mock_device.py 8765
"""
import json, threading, http.server, socketserver, urllib.parse, os, sys, time

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
                "tempC": 29.2, "summary": "Променливо",
                # An hour old, so the panel has an age to print, and the
                # refresh URL a build with the Kindle dashboard adds.
                "fetchedAt": int(time.time()) - 3600, "pending": False,
                "refresh": "/kindle/forecast"}},
]

# GET /api/modules/:id → config + schema. The schema is a STRING of
# {"fields":[…]} because that is what the firmware sends: each module returns
# a PROGMEM JSON literal from schema() and ModuleRegistry::toDetailJson
# assigns it straight through, so the page does JSON.parse on it. Copied in
# shape from TIME_SCHEMA (src/modules/TimeModule.cpp), including the showIf
# key, which is the one field rule the form evaluates rather than renders.
MODULE_SCHEMA = {
    "time": json.dumps({"fields": [
        {"id": "ntpServer", "type": "string", "max": 64, "label": "NTP server",
         "group": "NTP", "help": "Hostname queried at boot."},
        {"id": "timezone", "type": "int", "min": -12, "max": 14,
         "label": "Timezone", "unit": "h"},
        {"id": "dstOffsetHours", "type": "int", "min": 0, "max": 2,
         "label": "DST offset", "unit": "h"},
    ]}),
    "forecast": json.dumps({"fields": [
        {"id": "lat", "type": "float", "label": "Latitude"},
        {"id": "lon", "type": "float", "label": "Longitude"},
    ]}),
    "wifi": json.dumps({"fields": [
        {"id": "ssid", "type": "string", "max": 32, "label": "SSID"},
        {"id": "useStatic", "type": "bool", "label": "Use a static IP"},
        {"id": "ip", "type": "ipv4", "label": "IP address",
         "showIf": {"useStatic": True}},
    ]}),
}

# `polls`: how many status reads after "ok" still say pending, so the page is
# seen to wait for the fetch rather than for a fixed time. `offline`: answer
# as a collector with no station link does.
FORECAST = {"asked": False, "polls": 0, "left": 0, "offline": False}

MODULE_CONFIG = {
    "forecast": {"provider": "open-meteo", "lat": 42.7, "lon": 23.3, "interval_min": 30},
    "time": {"ntpServer": "pool.ntp.org", "timezone": 2, "dstOffsetHours": 1},
    "wifi": {"ssid": "MonkeyNet", "useStatic": False, "ip": "192.168.1.214"},
}

# GET /api/remote/status — the WiFi-remote half of the merged Nodes page
# (redesign 1a). No node/interval/battery fields here: a WiFi node's settings
# live in NODE_CFG below and are served through /api/nodes/config; this list
# only gains the per-node `cfg` summary (§7), added when it is served.
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

# GET /api/board-profiles — built from src/core/BoardProfiles.cpp itself, not
# typed out here, so the pin pages are always driven against the lists the
# firmware really has. MOCK_BOARD picks the active profile (default xiao_c3).
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent.parent / "tools"))
import check_boards_json as _cbj  # noqa: E402

def _board_profiles():
    import re
    text = re.sub(r"//[^\n]*", "", open(_cbj.PROFILES_CPP, encoding="utf-8").read())
    pat = re.compile(r"constexpr\s+BoardProfile\s+\w+\s*=\s*\{\s*\w+\s*,\s*\"([^\"]*)\"\s*,"
                     r"\s*\"(\w+)\"\s*,\s*(\d+)\s*,((?:\s*\{[^}]*\}\s*,?)+)\s*\};")
    out = []
    for m in pat.finditer(text):
        lists = [[int(x) for x in re.findall(r"\b\d+\b", l)] for l in re.findall(r"\{([^}]*)\}", m.group(4))]
        out.append({"id": m.group(2), "name": m.group(1).replace("\\xE2\\x80\\x94", "\u2014"),
                    "maxGpio": int(m.group(3)), "strapPins": lists[0], "usbPins": lists[1],
                    "flashPins": lists[2], "reservedPins": lists[3], "absentPins": lists[4]})
    return out

BOARD = {"active": os.environ.get("MOCK_BOARD", "xiao_c3"), "setupRequired": False}
# /__mock/profiles?fail=1 makes /api/board-profiles answer 503 until
# ?fail=0: the dropped request the Hardware page must survive.
PROFILES_FAIL = {"on": False}

# GET /export_settings → hardware; POST /save_hardware and /api/firstrun
# store what they are sent, and /__mock/hw hands it back to the driver.
# 255 is PIN_UNSET, exactly as the firmware exports an unassigned pin.
HARDWARE = {"storageType": 1, "wakeupMode": 0, "debounceMs": 100, "cpuFreqMHz": 80,
            "pinWifiTrigger": 9, "pinWakeupFF": 255, "pinWakeupPF": 255,
            "pinFlowSensor": 21, "pinRtcCE": 255, "pinRtcIO": 255, "pinRtcSCLK": 255,
            "pinSdCS": 10, "pinSdMOSI": 5, "pinSdMISO": 3, "pinSdSCK": 255}
HW_POSTS = []

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


# ── Node configuration (docs/NODE_CONFIG.md §7) ─────────────────────────────
#
# What the collector holds per node: the DESIRED config (with secrets — they
# are blanked on the way out, never stored blank), the rev the node last said
# it applied, what the node last reported, and whether it took the last rev.
# Deliberately one node in each state the page has to draw: applied, pending,
# rejected (with a reason that names a field), an ESP-NOW node that has never
# reported, and a WiFi node the collector has no config for at all.

BOARD_PINS_8266 = {"D0": 16, "D1": 5, "D2": 4, "D3": 0, "D4": 2, "D5": 14,
                   "D6": 12, "D7": 13, "D8": 15, "RX": 3, "TX": 1}
CAPS = {
    "esp8266": {
        "transport": "wifi", "hw": "esp8266",
        "sensor_types": ["bmx280", "bme688", "ds18b20", "bh1750", "sds011", "pulse"],
        "boards": [{"id": 0, "name": "NodeMCU V2/V3", "pins": BOARD_PINS_8266},
                   {"id": 1, "name": "Wemos D1 mini", "pins": BOARD_PINS_8266},
                   {"id": 2, "name": "ESP-12 module", "pins": {}}],
        "forbidden_pins": [6, 7, 8, 9, 10, 11],
        "warn_pins": {"0": "boot strap, must be high at reset",
                      "2": "boot strap, must be high at reset",
                      "15": "boot strap, must be low at reset",
                      "1": "serial console TX", "3": "serial console RX",
                      "16": "no interrupt, no pull-up"},
        "max_sensors": 8, "max_metrics": 8,
    },
    "esp32c3": {
        "transport": "espnow", "hw": "esp32c3",
        "sensor_types": ["bmx280", "bme688", "ds18b20", "bh1750", "sds011", "pulse"],
        "boards": [{"id": 0, "name": "Seeed XIAO ESP32-C3",
                    "pins": {"D0": 2, "D1": 3, "D2": 4, "D3": 5, "D4": 6, "D5": 7,
                             "D6": 21, "D7": 20, "D8": 8, "D9": 9, "D10": 10}},
                   {"id": 1, "name": "ESP32-C3 SuperMini", "pins": {}},
                   {"id": 2, "name": "Other ESP32-C3", "pins": {}}],
        "forbidden_pins": [12, 13, 14, 15, 16, 17],
        "warn_pins": {"2": "boot strap", "8": "boot strap", "9": "boot strap (BOOT button)",
                      "18": "USB D-", "19": "USB D+", "20": "serial console RX",
                      "21": "serial console TX"},
        "max_sensors": 8, "max_metrics": 8,
    },
}


def _en_doc(rev, name, sensors, **kw):
    d = {"rev": rev, "local": False, "transport": "espnow", "hw": "esp32c3",
         "fw": "2026.09.1", "name": name, "interval_s": 60, "altitude_m": 0.0,
         "board": 0, "sleep": True, "i2c": {"sda": 6, "scl": 7},
         "sensors": sensors,
         "link": {"ack_window_ms": 30, "rescan_fails": 3, "rescan_min_s": 3600,
                  "next_ssid": ""},
         "batt": {"pin": 2, "divider": 2.0, "trim": 1.0}}
    d.update(kw)
    return d


def _wifi_doc(rev, name, sensors):
    return {"rev": rev, "local": False, "transport": "wifi", "hw": "esp8266",
            "fw": "2026.09.1", "name": name, "interval_s": 60, "altitude_m": 312.0,
            "board": 0, "i2c": {"sda": 4, "scl": 5}, "sensors": sensors,
            "net": {"ssid": "MonkeyNet", "pass": "hunter22", "host": "192.168.1.214",
                    "port": 80, "token": "s3cret-token", "basic_user": "admin",
                    "basic_pass": "", "next": {"ssid": "", "pass": ""}}}


NODE_CFG = {
    "e:1": {"desired": _en_doc(4, "outdoor", [{"type": "bmx280", "addr": 0x76}]),
            "applied_rev": 4, "status": "applied", "error": None},
    # Refused: rev 5 put a probe on GPIO12, which is the C3's flash bus.
    "e:2": {"desired": _en_doc(5, "balcony", [{"type": "bmx280", "addr": 0},
                                              {"type": "ds18b20", "pin": 12, "count": 1,
                                               "metric": "probe_temp"}], interval_s=300),
            "applied_rev": 4, "status": "rejected",
            "error": {"field": "sensors[1].pin", "reason": "GPIO12 is the flash bus"}},
    # Paired, never reported: the collector's desired config is all there is.
    "e:3": {"desired": _en_doc(1, "espnow-03", []), "applied_rev": 0,
            "status": "pending", "error": None, "never_reported": True},
    "w:greenhouse": {"desired": _wifi_doc(3, "greenhouse", [
                         {"type": "bme688", "addr": 0x77},
                         {"type": "ds18b20", "pin": 14, "count": 2, "metric": "probe_temp"}]),
                     "applied_rev": 2, "status": "pending", "error": None},
    # w:shed-wifi: no entry at all — never reported, so nothing to edit.
}
for _k, _v in NODE_CFG.items():
    _rep = json.loads(json.dumps(_v["desired"]))
    _rep["rev"] = _v["applied_rev"]
    _v["reported"] = None if _v.get("never_reported") else _rep

SECRET_PATHS = {("net", "pass"), ("net", "token"), ("net", "basic_pass"), ("next", "pass")}
READONLY = {"rev", "local", "transport", "hw", "fw"}


def _blank_secrets(doc):
    """§0.5: a GET never returns a secret, only whether one is stored."""
    if not doc:
        return doc
    out = json.loads(json.dumps(doc))
    for parent in (out.get("net"), (out.get("net") or {}).get("next")):
        if not parent:
            continue
        for k in ("pass", "token", "basic_pass"):
            if k in parent:
                parent[k + "_set"] = bool(parent[k])
                parent[k] = ""
    return out


def _cfg_summary(key):
    c = NODE_CFG.get(key)
    if not c:
        return None
    s = {"key": key, "rev": c["desired"]["rev"], "applied_rev": c["applied_rev"],
         "status": c["status"]}
    if c["status"] == "rejected" and c["error"]:
        s["error"] = c["error"]
    return s


def _metric_count(sensors):
    per = {"bmx280": 4, "bme688": 5, "bh1750": 1, "sds011": 2, "pulse": 2}
    return sum(max(1, int(s.get("count") or 1)) if s.get("type") == "ds18b20"
               else per.get(s.get("type"), 0) for s in sensors)


def _validate(doc, caps):
    """A few of §1.2's rules, enough for the page to be shown each kind of
    refusal: a name, a range, a pin, the budget, sleep-unsafe sensors."""
    import re
    if not re.match(r"^[A-Za-z0-9_-]{1,16}$", str(doc.get("name", ""))):
        return "name", "1 to 16 letters, digits, - or _"
    iv = doc.get("interval_s")
    if not isinstance(iv, int) or not 10 <= iv <= 65535:
        return "interval_s", "must be 10..65535 seconds"
    sensors = doc.get("sensors") or []
    if len(sensors) > caps["max_sensors"]:
        return "sensors", "at most %d sensors" % caps["max_sensors"]
    if _metric_count(sensors) > caps["max_metrics"]:
        return "sensors", "%d metrics, the node can send %d" % (
            _metric_count(sensors), caps["max_metrics"])
    pins = []
    if any(s.get("type") in ("bmx280", "bme688", "bh1750") for s in sensors):
        pins += [("i2c.sda", doc["i2c"].get("sda")), ("i2c.scl", doc["i2c"].get("scl"))]
    for i, s in enumerate(sensors):
        if doc.get("transport") == "espnow" and doc.get("sleep") and s.get("type") in ("sds011", "pulse"):
            return "sensors[%d].type" % i, "%s needs the node awake (sleep is on)" % s["type"]
        for f in ("pin", "rx", "tx"):
            if f in s:
                pins.append(("sensors[%d].%s" % (i, f), s[f]))
    if "batt" in doc:
        pins.append(("batt.pin", doc["batt"].get("pin")))
    seen = {}
    for field, g in pins:
        if g is None:
            return field, "a pin is required"
        if g in caps["forbidden_pins"]:
            return field, "GPIO%d is the flash bus" % g
        if g in seen and not field.startswith("i2c"):
            return field, "GPIO%d is already used by %s" % (g, seen[g])
        seen[g] = field
    return None


def _merge(dst, src, path=()):
    """Partial document semantics (§1): missing keys keep their value,
    objects merge, lists replace, an empty secret keeps the stored one."""
    for k, v in src.items():
        if not path and k in READONLY:
            continue
        if k.endswith("_set") or (path == ("net",) and k == "next") or (path == ("link",) and k == "next_ssid"):
            continue
        if (path[-1:] + (k,)) in SECRET_PATHS and v == "":
            continue
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _merge(dst[k], v, path + (k,))
        else:
            dst[k] = v


def _status_with_cfg(payload, prefix, idkey):
    out = json.loads(json.dumps(payload))
    for n in out.get("nodes", []):
        s = _cfg_summary(prefix + str(n[idkey]))
        if s:
            n["cfg"] = s
    return out


# ── Network handover (docs/NODE_CONFIG.md §4) ───────────────────────────────
# start → every online node pending; each poll after the first promotes one
# pending node to ready; one poll after all are ready the collector switches
# by itself (active goes false), or on "switch"; "cancel" clears it.
HANDOVER = {"active": False}


def _node_keys():
    keys = []
    for n in STATUS["nodes"]:
        keys.append(("e:%d" % n["node_id"], not n.get("offline")))
    for n in REMOTE_STATUS["nodes"]:
        keys.append(("w:" + n["id"], bool(n.get("online"))))
    return keys


def _handover_next(ssid):
    """Hand every node the next network (or take it back, ssid == "")."""
    for key, c in NODE_CFG.items():
        d = c["desired"]
        if d["transport"] == "wifi":
            d["net"]["next"] = {"ssid": ssid, "pass": HANDOVER.get("pass", "") if ssid else ""}
        else:
            d["link"]["next_ssid"] = ssid
        d["rev"] += 1
        c["status"] = "pending"


def handover_get():
    h = HANDOVER
    if not h.get("active"):
        return {"active": False}
    if h["polls"] > 0:
        if h["pending"]:
            h["ready"].append(h["pending"].pop(0))
        elif h.get("all_ready_seen"):
            h["active"] = False
            h["switched"] = True
            return {"active": False}
    if not h["pending"]:
        h["all_ready_seen"] = True
    h["polls"] += 1
    return {"active": True, "ssid": h["ssid"], "ready": list(h["ready"]),
            "pending": list(h["pending"]), "offline": list(h["offline"])}


def handover_post(doc):
    action = doc.get("action")
    if action == "start":
        ssid = str(doc.get("ssid") or "")
        if not ssid:
            return {"ok": False, "field": "ssid", "reason": "ssid is required"}, 400
        keys = _node_keys()
        HANDOVER.clear()
        HANDOVER.update({"active": True, "ssid": ssid, "pass": doc.get("pass", ""),
                         "form": doc.get("form"), "polls": 0, "ready": [],
                         "pending": [k for k, up in keys if up],
                         "offline": [k for k, up in keys if not up]})
        _handover_next(ssid)
        return {"ok": True}, 200
    if action == "switch":
        if not HANDOVER.get("active"):
            return {"ok": False, "reason": "no handover in progress"}, 409
        HANDOVER["active"] = False
        HANDOVER["switched"] = True
        return {"ok": True}, 200
    if action == "cancel":
        if HANDOVER.get("active"):
            _handover_next("")
        HANDOVER["active"] = False
        return {"ok": True}, 200
    return {"ok": False, "reason": "unknown action"}, 400


def nodes_config_get(key):
    c = NODE_CFG.get(key)
    if c is None:
        # Known to the status lists but never reported: no config file yet.
        known = [k for k, _ in _node_keys()]
        if key not in known:
            return {"ok": False, "error": "unknown node"}, 404
        transport = "espnow" if key.startswith("e:") else "wifi"
        hw = "esp32c3" if transport == "espnow" else "esp8266"
        return {"key": key, "transport": transport, "desired": None, "reported": None,
                "applied_rev": 0, "status": None, "error": None, "caps": CAPS[hw]}, 200
    d = c["desired"]
    return {"key": key, "transport": d["transport"], "desired": _blank_secrets(d),
            "reported": _blank_secrets(c["reported"]), "applied_rev": c["applied_rev"],
            "status": c["status"], "error": c["error"], "caps": CAPS[d["hw"]]}, 200


def nodes_config_post(doc):
    key = (doc or {}).get("key")
    c = NODE_CFG.get(key)
    if c is None:
        return {"ok": False, "field": "key", "reason": "no config for this node yet"}, 400
    cand = json.loads(json.dumps(c["desired"]))
    _merge(cand, doc.get("config") or {})
    bad = _validate(cand, CAPS[cand["hw"]])
    if bad:
        return {"ok": False, "field": bad[0], "reason": bad[1]}, 400
    cand["rev"] = c["desired"]["rev"] + 1
    c["desired"] = cand
    c["status"] = "pending"
    c["error"] = None
    NODE_CFG_POSTS.append(doc)
    # The ESP-NOW label and interval the list shows are the desired ones.
    if key.startswith("e:"):
        for n in STATUS["nodes"]:
            if "e:%d" % n["node_id"] == key:
                n["id"] = cand["name"]
                n["interval"] = cand["interval_s"]
    return {"ok": True, "rev": cand["rev"]}, 200


# Every accepted POST body, for a driver to check a save was PARTIAL.
NODE_CFG_POSTS = []


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

    def _text(self, body, code=200):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
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
            return self._json(_status_with_cfg(STATUS, "e:", "node_id"))
        if path == "/api/remote/status":
            return self._json(_status_with_cfg(REMOTE_STATUS, "w:", "id"))
        if path == "/api/nodes/config":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            body, code = nodes_config_get(q.get("key", [""])[0])
            return self._json(body, code)
        if path == "/api/nodes/handover":
            return self._json(handover_get())
        # Mock-only: what the page sent, so a driver can check the save was
        # partial and the handover carried the rest of the Network form.
        if path == "/__mock/nodes":
            return self._json({"posts": NODE_CFG_POSTS, "handover": HANDOVER})
        if path == "/api/kindle/config":
            return self._json(KINDLE)
        if path == "/api/csrf-token":
            return self._json({"token": "test-token"})
        if path == "/api/platform_config":
            return self._json(PLATFORM)
        if path == "/__mock/profiles":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            PROFILES_FAIL["on"] = q.get("fail", ["0"])[0] == "1"
            return self._json(PROFILES_FAIL)
        if path == "/api/board-profiles" and PROFILES_FAIL["on"]:
            return self._json({"error": "unavailable"}, 503)
        if path == "/api/board-profiles":
            return self._json({"profiles": _board_profiles(),
                               "active": {"id": BOARD["active"], "setupRequired": BOARD["setupRequired"]},
                               "suggested": os.environ.get("MOCK_SUGGESTED", "xiao_c3")})
        if path == "/export_settings":
            return self._json({"hardware": HARDWARE, "flowMeter": {"testMode": False, "blinkDuration": 250},
                               "theme": {}})
        if path == "/__mock/hw":
            return self._json({"hardware": HARDWARE, "posts": HW_POSTS, "board": BOARD})
        # The index must be tested BEFORE the detail prefix, or "/api/modules"
        # falls into the startswith below and answers with one module.
        if path == "/api/modules":
            return self._json(MODULES)
        # The Network page's credential test: the POST below starts it (202),
        # and this is the poll, which finds it already connected.
        if path == "/api/modules/wifi/test":
            return self._json({"state": "success", "rssi": -51, "ip": "192.168.7.23"})
        # The forecast button's one-line answer (handleKindleForecast). "ok"
        # the first time; the status then says "pending" for FORECAST["polls"]
        # reads, and only after that carries the newer fetchedAt. Then "wait",
        # like the device's one-minute floor. /__mock/forecast?reset=1 starts
        # it over; &polls=N and &offline=1 set the next round up.
        if path == "/kindle/forecast":
            st = next(x for x in MODULES if x["id"] == "forecast")["status"]
            if FORECAST["offline"]:
                return self._text("offline")
            if FORECAST["asked"]:
                return self._text("wait 57")
            FORECAST["asked"] = True
            FORECAST["left"] = FORECAST["polls"]
            if FORECAST["left"] > 0:
                st["pending"] = True
            else:
                st["fetchedAt"] = int(time.time())
            return self._text("ok")
        if path == "/__mock/forecast":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            FORECAST["asked"] = False
            FORECAST["polls"] = int(q.get("polls", ["0"])[0])
            FORECAST["offline"] = q.get("offline", ["0"])[0] == "1"
            return self._json(FORECAST)
        if path == "/api/modules/forecast" and FORECAST["left"] > 0:
            st = next(x for x in MODULES if x["id"] == "forecast")["status"]
            FORECAST["left"] -= 1
            if FORECAST["left"] == 0:
                st["pending"] = False
                st["fetchedAt"] = int(time.time())
        if path.startswith("/api/modules/"):
            mid = path[len("/api/modules/"):]
            m = next((x for x in MODULES if x["id"] == mid), None)
            if m is None:
                return self._json({"ok": False, "error": "unknown module"}, 404)
            # A detail response is the index entry plus its form. `schema` is
            # a JSON *string* holding {"fields":[{"id":…}]}, not an object:
            # ModuleRegistry::toDetailJson assigns the module's PROGMEM
            # literal straight through, and settings.js does JSON.parse on it.
            # Handing back a parsed array here would make every detail pane
            # render "Bad schema JSON." while the driver still saw a populated
            # pane — the form, showIf, collect and save paths would all go
            # untested. Shaped after TIME_SCHEMA in src/modules/TimeModule.cpp.
            return self._json(dict(
                m,
                config=MODULE_CONFIG.get(mid, {}),
                **({"schema": MODULE_SCHEMA[mid]} if mid in MODULE_SCHEMA else {})))
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
        if path == "/api/nodes/config":
            doc = self._read_json()
            if doc is None:
                return self._json({"ok": False, "error": "bad json"}, 400)
            body, code = nodes_config_post(doc)
            return self._json(body, code)
        if path == "/api/modules/wifi/test":
            self._read_json()
            return self._json({"started": True}, 202)
        if path == "/api/nodes/handover":
            doc = self._read_json()
            if doc is None:
                return self._json({"ok": False, "error": "bad json"}, 400)
            body, code = handover_post(doc)
            return self._json(body, code)
        if path == "/api/firstrun":
            doc = self._read_json()
            if doc is None:
                return self._json({"ok": False, "error": "bad json"}, 400)
            HW_POSTS.append({"path": path, "body": doc})
            BOARD["active"] = doc.get("profile", "")
            return self._json({"ok": True})
        if path == "/save_platform":
            doc = self._read_json()
            if doc is None:
                return self._json({"ok": False, "error": "bad json"}, 400)
            PLATFORM.clear()
            PLATFORM.update(doc)
            return self._json({"ok": True})
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode()
        if "multipart/form-data" in (self.headers.get("Content-Type") or ""):
            # settingsSave() posts FormData, which the browser sends as
            # multipart; the firmware's hasParam(name, true) reads both.
            import re
            body = {}
            for m in re.finditer(r'name="([^"]+)"\r\n\r\n(.*?)\r\n--', raw, re.S):
                body.setdefault(m.group(1), []).append(m.group(2))
        else:
            body = urllib.parse.parse_qs(raw)
        if path == "/save_hardware":
            # A form POST, like the firmware's: every pin arrives as a GPIO
            # number or -1, never as the label the user typed.
            flat = {k: v[0] for k, v in body.items() if k != "csrf"}
            HW_POSTS.append({"path": path, "body": flat})
            for k, v in flat.items():
                if k.startswith("pin"):
                    HARDWARE[k] = 255 if v == "-1" else int(v)
            return self._json({"ok": True})
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
