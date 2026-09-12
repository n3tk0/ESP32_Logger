#!/bin/sh
# ============================================================================
# settings.sh — change the dashboard's settings from the Kindle itself
#
#   sh settings.sh show                 what is set now, on screen and stdout
#   sh settings.sh get KEY
#   sh settings.sh set KEY VALUE
#   sh settings.sh profile saver|normal|fast|days
#   sh settings.sh power awake|wifi|suspend|days|deep
#   sh settings.sh quiet night|off|FROM TO
#   sh settings.sh find                 look for the collector on this network
#   sh settings.sh next                 take the next collector that scan found
#   sh settings.sh diag                 what the device thinks is going on
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

# ── WHICH PANEL THIS IS ──────────────────────────────────────────────────────
# Every screen this script draws was laid out in 600x800 pixels and drawn at
# those coordinates whatever the reader — so on a 1072x1448 Paperwhite the
# settings pages came up as small type in the top-left corner of a mostly empty
# screen. The resolution is a value the collector sends, and the dashboard now
# keeps its last payload beside dash.conf, so it is here to be read without
# asking anybody.
#
# Read directly rather than through cache_load(), which would copy over the
# running dashboard's own payload in /tmp while it is using it.
load_kv "${DASH_CACHE:-$SELF_DIR/last.txt}" PAYLOAD 2>/dev/null
load_layout
# Type sizes as hundredths, so one number scales every screen here.
UI_S=$(( ${RES_W:-600} * 100 / 600 ))
[ "$UI_S" -ge 100 ] 2>/dev/null || UI_S=100
ui() { echo $(( $1 * UI_S / 100 )); }

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
    local line y tmpf="$TMP/say.$$"
    local pad=$(ui 40) top=$(ui 60) sz=$(ui 18) step=$(ui 26)
    cat > "$tmpf"
    cat "$tmpf"
    if have_fbink && font_setup 2>/dev/null; then
        # CLOSER TOGETHER WHEN ONE SCREEN WILL NOT HOLD THE PAGE, rather than
        # split into columns. `show` lists every key in conf_keys(), and that
        # list has grown past what a 600x800 panel fits at this step: the last
        # few were drawn below the bottom edge, where FBInk clips them and the
        # screen simply does not mention them.
        #
        # Two columns was the first answer and the wrong one: half the width is
        # not enough for `MENU_LBL  Refresh|Awake/Sleep|More|Exit`, and a
        # settings page that clips the VALUE is worse than one that clips a
        # row — the value is the whole reason to look. Full width, and the
        # step shrinks to fit however many rows there are.
        local n avail
        n=$(wc -l < "$tmpf" | tr -dc '0-9')
        [ -n "$n" ] || n=1
        [ "$n" -ge 1 ] || n=1
        avail=$(( ${RES_H:-800} - top - $(ui 60) ))
        if [ "$(( n * step ))" -gt "$avail" ]; then
            step=$(( avail / n ))
            # The type keeps its proportion to the line it sits on, with a
            # floor: below about this a 167 ppi panel is not being read from
            # anywhere, and a page nobody can read is not an improvement on a
            # page that was cut off.
            sz=$(( step * 70 / 100 ))
            [ "$sz" -lt "$(ui 11)" ] && sz=$(ui 11)
            [ "$step" -lt 1 ] && step=1
        fi
        clear_screen
        draw_text_bold "$pad" "$(ui 24)" "$(ui 26)" BLACK "ESP32 Dashboard"
        y=$top
        while IFS= read -r line; do
            draw_text_reg "$pad" "$y" "$sz" BLACK "$line"
            y=$((y + step))
        done < "$tmpf"
        draw_text_reg "$pad" "$((y + $(ui 16)))" "$(ui 14)" GRAY5 \
                      "Tap the screen for the menu · KUAL → ESP32 Dashboard"
        refresh_screen
    fi
    rm -f "$tmpf"
}

