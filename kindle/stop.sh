#!/bin/sh
# stop.sh — stop the dashboard and give the Kindle its normal behaviour back.

PIDFILE="${DASH_PIDFILE:-/tmp/dash.pid}"
DASH_TMP="${DASH_TMP:-/tmp/dash}"

# BY PID, NOT BY NAME. `killall update_dash.sh` only ever worked while the
# script was executed directly; started as `sh ./update_dash.sh` — which is
# what the FAT filesystem forces — the process is named `sh`, so killall
# matched nothing and Stop silently left the dashboard running. Worse, killing
# every `sh` on a Kindle is not an option.
if [ -f "$PIDFILE" ]; then
    PID=$(cat "$PIDFILE")
    kill "$PID" 2>/dev/null

    # Give the trap a moment to run — it restores the screensaver and clears
    # /tmp/dash, which this script cannot do on the dashboard's behalf as
    # tidily. Then insist. The pidfile is only removed once the process is
    # actually gone: leaving it behind while the old copy is still winding down
    # is how Start came to launch a second one on top of it.
    i=0
    while [ $i -lt 10 ] && kill -0 "$PID" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
    done
    kill -9 "$PID" 2>/dev/null
    rm -f "$PIDFILE"
fi

# The fallback, for a dashboard started before this script existed or by hand.
# Matched on the script name so it cannot catch an unrelated shell.
for p in $(ps 2>/dev/null | grep '[u]pdate_dash\.sh' | awk '{print $1}'); do
    kill "$p" 2>/dev/null
done

# ── Hand the device back, whatever state the dashboard left it in ────────────
#
# THE EXIT TRAP IS NOT A RECOVERY PATH, only the tidy case. update_dash.sh puts
# the radio and the reader framework back from cleanup(), which runs on
# SIGTERM — and ten seconds above this line we send SIGKILL to anything that
# did not go, which is exactly the wedged, OOM-killed or hand-killed dashboard
# whose cleanup() never ran. Both things it can leave behind are device-wide
# and outlive the process: a radio turned off stays off, and a stopped reader
# framework stays stopped until a reboot. Neither is something to hand back to
# somebody who just pressed Stop.
#
# FROM THE MARKERS, NOT UNCONDITIONALLY. They say what this dashboard actually
# took, so a reader who runs with POWER=awake and never had GUI_STOP on does not
# get their radio switched on by a script they asked to stop.
lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null

# The framework before the radio: it is what answers com.lab126.cmd on the
# firmware where GUI_STOP takes the radio down with it, so the other order is a
# restore that quietly does nothing.
case "$(cat "$DASH_TMP/gui-stopped" 2>/dev/null)" in
    1) start lab126_gui >/dev/null 2>&1 ;;
    2) killall -CONT cvm 2>/dev/null ;;
esac
[ -f "$DASH_TMP/radio-off" ] && lipc-set-prop com.lab126.cmd wirelessEnable 1 2>/dev/null

rm -rf "$DASH_TMP"
fbink -c 2>/dev/null
echo "Dashboard stopped."
