# Node firmware updates (OTA): the contract

How a sensor node's firmware is replaced remotely — for the WiFi node
(`node/`, ESP8266) and the ESP-NOW node (`node_espnow/`, XIAO ESP32-C3) — and
locally from the node's own page. Like [NODE_CONFIG.md](NODE_CONFIG.md), this
file is the contract: every piece of code that takes part implements a part of
it, and when they disagree this document wins.

## 0. Principles

1. **The node always pulls.** The collector never connects to a node. A WiFi
   node learns about an update in the reply to its own POST (§3); an ESP-NOW
   node from a flag in the ACK to its own DATA frame (§4). Same model as the
   config (NODE_CONFIG.md §0.1).
2. **One image per kind, many targets.** The collector holds at most one image
   per node kind on its SD card (§2). The user chooses which nodes of that
   kind should run it: one node, or all of them with one button.
3. **SD card only.** Images are ~0.5 MB (ESP8266) and ~1 MB (C3); LittleFS
   (1088 KB, shared with logs) is not a place for them. Without a mounted card
   the upload is refused with `409 {"error":"no_sd"}` and the page says so.
4. **Checked twice.** The collector refuses anything that is not a node image
   of a known kind (§1) before it is offered to a node; the node checks again
   before it switches (MD5 on the ESP8266, the image's own SHA-256 + id + kind
   on the C3).
5. **Done means running.** A target is `done` only when the node reports that
   it RUNS the image (MD5 of its sketch, or the image id), never when the
   download finished.
6. **The C3 node rolls itself back** (§4.4). The ESP8266 has no second app
   slot to go back to: a new image that boots but cannot reach the collector
   needs a local fix (its setup page, or USB). The UI says so next to the
   update button for WiFi nodes.
7. **The battery decides on ESP-NOW.** A C3 node below the rollout's minimum
   battery voltage (default 3600 mV, set on the collector) defers and says so.

## 1. What a node image is

`src/nodecfg/FwImage.h` — shared by all three firmwares and the host tests.

### 1.1 The marker

Every node firmware carries `NODEFW1|<kind>|<version>|` (NUL-terminated) in
its image, defined with `NODEFW_MARKER_TEXT(kind, NODE_FW_VERSION)` and
printed at boot (an unreferenced array is dropped by `--gc-sections`).

| kind        | firmware       | max size  | head check (`headOk`)                        |
|-------------|----------------|-----------|----------------------------------------------|
| `esp8266`   | `node/`        | 0xFF000   | first byte 0xE9                              |
| `espnow-c3` | `node_espnow/` | 0x140000  | 0xE9, chip id 5 at [12], app-desc magic at 0x20 |

An image with no marker, with two markers of different kinds, or whose kind
does not match what the receiver runs, is refused. `nodefw::MarkerScan` finds
the marker while streaming, across chunk boundaries.

### 1.2 Versions

`<version>` is the node's `NODE_FW_VERSION` ([A-Za-z0-9._+-], ≤23 chars). It
is shown to people; it is **not** what decides "done" (two builds can share a
version).

### 1.3 Identity

- **esp8266**: the image's **MD5**, lowercase hex. The node reports
  `ESP.getSketchMD5()` — the MD5 of exactly the bytes of the .bin it was
  flashed from — as `fw_md5` in every ingest POST.
- **espnow-c3**: the image **id** = the first 4 bytes of the ELF SHA-256 in
  the app descriptor (file offset 0xB0), little-endian `uint32`
  (`nodefw::c3ImageId`). The node computes the same number from
  `esp_ota_get_app_description()->app_elf_sha256` (`runningImageId()` in
  `node_espnow/src/main.cpp`). An upload whose id is 0 is refused (0 is
  `EN_FW_ANY` on the wire); rebuild it.

## 2. On the collector

### 2.1 Storage (SD, `sdAvailable`)

