# Kindle dashboard updates through the collector: the contract

How the Kindle extension (`kindle/`, installed on a reader as
`/mnt/us/extensions/esp32dash`) is replaced remotely, from the collector's
**E-ink dashboard** page, the way the sensor nodes' firmware is
([NODE_OTA.md](NODE_OTA.md)). Every piece of code that takes part implements a
part of this file; when they disagree, this document wins.

## 0. Principles

1. **The reader always pulls.** The collector never connects to a Kindle. A
   reader learns about an update from the `/kindle/data` payload it fetches
   anyway (§3), and downloads the package itself.
2. **One package, every reader.** The collector keeps at most one package on
   its SD card. When it is offered, every reader that runs a different version
   installs it on its next fetch.
3. **SD card only.** The package is ~550 KB; LittleFS is not a place for it.
   Without a card every mutating call answers `409 {"error":"no_sd"}`.
4. **Checked twice.** The collector refuses anything that is not a package
   before it can be offered (§1); the reader checks the download again, all of
   it, before it touches its own folder (§4). The reader's checks are the ones
   that make the install safe — it runs as root, on a file that came over HTTP.
5. **The reader's own files are never replaced.** `dash.conf`, `collectors`,
   `kual.log` and `last.txt` are refused in a package on both ends.
6. **A refusal changes nothing.** Every failed check leaves the reader's folder
   exactly as it was, and the running dashboard keeps running.
7. **Older scripts are left alone.** A reader whose script predates this sends
   no `?pkg=`, is never offered anything, and needs one install by hand
   (MRPI package or zip, [kindle/README.md](../kindle/README.md)).

## 1. What a package is

`esp32dash-kindle-<version>.tar`, built by `tools/mk_kindle_package.sh` from
every file git tracks under `kindle/` (except `kindle/package/`) plus a
`VERSION` file, and attached to each release by
`.github/workflows/build-kindle-package.yml`.

Checked by `src/web/KindlePkgTar.h` (streaming, while the upload is written)
and tested by `tests/host/test_kindle_pkg.cpp`:

- a POSIX **ustar** archive, uncompressed, header checksums valid, ending in
  two zero blocks (record padding after them is fine);
- **regular files and directories only** — no links, devices, FIFOs, GNU
  long-name or pax records (`bad_entry`);
- every path under `esp32dash/`, components of `[A-Za-z0-9._-]`, none of them
  empty, `.` or `..`, and none of the reader's own files (`bad_path`);
- `esp32dash/update_dash.sh`, `start.sh`, `stop.sh`, `menu.json` and `VERSION`
  present (`missing_files`);
- `VERSION`'s first line is the version: `[A-Za-z0-9._+-]`, at most 47
  characters (`bad_version`);
- at most 1.5 MB (`too_big`). Anything else is `not_package` or `truncated`.

The `.bin` (MRPI) and the `.zip` are refused as `not_package`: they are for
the first install, not for this.

## 2. On the collector

### 2.1 Storage (SD, `sdAvailable`)

```
/kindlepkg/esp32dash.tar   the package
/kindlepkg/pkg.json        {"ver","size","md5","uploaded","offer"}
/kindlepkg/upload.tmp      an upload in flight
```

An upload is streamed to `upload.tmp` while the MD5 and the §1 check run over
it; the first wrong header refuses it. It becomes `esp32dash.tar` only when
every check passed, and is stored **unoffered**. The package it replaces steps
aside to `esp32dash.old` and comes back if the rename fails. At boot
`pkg.json` is believed only if the tar beside it has the size it records.
Every SD access holds `fsMutex`. Code: `src/web/KindlePkg.cpp`.

### 2.2 Readers

Remembered in RAM only, by address — up to four, the one heard from longest
ago giving way — with the version each reported and its update status:

| st        | meaning                                                          |
|-----------|------------------------------------------------------------------|
| `current` | runs the stored package's version                                |
| `pending` | offered on its last fetch                                        |
| `sending` | it asked for `/kindle/pkg.tar`                                   |
| `failed`  | it reported a refusal (`err`, one of the §4 words)               |
| `idle`    | nothing is on offer for it                                       |
| `old`     | it sent no `?pkg=`: a script from before this, never offered     |

A new upload, and every offer/stop, clears `failed`.

### 2.3 HTTP API (behind the web UI's auth)

