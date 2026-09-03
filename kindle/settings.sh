#!/bin/sh
# ============================================================================
# settings.sh — change the dashboard's settings from the Kindle itself
#
#   sh settings.sh show                 what is set now, on screen and stdout
#   sh settings.sh get KEY
#   sh settings.sh set KEY VALUE
#   sh settings.sh profile saver|normal|fast
#   sh settings.sh find                 look for the collector on this network
#   sh settings.sh next                 take the next collector that scan found
#   sh settings.sh reset                back to dash.conf.default
#
# Reached from KUAL: ESP32 Dashboard → Settings → …
#
# WHY THIS EXISTS. The collector's address was a line in the middle of
# update_dash.sh, which meant changing it needed a text editor, a USB cable and
# a computer — for the one setting that changes when a router hands out a new
# lease. A Kindle has no keyboard outside its own reader, so `find` scans the
# network instead of asking anyone to type an address, and everything else is
# a menu entry that steps a value.
#
# It shares update_dash.sh's helpers rather than repeating them: the validation
# that keeps a bad interval out of the loop is the same code that reads the file
# back, so the two cannot drift apart.
# ============================================================================
SELF_DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || SELF_DIR=$(dirname "$0")
DASH_LIB_ONLY=1 . "$SELF_DIR/update_dash.sh"

TMP="${DASH_TMP:-/tmp/dash}"
# Beside dash.conf, NOT under /tmp/dash: stop.sh and the dashboard's own
# cleanup() both `rm -rf` that directory, so a scan run before pressing Stop
# left "Next collector" with nothing to step through — and a reboot cleared it
# anyway, /tmp being a ramdisk. The list is as durable as the address it feeds.
SCAN_LIST="${DASH_SCAN_LIST:-$SELF_DIR/collectors}"
mkdir -p "$TMP" 2>/dev/null

conf_init
conf_load

# ── Saying things ────────────────────────────────────────────────────────────
# To stdout always (KUAL shows nothing, but a shell does), and to the panel when
# FBInk is there and a font was found — a KUAL action that reports only into a
# log nobody opens is a button that appears to do nothing.
have_fbink() { command -v fbink >/dev/null 2>&1; }

# A running dashboard re-reads dash.conf every minute and repaints when it finds
# this file, so a change applies — and the settings screen this script painted
# over the page goes away — within one tick. Nothing has to be restarted.
#
# ALWAYS CALLED AFTER say_lines, never before. say_lines clears and flashes the
# whole panel, which is a second or two of e-ink; with the flag already down,
# a tick landing in that window would consume it, repaint the dashboard, and
# then have the settings page painted on top of it with no flag left to undo
# that — the page would sit there until the next full refresh, an hour later.
ask_redraw() { : > "$TMP/redraw" 2>/dev/null; }

say_lines() {
    # Reads lines from stdin, prints them, and paints them on the screen.
    local line y=60
    local tmpf="$TMP/say.$$"
    cat > "$tmpf"
    cat "$tmpf"
    if have_fbink && font_setup 2>/dev/null; then
        clear_screen
        draw_text_bold 40 24 26 BLACK "ESP32 Dashboard"
        while IFS= read -r line; do
            draw_text_reg 40 "$y" 18 BLACK "$line"
            y=$((y + 26))
        done < "$tmpf"
        draw_text_reg 40 "$((y + 16))" 14 GRAY5 "KUAL → ESP32 Dashboard → Start Dashboard"
        refresh_screen
    fi
    rm -f "$tmpf"
}

# ── show ─────────────────────────────────────────────────────────────────────
cmd_show() {
    local k v
    {
        for k in $(conf_keys); do
            eval "v=\$$k"
            printf '%-18s %s\n' "$k" "$v"
        done
    } | say_lines
}

# ── get / set ────────────────────────────────────────────────────────────────
cmd_get() {
    local k="$1" v
    case " $(conf_keys) " in
        *" $k "*) eval "v=\$$k"; echo "$v" ;;
        *) echo "unknown setting: $k" >&2; return 1 ;;
    esac
}

cmd_set() {
    local k="$1" v="$2"
    case " $(conf_keys) " in
        *" $k "*) ;;
        *) echo "unknown setting: $k" >&2; return 1 ;;
    esac
    # 08 is not a number the shell can divide by. Normalise before validating,
    # so `set DATA_EVERY 05` stores 5 rather than a value that would break the
    # loop the first time it came round.
    case "$k" in
        HOST) ;;
        *) v=$(strip_zeros "$v") ;;
    esac
    if ! conf_valid "$k" "$v"; then
        printf '%s is not a valid %s\n' "$v" "$k" | say_lines
        return 1
    fi
    eval "$k=\$v"
    conf_write || { echo "could not write $CONF" >&2; return 1; }
    printf '%s = %s\n\nThe dashboard picks this up within a minute.\n' "$k" "$v" | say_lines
    ask_redraw
}

