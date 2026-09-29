#!/usr/bin/env python3
"""
drive_remote_ota.py — run deploy steps 13 and 14 against fake devices.

Steps 13 and 14 of the deploy tools update firmware over WiFi through the
HTTP API the collector and the WiFi node already serve (tools/remote_ota.py).
This starts a fake collector and a fake WiFi node on localhost that answer
those routes the way the firmware does (docs/NODE_OTA.md, /do_update in
src/web/WebServer.cpp), runs the real DeployManager steps against them, and
asserts on what the devices received: the image, its SHA-256, the CSRF token,
the login, the rollout request.

No hardware, no PlatformIO, standard library only:

    python3 tests/tools/drive_remote_ota.py
"""
from __future__ import annotations

import base64
import hashlib
import json
import sys
import tempfile
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import deploy_core as dc   # noqa: E402
import remote_ota as ota   # noqa: E402

FAILURES: list[str] = []
TOKEN = "0123456789abcdef0123456789abcdef"


def check(cond: bool, what: str) -> None:
    print(f"  {'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


# ── Images ───────────────────────────────────────────────────────────────────

def app_image(chip_id: int, size: int = 40_000, tail: bytes = b"") -> bytes:
    head = bytearray(24)
    head[0] = 0xE9
    head[12:14] = chip_id.to_bytes(2, "little")
    body = bytes((i * 7) & 0xFF for i in range(size))
    return bytes(head) + body + tail


def node_image(kind: str, ver: str) -> bytes:
    chip = 5 if kind == "espnow-c3" else 0
    return app_image(chip, 30_000, f"NODEFW1|{kind}|{ver}|".encode() + b"\x00")


# ── Fake devices ─────────────────────────────────────────────────────────────

def file_part(handler: BaseHTTPRequestHandler, body: bytes) -> tuple[str, bytes]:
    """(field name, content) of the one file part of a multipart body."""
    ctype = handler.headers.get("Content-Type", "")
    boundary = ctype.split("boundary=", 1)[1].encode()
    part = body.split(b"--" + boundary)[1]
    head, content = part.split(b"\r\n\r\n", 1)
    field = head.split(b'name="', 1)[1].split(b'"', 1)[0].decode()
    return field, content[:-2]          # the CRLF before the next boundary


class Device:
    """State shared by a fake server's handler, and what it was sent."""

    def __init__(self, auth: tuple[str, str] | None = None):
        self.auth = auth
        self.log: list[tuple[str, str]] = []
        self.version = "v-old"
        self.pending = False
        self.confirmed = False
        self.image = b""
        self.sha_param = ""
        self.images: dict = {"esp8266": None, "espnow-c3": None}
        self.targets: dict = {}
        self.start_body: dict | None = None
        self.uploads = 0
        self.gets_after_start = 0


def make_handler(dev: Device, node: bool):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code: int, obj=None) -> None:
            raw = b"" if obj is None else json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def _authed(self) -> bool:
            if not dev.auth:
                return True
            want = "Basic " + base64.b64encode(":".join(dev.auth).encode()).decode()
            return self.headers.get("Authorization") == want

        def _route(self):
            u = urllib.parse.urlparse(self.path)
            return u.path, dict(urllib.parse.parse_qsl(u.query))

        def do_GET(self):
            path, q = self._route()
            dev.log.append(("GET", path))
            if not self._authed():
                return self._send(401)
            if node:
                if path == "/api/status":
                    return self._send(200, {"fw": dev.version})
                return self._send(404)
            if path == "/api/status":
                return self._send(200, {"version": dev.version})
            if path == "/api/csrf-token":
                return self._send(200, {"token": TOKEN})
            if path == "/api/ota/status":
                return self._send(200, {"pending_verify": dev.pending,
                                        "running_partition": "app1"})
            if path == "/api/nodes/fw":
                if dev.start_body is not None:
                    dev.gets_after_start += 1
                    if dev.gets_after_start >= 2:      # the nodes woke up
                        for t in dev.targets.values():
                            t["st"] = "done"
                return self._send(200, {"sd": True, "images": dev.images,
                                        "targets": dev.targets})
            if path == "/api/espnow/status":
                return self._send(200, {"nodes": [
                    {"id": "garden", "node_id": 3, "offline": False,
                     "cfg": {"key": "e:3"}},
                    {"id": "shed", "node_id": 4, "offline": True}]})
            if path == "/api/remote/status":
                return self._send(200, {"nodes": [{"id": "balcony", "online": True}]})
            return self._send(404)

        def do_POST(self):
            path, q = self._route()
            dev.log.append(("POST", path))
            body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if not self._authed():
                return self._send(401)
            if node:
                if path != "/update":
                    return self._send(404)
                field, data = file_part(self, body)
                if field != "fw" or ota.node_marker(data) is None:
                    return self._send(400, {"ok": False, "error": "not_node_image"})
                dev.image = data
                dev.version = ota.node_marker(data)[1]
                return self._send(200, {"ok": True})
            if q.get("csrf") != TOKEN:
                return self._send(403, {"ok": False, "error": "csrf"})
            if path == "/do_update":
                _, data = file_part(self, body)
                dev.sha_param = q.get("sha256", "")
                if hashlib.sha256(data).hexdigest() != dev.sha_param:
                    return self._send(400, {"success": False,
                                            "message": "SHA-256 mismatch — image rejected"})
                dev.image = data
                dev.version = "v-new"
                dev.pending = True
                return self._send(200, {"success": True,
                                        "message": "Update complete, restarting..."})
            if path == "/api/ota/confirm":
                dev.pending = False
                dev.confirmed = True
                return self._send(200, {"ok": True})
            if path == "/api/nodes/fw/upload":
                field, data = file_part(self, body)
                m = ota.node_marker(data)
                if field != "fw" or not m:
                    return self._send(400, {"ok": False, "error": "not_node_image"})
                dev.uploads += 1
                dev.images[m[0]] = {"ver": m[1], "md5": hashlib.md5(data).hexdigest(),
                                    "size": len(data)}
                return self._send(200, {"ok": True, "kind": m[0], "ver": m[1],
                                        "size": len(data)})
            if path == "/api/nodes/fw":
                req = json.loads(body)
                dev.start_body = req
                keys = req["keys"] if req["keys"] != "all" else ["e:3", "e:4"]
                for k in keys:
                    dev.targets[k] = {"kind": req["kind"], "st": "pending", "attempt": 1}
                return self._send(200, {"ok": True, "targets": len(keys)})
            return self._send(404)
    return H


