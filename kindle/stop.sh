#!/bin/sh
# stop.sh — stop the dashboard and give the Kindle its normal behaviour back.

PIDFILE=/tmp/dash.pid

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

lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
rm -rf /tmp/dash
fbink -c 2>/dev/null
echo "Dashboard stopped."