# ── profiles ─────────────────────────────────────────────────────────────────
# Named sets of the four intervals, because "how often should the chart be
# redrawn" is not a question anyone wants to answer four times with a menu that
# can only step numbers.
cmd_profile() {
    case "$1" in
        saver)   CLOCK_EVERY=5;  DATA_EVERY=15; GRAPH_EVERY=30; FORECAST_EVERY=60; FULL_EVERY=120 ;;
        normal)  CLOCK_EVERY=1;  DATA_EVERY=5;  GRAPH_EVERY=15; FORECAST_EVERY=30; FULL_EVERY=60 ;;
        fast)    CLOCK_EVERY=1;  DATA_EVERY=2;  GRAPH_EVERY=5;  FORECAST_EVERY=15; FULL_EVERY=30 ;;
        *) echo "profiles: saver, normal, fast" >&2; return 1 ;;
    esac
    conf_write || return 1
    printf 'Refresh profile: %s\n\nclock %s min  data %s min\nchart %s min  forecast %s min\nfull screen %s min\n\nApplies within a minute.\n' \
        "$1" "$CLOCK_EVERY" "$DATA_EVERY" "$GRAPH_EVERY" "$FORECAST_EVERY" "$FULL_EVERY" | say_lines
    ask_redraw
}

# ── finding the collector ────────────────────────────────────────────────────
# The Kindle's own address gives the subnet; every host on it is asked for
# /kindle/data and the ones that answer with a dashboard payload are collectors.
# In batches, because 254 sequential probes at a two-second timeout is eight
# minutes and nobody waits that long in front of a menu.
own_ipv4() {
    local ip
    ip=$(ifconfig 2>/dev/null | sed -n 's/.*inet addr:\([0-9.][0-9.]*\).*/\1/p' \
         | grep -v '^127\.' | head -1)
    [ -n "$ip" ] && { echo "$ip"; return 0; }
    ip=$(ip -4 addr show 2>/dev/null | sed -n 's#.*inet \([0-9.][0-9.]*\)/.*#\1#p' \
         | grep -v '^127\.' | head -1)
    echo "$ip"
}

probe_host() {
    # A collector is a host that answers /kindle/data with a dashboard payload.
    # Anything else on the network answering port 80 is not one, so the body is
    # checked rather than the status code.
    wget -q -T "${SCAN_TIMEOUT:-2}" -O - "http://$1/kindle/data" 2>/dev/null \
        | grep -q '^RES_W=' 2>/dev/null
}

cmd_find() {
    local ip base i batch found first
    ip=$(own_ipv4)
    case "$ip" in
        *.*.*.*) ;;
        *) printf 'No network address.\n\nJoin WiFi first.\n' | say_lines; return 1 ;;
    esac
    base=${ip%.*}

    printf 'Looking for a collector on %s.0/24…\n' "$base" | say_lines
    : > "$SCAN_LIST"

    i=1
    while [ "$i" -le 254 ]; do
        batch=0
        while [ "$batch" -lt "${SCAN_BATCH:-24}" ] && [ "$i" -le 254 ]; do
            ( probe_host "$base.$i" && echo "$base.$i" >> "$SCAN_LIST" ) &
            i=$((i + 1))
            batch=$((batch + 1))
        done
        wait
    done

    found=$(wc -l < "$SCAN_LIST" 2>/dev/null | tr -d ' ')
    [ -n "$found" ] || found=0
    if [ "$found" = "0" ]; then
        printf 'No collector answered on %s.0/24.\n\nIs it powered and on this WiFi?\nSet the address by hand in dash.conf if it\nis on another network.\n' \
            "$base" | say_lines
        return 1
    fi

    first=$(head -1 "$SCAN_LIST")
    HOST="$first"
    conf_write
    printf 'Collector: %s\n\n%s host(s) answered. If this is the wrong\none, use Settings → Next collector.\n\nApplies within a minute.\n' \
        "$first" "$found" | say_lines
    ask_redraw
}

cmd_next() {
    # Steps to the next address the last scan found, wrapping round. Keyboard-
    # free by construction: pressing the menu entry again is the whole
    # interaction, and the screen says which one is current.
    local cur next
    if [ ! -s "$SCAN_LIST" ]; then
        printf 'No scan results.\n\nRun Settings → Find collector first.\n' | say_lines
        return 1
    fi
    cur=${HOST#http://}
    cur=${cur#https://}
    cur=${cur%%/*}
    next=$(awk -v cur="$cur" '
        { hosts[NR] = $0 }
        END {
            for (i = 1; i <= NR; i++) if (hosts[i] == cur) { print hosts[i % NR + 1]; exit }
            print hosts[1]
        }' "$SCAN_LIST")
    [ -n "$next" ] || return 1
    HOST="$next"
    conf_write
    printf 'Collector: %s\n\nApplies within a minute.\n' "$next" | say_lines
    ask_redraw
}

# ── reset ────────────────────────────────────────────────────────────────────
cmd_reset() {
    if [ -f "$CONF_DEFAULT" ]; then
        cp "$CONF_DEFAULT" "$CONF" || return 1
    else
        return 1
    fi
    conf_load
    printf 'Settings reset to defaults.\n\nCollector: %s\n\nApplies within a minute.\n' "$HOST" | say_lines
    ask_redraw
}

# ── Dispatch ─────────────────────────────────────────────────────────────────
case "${1:-show}" in
    show)    cmd_show ;;
    get)     shift; cmd_get "$@" ;;
    set)     shift; cmd_set "$@" ;;
    profile) shift; cmd_profile "$@" ;;
    find)    cmd_find ;;
    next)    cmd_next ;;
    reset)   cmd_reset ;;
    *)
        echo "usage: settings.sh {show|get KEY|set KEY VALUE|profile NAME|find|next|reset}" >&2
        exit 2
        ;;
esac