class _Quiet(ThreadingHTTPServer):
    # The STOP test hangs up mid-body on purpose; its broken pipe is expected.
    def handle_error(self, request, client_address):
        pass


def serve(dev: Device, node: bool = False) -> str:
    srv = _Quiet(("127.0.0.1", 0), make_handler(dev, node))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return f"127.0.0.1:{srv.server_address[1]}"


# ── Runs ─────────────────────────────────────────────────────────────────────

_real_wait_back = ota.wait_back


def _fast_wait_back(http, path, *, down_s, up_s, cancelled):
    # The fake restarts instantly; no need to sit out the real device's delay.
    return _real_wait_back(http, path, down_s=0, up_s=5, cancelled=cancelled)


ota.wait_back = _fast_wait_back


def run(step: int, cfg: dict, stop_on: str = "") -> tuple[int, str, dc.DeployManager]:
    base = dict(dc.DEFAULT_CFG)
    base.update({"env": "esp32c3", "chip": "esp32c3"})
    base.update(cfg)
    m = dc.DeployManager(base)
    out: list[str] = []

    def sink(text: str) -> None:
        out.append(text)
        if stop_on and stop_on in text:
            m.cancel()
    m.on_step_output = sink
    m.on_step_start = lambda n, name: None
    m.on_step_complete = lambda n, rc: None
    rc = m.s13_remote_collector() if step == 13 else m.s14_remote_node()
    return rc, "".join(out), m


def write(tmp: Path, name: str, data: bytes) -> str:
    p = tmp / name
    p.write_bytes(data)
    return str(p)