| Method | Path | Body | Answer |
|---|---|---|---|
| GET  | `/api/kindle/pkg` | — | `{"sd":bool,"pkg":{"ver","size","md5","uploaded"}\|null,"offer":bool,"readers":[{"ip","ver","seen","st","err"?}]}` (`seen` in seconds) |
| POST | `/api/kindle/pkg/upload` | multipart, field `pkg` | `{"ok":true,"ver","size"}`; 400 `{"error":…}` with a §1 word; 409 `no_sd` / `busy`; 500 `write_failed` |
| POST | `/api/kindle/pkg` | form `action=offer` | `{"ok":true}`; 409 `no_pkg` |
| POST | `/api/kindle/pkg` | form `action=stop` | `{"ok":true}` |
| POST | `/api/kindle/pkg` | form `action=delete` | `{"ok":true}` — the package and its offer |
| GET  | `/kindle/pkg.tar` | — | the package, `x-MD5`, for a reader. No token: a reader has none, and the bytes are the public extension |

The download reads the card a chunk at a time under `fsMutex`, against the
serial of the package it started on: a package replaced or deleted mid-way
ends the body short, which the reader's size and MD5 checks refuse.

## 3. The wire

Every `/kindle/data` request from a current script carries:

- `pkg=<VERSION>` — the first line of the reader's `VERSION`, `+` sent as
  `%2B`; `none` when the folder has no usable `VERSION` (a copy from a
  checkout). A reader sending `none` is offered the package.
- `pkgerr=<word>&pkgfor=<md5>` — while the last package it tried this run
  was refused, and which one. The collector counts the refusal only when
  `pkgfor` is the stored package's MD5, so the fetch that brings a new
  upload's offer does not mark it refused for the old one's reason.

When a package is stored, offered, and the reader's `pkg` differs from its
version, the payload carries, before `END=1`:

```
PKG_VER="<version>"
PKG_MD5="<32 hex>"
PKG_SIZE=<bytes>
```

`payload_key_ok()` in `update_dash.sh` accepts exactly these three, and
`zones_forget()` clears them before every load, so an offer that stops is
forgotten on the next fetch.

## 4. On the reader (`kindle/update_dash.sh`)

`pkg_check()` runs at the end of `load_data()`, and only:

- in the dashboard's own process (`PKG_MAIN=1` — `settings.sh` sources the
  same file and fetches too);
- for a payload fetched this run (`PKG_FRESH`), never for the cached page;
- when `PKG_VER` differs from its own version;
- once per offered MD5 per run: a refused package is not retried until the
  dashboard restarts, or a different package is offered.

`pkg_install()` then, in order — each failure is the word in brackets, and
leaves the folder untouched:

1. checks the offer's own values (`bad_offer`, `too_big`);
2. downloads `/kindle/pkg.tar` into `/tmp/dash` (`download`), checks its size
   (`size`) and MD5 (`md5`; `no_md5sum` on a reader without the applet);
3. lists it with `tar -tv` **before unpacking**: only `-` and `d` entries
   (`bad_entry`), only plain names under `esp32dash/` and none of the reader's
   own files (`bad_path`). A tar that cannot list modes fails here, which is
   the safe way to fail;
4. unpacks into `/tmp/dash/pkg` (`unpack`) and looks for any link that landed
   anyway (`bad_entry`);
5. checks the required files (`missing_files`), that `VERSION` is `PKG_VER`
   (`version`), and that every `*.sh` passes `sh -n` (`syntax`);
6. copies every file beside its target as `<name>.new` — the step a full
   volume can fail (`write`, and every `.new` is removed) — and only then
   renames each over the old one. Renamed, not written in place: the running
   shell still has the old `update_dash.sh` open.

On success it writes a line to `kual.log` and `pkg_restart()` runs
`handback()` — everything Stop gives back: the screen, the framework, the
radio, the rotation — and `exec`s the new `update_dash.sh` in the same
process, so `/tmp/dash.pid` still names it and Stop still works. The new copy
starts like a fresh Start: cached page first, then its own fetch, which
reports the new version and reads as `current`.

On a refusal it logs the word to `kual.log` and `/tmp/dash.log`, and sends it
as `pkgerr` on every fetch.

Files that the new package no longer carries are left where they are.

## 5. Testing

- `tests/host/test_kindle_pkg.cpp` — the collector's §1 check, fed in every
  split, against every refusal.
- `tests/kindle/drive_dash.sh`, "Updating itself from the collector" — the
  reader side against the package the build makes: a good install, and a bad
  MD5, the wrong version, a symlink, a `dash.conf`, a script that does not
  parse and a short download, each leaving the folder as it was.
- `tests/web/drive_kindle_page.py` — the card on the E-ink dashboard page.