```
/nodefw/esp8266.bin      the image
/nodefw/esp8266.json     {"kind","ver","size","md5","sha256","uploaded"}
/nodefw/espnow-c3.bin
/nodefw/espnow-c3.json   {… ,"img_id","min_mv"}
/nodefw/rollout.json     {"esp8266":   {"attempt":N,"targets":{"w:balcony":{"st","err"}}},
                          "espnow-c3": {"attempt":N,"targets":{"e:3":{"st","err"}}}}
```

An upload is streamed to `/nodefw/upload.tmp` while MD5, SHA-256 and the
marker scan run over it; it becomes `<kind>.bin` only when every check passed.
The image it replaces is renamed to `<kind>.old` first and put back if that
rename fails; only when neither file can be put in place is the kind left
with no image, exactly as if it had been deleted.
A new image for a kind resets every target of that kind to `pending` with a
new `attempt` (§4.5) — the targets stay chosen. Deleting an image drops its
rollout. Every SD access holds `fsMutex`.

The state is mirrored in RAM (`src/nodes/NodeFwStore`), so the ingest
handler and the ESP-NOW receive path never touch the card except to read
image bytes. `rollout.json` is written on status changes, not on progress.

### 2.2 Target status

| st         | meaning                                                            |
|------------|--------------------------------------------------------------------|
| `pending`  | chosen; the node has not started (or has not reported since)       |
| `sending`  | the node is downloading (`pct` in the API, RAM only)               |
| `staged`   | ESP-NOW only: verified and restarting into it                      |
| `deferred` | ESP-NOW only: battery below `min_mv` (`err` names the voltage)     |
| `done`     | the node reports running this image — terminal                     |
| `failed`   | refused / could not write / rolled back (`err`) — terminal until the user retries |

A node that reports running the image is `done` from any state. `failed` and
`done` targets are no longer offered. **Retry** (`start` again) sets the
target back to `pending` and bumps the kind's `attempt`.

### 2.3 HTTP API (behind the web UI's auth, like `/api/nodes/config`)

| Method | Path | Body / query | Answer |
|---|---|---|---|
| GET  | `/api/nodes/fw` | — | `{"sd":bool,"images":{"esp8266":{…json…}\|null,"espnow-c3":…},"targets":{"w:balcony":{"kind","st","pct"?,"err"?,"attempt"}}}` |
| POST | `/api/nodes/fw/upload` | multipart, field `fw` | `{"ok":true,"kind","ver","size"}`; 409 `no_sd`; 400 `{"error":"not_node_image"\|"too_big"\|"bad_header"\|"zero_id"}` |
| POST | `/api/nodes/fw` | `{"action":"start","kind","keys":[…]\|"all"}` | `{"ok":true,"targets":N}` |
| POST | `/api/nodes/fw` | `{"action":"cancel","kind","keys":[…]\|"all"}` | `{"ok":true}` — drops the targets |
| POST | `/api/nodes/fw` | `{"action":"delete","kind"}` | `{"ok":true}` — image and its rollout |
| POST | `/api/nodes/fw` | `{"action":"min_mv","kind":"espnow-c3","min_mv":3600}` | `{"ok":true}` (0 or 3000..4200) |
| GET  | `/api/nodes/fw/bin` | `?kind=esp8266`, `X-Ingest-Token` | the image, `x-MD5` header. For WiFi nodes, not people. |

`keys` are the Nodes page keys: `w:<name>`, `e:<id>`. `"all"` = every node of
that kind the collector knows (a config file or a status-list row); to
`cancel`, every target of that kind. A key whose kind does not match is
refused with 400 `bad_key`.

Other refusals, all `{"ok":false,"error":…}`: 409 `no_image` (`start` or
`min_mv` with no image of that kind), 409 `busy` (a second upload while one
is streaming — there is one `upload.tmp`), 400 `bad_request` (unknown action
or kind, `min_mv` out of range), 500 `write_failed` (the card refused the
upload), 503 `busy` (the store did not answer in time; try again). Every
mutating call answers 409 `no_sd` without a card. A deferred target's `err`
is `"battery 3.41 V"`.

## 3. WiFi node (ESP8266)

