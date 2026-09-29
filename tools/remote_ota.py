"""
tools/remote_ota.py — firmware over WiFi, through the HTTP API the devices
already serve. Used by steps 13 and 14 of the deploy tools.

Nothing here is new on the device side; every call is one the web UI makes:

  collector   POST /do_update?sha256=…      the app image, SHA-256 checked
              GET  /api/ota/status           pending_verify after the restart
              POST /api/ota/confirm          keep the new image
  nodes       POST /api/nodes/fw/upload      field `fw`, image kept on the SD card
              POST /api/nodes/fw             {"action":"start","kind","keys"}
              GET  /api/nodes/fw             rollout status per node
  WiFi node   POST <node>/update             field `fw`, the node's basic auth

The contract for the node half is docs/NODE_OTA.md. Every mutating call on the
collector wants the per-boot CSRF token, as a query parameter.

Standard library only: the frozen GUI bundles nothing else for this, and the
CLI must run on a bare Python.
"""

from __future__ import annotations

import base64
import hashlib
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Optional

#: ESP-IDF's esp_chip_id_t, at byte 12 of an app image header. A mismatch here
#: is a C3 image on its way to an S3 — the bootloader would refuse it after the
#: restart, and the device would come back on its old image with no reason
#: given. Refused here instead, before the upload.
CHIP_IDS: dict[str, int] = {
    "esp32": 0, "esp32s2": 2, "esp32c3": 5, "esp32s3": 9,
    "esp32c2": 12, "esp32c6": 13, "esp32h2": 16,
}

#: Which marker kind each node project builds (docs/NODE_OTA.md §1.1).
NODE_KINDS: dict[str, str] = {"node": "esp8266", "node_espnow": "espnow-c3"}

#: The marker every node firmware carries: NODEFW1|<kind>|<version>| + NUL.
_MARKER = re.compile(rb"NODEFW1\|(esp8266|espnow-c3)\|([A-Za-z0-9._+-]{1,23})\|\x00")

#: Terminal rollout states (NODE_OTA.md §2.2).
NODE_DONE_STATES = ("done", "failed")

#: What the collector's node-image refusals mean, in words.
NODE_UPLOAD_ERRORS: dict[str, str] = {
    "no_sd":          "the collector has no SD card; node images are kept on it",
    "busy":           "another node image is uploading right now; try again",
    "not_node_image": "the collector found no node marker in this image",
    "too_big":        "the image is larger than the node's update slot",
    "bad_header":     "the image header does not match its kind",
    "zero_id":        "the image id is 0; rebuild the node firmware",
    "write_failed":   "the SD card refused the write",
    "no_image":       "the collector holds no image of this kind",
    "bad_key":        "a node key does not belong to this kind",
    "bad_request":    "the collector refused the request",
    "csrf":           "CSRF token refused (did the collector restart?)",
}


class Cancelled(Exception):
    """STOP was pressed while a request body was going out."""


# ── Images ───────────────────────────────────────────────────────────────────

def node_marker(data: bytes) -> Optional[tuple[str, str]]:
    """(kind, version) of a node image, or None when it has no single marker.

    Two markers of different kinds are no marker at all, the same rule the
    collector and the nodes apply (NODE_OTA.md §1.1).
    """
    found = _MARKER.findall(data)
    kinds = {k.decode() for k, _ in found}
    if len(kinds) != 1:
        return None
    return found[0][0].decode(), found[0][1].decode()


