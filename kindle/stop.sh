#!/bin/sh
# stop.sh — Stop the dashboard and restore normal Kindle behaviour.
killall update_dash.sh 2>/dev/null
lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
rm -rf /tmp/dash
fbink -c
echo "Dashboard stopped."