Every ingest POST carries `"fw_md5":"<32 hex>"` (computed once at boot).

The collector's ingest reply carries, when the node is a target in `pending`
or `sending` and `fw_md5` differs from the image's MD5:

```json
"fw": {"path":"/api/nodes/fw/bin?kind=esp8266","md5":"…","size":466848,
       "ver":"2026.10.1","attempt":3}
```

and marks the target `sending`. When `fw_md5` equals the image's MD5 the
target becomes `done` and no `fw` is sent. A POST without `fw_md5` (a node
that predates this) is never offered anything.

The node, after the cycle's POSTs:

1. skips an offer whose `(md5, attempt)` already failed since boot;
2. GETs `http://host:port` + `path` with `X-Ingest-Token` (and basic auth when
   it has one) and streams the body into `Update` (`Update.begin(size)`,
   `Update.setMD5(md5)`), feeding every chunk to `nodefw::MarkerScan` too;
3. before `Update.end()`: a marker whose kind is not `esp8266` (or none)
   aborts — the ESP8266 updater has no `abort()`, so the node sets an MD5
   that cannot match and lets `end()` fail, which leaves the running sketch
   untouched. `end()` itself refuses a wrong MD5 or a short body;
4. success → flush the backlog (as `restartForConfig()` does) and restart;
5. failure → remember `(md5, attempt)` and send
   `"fw_error":{"md5","attempt","reason"}` with every POST until one is
   answered; the collector marks the target `failed` with that reason.

## 4. ESP-NOW node (C3)

### 4.1 Frames (`src/espnow/EspNowProto.h`, new types, no version bump)

| type | dir | fields |
|---|---|---|
| ACK flag `EN_ACK_FW_PENDING` (bit 2) | c→n | the node is a target in `pending`/`sending`/`staged`/`deferred` |
| `FW_GET` (10) | n→c | `imgId` (`EN_FW_ANY` = 0 on the first request), `offset` |
| `FW` (11) | c→n | `imgId, size, offset, minMv, attempt, flags, len, data[≤200]` |
| `FW_DONE` (12) | n→c | `imgId, status, attempt, value` |

`FW` with `size = 0` = "nothing for you": stop and forget a partial download.
`FW` with `flags & EN_FW_BUSY` (len 0) = the collector is sending another node;
try on a later wake.

### 4.2 Serving (collector)

One transfer at a time. The collector keeps two 4 KB windows of the image in
RAM; the receive callback answers a `FW_GET` whose slice lies in the current
window at once, and asks the loop to load the window starting at the
requested offset when it does not, or when the request passes the middle of
the current window (read-ahead). An unanswered request is retried by the
node. A transfer idle for 3 s is dropped; another node's `FW_GET` during an
active transfer gets `EN_FW_BUSY`. `FW_GET` with `EN_FW_ANY` is answered with
the image's id, size, `minMv`, `attempt` and the slice at offset 0 — once
that slice is in a window; until then it is not answered, like any other.

**A slice ends at its window's end**, so a `FW` may carry fewer than 200
bytes before the last one; the node asks next at `offset + len` (never at
`offset + 200`). That is what lets a window start at whatever offset a miss
asked for. A node's `FW_DONE` releases the transfer at once.

Progress (`pct`) comes from the offsets requested.

### 4.3 Downloading (node)

On an ACK with `EN_ACK_FW_PENDING` — after the config exchange, which takes
precedence — the node:

1. sends `FW_GET(EN_FW_ANY, 0)`;
2. if `imgId` = its running image id → `FW_DONE(RUNNING)`; stop;
3. if `(imgId, attempt)` is the one it rolled back from → `FW_DONE(ROLLED_BACK)`;
   stop;
4. if the battery is measured and below `minMv` → `FW_DONE(LOW_BATTERY, mV)`;
   stop;
5. writes slices in order to the next OTA partition with the raw partition
   API (erasing each 4 KB sector as it is entered), keeping `imgId`, `size`,
   the bytes written and `attempt` in RTC memory, so a wake that runs out of
   budget (`NODE_FW_BUDGET_MS`) or misses too many replies resumes where it
   stopped on a later wake. A different `imgId`/`attempt` starts over;