# One line, redrawn in place, on the page that is already up.
#
# say_lines() clears and flashes the whole panel, which is right for a screen
# of settings and wrong for progress: a scan of 254 addresses reporting as it
# went would have flashed the screen eleven times. This paints over its own
# rectangle and refreshes that alone.
say_progress() {
    have_fbink || return 0
    [ -n "${FONT_REG:-}" ] || return 0
    local y=$(( ${RES_H:-800} / 6 )) w=$(( ${RES_W:-600} - $(ui 80) ))
    local h=$(ui 30) x=$(ui 40)
    echo "$1"
    fill_rect "$x" "$y" "$w" "$h" WHITE
    draw_text_reg "$x" "$y" "$(ui 18)" BLACK "$1"
    refresh_zone "$x" "$y" "$w" "$h" 0
    return 0
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
    if conf_is_text "$k"; then :; else
        v=$(strip_zeros "$v")
    fi
    if ! conf_valid "$k" "$v"; then
        printf '%s is not a valid %s\n' "$v" "$k" | say_lines
        return 1
    fi
    eval "$k=\$v"
    conf_write || { echo "could not write $CONF" >&2; return 1; }
    printf '%s = %s\n\nThe dashboard picks this up within a minute.\n' "$k" "$v" | say_lines
    ask_redraw
}

# ── power ────────────────────────────────────────────────────────────────────
# The battery settings, as three named choices rather than three keys to type.
#
# THEY WERE UNREACHABLE. `set KEY VALUE` has always existed here, but kual-run.sh
# forwards a fixed list of commands and menu.json lists a fixed set of entries,
# and neither had grown POWER, WIFI_WAIT or GUI_STOP. The documentation said
# "try wifi first, and suspend when you are ready", which on the device meant a
# USB cable and a text editor — the exact thing this file's header says the
# whole settings subsystem exists to remove. They are the settings most likely
# to be tried, reverted and tried again, so they are the last ones that should
# need a computer.
# The four intervals, as one named set. Shared by `profile` and by the battery
# modes below, which have to be able to set them: see cmd_power's `days`.
set_intervals() {
    case "$1" in
        days)    CLOCK_EVERY=15; DATA_EVERY=30; GRAPH_EVERY=60; FORECAST_EVERY=60; FULL_EVERY=240 ;;
        saver)   CLOCK_EVERY=5;  DATA_EVERY=15; GRAPH_EVERY=30; FORECAST_EVERY=60; FULL_EVERY=120 ;;
        normal)  CLOCK_EVERY=1;  DATA_EVERY=5;  GRAPH_EVERY=15; FORECAST_EVERY=30; FULL_EVERY=60 ;;
        fast)    CLOCK_EVERY=1;  DATA_EVERY=2;  GRAPH_EVERY=5;  FORECAST_EVERY=15; FULL_EVERY=30 ;;
        *) return 1 ;;
    esac
    return 0
}