def app_image_problem(data: bytes, chip: str | None) -> Optional[str]:
    """Why this is not a collector image for `chip`, or None when it looks fine."""
    if len(data) < 24 or data[0] != 0xE9:
        return "not an ESP32 firmware image (first byte is not 0xE9)"
    marker = node_marker(data)
    if marker:
        return (f"this is a node firmware ({marker[0]} {marker[1]}), not the "
                f"collector's — use step 14 for it")
    want = CHIP_IDS.get((chip or "").lower())
    got = int.from_bytes(data[12:14], "little")
    if want is not None and got != want:
        names = {v: k for k, v in CHIP_IDS.items()}
        return (f"the image is built for {names.get(got, f'chip id {got}')}, "
                f"this environment is {chip}")
    return None


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def md5_hex(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


# ── HTTP ─────────────────────────────────────────────────────────────────────

class _Body:
    """A request body urllib reads in blocks, so it can report and be stopped.

    Handed to urllib as a file object with an explicit Content-Length. Without
    that length http.client falls back to chunked transfer encoding, which the
    ESP8266 web server does not take.
    """

    def __init__(self, data: bytes,
                 progress: Optional[Callable[[int, int], None]],
                 cancelled: Callable[[], bool]):
        self._data = data
        self._pos = 0
        self._progress = progress
        self._cancelled = cancelled

    def read(self, n: int = -1) -> bytes:
        if self._cancelled():
            raise Cancelled()
        if n is None or n < 0:
            n = len(self._data) - self._pos
        chunk = self._data[self._pos:self._pos + n]
        self._pos += len(chunk)
        if self._progress and chunk:
            self._progress(self._pos, len(self._data))
        return chunk


def multipart(field: str, filename: str, data: bytes) -> tuple[bytes, str]:
    """One file part, as (body, content type)."""
    boundary = "----ESP32DeployOta" + sha256_hex(data)[:16]
    head = (f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    return head + data + tail, f"multipart/form-data; boundary={boundary}"


class Http:
    """One device: its base URL, its basic auth, its CSRF token.

    Every call answers (status, parsed body). A refused request is an answer,
    not an exception — the caller wants the 409 and its reason, not a
    traceback. Only a device that does not answer at all raises (OSError).
    """

    def __init__(self, host: str, user: str = "", password: str = "",
                 cancelled: Callable[[], bool] = lambda: False):
        host = (host or "").strip().rstrip("/")
        self.base = host if host.startswith(("http://", "https://")) else f"http://{host}"
        self._auth = ""
        if user and password:
            tok = base64.b64encode(f"{user}:{password}".encode()).decode()
            self._auth = f"Basic {tok}"
        self._cancelled = cancelled
        self._csrf = ""

    def request(self, method: str, path: str, *, query: dict | None = None,
                body: bytes | None = None, content_type: str = "",
                timeout: float = 10,
                progress: Optional[Callable[[int, int], None]] = None,
                ) -> tuple[int, Any]:
        url = self.base + path
        if query:
            url += ("&" if "?" in url else "?") + urllib.parse.urlencode(query)
        headers = {}
        if self._auth:
            headers["Authorization"] = self._auth
        data: Any = None
        if body is not None:
            headers["Content-Length"] = str(len(body))
            if content_type:
                headers["Content-Type"] = content_type
            data = _Body(body, progress, self._cancelled) if progress else body
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, _parse(resp.read())
        except urllib.error.HTTPError as exc:
            try:
                raw = exc.read()
            except Exception:
                raw = b""
            return exc.code, _parse(raw)

    def get(self, path: str, timeout: float = 6) -> tuple[int, Any]:
        return self.request("GET", path, timeout=timeout)

    def csrf(self, refresh: bool = False) -> str:
        """The collector's per-boot token; '' when it has none to give."""
        if self._csrf and not refresh:
            return self._csrf
        try:
            st, js = self.get("/api/csrf-token", timeout=5)
        except OSError:
            return ""
        tok = js.get("token") if st == 200 and isinstance(js, dict) else ""
        self._csrf = tok if isinstance(tok, str) else ""
        return self._csrf

    def post_json(self, path: str, obj: dict, timeout: float = 10) -> tuple[int, Any]:
        q = {"csrf": self.csrf()} if self.csrf() else None
        return self.request("POST", path, query=q, body=json.dumps(obj).encode(),
                            content_type="application/json", timeout=timeout)

    def post_file(self, path: str, field: str, filename: str, data: bytes, *,
                  query: dict | None = None, timeout: float = 240,
                  progress: Optional[Callable[[int, int], None]] = None,
                  ) -> tuple[int, Any]:
        body, ctype = multipart(field, filename, data)
        return self.request("POST", path, query=query, body=body,
                            content_type=ctype, timeout=timeout,
                            progress=progress)


def _parse(raw: bytes) -> Any:
    """JSON when it is JSON, else the text (or None for an empty body)."""
    if not raw:
        return None
    try:
        return json.loads(raw.decode("utf-8", "replace"))
    except ValueError:
        return raw.decode("utf-8", "replace").strip()


def error_of(js: Any) -> str:
    """The `error`/`message` a device put in a refusal, or ''."""
    if isinstance(js, dict):
        return str(js.get("error") or js.get("message") or "")
    return js[:120] if isinstance(js, str) else ""


def wait_back(http: Http, path: str, *, down_s: float, up_s: float,
              cancelled: Callable[[], bool]) -> Optional[dict]:
    """Wait for a device to restart and answer `path` with JSON again.

    Sleeps `down_s` first: the collector answers the upload and restarts two
    seconds later, so polling straight away reads the OLD firmware's status
    and calls that success.
    """
    end = time.monotonic() + down_s
    while time.monotonic() < end:
        if cancelled():
            return None
        time.sleep(0.25)
    end = time.monotonic() + up_s
    while time.monotonic() < end:
        if cancelled():
            return None
        try:
            st, js = http.get(path, timeout=3)
            if st == 200 and isinstance(js, dict):
                return js
        except OSError:
            pass
        time.sleep(2)
    return None


def list_nodes(http: Http, kind: str = "") -> list[dict]:
    """The nodes the collector knows, as the Nodes page lists them.

    Each: {"key": "w:balcony"|"e:3", "name", "transport", "online"}. `key` is
    what /api/nodes/fw takes. Filtered to one kind when `kind` is given.
    Raises OSError when the collector does not answer.
    """
    out: list[dict] = []
    if kind in ("", "espnow-c3"):
        st, js = http.get("/api/espnow/status")
        for n in (js.get("nodes") or []) if st == 200 and isinstance(js, dict) else []:
            cfg = n.get("cfg") or {}
            out.append({"key": cfg.get("key") or f"e:{n.get('node_id')}",
                        "name": str(n.get("id") or n.get("node_id")),
                        "transport": "espnow", "online": not n.get("offline")})
    if kind in ("", "esp8266"):
        st, js = http.get("/api/remote/status")
        for n in (js.get("nodes") or []) if st == 200 and isinstance(js, dict) else []:
            cfg = n.get("cfg") or {}
            out.append({"key": cfg.get("key") or f"w:{n.get('id')}",
                        "name": str(n.get("id")),
                        "transport": "wifi", "online": bool(n.get("online"))})
    return out


def parse_targets(value: Any) -> Any:
    """"all", or the list of node keys a config holds (list or comma text)."""
    if isinstance(value, list):
        keys = [str(k).strip() for k in value if str(k).strip()]
    else:
        text = str(value or "").strip()
        if not text or text.lower() == "all":
            return "all"
        keys = [k.strip() for k in text.split(",") if k.strip()]
    return keys or "all"


def fmt_targets(targets: dict, kind: str) -> list[str]:
    """One line per rollout target of `kind` from GET /api/nodes/fw."""
    lines = []
    for key, t in sorted((targets or {}).items()):
        if not isinstance(t, dict) or t.get("kind") != kind:
            continue
        st = t.get("st", "?")
        extra = f" {t['pct']}%" if "pct" in t else ""
        err = f"  ({t['err']})" if t.get("err") else ""
        lines.append(f"  {key:<20} {st}{extra}{err}")
    return lines
