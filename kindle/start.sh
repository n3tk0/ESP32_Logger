#!/bin/sh
# start.sh — launch the dashboard in the background.
#
# Reached from KUAL (ESP32 Dashboard → Start Dashboard) or from a shell:
#   sh /mnt/us/extensions/esp32dash/start.sh
#
# Invoked with `sh` rather than `./`, here and in menu.json, because /mnt/us is
# a FAT filesystem: it has no execute bit to set, so whether `./update_dash.sh`
# runs at all depends on the mount options of the particular firmware. `sh` does
# not care.
cd "$(dirname "$0")" || exit 1

PIDFILE=/tmp/dash.pid

# Already running? Starting a second copy leaves two processes drawing to the
# same framebuffer on different timers, which looks like a corrupted panel
# rather than like the mistake it is.
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "Dashboard already running (PID $(cat "$PIDFILE")). Stop it first."
    exit 0
fi

nohup sh ./update_dash.sh > /tmp/dash.log 2>&1 &
echo $! > "$PIDFILE"

echo "Dashboard started (PID $(cat "$PIDFILE")). Log: /tmp/dash.log"
echo "Stop it from KUAL, or with: sh $(pwd)/stop.sh"