cmd_power() {
    case "$1" in
        awake)   POWER=awake;   GUI_STOP=0 ;;
        wifi)    POWER=wifi;    GUI_STOP=0 ;;
        suspend) POWER=suspend; GUI_STOP=0 ;;
        # ── THE ONE THAT ACTUALLY BUYS DAYS ─────────────────────────────────
        # POWER=suspend on its own does not, and the menu entry that set it
        # said it did. With CLOCK_EVERY=1 the panel suspends and comes back
        # sixty times an hour, and every one of those is a resume, a draw and a
        # flashing refresh of the clock — 1440 of each a day. The saving is in
        # not waking up, so the mode that promises days has to slow the clock
        # down as well, and this is the setting that does both in one write.
        days)    POWER=suspend; GUI_STOP=0; set_intervals days ;;
        # The framework is the one that makes the device stop being a reader,
        # and takes KUAL — this menu — with it. It is offered, and it says so.
        deep)    POWER=suspend; GUI_STOP=1 ;;
        *) echo "power: awake, wifi, suspend, days, deep" >&2; return 1 ;;
    esac
    conf_write || return 1
    if [ "$1" = "days" ]; then
        # Named with the reader's own word for that button, because the whole
        # point of this screen is to say how to get back out of the setting.
        menu_word wake || MENU_WORD="Awake"
        printf 'Battery: deep sleep\n\nThe panel suspends between updates and\nwakes rarely: clock %s min, readings %s min.\n\nPress the power button to wake it and get\nthe menu; tap %s to stop sleeping.\n\nApplies within a minute.\n' \
            "$CLOCK_EVERY" "$DATA_EVERY" "$MENU_WORD" | say_lines
        ask_redraw
        return 0
    fi
    if [ "$GUI_STOP" = "1" ]; then
        printf 'Battery: %s\n\nThe reader framework will be stopped, and\nthis menu goes with it.\n\nTo undo: run Stop over USB, or hold the\npower button until the Kindle reboots.\n\nApplies within a minute.\n' \
            "$1" | say_lines
    else
        printf 'Battery: %s\n\nPOWER %s  GUI_STOP %s\n\nApplies within a minute.\n' \
            "$1" "$POWER" "$GUI_STOP" | say_lines
    fi
    ask_redraw
}

# ── profiles ─────────────────────────────────────────────────────────────────
# Named sets of the four intervals, because "how often should the chart be
# redrawn" is not a question anyone wants to answer four times with a menu that
# can only step numbers.
cmd_profile() {
    if ! set_intervals "$1"; then
        echo "profiles: saver, normal, fast, days" >&2
        return 1
    fi
    conf_write || return 1
    printf 'Refresh profile: %s\n\nclock %s min  data %s min\nchart %s min  forecast %s min\nfull screen %s min\n\nApplies within a minute.\n' \
        "$1" "$CLOCK_EVERY" "$DATA_EVERY" "$GRAPH_EVERY" "$FORECAST_EVERY" "$FULL_EVERY" | say_lines
    ask_redraw
}

# ── quiet hours ──────────────────────────────────────────────────────────────
# Two keys, and nobody should have to set them one at a time from a menu that
# can only step numbers — `night` is what almost everybody means.
cmd_quiet() {
    case "${1:-}" in
        night)  QUIET_FROM=22; QUIET_TO=7 ;;
        off)    QUIET_FROM=0;  QUIET_TO=0 ;;
        *)
            if conf_valid QUIET_FROM "${1:-}" && conf_valid QUIET_TO "${2:-}"; then
                QUIET_FROM="$1"; QUIET_TO="$2"
            else
                echo "quiet: night, off, or two hours (0-23)" >&2
                return 1
            fi ;;
    esac
    conf_write || return 1
    if [ "$QUIET_FROM" = "$QUIET_TO" ]; then
        printf 'Quiet hours: off\n\nThe panel flashes and refreshes on its\nusual schedule around the clock.\n\nApplies within a minute.\n' | say_lines
    else
        printf 'Quiet hours: %s:00 to %s:00\n\nNothing flashes between them, and the clock\nis drawn every %s min instead.\n\nOne full refresh clears the night when they\nend.\n\nApplies within a minute.\n' \
            "$QUIET_FROM" "$QUIET_TO" "$QUIET_EVERY" | say_lines
    fi
    ask_redraw
}