def main() -> int:
    tmp = Path(tempfile.mkdtemp())

    print("Images:")
    c3 = app_image(5)
    check(ota.app_image_problem(c3, "esp32c3") is None, "a C3 image passes for a C3 env")
    check("esp32s3" in (ota.app_image_problem(app_image(9), "esp32c3") or ""),
          "an S3 image is refused for a C3 env, and says which chip it is for")
    check("node firmware" in (ota.app_image_problem(node_image("espnow-c3", "1.2"), "esp32c3") or ""),
          "a node image is refused as the collector's")
    check(ota.node_marker(node_image("esp8266", "2.0")) == ("esp8266", "2.0"),
          "the node marker gives kind and version")
    two = node_image("esp8266", "1") + b"NODEFW1|espnow-c3|1|\x00"
    check(ota.node_marker(two) is None, "two markers of different kinds are no marker")
    check(dc.run_order([1, 5, 8, 13]) == [1, 5, 13, 8],
          "step 13 runs before step 8: firmware first, then its pages")
    check(dc.PRESETS["O"][1] == [1, 5, 13, 8], "preset O is compile, remote update, pages")

    print("Step 13, the collector:")
    col = Device()
    host = serve(col)
    fw = write(tmp, "c3.bin", c3)
    rc, log, _ = run(13, {"device_ip": host, "remote_fw_file": fw})
    check(rc == 0, f"step 13 succeeds (rc={rc})")
    check(col.image == c3, "the collector received the exact image")
    check(col.sha_param == hashlib.sha256(c3).hexdigest(), "with its SHA-256 for /do_update to check")
    check(col.confirmed and not col.pending, "and the new firmware was confirmed after the restart")
    check("v-new" in log, "the log names the version running after the restart")

    col2 = Device()
    host2 = serve(col2)
    rc, log, _ = run(13, {"device_ip": host2, "remote_fw_file": write(tmp, "s3.bin", app_image(9))})
    check(rc == 2 and not col2.log, "a wrong-chip image is refused before the device is contacted")

    rc, log, _ = run(13, {"device_ip": host2, "remote_fw_file": str(tmp / "missing.bin")})
    check(rc == 2 and "not found" in log, "a missing file is an error that says so")

    locked = Device(auth=("admin", "pw"))
    host3 = serve(locked)
    rc, log, _ = run(13, {"device_ip": host3, "remote_fw_file": fw})
    check(rc == 1 and "basic auth" in log and not locked.image,
          "a collector with basic auth and no password: refused, and the log says why")
    rc, log, _ = run(13, {"device_ip": host3, "remote_fw_file": fw,
                          "http_user": "admin", "http_pass": "pw"})
    check(rc == 0 and locked.image == c3, "with the password it goes through")

    col4 = Device()
    host4 = serve(col4)
    big = write(tmp, "big.bin", app_image(5, 600_000))
    rc, log, m = run(13, {"device_ip": host4, "remote_fw_file": big}, stop_on=" 25%")
    check(rc == dc.RC_CANCELLED and not col4.image,
          f"STOP mid-upload ends the step as stopped and nothing is flashed (rc={rc})")

    print("Step 14, through the collector:")
    col5 = Device()
    host5 = serve(col5)
    nimg = node_image("espnow-c3", "1.4.0")
    nfw = write(tmp, "node.bin", nimg)
    cfg = {"device_ip": host5, "node_fw_file": nfw, "node_fw_targets": ["e:3"]}
    rc, log, _ = run(14, cfg)
    check(rc == 0, f"step 14 succeeds (rc={rc})")
    check(col5.images["espnow-c3"] and col5.images["espnow-c3"]["ver"] == "1.4.0",
          "the image reached the collector")
    check(col5.start_body == {"action": "start", "kind": "espnow-c3", "keys": ["e:3"]},
          f"and the rollout was started for the picked node: {col5.start_body}")
    rc, log, _ = run(14, cfg)
    check(rc == 0 and col5.uploads == 1 and "already holds" in log,
          "the same image again is not re-uploaded (that would reset done nodes)")

    col6 = Device()
    host6 = serve(col6)
    rc, log, _ = run(14, {"device_ip": host6, "node_fw_file": nfw, "node_fw_watch": True})
    check(rc == 0 and col6.start_body["keys"] == "all" and "Every node runs" in log,
          "\"all\" starts every node of the kind, and following it waits for done")

    nodes = ota.list_nodes(ota.Http(host6))
    keys = [n["key"] for n in nodes]
    check(keys == ["e:3", "e:4", "w:balcony"], f"the node list uses the Nodes page keys: {keys}")
    check(ota.parse_targets("e:3, w:balcony") == ["e:3", "w:balcony"]
          and ota.parse_targets("") == "all", "targets parse from text")

    rc, log, _ = run(14, {"device_ip": host6, "node_fw_file": fw})
    check(rc == 2 and "not a node firmware" in log, "a collector image is refused as a node's")

    print("Step 14, directly to the WiFi node:")
    wnode = Device(auth=("node", "secret"))
    whost = serve(wnode, node=True)
    wimg = node_image("esp8266", "2.1")
    wfw = write(tmp, "w.bin", wimg)
    direct = {"node_fw_route": "direct", "node_ip": whost, "node_fw_file": wfw,
              "node_http_user": "node", "node_http_pass": "secret"}
    rc, log, _ = run(14, direct)
    check(rc == 0 and wnode.image == wimg, f"the WiFi node got the image directly (rc={rc})")
    check("2.1" in log, "and the log names the version it came back with")
    rc, log, _ = run(14, {**direct, "node_http_pass": "wrong"})
    check(rc == 1 and "refused the user" in log, "a wrong node password is reported as such")
    rc, log, _ = run(14, {**direct, "node_fw_file": nfw})
    check(rc == 2 and "WiFi node only" in log, "an ESP-NOW image is not sent the direct way")

    print("Passwords:")
    saved = dc.CFG_FILE
    dc.CFG_FILE = tmp / "flash_tool.json"
    try:
        cfg = dc.load_cfg()
        cfg.update({"http_pass": "pw1", "node_http_pass": "pw2", "http_user": "admin"})
        dc.save_cfg(cfg)
        on_disk = json.loads(dc.CFG_FILE.read_text())
        check("http_pass" not in on_disk and "node_http_pass" not in on_disk,
              "passwords never reach .flash_tool.json")
        check(on_disk.get("http_user") == "admin", "the user name does")
    finally:
        dc.CFG_FILE = saved

    print()
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} check(s) failed")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("OK: remote update talks to the devices the way their API expects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
