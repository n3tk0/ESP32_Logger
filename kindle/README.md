# Kindle FBInk Dashboard

Direct framebuffer weather dashboard for jailbroken Kindle devices.
Fetches data from an ESP32_Logger collector and renders with FBInk.

## Requirements

- Jailbroken Kindle (7th Gen or Paperwhite 4)
- [FBInk](https://github.com/NiLuJe/FBInk) installed
- ESP32_Logger collector on the same network

## Installation

1. Copy this entire `kindle/` folder to `/mnt/us/dashboard/` on the Kindle:
   ```
   scp -r kindle/* root@kindle-ip:/mnt/us/dashboard/
   ```

2. Edit `update_dash.sh` and set `HOST` to your ESP32's IP address.

3. Ensure FBInk is installed and in PATH.

4. On the ESP32 WebUI, go to Settings → E-ink Dashboard and set the
   FBInk resolution to match your Kindle.

## Usage

```bash
# Start the dashboard:
ssh root@kindle-ip
cd /mnt/us/dashboard
./start.sh

# Stop:
./stop.sh
```

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