# ── what the device thinks is going on ───────────────────────────────────────
#
# THE SAME FACTS AS kual.log, ON THE SCREEN. Every fault reported against this
# extension so far has been an installation one — no FBInk, the wrong folder,
# a collector at another address — and every one of them was diagnosed by
# plugging the reader into a computer and reading a log. This is that log's
# first page, drawn where somebody standing in front of the panel can read it
# without a cable, and it is reachable from the tap menu as well as from KUAL.
cmd_diag() {
    local ip fb dev rtc pid wifi h
    ip=$(own_ipv4); [ -n "$ip" ] || ip="none"
    fb=$(command -v fbink 2>/dev/null); [ -n "$fb" ] || fb="NOT FOUND"
    dev=$(touch_find 2>/dev/null); [ -n "$dev" ] || dev="none found"
    wifi=$(lipc-get-prop com.lab126.wifid cmState 2>/dev/null); [ -n "$wifi" ] || wifi="unknown"
    batt_read; [ -n "$BATT" ] && BATT="$BATT%" || BATT="unknown"
    # Which RTC can actually hold a wake alarm — the question POWER=suspend
    # turns on, and the one nothing could answer from the device before.
    if rtc_pick 2>/dev/null; then rtc="${RTC_WAKEALARM##*/rtc}"; rtc="rtc${rtc%%/*}"
    else rtc="none"
    fi
    pid="not running"
    if [ -f "${DASH_PIDFILE:-/tmp/dash.pid}" ] &&
       kill -0 "$(cat "${DASH_PIDFILE:-/tmp/dash.pid}" 2>/dev/null)" 2>/dev/null; then
        pid="running (PID $(cat "${DASH_PIDFILE:-/tmp/dash.pid}"))"
    fi
    {
        printf 'version    %s\n' "$(cat "$SELF_DIR/VERSION" 2>/dev/null || echo unknown)"
        printf 'dashboard  %s\n' "$pid"
        printf 'panel      %sx%s\n' "${RES_W:-600}" "${RES_H:-800}"
        printf 'fbink      %s\n' "$fb"
        printf 'this Kindle %s  batt %s\n' "$ip" "$BATT"
        printf 'wifi       %s\n' "$wifi"
        printf 'collector  %s\n' "$(host_url)"
        h=${HOST#http://}; h=${h#https://}; h=${h%%/*}
        if probe_host "$h"; then
            printf '           answering\n'
        else
            printf '           NOT answering — try Find collector\n'
        fi
        printf 'touch      %s (TOUCH=%s)\n' "$dev" "$TOUCH"
        printf 'power      %s  wake menu %s  alarm %s\n' "$POWER" "$WAKE_MENU" "$rtc"
        printf 'refresh    clock %s  data %s  chart %s\n' \
               "$CLOCK_EVERY" "$DATA_EVERY" "$GRAPH_EVERY"
        printf 'quiet      %s:00-%s:00\n' "$QUIET_FROM" "$QUIET_TO"
    } | say_lines
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

    # 254 addresses at a two-second timeout is half a minute or more of a
    # screen that does not change, which on e-ink is indistinguishable from a
    # menu entry that did nothing at all — the exact complaint this whole
    # extension's logging exists to answer. So it says where it has got to,
    # into its own rectangle rather than by repainting the page.
    i=1
    while [ "$i" -le 254 ]; do
        batch=0
        while [ "$batch" -lt "${SCAN_BATCH:-24}" ] && [ "$i" -le 254 ]; do
            ( probe_host "$base.$i" && echo "$base.$i" >> "$SCAN_LIST" ) &
            i=$((i + 1))
            batch=$((batch + 1))
        done
        wait
        say_progress "$(printf '%s.1 - %s.%s  ·  %s found' \
                        "$base" "$base" "$((i - 1))" \
                        "$(wc -l < "$SCAN_LIST" 2>/dev/null | tr -dc '0-9')")"
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
    power)   shift; cmd_power "$@" ;;
    profile) shift; cmd_profile "$@" ;;
    quiet)   shift; cmd_quiet "$@" ;;
    diag)    cmd_diag ;;
    find)    cmd_find ;;
    next)    cmd_next ;;
    reset)   cmd_reset ;;
    *)
        echo "usage: settings.sh {show|get KEY|set KEY VALUE|power NAME|profile NAME|quiet NAME|diag|find|next|reset}" >&2
        exit 2
        ;;
esac