6. once complete: checks the marker kind (`espnow-c3`), the app descriptor's
   id = `imgId`, and `esp_ota_set_boot_partition()` (which verifies the
   image's appended SHA-256); any failure → `FW_DONE(BAD_IMAGE)` (or
   `FLASH_ERROR`) and the partial state is dropped;
7. arms the trial (§4.4), sends `FW_DONE(STAGED)`, restarts.

### 4.4 Trial and rollback (node)

NVS namespace `nodefw`: `prev` (address of the partition it came from),
`img`, `att`, `boots`.

- At boot with a trial armed: running partition = `prev` → the bootloader
  already went back (the image did not start): record rolled-back, clear.
  Otherwise `boots++`.
- The first ACK received → trial confirmed: clear the keys.
- `boots > 3`, or 3 unanswered wakes in a row during the trial → set `prev`
  as the boot partition, record `(img, att)` as rolled back (NVS `rb_img`,
  `rb_att`), clear the trial, restart. The old firmware then answers the next
  `FW_PENDING` with `FW_DONE(ROLLED_BACK)`.

### 4.5 `attempt`

A per-kind counter on the collector, bumped by every upload and every
`start`. It travels in `FW`/`fw` and back in `FW_DONE`/`fw_error`; a node
refuses to retry only the exact `(image, attempt)` that failed, so a Retry on
the collector always gets a fresh try.

## 5. The node's own page

Both nodes' setup pages (`node_portal/`, shared) get a **Firmware** section:
the running version (the config's `fw`) and an upload field that POSTs the
.bin as `multipart/form-data`, field `fw`, to `/update`. The node streams it
into its update slot while scanning for the marker and answers:

| status | body | meaning |
|---|---|---|
| 200 | `{"ok":true}` | written and verified; the node restarts about a second later |
| 400 | `{"ok":false,"error":"not_node_image"}` | bad header, no marker, or another kind |
| 400 | `{"ok":false,"error":"too_big"}` | larger than the update slot |
| 500 | `{"ok":false,"error":"write_failed","detail":"…"}` | flash error |
| 404 | — | a node firmware older than this feature |

A refused image is never committed (the ESP8266 uses the MD5 trick of §3
step 3, the C3 `Update.abort()`), so the running firmware is untouched. On
the WiFi node's background server (on the LAN) the route is behind the same
basic auth as the rest of the page. A local update arms no trial: the person
doing it is standing there. On the C3 it also clears a trial still running
(§4.4) — the slot that trial would go back to is the one just overwritten.

## 6. Where it lives

| Part | Files |
|---|---|
| image format | `src/nodecfg/FwImage.h` |
| frames | `src/espnow/EspNowProto.h` |
| collector store + rules | `src/nodes/NodeFwStore.{h,cpp}`, `src/nodes/NodeFwRules.h` |
| collector HTTP | `src/web/NodeFwApi.{h,cpp}`, ingest reply in `src/web/IngestHandler.cpp` |
| collector radio | `src/espnow/EspNowIngest.cpp` |
| collector UI | `www/pages/settings_nodes.html`, `www/js/nodes.js`, `www/i18n/nodes.js` |
| WiFi node | `node/src/main.cpp`, `node/src/ConfigPortal.cpp`, `node/src/FwFlash.{h,cpp}`, `node/src/FwOffer.h` |
| ESP-NOW node | `node_espnow/src/FwFetch.{h,cpp}`, `node_espnow/src/FwTrial.h`, `node_espnow/src/main.cpp`, `node_espnow/src/Link.cpp`, `node_espnow/src/Portal.cpp` |
| node page | `node_portal/`, regenerated into `src/nodecfg/NodePortalPage.h` |
| tests | `tests/host/test_node_fw.cpp`, `tests/host/test_node_fw_offer.cpp`, `tests/web/drive_node_portal.py` |
