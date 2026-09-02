# Kindle FBInk Dashboard

Direct framebuffer weather dashboard for jailbroken Kindle devices.
Fetches data from an ESP32_Logger collector and renders with FBInk.

## Requirements

- Jailbroken Kindle (7th Gen or Paperwhite 4)
- [FBInk](https://github.com/NiLuJe/FBInk) installed
- ESP32_Logger collector on the same network

## Installation

**The folder has to go in `/mnt/us/extensions/`, and the name of the folder is
up to you.** KUAL builds its menu by scanning the subdirectories of
`/mnt/us/extensions` for a `menu.json`, and it looks nowhere else — an earlier
version of this file said to install to `/mnt/us/dashboard/`, which is why the
entry never appeared in the launcher.

1. Copy the whole `kindle/` folder to `/mnt/us/extensions/esp32dash/`:
   ```
   ssh root@kindle-ip mkdir -p /mnt/us/extensions/esp32dash
   scp -r kindle/* root@kindle-ip:/mnt/us/extensions/esp32dash/
   ```
   Over USB instead: mount the Kindle and copy `kindle/` into
   `extensions/`, renaming it `esp32dash`.

   The script finds its own layout, icons and fonts relative to wherever it is
   installed, so the folder name and location are free — this one only has to
   be under `extensions/` for KUAL to see it.

2. Edit `update_dash.sh` and set `HOST` to your ESP32's IP address.

3. Ensure FBInk is installed and in PATH.

4. On the ESP32 WebUI, go to Settings → E-ink Dashboard and set the
   FBInk resolution to match your Kindle.

5. Restart KUAL (leave the launcher and open it again). It reads the extension
   list once at startup, so a folder added while it is open does not appear.

## Usage

**From KUAL:** open the launcher → **ESP32 Dashboard** → *Start Dashboard*.
*Stop Dashboard* ends it and restores the screensaver.

**From a shell**, if you prefer:

```bash
ssh root@kindle-ip
sh /mnt/us/extensions/esp32dash/start.sh
sh /mnt/us/extensions/esp32dash/stop.sh
```

Note the `sh`. `/mnt/us` is a FAT filesystem with no execute bit to set, so
`./start.sh` may or may not run depending on how the firmware mounted it —
which is also why `menu.json` invokes `/bin/sh` rather than the scripts
directly.

## It does not appear in KUAL

In order of how often each one is the answer:

1. **Wrong folder.** It must be `/mnt/us/extensions/<anything>/`, with
   `menu.json` directly inside it — not in a subfolder.
2. **KUAL was already open.** It enumerates extensions at startup; leave it
   and reopen.
3. **CRLF line endings.** Copying through a Windows editor can rewrite
   `menu.json`, and KUAL's parser rejects it silently, listing nothing. The
   repository pins these files to LF; check with `file menu.json` if in doubt.
4. **Truncated copy.** `menu.json` must be valid JSON — an interrupted `scp`
   leaves a file that parses as nothing.

## Custom Fonts

Drop `.ttf` files into the `fonts/` directory to override system fonts.
The script looks for `Bookerly-Regular.ttf` and `Bookerly-Bold.ttf`.

## Refresh Strategy

| Interval | Action |
|---|---|
| 1 min | Clock update (partial refresh) |
| 5 min | Fetch data + graph, update text |
| 10 min | Clear clock zone (anti-ghosting) |
| 30 min | Full screen flash (clear all ghosting) |
