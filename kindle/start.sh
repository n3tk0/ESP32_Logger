#!/bin/sh
# start.sh — Launch the dashboard in the background.
# Run from USB shell or KUAL: cd /mnt/us/dashboard && ./start.sh
cd "$(dirname "$0")"
nohup ./update_dash.sh > /tmp/dash.log 2>&1 &
echo "Dashboard started (PID $!). Log: /tmp/dash.log"
echo "Stop with: killall update_dash.sh"
