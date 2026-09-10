#!/bin/sh
# ============================================================================
# update_dash.sh — FBInk Kindle dashboard
#
# Fetches sensor data from an ESP32 collector and draws it straight to the
# Kindle's framebuffer with FBInk. Built for an always-on, wall-powered panel.
#
# ── Settings ────────────────────────────────────────────────────────────────
# Everything a person needs to change lives in dash.conf beside this script —
# the collector address and the four refresh intervals. settings.sh edits it
# from KUAL, so the device needs no keyboard and this file needs no editing.
#
# ── How the screen is refreshed ─────────────────────────────────────────────
# E-ink ghosts: a partial update leaves a faint impression of what was there
# before, and impressions accumulate. The cure is a flashing update (the panel
# driven to black and back), which is slow and visible — so it is spent where
# it buys the most and withheld where it would only annoy.
#
#   every  CLOCK_EVERY min — the clock rectangle only, with a flashing refresh
#                            of that rectangle: the one region that changes
#                            every minute is also the one that ghosts first,
#                            and a flash 200 px wide is barely noticeable
#   every  DATA_EVERY  min — fetch, then redraw the readings block, one plain
#                            (non-flashing) refresh of that rectangle
#   every  GRAPH_EVERY min — refetch the 24 h chart and blit it
#   every  FORECAST_EVERY  — redraw the forecast, week strip and footer; the
#                            collector polls the weather API on its own
#                            interval (Settings → Forecast), so asking more
#                            often than that just redraws the same values
#   every  FULL_EVERY  min — clear and redraw the whole page with a full
#                            flashing refresh, which resets ghosting entirely
#
# Each tier draws with `fbink -b` (framebuffer only, no refresh) and then
# issues ONE refresh for the rectangle it touched. Refreshing per draw call —
# which is what happens if -b is left off — costs one visible repaint per piece
# of text, twenty of them for a page like this, and every one leaves its own
# ghost.
#
# ── A note on the FBInk command line ────────────────────────────────────────
# This file used to invoke flags that do not exist: `-p` for pixel coordinates
# (it means --padded), `-M` for a partial refresh (it means --halfway, which
# centres the text vertically), `-R WxH` for a filled rectangle and `-L W` for
# a line (neither is an FBInk option; -L is --linecountcode), `-F` with a path
# (it names a BUILT-IN font, not a file), and colours GRAY10/GRAY14/GRAY15
# (the scale is GRAY1..GRAY9 then GRAYA..GRAYE). Text was positioned in
# character cells while the layout files are in pixels.
#
# The primitives below use the documented interface: -t/--truetype for text at
# a pixel position, -k/--cls for filled rectangles, -s/--refresh with a region
# for refreshes, -b/--norefresh to batch. See fbink --help.
#
# All temporary files land on /tmp (tmpfs) to spare the eMMC.
# ============================================================================

# ── Where this script lives ──────────────────────────────────────────────────
# Derived, not fixed: a KUAL extension can be installed under any name in
# /mnt/us/extensions/, and the layout, icons, fonts and dash.conf all have to be
# found relative to wherever that turned out to be.
if [ -z "$DASH_DIR" ]; then
    DASH_DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || DASH_DIR=$(dirname "$0")
fi
TMP="${DASH_TMP:-/tmp/dash}"
CONF="${DASH_CONF:-$DASH_DIR/dash.conf}"
CONF_DEFAULT="$DASH_DIR/dash.conf.default"

# ── Settings and their defaults ──────────────────────────────────────────────
# dash.conf overrides these. The names are also the ones settings.sh accepts,
# and conf_keys() below is the single list both ends read.
HOST="http://192.168.1.50"
FETCH_TIMEOUT=10
CLOCK_EVERY=1
DATA_EVERY=5
GRAPH_EVERY=15
FORECAST_EVERY=30
FULL_EVERY=60
CLOCK_FLASH_EVERY=1
SENSOR_FLASH_EVERY=0

# Every FBInk call, into kual.log, and nothing drawn differently.
#
# THE PANEL IS THE ONE RENDERER NOBODY CAN WATCH. The tests drive it against a
# fake FBInk that records its argv, and a browser has a devtools pane — the
# thing on the wall has neither, so a report that a cell "does not look right"
# has no evidence behind it and is argued about from photographs. One line per
# call is the difference between guessing and reading.
#
# Off by default: it is a line per string, sixty a redraw.
TRACE=0

conf_keys() {
    echo "HOST FETCH_TIMEOUT CLOCK_EVERY DATA_EVERY GRAPH_EVERY FORECAST_EVERY FULL_EVERY CLOCK_FLASH_EVERY SENSOR_FLASH_EVERY TRACE"
}

# What each key means, for `settings.sh show` and for dash.conf's comments.
conf_help() {
    case "$1" in
        HOST)               echo "Collector address (IP or http://host:port)" ;;
        FETCH_TIMEOUT)      echo "Seconds to wait for the collector" ;;
        CLOCK_EVERY)        echo "Minutes between clock updates" ;;
        DATA_EVERY)         echo "Minutes between sensor updates" ;;
        GRAPH_EVERY)        echo "Minutes between chart updates" ;;
        FORECAST_EVERY)     echo "Minutes between forecast updates" ;;
        FULL_EVERY)         echo "Minutes between whole-screen refreshes" ;;
        CLOCK_FLASH_EVERY)  echo "Flash the clock zone every N clock updates (0 = never)" ;;
        SENSOR_FLASH_EVERY) echo "Flash the readings zone every N sensor updates (0 = never)" ;;
        TRACE)              echo "Log every FBInk call to kual.log (1 = on)" ;;
        *)                  echo "" ;;
    esac
}

# ── Reading key=value files without executing them ───────────────────────────
# Used for BOTH dash.conf and the collector's payload, and for the payload it is
# the whole defence. `. "$TMP/data.txt"` was doing exactly that — executing it —
# as root. The file arrives over plain HTTP from a device on the network and one
# of the values in it is the forecast provider's free-text summary, so "the
# collector is trusted" was not enough even before considering anyone able to
# answer for it on the wire. A summary of  "; reboot; #  is a command, not a
# description.
#
# Only names matching the convention are accepted, the value is taken literally
# up to the closing quote, and the shell's own unescaping is applied to nothing.
#
# $2 restricts WHICH names may be assigned, and both callers pass one, because
# "a plain variable name" is not a safe thing to let the network choose. PATH
# is a plain variable name; so are IFS, TMP, DASH_DIR and SLEEP_PID. A payload
# carrying PATH=/mnt/us/evil would have the next fbink, wget or date call run
# an attacker's binary as root, and TMP=/mnt/us would turn cleanup()'s
# `rm -rf "$TMP"` into a wipe of the user's documents on Stop.
#
# Pass either a space-separated list of exact names (dash.conf) or the token
# PAYLOAD, which accepts the shapes the collector emits and nothing else.
# The names /kindle/data is allowed to set. Shapes rather than a list, because
# the place keys carry names the collector chooses (Z_<PLACE>_VALUE) — but the
# shapes are narrow, and everything outside them, PATH included, is dropped.
# See kdShellVar() and emitZones() in src/web/KindleDashboard.cpp.
payload_key_ok() {
    case "$1" in
        Z_*|GRID_ZONES|GRID_ROWS|IN_ZONES|LBL_*|FC_*|FC[0-9]_*|WK[0-9]_*|WK_TODAY|WK_MON_MONTH|WK_SUN_MONTH) return 0 ;;
        OUT_*|IN_*|RES_W|RES_H|LANG|DECIMALS|CLOCK_STYLE|SHOW_FLAGS) return 0 ;;
        # What the panel needs to draw the same page the browser draws: the
        # clock's style and format and the sample the collector formatted with
        # them, the two section switches, and whether the chart has a line in
        # it. Exact names, not a shape — each one is read by this script and
        # nothing here wants a family of them.
        CLOCK|CLOCK_ADVW|DATE|TIME_FORMAT) return 0 ;;
        SHOW_CHART|SHOW_WEEK|CHART_OUT|CHART_IN|KEY_OUT_ADVW) return 0 ;;
        # The chart's axis: the five values, the five hours, their widths, and
        # where the image's plot area is inside the image.
        CH_Y[0-9]|CH_Y[0-9]W|CH_H[0-9]|CH_H[0-9]W|CH_L|CH_R|CH_T|CH_B|CH_NOTE) return 0 ;;
    esac
    return 1
}

load_kv() {
    local file="$1" allow="${2:-}"
    local line key val
    [ -f "$file" ] || return 1
    # `|| [ -n "$line" ]`: read returns non-zero on a final line with no
    # trailing newline, having already assigned it. Without this the last
    # setting in a hand-edited dash.conf is silently dropped — and in the
    # collector's payload the last key is RES_H, so a 1072x1448 panel would
    # quietly fall back to the 600x800 layout.
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac
        key=${line%%=*}
        [ "$key" = "$line" ] && continue          # no '=' — not an assignment
        case "$key" in
            *[!A-Za-z0-9_]* | '' ) continue ;;    # not a plain variable name
        esac
        if [ "$allow" = "PAYLOAD" ]; then
            payload_key_ok "$key" || continue
        elif [ -n "$allow" ]; then
            case " $allow " in
                *" $key "*) ;;
                *) continue ;;
            esac
        fi
        val=${line#*=}
        case "$val" in
            '"'*'"' )
                val=${val#\"}
                val=${val%\"}
                # Undo the collector's backslash escaping, literally.
                val=$(printf '%s' "$val" | sed 's/\\\(["\\$`]\)/\1/g')
                ;;
        esac
        eval "$key=\$val"                         # value expanded, never parsed
    done < "$file"
    return 0
}

# ── Settings validation ──────────────────────────────────────────────────────
# Applied on load as well as on save, so a hand-edited dash.conf cannot put the
# loop into a state with no tiers — an interval of 0 or a stray word would
# otherwise make `expr % 0` fail once a minute, forever, silently.
# Leading zeros are stripped before anything does arithmetic with the value.
# $((08)) is an error in every POSIX shell — "value too great for base" — and
# the loop evaluates MINUTE % DATA_EVERY once a minute, so a single
# zero-padded interval in a hand-edited dash.conf would kill every tier.
strip_zeros() {
    local v="$1"
    while [ "${v#0}" != "$v" ] && [ -n "${v#0}" ]; do v="${v#0}"; done
    [ -n "$v" ] || v=0
    printf '%s' "$v"
}

conf_valid() {
    # $1=key $2=value → 0 if acceptable
    local k="$1" v="$2"
    case "$k" in
        HOST)
            case "$v" in
                ''|*' '*|*'"'*|*'`'*|*'$'*|*';'*|*'|'*|*'&'*) return 1 ;;
            esac
            return 0 ;;
        *)
            case "$v" in
                ''|*[!0-9]*) return 1 ;;
            esac
            # Zero disables the two flash counters; every other key needs a tier
            # that actually comes round.
            case "$k" in
                # A switch, not a tier: 0 or 1, and nothing in between to mean.
                TRACE) [ "$v" -le 1 ] ;;
                CLOCK_FLASH_EVERY|SENSOR_FLASH_EVERY) [ "$v" -le 1440 ] ;;
                *) [ "$v" -ge 1 ] && [ "$v" -le 1440 ] ;;
            esac
            ;;
    esac
}

#: Keys already complained about, so a bad line in dash.conf is reported once
#: rather than once a minute for as long as the dashboard runs — on a device
#: whose /tmp is a ramdisk and whose log is never rotated.
CONF_WARNED=""

conf_load() {
    local k v
    [ -f "$CONF" ] || return 0
    # Into the shell only through the whitelist, and only if it passes.
    for k in $(conf_keys); do
        eval "DASH_PREV_$k=\$$k"
    done
    load_kv "$CONF" "$(conf_keys)"
    for k in $(conf_keys); do
        eval "v=\$$k"
        case "$k" in
            HOST) ;;
            *) v=$(strip_zeros "$v"); eval "$k=\$v" ;;
        esac
        if ! conf_valid "$k" "$v"; then
            # The last value that WAS valid, which at startup is the built-in
            # default and later is whatever was running. Saying "the default"
            # when it is the latter would be a lie about which number is now
            # in force.
            eval "$k=\$DASH_PREV_$k"
            eval "v=\$$k"
            case " $CONF_WARNED " in
                *" $k "*) ;;
                *) CONF_WARNED="$CONF_WARNED $k"
                   echo "dash.conf: $k is not a usable value; keeping $v" >&2 ;;
            esac
        else
            # It parses now, so a later mistake in the same key is worth
            # hearing about again. Rebuilt in the shell rather than with
            # `sed s/ $k\b//`: \b is a GNU extension that busybox sed is not
            # obliged to have, and a pattern that silently matches nothing
            # would make the warning fire once a minute again.
            local w kept=""
            for w in $CONF_WARNED; do
                [ "$w" = "$k" ] || kept="$kept $w"
            done
            CONF_WARNED="$kept"
        fi
    done
}

# The address as wget needs it. The scheme is added HERE rather than folded
# into HOST on load, so dash.conf keeps exactly what was set and `settings.sh
# get HOST` answers with it — a value that changes shape between being written
# and being read back is one nobody can check against what they typed.
host_url() {
    case "$HOST" in
        http://*|https://*) printf '%s' "$HOST" ;;
        *)                  printf 'http://%s' "$HOST" ;;
    esac
}

conf_write() {
    # Rewrite dash.conf from the values currently in the environment.
    local k v tmp="$CONF.tmp$$"
    {
        echo "# dash.conf — ESP32 Logger Kindle dashboard"
        echo "#"
        echo "# Edited from KUAL (ESP32 Dashboard → Settings) or by hand."
        echo "# A running dashboard re-reads this file every minute; no restart."
        echo ""
        for k in $(conf_keys); do
            eval "v=\$$k"
            echo "# $(conf_help "$k")"
            echo "$k=$v"
        done
    } > "$tmp" || return 1
    mv "$tmp" "$CONF"
}

conf_init() {
    # First run: seed dash.conf from the shipped defaults, so that copying a new
    # version of the extension over the old one cannot overwrite settings —
    # dash.conf is not in the repository, dash.conf.default is.
    [ -f "$CONF" ] && return 0
    if [ -f "$CONF_DEFAULT" ]; then
        cp "$CONF_DEFAULT" "$CONF" 2>/dev/null && return 0
    fi
    conf_write
}

# ── Prevent sleep & screensaver ──────────────────────────────────────────────
prevent_sleep() {
    lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null
    lipc-set-prop com.lab126.cmd intrf enable 2>/dev/null
    lipc-set-prop com.lab126.blanket unload 2>/dev/null
}

restore_sleep() {
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
}

# ── Font selection ───────────────────────────────────────────────────────────
# -t/--truetype needs a FILE. A name that does not resolve leaves every string
# undrawn, so the candidates are tried in order and the first file that exists
# wins; the Kindle's own font directory is the fallback for a device with no
# Bookerly, and dropping any .ttf into fonts/ overrides the lot.
SYS_FONTS="/usr/java/lib/fonts"
USR_FONTS="$DASH_DIR/fonts"

pick_font() {
    # $1=preferred file name, $2...=alternatives
    local name
    for name in "$@"; do
        [ -f "$USR_FONTS/$name" ] && { echo "$USR_FONTS/$name"; return 0; }
    done
    for name in "$@"; do
        [ -f "$SYS_FONTS/$name" ] && { echo "$SYS_FONTS/$name"; return 0; }
    done
    echo ""
}

font_setup() {
    FONT_REG=$(pick_font "Bookerly-Regular.ttf" "Caecilia_LT_65_Medium.ttf" \
                         "Helvetica_LT_65_Medium.ttf" "Futura_LT_Book.ttf")
    FONT_BOLD=$(pick_font "Bookerly-Bold.ttf" "Caecilia_LT_75_Bold.ttf" \
                          "Helvetica_LT_75_Bold.ttf" "Futura_LT_Bold.ttf")
    [ -z "$FONT_BOLD" ] && FONT_BOLD="$FONT_REG"
    [ -z "$FONT_REG" ]  && FONT_REG="$FONT_BOLD"
    if [ -z "$FONT_REG" ]; then
        echo "No TrueType font found in $USR_FONTS or $SYS_FONTS." >&2
        echo "Drop one into $USR_FONTS — text cannot be drawn without it." >&2
        return 1
    fi
    return 0
}

# ── Network helpers ──────────────────────────────────────────────────────────
fetch_data() {
    wget -q -T "$FETCH_TIMEOUT" -O "$TMP/data.txt" "$(host_url)/kindle/data" 2>/dev/null
}

# Is this a WHOLE BMP, or the first part of one?
#
# "Not empty" was the only thing asked before, and it is not enough. The image
# is streamed off an ESP32 a kilobyte at a time while it is also serving the
# web UI and taking readings; the connection is a ten-year-old reader on wifi.
# When that connection dies mid-image the collector has already sent a
# Content-Length, and busybox wget does not always call a short read an error —
# so a half-drawn chart lands on disk looking exactly like a good one, replaces
# the good one, and FBInk then refuses to blit it. The chart vanishes and comes
# back at some later fetch, which is precisely the symptom that was reported.
#
# A BMP says how long it is in its own header: "BM", then the file size as a
# 32-bit little-endian count at offset 2. Comparing that with the bytes on disk
# catches a truncated transfer without knowing anything about the image.
#
# THE CHECK REFUSES ONLY WHAT IT IS SURE OF. If od is not on the device, or the
# declared size is not a number this file could plausibly have, the size test is
# skipped rather than failed — a check that throws away good charts because it
# could not read them would be worse than the bug it is here for.
graph_ok() {
    # $1=file
    [ -s "$1" ] || return 1
    [ "$(dd if="$1" bs=1 count=2 2>/dev/null)" = "BM" ] || return 1

    local have want
    have=$(wc -c < "$1" 2>/dev/null | tr -dc '0-9')
    want=$(od -An -tu4 -j2 -N4 "$1" 2>/dev/null | tr -dc '0-9')

    [ -n "$want" ] || return 0                          # no od, or no answer
    [ "$want" -ge 118 ] 2>/dev/null || return 0          # smaller than a header
    [ "$want" -le 4194304 ] 2>/dev/null || return 0      # bigger than any panel
    [ "$have" = "$want" ]
}

# Is the chart switched on? Consulted before FETCHING as well as before
# drawing: a reader who turns the chart off in Settings should not have the
# Kindle keep downloading a 56 KB image over WiFi every GRAPH_EVERY minutes
# for a section nothing draws.
chart_wanted() { [ "${SHOW_CHART:-1}" = "1" ]; }

fetch_graph() {
    chart_wanted || return 1
    # Into a scratch file, and only into place once it is whole. wget -O
    # truncates its target the moment it opens it, so fetching straight onto
    # graph.bmp turned one WiFi hiccup into a zero-byte file — and since
    # redraw_chart clears its rectangle before drawing, that showed as a blank
    # strip where the chart was until the next successful fetch. Keeping the
    # last good chart is the better failure.
    if wget -q -T 15 -O "$TMP/graph.new" "$(host_url)/kindle/graph.bmp" 2>/dev/null \
       && graph_ok "$TMP/graph.new"; then
        mv "$TMP/graph.new" "$TMP/graph.bmp"
        return 0
    fi
    # Say which of the two it was. Both leave the previous chart alone, but they
    # need different things done about them — one is the network, the other is
    # the collector running out of heap partway through the image — and
    # /tmp/dash.log is the only place anybody can see the difference.
    if [ -s "$TMP/graph.new" ]; then
        echo "$(date '+%H:%M') chart: incomplete image ($(wc -c < "$TMP/graph.new" | tr -dc '0-9') bytes), keeping the last good one" >&2
    else
        echo "$(date '+%H:%M') chart: no image from $(host_url)" >&2
    fi
    rm -f "$TMP/graph.new"
    return 1
}

load_data() {
    load_kv "$TMP/data.txt" PAYLOAD || return 1

    # The layout follows the data, always — RES_W and RES_H come from the
    # collector, so the two cannot be loaded independently.
    #
    # It used to be called once, inside the branch taken when the FIRST fetch
    # succeeded. A Kindle that finished booting before the ESP32 did took the
    # other branch, and nothing in the main loop ever called it again: every
    # coordinate stayed unset for as long as the script ran.
    load_layout
    HAVE_DATA=1
    return 0
}

#: 1 once a payload has been parsed. Until then every Z_*, OUT_* and IN_* is
#: empty, and the drawing paths would compose "°", "/%" and "°/%" out of them —
#: three punctuation marks and no explanation, on a Kindle that finished
#: booting before the ESP32 did.
HAVE_DATA=0

# ── Layout loading ───────────────────────────────────────────────────────────
# RES_W and RES_H come off the wire and are interpolated into a path that is
# then EXECUTED with `.`, so they are checked here rather than trusted: a
# payload sending RES_W=../../../../mnt/us/x reaches any .conf-suffixed file on
# the device. Digits only, and a size the panel could plausibly be.
load_layout() {
    local conf
    case "${RES_W:-}" in ''|*[!0-9]*) RES_W=600 ;; esac
    case "${RES_H:-}" in ''|*[!0-9]*) RES_H=800 ;; esac
    [ "$RES_W" -ge 100 ] && [ "$RES_W" -le 4000 ] 2>/dev/null || RES_W=600
    [ "$RES_H" -ge 100 ] && [ "$RES_H" -le 4000 ] 2>/dev/null || RES_H=800

    conf="$DASH_DIR/layout/${RES_W}x${RES_H}.conf"
    if [ -f "$conf" ]; then
        . "$conf"
    else
        . "$DASH_DIR/layout/600x800.conf"
    fi
    # Derived once here rather than per string: both depend only on the layout,
    # and a fork per draw is a fork this script has spent years avoiding.
    TEXT_PX_MILLE="${TEXT_PX_MILLE:-1160}"
    BASELINE_MILLE=$(( 800 * TEXT_PX_MILLE / 1000 - (TEXT_PX_MILLE - 1000) / 2 ))
    zones_derive
}

# ── The four rectangles the page is refreshed in ─────────────────────────────
# They tile the screen top to bottom: readings, chart, forecast — with the clock
# inside the readings block, because it is the one region that changes on its
# own timer and so needs a rectangle of its own too.
#
# Derived from the layout's own dividers rather than listed again in it: the
# chart starts at RULE2_Y and the forecast at RULE3_Y whatever the panel, and a
# second copy of those numbers is a second copy to keep in step.
zones_derive() {
    ZW=${RES_W:-600}
    ZH=${RES_H:-800}

    Z_CLOCK_X=${CL_X:-318};  Z_CLOCK_Y=${CL_Y:-26}
    Z_CLOCK_W=${CL_W:-264};  Z_CLOCK_H=${CL_H:-104}

    Z_SENS_X=0
    Z_SENS_Y=0
    Z_SENS_W=$ZW
    Z_SENS_H=${RULE2_Y:-255}

    # The chart, plus the caption above it.
    Z_CHART_X=0
    Z_CHART_Y=${RULE2_Y:-255}
    Z_CHART_W=$ZW
    Z_CHART_H=$(( ${RULE3_Y:-490} - ${RULE2_Y:-255} ))

    Z_FC_X=0
    Z_FC_Y=${RULE3_Y:-490}
    Z_FC_W=$ZW
    Z_FC_H=$(( ZH - ${RULE3_Y:-490} ))
}

# ── Drawing primitives ───────────────────────────────────────────────────────
# Every one of them draws with -b (framebuffer only). The refresh is a separate
# call per zone, at the end — see the header.
#
#   -t regular=FILE,px=N,left=X,top=Y   text at a pixel position, TrueType
#   -k top=,left=,width=,height=        fill a rectangle with -B
#   -s top=,left=,width=,height= [-f]   refresh that rectangle, flashing or not
#   -g file=,x=,y=                      blit an image
#
# The `--` before a string is not decoration: the outdoor temperature is
# regularly "-2.4", and without it FBInk reads that as options.

fb() {
    # stderr, so kual-run.sh's own redirection carries it into kual.log next to
    # the scripts, where a USB cable can read it.
    [ "${TRACE:-0}" = "1" ] && printf 'fbink %s\n' "$*" >&2
    fbink "$@" 2>/dev/null
}

# ── FBInk's px is not the CSS px ─────────────────────────────────────────────
#
# THIS IS WHY THE PANEL'S TYPE CAME OUT SMALLER THAN THE PAGE'S. FBInk sizes
# OpenType text with stbtt_ScaleForPixelHeight(font, px) — fbink.c, in the
# print_ot() setup — and stb_truetype documents that as
#
#     scale = pixels / (ascent - descent)
#
# i.e. `px` is the font's WHOLE LINE HEIGHT, top of the ascender to bottom of
# the descender. CSS font-size is the em square, which for a text serif is some
# 14-20% smaller than that span. So `px=88` and `font-size:88px` are not the
# same size, and the panel drew every string about a sixth small next to the
# browser page: thinner stems, more air, a worse-looking screen made of the
# right numbers.
#
# It also moved things. The script places a value after another by adding
# `size × advance-in-mille / 1000`, with the advances measured at the collector
# in thousandths of the EM — so while the em was a sixth smaller than the size,
# every gap was a sixth too wide, and the headline's "/ 993 hPa" was pushed
# into the divider and clipped. Correcting the size corrects the arithmetic
# with it: past here, one design pixel is one em pixel again.
#
# TEXT_PX_MILLE is (ascent - descent) / unitsPerEm for the panel's font, in
# thousandths — the number to turn if the type ends up a hair large or small,
# and the only one. It lives in the layout file because the fonts a Kindle
# carries differ by model.
# NO SUBSHELL, because this is called for every string on the panel and the
# clock tier redraws every minute. `echo` in `$( )` is a fork, and three of
# them per string is ~180 forks a redraw on a ten-year-old ARM — in a file
# whose other comments count forks. It sets two globals instead, the way
# centre_in() already sets CENTRE_X.
#
# Sets:  TX_PX  the px FBInk has to be asked for
#        TX_TOP where the box has to start so the string stays optically where
#               the layout put it (FBInk grows the box downward from `top`, so
#               half the growth comes back off the top)
text_geom() {
    # $1=design top  $2=design size
    TX_PX=$(( $2 * ${TEXT_PX_MILLE:-1160} / 1000 ))
    TX_TOP=$(( $1 - (TX_PX - $2) / 2 ))
    [ "$TX_TOP" -lt 0 ] && TX_TOP=0
}

# The other direction: the design `top` that puts a string's BOX at $1.
# For anything positioned by where its box has to sit rather than by where the
# layout tuned its top — the week strip centres its two rows in the cell.
box_top() {
    # $1=wanted box top  $2=design size
    BOX_TOP=$(( $1 + ($2 * ${TEXT_PX_MILLE:-1160} / 1000 - $2) / 2 ))
}

draw_text() {
    # $1=x $2=y $3=px $4=font file $5=colour $6=text [$7=INV]
    #
    # WITHOUT $7: -O/--bgless, the glyphs and nothing else. FBInk's OpenType
    # renderer otherwise fills the text's whole box with the background pen,
    # which is WHITE unless -B says otherwise, and every tier has already
    # cleared its own rectangle — so a box of white would rub out whatever the
    # tier drew before it.
    #
    # WITH $7 ("INV"): the string is knocked out of a dark plate, and this is
    # -h/--invert with the ORDINARY pens rather than -C WHITE -B BLACK.
    #
    # THE OBVIOUS SPELLING DRAWS THE OPPOSITE. FBInk has a fast path for text
    # whose two pens are pure black and pure white — `abs(fgcolor - bgcolor)
    # == 0xFF` in print_ot() — where it skips blending and uses stb's coverage
    # mask directly, XORed with 0xFF. That XOR is the assumption that B&W text
    # means BLACK ON WHITE unless --invert says otherwise, so it turns the
    # empty ground white and the glyphs black: on the panel, a white box with
    # a black date in it, sitting in the middle of the black plate meant to
    # contain white ones. Asking for WHITE on BLACK is exactly what triggers
    # it, because that is exactly the pair the fast path is for.
    #
    # --invert flips it, and flips the pens with it, so passing the pens a
    # NON-inverted call would use gets white-on-black out of both FBInk's
    # paths: the fast one (the mask is used as-is) and the general blend (the
    # pens are swapped before it runs). And it is right on every Kindle:
    # FBInk's own condition compensates for the legacy models' inverted colour
    # map, so --invert means the same thing to the eye on a K3 as on a KT2.
    [ -n "$6" ] || return 0
    [ -n "$4" ] || return 0
    text_geom "$2" "$3"
    if [ -n "$7" ]; then
        fb -q -b -h -C BLACK -B WHITE -t regular="$4",px="$TX_PX",left="$1",top="$TX_TOP" -- "$6"
    else
        fb -q -b -O -C "$5" -t regular="$4",px="$TX_PX",left="$1",top="$TX_TOP" -- "$6"
    fi
}

draw_text_bold() { draw_text "$1" "$2" "$3" "$FONT_BOLD" "$4" "$5"; }
draw_text_reg()  { draw_text "$1" "$2" "$3" "$FONT_REG"  "$4" "$5"; }

# The same two, knocked out of a dark plate. No colour: --invert decides it,
# and passing one would only be a colour that is ignored.
draw_text_bold_inv() { draw_text "$1" "$2" "$3" "$FONT_BOLD" "" "$4" INV; }
draw_text_reg_inv()  { draw_text "$1" "$2" "$3" "$FONT_REG"  "" "$4" INV; }

fill_rect() {
    # $1=x $2=y $3=w $4=h $5=colour
    fb -q -b -B "$5" -k top="$2",left="$1",width="$3",height="$4"
}

draw_hline() {
    # $1=x $2=y $3=width $4=colour — a rectangle one pixel tall, because FBInk
    # has no line primitive and -L is --linecountcode, which took this script's
    # width as a string to print.
    fill_rect "$1" "$2" "$3" "${RULE_H:-1}" "$4"
}

draw_image() {
    # $1=file $2=x $3=y
    #
    # NON-ZERO WHEN NOTHING WAS DRAWN, and that is the whole point of the
    # change: a missing file used to return success, so the chart tier cleared
    # its rectangle, drew nothing into it, refreshed the panel and reported that
    # it had worked. The blank strip that left behind is indistinguishable from
    # a chart with no data in it. Now the caller can say so instead.
    [ -f "$1" ] || return 1
    fb -q -b -g file="$1",x="$2",y="$3"
}

refresh_zone() {
    # $1=x $2=y $3=w $4=h $5=1 to flash
    if [ "${5:-0}" = "1" ]; then
        fb -q -f -s top="$2",left="$1",width="$3",height="$4"
    else
        fb -q -s top="$2",left="$1",width="$3",height="$4"
    fi
}

refresh_screen() { fb -q -f -s; }

clear_screen()   { fb -q -b -B WHITE -k; }

# ── Clock ────────────────────────────────────────────────────────────────────
#
# THE TIME COMES FROM THIS DEVICE, THE FORMAT FROM THE COLLECTOR. The Kindle
# has its own clock and redraws once a minute; the collector is fetched every
# few minutes at best, so its CLOCK is a sample and not what is drawn. But the
# CHOICE of format is a setting the reader made once, on the same page as
# everything else here, and it used to reach the browser and stop there — a
# reader who picked the twelve-hour clock got it on the web page and 24-hour on
# the panel, from one setting on one device.
#
# The three cases are kdFmtTime()'s, written the same way: no space before
# "am", lower case, no seconds anywhere.
now_clock() {
    local hm h m
    hm=$(date '+%H:%M')
    case "${TIME_FORMAT:-0}" in
        1)  echo "${hm#0}" ;;                      # 9:05 — no leading zero
        2)  h=$(strip_zeros "${hm%%:*}"); m="${hm#*:}"
            if [ "${h:-0}" -lt 12 ] 2>/dev/null; then m="${m}am"; else m="${m}pm"; fi
            h=$(( ${h:-0} % 12 ))
            [ "$h" -eq 0 ] && h=12
            echo "$h:$m" ;;
        *)  echo "$hm" ;;                          # 09:05
    esac
}

# Where a centred clock starts.
#
# CLOCK_ADVW is how wide the collector's own copy of the time came out, in
# thousandths of the type size — the same measurement every place carries, and
# for the same reason: FBInk draws one size per call and will not say how wide
# it drew. It is a SAMPLE, not this minute's string, so on the minutes where
# the two differ in width — 9:59 to 10:00 on the lean clock — the centring is
# out by half a digit until the next fetch. Half a digit beats the time set
# hard against the left edge of a black plate.
clock_centre_x() {
    # $1=type size  -> CENTRE_X
    local sz="$1" w
    w=$(( sz * ${CLOCK_ADVW:-0} / 1000 ))
    if [ "$w" -gt 0 ] && [ "$w" -lt "${Z_CLOCK_W:-0}" ] 2>/dev/null; then
        CENTRE_X=$(( Z_CLOCK_X + (Z_CLOCK_W - w) / 2 ))
    else
        CENTRE_X="$CL_X"
    fi
}

# The four styles the page offers, at the sizes in the layout file. The page
# sets them in CSS (kdSkinCss); this draws the same four with the primitives a
# framebuffer has.
draw_clock() {
    local now_time="$1"
    local sz cy
    # The clearing fill is per-style, not up front: the boxed clock covers the
    # whole rectangle in black anyway, so a white fill before it was a second
    # fbink process a minute — 1440 forks a day on a ten-year-old ARM device —
    # painting something nothing would ever see.
    case "${CLOCK_STYLE:-0}" in
        1)  # BOXED — knocked out of a black plate, the treatment the current
            # weekday already gets in the week strip. On a screen with no
            # colour a filled block is the one mark that survives dithering
            # unambiguously, which is why the style exists at all.
            sz="${CL_SZ_BOXED:-$CL_SIZE}"
            fill_rect "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" BLACK
            cy=$(( Z_CLOCK_Y + (Z_CLOCK_H - sz) / 2 ))
            clock_centre_x "$sz"
            # On the plate, not bgless over it — the same reason today's cell
            # in the week strip is, and the same symptom if it is not: a black
            # box with no time in it.
            draw_text_bold_inv "$CENTRE_X" "$cy" "$sz" "$now_time"
            ;;
        2)  # RULED — a hairline over it and set smaller, so it reads as a rule
            # rather than as a number that happens to have a line above it. The
            # hairline under it is the one the indoor row already draws.
            fill_rect "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" WHITE
            sz="${CL_SZ_RULED:-$CL_SIZE}"
            draw_hline "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" BLACK
            cy=$(( Z_CLOCK_Y + ${CL_RULED_PAD:-15} ))
            clock_centre_x "$sz"
            draw_text_bold "$CENTRE_X" "$cy" "$sz" "BLACK" "$now_time"
            ;;
        3)  # DATED — the room for the date is taken FROM the clock rather than
            # added under it, exactly as the CSS does it, because the rectangle
            # this is drawn in has to end above the indoor rule either way.
            #
            # The date is the collector's, formatted to the reader's choice. It
            # changes once a day, so a value up to one fetch old is right.
            fill_rect "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" WHITE
            sz="${CL_SZ_DATED:-$CL_SIZE}"
            draw_text_bold "$CL_X" "$CL_Y" "$sz" "BLACK" "$now_time"
            # An `&&` here would make draw_clock's exit status the test's, so
            # a collector that sends no date — an older one, or the offline
            # path before any fetch — would report a failure for a clock it
            # drew perfectly. Nothing tests it today; redraw_all is one `&&`
            # away from turning that into a skipped repaint.
            if [ -n "${DATE:-}" ]; then
                # UNDER THE TIME'S BOX, not under its design size. The box is
                # TEXT_PX_MILLE tall and starts half the growth higher, so it
                # ends at top + px - (px - sz)/2 — past CL_Y + sz, which ate
                # the whole of CL_DATE_GAP and left the two rows touching.
                text_geom "$CL_Y" "$sz"
                draw_text_reg "$CL_X" "$(( TX_TOP + TX_PX + ${CL_DATE_GAP:-6} ))" \
                    "${CL_DATE_SZ:-14}" "GRAY4" "$DATE"
            fi
            ;;
        *)  fill_rect "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" WHITE
            draw_text_bold "$CL_X" "$CL_Y" "$CL_SIZE" "BLACK" "$now_time" ;;
    esac
}

# ── One place's value ────────────────────────────────────────────────────────
# ONE PLACE'S VALUE IS THREE DRAWS, not one.
#
# The number at its full size, then its unit as a footnote at four tenths of it,
# then the tendency arrow if it has one. FBInk draws one size per call, so each
# piece is a separate call at an x that depends on how wide the piece before it
# came out — and FBInk will not say. The collector measures them for us and
# sends the widths as Z_<PLACE>_VADVW and _UADVW, in thousandths of the type
# size; see kdAdvanceMille() in KindleDashboard.cpp.
draw_field() {
    # $1=x $2=row top $3=size $4=bold $5=value $6=unit $7=arrow
    # $8=value advance $9=unit advance ${10}=ink colour
    local x="$1" y="$2" sz="$3" bold="$4" val="$5" unit="$6" arrow="$7"
    local vadv="$8" uadv="$9" ink="${10:-BLACK}"
    local ux usz asz

    if [ -z "$val" ]; then
        # An em dash, not "--" and not nothing: a place that is configured but
        # has no reading has to look different from one nobody filled in.
        draw_text_reg "$x" "$y" "$sz" "GRAY7" "${LBL_DASH:-—}"
        return
    fi

    if [ "$bold" = "1" ]; then
        draw_text_bold "$x" "$y" "$sz" "$ink" "$val"
    else
        draw_text_reg  "$x" "$y" "$sz" "$ink" "$val"
    fi
    ux=$(( x + sz * vadv / 1000 ))

    if [ -n "$unit" ]; then
        # 42 % for a unit, 34 % for a degree — the page's .unit and .unit-d.
        # The degree was set at the same 42 % as "hPa", which at a headline
        # size is a circle the height of a lower-case o sitting where a
        # footnote should be, and it pushed everything after it too far right.
        if [ "$unit" = "°" ]; then usz=$(( sz * 34 / 100 ))
        else                       usz=$(( sz * 42 / 100 ))
        fi
        [ "$usz" -lt 9 ] && usz=9
        case "$unit" in
            # Degrees and per-cent set tight against the number — "8.4 °" and
            # "71 %" are not how either is written. Everything else takes a
            # space, as "1008 hPa" does.
            '°'|'%') ;;
            *)       ux=$(( ux + sz / 12 )) ;;
        esac
        if [ "$unit" = "°" ]; then
            # The degree rides at the cap line rather than on the baseline,
            # where at a third of the size it reads as a lower-case o.
            draw_text_reg "$ux" "$y" "$usz" "GRAY4" "$unit"
        else
            draw_text_reg "$ux" "$(baseline_y "$y" "$sz" "$usz")" "$usz" "GRAY4" "$unit"
        fi
        ux=$(( ux + usz * uadv / 1000 ))
    fi

    if [ -n "$arrow" ]; then
        asz=$(( sz * 50 / 100 ))
        [ "$asz" -lt 10 ] && asz=10
        draw_text_reg "$(( ux + sz / 10 ))" "$(baseline_y "$y" "$sz" "$asz")" \
                      "$asz" "GRAY4" "$arrow"
    fi
}

# Where a smaller value has to start so it shares a baseline with a larger one
# beside it. FBInk's top is the TOP of the text, so two sizes drawn at one y sit
# on two baselines and the row looks dropped; the ascent is about eight tenths
# of the size, which is where the 80 comes from.
# How far below a design `top` the baseline lands, in thousandths of the size.
#
# THE 800 IS THE FONT'S; THE REST IS FBInk's. A serif's ascent is about eight
# tenths of the span FBInk sizes by (ascent - descent), so with px=size the
# baseline sat 0.80 x size below the box top and everything here used 80/100.
# text_geom() moved that: the box is TEXT_PX_MILLE bigger and starts half the
# growth higher, so the baseline is now
#
#     0.800 x M - (M - 1)/2   of the size, M = TEXT_PX_MILLE/1000
#
# — 848/1000 at M = 1.16. Derived rather than written down again, because it
# is the constant that has to move when somebody turns TEXT_PX_MILLE and the
# one nobody would think to. Computed once by load_layout, into BASELINE_MILLE.
baseline_y() {
    # $1=row top  $2=largest size in the row  $3=this size
    echo $(( $1 + ($2 - $3) * ${BASELINE_MILLE:-848} / 1000 ))
}

# Where something `w` pixels wide starts if it is to be centred in a cell that
# begins at `x`. Used by the outlook columns and the week strip, both of which
# the page centres (.per and .wd are text-align:center) and both of which the
# panel used to draw hard against the cell's left edge.
#
# A width of zero — an older collector, which measures nothing — falls back to
# the left edge, which is what this always did.
#
# THE ANSWER IS A VARIABLE, NOT AN ECHO. `x=$(centre_in ...)` forks a subshell,
# and a full repaint centres twenty-four things: seven weekday names, seven
# numerals, three outlook labels, three icons, three temperatures and the
# clock. Two dozen process creations on a ten-year-old ARM device, once a
# minute for the clock and once a tier for the rest, to do three integer
# operations. draw_field() already passes its widths this way.
CENTRE_X=0
centre_in() {
    # $1=cell left  $2=cell width  $3=content width  -> CENTRE_X
    if [ "${3:-0}" -gt 0 ] && [ "$3" -lt "$2" ] 2>/dev/null; then
        CENTRE_X=$(( $1 + ($2 - $3) / 2 ))
    else
        CENTRE_X="$1"
    fi
}

# The same, for an outlook column, whose width is one number for all three.
ol_centre() {
    # $1=column left  $2=content width  -> CENTRE_X
    centre_in "$1" "${OL_PLATE_W:-0}" "$2"
}

# ── The readings block ───────────────────────────────────────────────────────
# The collector sends what is in each named place and, for the two groups that
# can close up behind an empty one, which places survived and in what order —
# and for the grid, how they break into rows. All this end does is turn that
# into pixels for one panel.
#
# NAMES, NOT COORDINATES. 600x800 and 1072x1448 want different pixel positions
# for the same design, and the reader should not have to place every value
# twice; the layout file carries the positions, this carries the drawing.
draw_zones() {
    # A collector running older firmware sends no Z_GROUP_OUT, and draw_sensors
    # falls back to the fixed keys it does send — so the reader and the
    # collector do not have to be updated in the same minute.
    [ -n "${Z_GROUP_OUT:-}" ] || return 1

    local lx="${COL_L_X:-18}" rx="${COL_R_X:-318}" rw="${COL_R_W:-264}"
    local lab_sz="${GROUP_LAB_SZ:-12}"
    local z val unit lab arrow bold vadv uadv ink n i cx cw vsz y

    # ── Left column: the outdoor headline ───────────────────────────────────
    draw_text_reg "$lx" "${TOP_Y:-20}" "$lab_sz" "GRAY7" "$Z_GROUP_OUT"

    local hero_sz="${HERO_SZ:-84}" hero_y="${HERO_Y:-38}"
    draw_field "$lx" "$hero_y" "$hero_sz" "${Z_HERO_BOLD:-0}" \
               "$Z_HERO_VALUE" "$Z_HERO_UNIT" "$Z_HERO_ARROW" \
               "${Z_HERO_VADVW:-0}" "${Z_HERO_UADVW:-0}" "${Z_HERO_INK:-BLACK}"

    # The slash and the second value, after the whole of the headline. The
    # collector measured that width for us — see kdAdvanceMille() — because
    # FBInk will not say how wide it drew something and ${#var} counts bytes,
    # which makes "8.4°" five characters long.
    if [ -n "${Z_BIG_VALUE:-}" ]; then
        local big_sz="${BIG_SZ:-44}"
        local hw=$(( hero_sz * ${Z_HERO_ADVW:-0} / 1000 ))
        local sx=$(( lx + hw + ${HEAD_GAP:-8} ))
        y=$(baseline_y "$hero_y" "$hero_sz" "$big_sz")
        draw_text_reg "$sx" "$y" "$big_sz" "GRAYA" "/"
        draw_field "$(( sx + ${SLASH_W:-22} ))" "$y" "$big_sz" "${Z_BIG_BOLD:-0}" \
                   "$Z_BIG_VALUE" "$Z_BIG_UNIT" "$Z_BIG_ARROW" \
                   "${Z_BIG_VADVW:-0}" "${Z_BIG_UADVW:-0}" "${Z_BIG_INK:-BLACK}"
    fi

    # The 24 h low-to-high and the age, composed by the collector so that the
    # wording, the unit and the rounding are the page's and not this script's.
    [ -n "${Z_SUB:-}" ] && \
        draw_text_reg "$lx" "${SUB_Y:-128}" "${SUB_SZ:-14}" "GRAY7" "$Z_SUB"

    # ── Left column: the grid ───────────────────────────────────────────────
    # GRID_ROWS says how many cells are on each row; each row then divides its
    # own width by its own count, so two cells are two halves rather than two of
    # three thirds with the last one left white.
    local gy="${GRID_Y:-150}" gcols gvsz gcw gi=0
    set -- ${GRID_ZONES:-}
    for gcols in ${GRID_ROWS:-}; do
        gcw=$(( ${COL_L_W:-270} / gcols ))
        if [ "$gcols" -ge 3 ]; then gvsz="${GRID_VAL_SZ_3:-26}"
        else                        gvsz="${GRID_VAL_SZ:-31}"
        fi
        gi=0
        while [ "$gi" -lt "$gcols" ] && [ -n "${1:-}" ]; do
            z="$1"; shift
            eval "val=\$Z_${z}_VALUE; unit=\$Z_${z}_UNIT; lab=\$Z_${z}_LABEL"
            eval "arrow=\$Z_${z}_ARROW; bold=\$Z_${z}_BOLD; ink=\${Z_${z}_INK:-BLACK}"
            eval "vadv=\${Z_${z}_VADVW:-0}; uadv=\${Z_${z}_UADVW:-0}"
            cx=$(( lx + gi * gcw ))
            draw_text_reg "$cx" "$gy" "${GRID_LAB_SZ:-10}" "GRAY7" "$lab"
            draw_field "$cx" "$(( gy + ${GRID_LAB_SZ:-10} + 4 ))" "$gvsz" \
                       "$bold" "$val" "$unit" "$arrow" "$vadv" "$uadv" "$ink"
            gi=$((gi + 1))
        done
        gy=$((gy + ${GRID_ROW_H:-46}))
    done

    # ── Right column: the indoor row ────────────────────────────────────────
    n=0
    for z in ${IN_ZONES:-}; do n=$((n + 1)); done
    if [ "$n" -gt 0 ]; then
        # GRAYD, not GRAYA: .inrule is #d8d8d8 and .rule is #aaa. This one
        # separates two things inside one column, where the page's section
        # rules separate the columns from what is under them, and drawn at the
        # heavier weight it read as a third section break.
        draw_hline "$rx" "${IN_RULE_Y:-126}" "$rw" "GRAYD"
        draw_text_reg "$rx" "${IN_LAB_Y:-134}" "$lab_sz" "GRAY7" "$Z_GROUP_IN"

        # The first field gets more of the row, not an equal share: it is set
        # larger, so equal columns crowd it against its neighbour while leaving
        # the small ones space they do not need.
        local w1
        if [ "$n" -ge 3 ]; then w1=$(( rw * 42 / 100 ))
        elif [ "$n" -eq 2 ]; then w1=$(( rw * 58 / 100 ))
        else w1="$rw"
        fi
        cw=$(( n > 1 ? (rw - w1) / (n - 1) : rw ))

        # ALL THREE VALUES SIT ON ONE BOTTOM EDGE. The first has no caption —
        # the heading above already says which room this is — so it is half
        # again as tall and starts higher.
        local big="${IN_VAL_SZ_1:-52}" small="${IN_VAL_SZ:-28}"
        local big_y="${IN_VAL_Y:-158}"
        local small_y=$(( big_y + big - small ))

        i=0
        for z in $IN_ZONES; do
            eval "val=\$Z_${z}_VALUE; unit=\$Z_${z}_UNIT; lab=\$Z_${z}_LABEL"
            eval "arrow=\$Z_${z}_ARROW; bold=\$Z_${z}_BOLD; ink=\${Z_${z}_INK:-BLACK}"
            eval "vadv=\${Z_${z}_VADVW:-0}; uadv=\${Z_${z}_UADVW:-0}"
            if [ "$i" = "0" ]; then
                cx="$rx"; vsz="$big"; y="$big_y"
            else
                cx=$(( rx + w1 + (i - 1) * cw )); vsz="$small"; y="$small_y"
                draw_text_reg "$cx" "$(( y - ${GRID_LAB_SZ:-10} - 4 ))" \
                              "${GRID_LAB_SZ:-10}" "GRAY7" "$lab"
            fi
            draw_field "$cx" "$y" "$vsz" "$bold" "$val" "$unit" "$arrow" \
                       "$vadv" "$uadv" "$ink"
            i=$((i + 1))
        done
    fi

    # ── The hairline between the columns ────────────────────────────────────
    if [ "${SEP_W:-1}" -gt 0 ] 2>/dev/null; then
        fill_rect "${SEP_X:-300}" "${TOP_Y:-20}" "${SEP_W:-1}" "${SEP_H:-210}" GRAYA
    fi

    return 0
}

# A battery node whose cells are running down stops reporting without saying
# anything first, and the page it stops appearing on is the one that ought to
# warn about it.
draw_battery_badge() {
    [ "${OUT_BATT_WARN:-0}" = "1" ] || return 0
    draw_image "$ICON_DIR/fc_batt.bmp" "$BATT_X" "$BATT_Y"
}

# The layout the collector describes, or the fixed one it used to.
draw_sensors_body() {
    if draw_zones; then
        draw_battery_badge
        return 0
    fi

    # ── The legacy fixed layout ─────────────────────────────────────────────
    # It formats "${OUT_TEMP}°" and "${IN_TEMP}°/${IN_HUM}%" from whatever is
    # in those variables, and `[ "$OUT_HUM" != "-1" ]` is true when OUT_HUM is
    # unset — so with no payload at all it draws the punctuation and nothing
    # else. The caller shows the offline page instead.
    [ -n "${OUT_TEMP:-}" ] || return 1

    draw_text_reg "$LAB_OUT_X" "$LAB_OUT_Y" "$LAB_SZ" "GRAY7" "$LBL_OUTSIDE"
    draw_battery_badge
    draw_text_bold "$LEGACY_HERO_X" "$LEGACY_HERO_Y" "$LEGACY_HERO_SZ" "BLACK" "${OUT_TEMP}°"

    if [ "$OUT_HUM" != "-1" ]; then
        draw_text_reg "$HUM_X" "$HUM_Y" "$HUM_SZ" "GRAY4" "/${OUT_HUM}%"
    fi

    local sub=""
    if [ -n "$OUT_RANGE_LO" ] && [ -n "$OUT_RANGE_HI" ]; then
        sub="${OUT_RANGE_LO} ${LBL_TO} ${OUT_RANGE_HI}°"
    fi
    if [ "$OUT_AGE_MIN" -gt 1 ] 2>/dev/null; then
        sub="$sub  ${OUT_AGE_MIN}m"
    fi
    [ -n "$sub" ] && draw_text_reg "$LEGACY_SUB_X" "$LEGACY_SUB_Y" "$LEGACY_SUB_SZ" "GRAY4" "$sub"

    if [ -n "$OUT_PRES" ] && [ "$OUT_PRES" != "--" ]; then
        draw_text_bold "$PRES_X" "$PRES_Y" "$PRES_SZ" "BLACK" "$OUT_PRES $OUT_PRES_UNIT"
    fi
    if [ -n "$OUT_TEND" ]; then
        draw_text_reg "$TEND_X" "$TEND_Y" "$TEND_SZ" "GRAY4" "$OUT_TEND_ARROW $OUT_TEND ($OUT_TEND_DELTA)"
    fi

    draw_hline "$RULE1_X" "$RULE1_Y" "$RULE1_W" "GRAYA"
    draw_text_reg "$LAB_IN_X" "$LAB_IN_Y" "$LAB_SZ" "GRAY7" "$LBL_INSIDE"

    local in_str="${IN_TEMP}°"
    [ "$IN_HUM" != "-1" ] && in_str="${in_str}/${IN_HUM}%"
    if [ "$IN_AGE_MIN" -gt 1 ] 2>/dev/null; then
        in_str="$in_str  ${IN_AGE_MIN}m"
    fi
    draw_text_reg "$IN_X" "$IN_Y" "$IN_SZ" "BLACK" "$in_str"
    return 0
}

# ── The chart ────────────────────────────────────────────────────────────────
# The key under it, the same two entries the web page draws.
#
# ONLY WHEN THERE IS A LINE FOR IT TO NAME. The page tests haveOut/haveIn
# before drawing its own, because a key describing two lines over an empty grid
# is worse than no key: it says the chart is showing something. CHART_OUT and
# CHART_IN carry that answer. An older collector sends neither and this draws
# nothing, which is what it did before there was a key at all.
#
# The weights are the page's and are not decoration: the outdoor mean is a
# solid 3 px rule and the indoor line a dashed 2 px one, and on a panel with no
# colour the dashes are the whole of what tells the two lines apart.
draw_chart_key() {
    local y="${KEY_Y:-0}" sz="${KEY_SZ:-13}" sw="${KEY_SW_W:-26}"
    local gap="${KEY_GAP:-6}" mid tx dx dash seg end
    [ "$y" -gt 0 ] 2>/dev/null || return 0

    # The swatch sits on the middle of the type, not on its top edge.
    mid=$(( y + sz / 2 ))

    if [ "${CHART_OUT:-0}" = "1" ]; then
        fill_rect "$GR_X" "$mid" "$sw" "${KEY_SW_H:-3}" BLACK
        tx=$(( GR_X + sw + gap ))
        draw_text_reg "$tx" "$y" "$sz" "GRAY4" "${LBL_KEY_OUT:-outside mean}"
        # The band clause after it, in the lighter grey the page's .dim sets.
        # It starts where the label ended, which the collector measured for us
        # — see KEY_OUT_ADVW, and draw_field() for why this is not ${#var}.
        if [ -n "${LBL_KEY_BAND:-}" ] && [ "${KEY_OUT_ADVW:-0}" -gt 0 ] 2>/dev/null; then
            draw_text_reg "$(( tx + sz * KEY_OUT_ADVW / 1000 ))" "$y" "$sz" \
                          "GRAY7" ", $LBL_KEY_BAND"
        fi
    fi

    if [ "${CHART_IN:-0}" = "1" ]; then
        tx="${KEY_IN_X:-0}"
        [ "$tx" -gt 0 ] 2>/dev/null || tx=$(( GR_X + GR_W * 3 / 4 ))
        # FBInk has no dashed rule, so it is drawn as segments.
        dash="${KEY_DASH:-7}"
        end=$(( tx + sw ))
        dx="$tx"
        while [ "$dx" -lt "$end" ]; do
            seg="$dash"
            [ $(( dx + seg )) -gt "$end" ] && seg=$(( end - dx ))
            fill_rect "$dx" "$mid" "$seg" "${KEY_SW_H_IN:-2}" GRAY7
            dx=$(( dx + dash + ${KEY_DASH_GAP:-5} ))
        done
        draw_text_reg "$(( tx + sw + gap ))" "$y" "$sz" "GRAY4" \
                      "${LBL_KEY_IN:-inside}"
    fi
}

# The five values down the side and the five hours along the bottom.
#
# THE IMAGE CANNOT CARRY THEM. Drawing text into a 4-bit BMP would need a
# bitmap font on the ESP32 that the firmware does not have, so the collector
# sends the labels as text and says where its own plot area is inside the image
# (CH_L/CH_R/CH_T/CH_B, in image pixels). Without this the panel showed a bare
# grid while the browser page showed the same grid with numbers on it, and a
# grid with no numbers is a picture of a chart rather than a chart.
#
# The positions are the page's: the value 7 px left of the axis, dropped 4 px
# so it sits on its grid line; the hour centred on its vertical, 8 px up from
# the image's bottom edge. Both are measured by the collector, because FBInk
# will not say how wide it drew something — see draw_field().
draw_chart_axis() {
    [ -n "${CH_L:-}" ] || return 0
    local sz="${AX_SZ:-11}" gap="${AX_GAP:-7}" base="${AX_BASE:-4}"
    local k y w x lab

    # FBInk's `top` is the TOP of the text, and the page positions these by
    # their BASELINE — so each one is lifted by the ascent. Through
    # baseline_mille() rather than a copy of the number, for the reason on
    # that function: it moves with TEXT_PX_MILLE and a written-down 80 would
    # not.

    #
    # Down the side, right-aligned on the axis and sitting on its grid line.
    k=0
    while [ "$k" -le 4 ]; do
        eval "lab=\${CH_Y${k}:-}; w=\${CH_Y${k}W:-0}"
        if [ -n "$lab" ]; then
            y=$(( GR_Y + CH_T + (CH_B - CH_T) * k / 4 + base - sz * ${BASELINE_MILLE:-848} / 1000 ))
            x=$(( GR_X + CH_L - gap - sz * w / 1000 ))
            draw_text_reg "$x" "$y" "$sz" "GRAY7" "$lab"
        fi
        k=$((k + 1))
    done

    # Along the bottom, centred on the three-hourly rules the image draws at
    # hours 0, 6, 12 and 18 — except "now", which is set against the right-hand
    # edge because that is where the axis ends.
    k=0
    while [ "$k" -le 4 ]; do
        eval "lab=\${CH_H${k}:-}; w=\${CH_H${k}W:-0}"
        if [ -n "$lab" ]; then
            w=$(( sz * w / 1000 ))
            if [ "$k" -eq 4 ]; then
                x=$(( GR_X + CH_R - w ))
            else
                x=$(( GR_X + CH_L + (CH_R - CH_L) * (k * 6) / 23 - w / 2 ))
            fi
            draw_text_reg "$x" \
                "$(( GR_Y + GR_H - ${HX_DROP:-8} - sz * 80 / 100 ))" \
                "$sz" "GRAY7" "$lab"
        fi
        k=$((k + 1))
    done
}

draw_chart_body() {
    # SWITCHED OFF MEANS OFF HERE TOO. The reader can turn the chart off in
    # Settings → Kindle; the page then draws neither the section nor the rule
    # above it, and this drew both regardless. One setting, one device, two
    # answers.
    chart_wanted || return 0

    draw_hline "$RULE2_X" "$RULE2_Y" "$RULE2_W" "GRAYA"
    draw_text_reg "$LAB_CHART_X" "$LAB_CHART_Y" "$LAB_SZ" "GRAY7" "$LBL_LAST24"

    # NOTHING RECORDED YET IS NOT THE SAME AS NOTHING HAPPENING. The image is
    # still a grid when the ring is empty, and a grid with no line in it reads
    # as a sensor that has stopped. The page prints a sentence instead; so does
    # this. CH_NOTE carries it, so the wording and the language are the page's.
    if [ -n "${CH_NOTE:-}" ]; then
        draw_text_reg "${GR_X:-20}" "$(( ${GR_Y:-278} + ${GR_H:-200} / 3 ))" \
                      "${LAB_SZ:-16}" "GRAY5" "$CH_NOTE"
        return 0
    fi

    if draw_image "$TMP/graph.bmp" "$GR_X" "$GR_Y"; then
        draw_chart_axis
        draw_chart_key
        return 0
    fi

    # NOTHING WAS DRAWN — SAY SO, rather than refreshing an empty rectangle.
    #
    # The caption and the rule above it are drawn either way, so a blank strip
    # underneath them reads as "the chart is empty": no readings, or a sensor
    # that stopped reporting. It is usually neither. It is the image not being
    # here — the first fetch after a start has not happened yet, the collector
    # was rebooting when it did, or what arrived was not a whole BMP. One line
    # where the chart would be turns a mystery into a fact, and it names the
    # address so a wrong one is visible from across the room.
    local y=$(( ${GR_Y:-278} + ${GR_H:-200} / 3 ))
    draw_text_reg "${GR_X:-20}" "$y" "${LAB_SZ:-16}" "GRAY5" \
                  "${LBL_NO_CHART:-No chart yet} — $(host_url)"
    return 1
}

# ── Forecast, week strip and footer ──────────────────────────────────────────
# None of it is a sensor reading, so none of it is a slot: the forecast comes
# from an API, the week strip from the clock, and the footer is a constant.
draw_forecast_body() {
    draw_hline "$RULE3_X" "$RULE3_Y" "$RULE3_W" "GRAYA"

    if [ -n "$FC_SUMMARY" ]; then
        draw_text_reg "$LAB_FC_X" "$LAB_FC_Y" "$LAB_SZ" "GRAY7" "$LBL_FORECAST"

        # FC_ICON, NOT FC_CODE. There are eleven icon files, one per range of
        # WMO codes, and this script cannot reduce a code to its range — so it
        # asked for fc_2_52.bmp on a partly-cloudy day, did not find it, and
        # drew fc_-1, the circled question mark that is supposed to mean "no
        # forecast". The collector reduces it now (weatherIconCode), which is
        # also where the browser page's ranges live, so the two cannot disagree.
        # FC_CODE is the fallback for a collector too old to send FC_ICON.
        local icon="$ICON_DIR/fc_${FC_ICON:-$FC_CODE}_${FC_MAIN_SZ}.bmp"
        [ ! -f "$icon" ] && icon="$ICON_DIR/fc_-1_${FC_MAIN_SZ}.bmp"
        draw_image "$icon" "$FC_ICON_X" "$FC_ICON_Y"

        draw_text_reg "$FC_TEXT_X" "$FC_TEXT_Y" "$FC_TEXT_SZ" "BLACK" "$FC_SUMMARY"
        draw_text_bold "$FC_TEMP_X" "$FC_TEMP_Y" "$FC_TEMP_SZ" "BLACK" "${FC_HIGH}°/${FC_LOW}°"
        # THE AGE BELONGS ON THIS LINE. The page draws "вятър 5 km/h · 8 мин"
        # and the panel drew only the wind, so the one thing that says whether
        # to believe a forecast — how old it is — was on the screen nobody
        # looks at. Formatted by the collector (FC_AGE), like every other
        # string, so the wording follows the language setting.
        local fc_sub=""
        if [ -n "$FC_WIND" ] && [ "$FC_WIND" != "0" ]; then
            fc_sub="$LBL_WIND ${FC_WIND} km/h"
        fi
        if [ -n "$FC_AGE" ]; then
            if [ -n "$fc_sub" ]; then fc_sub="$fc_sub · $FC_AGE"
            else                      fc_sub="$FC_AGE"; fi
        fi
        if [ -n "$fc_sub" ]; then
            draw_text_reg "$FC_WIND_X" "$FC_WIND_Y" "$FC_WIND_SZ" "GRAY4" "$fc_sub"
        fi

        # EACH COLUMN ON A PLATE, AND CENTRED ON IT. The page sets .per to
        # `width:88px; text-align:center; background:#f0f0f0`; this drew the
        # label, the icon and the temperature at the column's left edge on
        # white, so three tidy grey cards came out as three ragged stacks. The
        # widths come from the collector (FC*_LABELW / FC*_TEMPW) for the
        # reason every other width does: FBInk will not say how wide it drew
        # something, and ${#var} counts bytes.
        local i ol_label ol_code ol_temp ol_x ol_y ol_icon ol_icon_y ol_temp_y
        local plate_w="${OL_PLATE_W:-0}" ol_w
        for i in 0 1 2; do
            eval "ol_label=\$FC${i}_LABEL"
            eval "ol_code=\${FC${i}_ICON:-\$FC${i}_CODE}"
            eval "ol_temp=\$FC${i}_TEMP"
            eval "ol_x=\$OL${i}_X"
            eval "ol_y=\$OL${i}_Y"

            [ -n "$ol_label" ] || continue

            if [ "$plate_w" -gt 0 ] 2>/dev/null; then
                fill_rect "$ol_x" "$(( ol_y - ${OL_PLATE_TOP:-4} ))" \
                          "$plate_w" "${OL_PLATE_H:-80}" GRAYE
            fi

            eval "ol_w=\${FC${i}_LABELW:-0}"
            ol_centre "$ol_x" "$(( OL_LABEL_SZ * ol_w / 1000 ))"
            draw_text_reg "$CENTRE_X" "$ol_y" "$OL_LABEL_SZ" "GRAY7" "$ol_label"

            ol_icon="$ICON_DIR/fc_${ol_code}_${FC_OL_SZ}.bmp"
            [ ! -f "$ol_icon" ] && ol_icon="$ICON_DIR/fc_-1_${FC_OL_SZ}.bmp"
            ol_icon_y=$((ol_y + OL_ICON_OFFSET))
            ol_centre "$ol_x" "$FC_OL_SZ"
            draw_image "$ol_icon" "$CENTRE_X" "$ol_icon_y"

            eval "ol_w=\${FC${i}_TEMPW:-0}"
            ol_temp_y=$((ol_y + OL_TEMP_OFFSET))
            ol_centre "$ol_x" "$(( OL_TEMP_SZ * ol_w / 1000 ))"
            draw_text_bold "$CENTRE_X" "$ol_temp_y" "$OL_TEMP_SZ" "BLACK" "${ol_temp}°"
        done
    fi

    # ── The week strip ──────────────────────────────────────────────────────
    # Behind its own switch, as it is on the page (KSHOW_WEEK). Everything
    # under it — the footer rule and the line of type — is drawn either way,
    # because the page's footer is not part of the strip.
    #
    # WRAPPED RATHER THAN RETURNED FROM. The early return needed its own copy
    # of the footer, so the two lines that draw it existed twice — and the
    # branch that is harder to reach by hand, the switch turned off, is the one
    # a later change to the footer would miss. Which is exactly the class of
    # silent divergence this file has spent the last few commits removing.
    if [ "${SHOW_WEEK:-1}" = "1" ]; then

        # Week heading (month)
        if [ -n "${WK_MON_MONTH:-}" ]; then
            draw_hline "${WK_HDG_RULE_X:-$FOOT_RULE_X}" "${WK_HDG_RULE_Y:-$WK_Y}" \
                       "${WK_HDG_RULE_W:-$FOOT_RULE_W}" "GRAYA"
            local wk_heading="$WK_MON_MONTH"
            [ -n "${WK_SUN_MONTH:-}" ] && wk_heading="$wk_heading – $WK_SUN_MONTH"
            draw_text_reg "${WK_HDG_X:-$WK_X}" "${WK_HDG_Y:-$WK_Y}" \
                          "${WK_HDG_SZ:-$LAB_SZ}" "GRAY7" "$wk_heading"
        fi

        # Week strip
        local wk_x="$WK_X" wk_name wk_day wk_bg i wk_nw wk_dw wk_nx wk_dx
        for i in 0 1 2 3 4 5 6; do
            eval "wk_name=\$WK${i}_NAME"
            eval "wk_day=\$WK${i}_DAY"

            # CENTRED IN THE CELL, and the number set REGULAR. .wd is
            # text-align:center and .wd-d carries no font-weight, so the page draws
            # seven centred regular numerals; the panel drew seven bold ones hard
            # against the left edge of their cells, which on a row of identical
            # boxes is the one place a misalignment cannot hide. The widths are the
            # collector's — see draw_field() for why they are not ${#var}.
            # CENTRED IN THE CELL VERTICALLY TOO, not only across it. The two
            # offsets in the layout were tuned when a box was exactly its
            # design size; text_geom() made the day's box 27 px instead of 24,
            # so the pair ended up 5 px from the top of the cell and 2 from the
            # bottom — the one row of identical boxes where three pixels of
            # list is visible. The layout keeps the SPACING between the two
            # rows; where the pair sits is derived, so it stays centred
            # whatever TEXT_PX_MILLE is set to.
            wk_gap=$(( WK_DAY_OFFSET - WK_NAME_OFFSET ))
            wk_dh=$(( WK_DAY_SZ * TEXT_PX_MILLE / 1000 ))
            wk_top=$(( WK_Y + (WK_CELL_H - wk_gap - wk_dh) / 2 ))
            [ "$wk_top" -lt "$WK_Y" ] && wk_top="$WK_Y"
            box_top "$wk_top" "$WK_NAME_SZ";            wk_ny="$BOX_TOP"
            box_top "$(( wk_top + wk_gap ))" "$WK_DAY_SZ"; wk_dy="$BOX_TOP"

            eval "wk_nw=\$WK${i}_NAMEW; wk_dw=\$WK${i}_DAYW"
            centre_in "$wk_x" "$WK_CELL_W" "$(( WK_NAME_SZ * ${wk_nw:-0} / 1000 ))"
            wk_nx="$CENTRE_X"
            centre_in "$wk_x" "$WK_CELL_W" "$(( WK_DAY_SZ * ${wk_dw:-0} / 1000 ))"
            wk_dx="$CENTRE_X"

            if [ "$i" = "$WK_TODAY" ]; then
                # Today: knocked out of a black plate — and drawn ON that
                # plate, not bgless over it. Bgless left an empty black
                # rectangle where the date should be: the one cell on the
                # screen that has to be legible, reading as a hole. See
                # draw_text().
                fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$WK_CELL_H" BLACK
                draw_text_reg_inv "$wk_nx" "$wk_ny" "$WK_NAME_SZ" "$wk_name"
                draw_text_reg_inv "$wk_dx" "$wk_dy" "$WK_DAY_SZ" "$wk_day"
            else
                wk_bg="GRAYE"
                { [ "$i" = "5" ] || [ "$i" = "6" ]; } && wk_bg="GRAYD"
                fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$WK_CELL_H" "$wk_bg"
                draw_text_reg "$wk_nx" "$wk_ny" "$WK_NAME_SZ" "GRAY7" "$wk_name"
                draw_text_reg "$wk_dx" "$wk_dy" "$WK_DAY_SZ" "BLACK" "$wk_day"
            fi
            wk_x=$((wk_x + WK_CELL_W))
        done

    fi   # SHOW_WEEK

    draw_hline "$FOOT_RULE_X" "$FOOT_RULE_Y" "$FOOT_RULE_W" "GRAYA"
    draw_text_reg "$FOOT_X" "$FOOT_Y" "$FOOT_SZ" "GRAY5" "$LBL_MEASURED"
}

# ── The four repaints ────────────────────────────────────────────────────────
# Each one clears its own rectangle, draws into the framebuffer, and refreshes
# that rectangle exactly once. Clearing first is not optional: e-ink does not
# erase what it is drawn over, so "21.0" replaced by "9.8" leaves the "0"
# standing where nothing wrote.

redraw_clock() {
    # $1=HH:MM  $2=1 to flash this rectangle
    draw_clock "$1"
    refresh_zone "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" "${2:-0}"
}

redraw_sensors() {
    # $1=HH:MM  $2=1 to flash
    fill_rect "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" WHITE
    draw_sensors_body
    draw_clock "$1"                 # inside this rectangle, so it goes with it
    refresh_zone "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" "${2:-0}"
}

redraw_chart() {
    fill_rect "$Z_CHART_X" "$Z_CHART_Y" "$Z_CHART_W" "$Z_CHART_H" WHITE
    draw_chart_body
    refresh_zone "$Z_CHART_X" "$Z_CHART_Y" "$Z_CHART_W" "$Z_CHART_H" 0
}

redraw_forecast() {
    fill_rect "$Z_FC_X" "$Z_FC_Y" "$Z_FC_W" "$Z_FC_H" WHITE
    draw_forecast_body
    refresh_zone "$Z_FC_X" "$Z_FC_Y" "$Z_FC_W" "$Z_FC_H" 0
}

# What the panel shows when the collector cannot be reached. In the READINGS
# zone, deliberately: the startup message used to be drawn at y=380, which is
# inside the chart rectangle, so the chart tier cleared the explanation off the
# screen fifteen minutes later and left nothing in its place.
#
# The chart and the forecast below are left alone — a stale page with a reason
# on it beats a blank one.
redraw_offline() {
    # $1=HH:MM
    # THE ONE MESSAGE THAT CANNOT BE FETCHED WHEN IT IS NEEDED. Every other
    # string on this panel comes from /kindle/data in whatever language the
    # collector is set to; this one is drawn precisely because the collector
    # cannot be reached. So it uses the wording the LAST successful fetch left
    # behind — which is every outage after the first contact — and falls back
    # to English before that, on a panel nobody has set a language on yet.
    local y=$(( Z_SENS_H / 3 ))
    fill_rect "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" WHITE
    draw_text_bold "${OFF_X:-40}" "$y" "${OFF_SZ:-26}" "BLACK" \
                   "${LBL_OFFLINE:-Cannot reach} $(host_url)"
    draw_text_reg "${OFF_X:-40}" "$(( y + ${OFF_SZ:-26} + 12 ))" \
                  "${OFF_SUB_SZ:-16}" "GRAY5" \
                  "${LBL_OFFLINE_HINT:-Check WiFi, or KUAL → Settings → Find collector}"
    draw_clock "$1"
    refresh_zone "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" 0
}

# The whole page, one flashing refresh at the end. This is the tier that
# actually resets ghosting, and it is why the others do not have to.
redraw_all() {
    # $1=HH:MM
    clear_screen
    draw_sensors_body
    draw_clock "$1"
    draw_chart_body
    draw_forecast_body
    refresh_screen
}

# ── Which tiers are due this minute ──────────────────────────────────────────
# Split out of the loop so it can be tested: tests/kindle/drive_dash.sh walks a
# simulated hour and asserts the schedule, which is the part of this file most
# likely to be got wrong by a plausible-looking edit.
due() {
    # $1=minute counter $2=interval — 0 or empty means "never"
    [ -n "$2" ] && [ "$2" -gt 0 ] 2>/dev/null || return 1
    [ $(( $1 % $2 )) -eq 0 ]
}

plan_minute() {
    # Echoes the tiers due at minute $1, most expensive first. "full" stands in
    # for all of them: it redraws the page, so nothing else has anything to do.
    local m="$1" out=""
    if due "$m" "$FULL_EVERY"; then
        echo "full"
        return 0
    fi
    due "$m" "$DATA_EVERY"     && out="$out sensors"
    due "$m" "$GRAPH_EVERY"    && out="$out chart"
    due "$m" "$FORECAST_EVERY" && out="$out forecast"
    due "$m" "$CLOCK_EVERY"    && out="$out clock"
    echo "${out# }"
}

flash_due() {
    # $1=minute counter $2=tier interval $3=flash every N of those tiers
    [ -n "$3" ] && [ "$3" -gt 0 ] 2>/dev/null || return 1
    [ "$2" -gt 0 ] 2>/dev/null || return 1
    [ $(( ($1 / $2) % $3 )) -eq 0 ]
}

# ── Cleanup on exit ──────────────────────────────────────────────────────────
cleanup() {
    # The sleep is a child process and does not get the signal we did. Left
    # alone it holds the script here for the rest of the minute, which is what
    # made Stop look like it had failed.
    [ -n "$SLEEP_PID" ] && kill "$SLEEP_PID" 2>/dev/null
    restore_sleep
    rm -rf "$TMP"
    rm -f /tmp/dash.pid
    exit 0
}

# A sleep that can be interrupted.
#
# `sleep 60` in the foreground cannot: a shell running a foreground command does
# not act on a trapped signal until that command returns, so pressing Stop in
# KUAL did nothing visible for up to a minute.
SLEEP_PID=""
nap() {
    sleep "$1" &
    SLEEP_PID=$!
    wait "$SLEEP_PID" 2>/dev/null
    SLEEP_PID=""
}

# Sleep until the top of the next minute, rather than for sixty seconds.
#
# A tick costs time — a fetch, a dozen draws, a refresh — so `nap 60` makes
# each cycle 60+N seconds and the clock walks away from the minute it is
# supposed to show. After forty ticks at a second and a half apiece it is a
# minute behind, displaying the wrong time and skipping a minute now and then
# to catch up. Aiming at the boundary costs nothing and never drifts.
nap_to_minute() {
    local sec
    sec=$(date +%S)
    sec=$(strip_zeros "$sec")            # 08 and 09 are not octal here either
    local delay=$(( 60 - sec ))
    # At :00 exactly, wait the whole minute rather than ticking twice for it.
    [ "$delay" -le 0 ] && delay=60
    nap "$delay"
}

# Sourced for the helpers alone — by settings.sh, and by the tests.
[ -n "${DASH_LIB_ONLY:-}" ] && return 0

# ══════════════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════════════

# FBInk is this script's entire output, and it is NOT part of the Kindle
# firmware — it is a separate binary the reader has to be given, and KUAL hands
# an extension a PATH that does not include the places people put it. Without
# it every `fb` call fails, stderr is on /dev/null where it belongs for a draw,
# the schedule keeps its minute, and the panel stays exactly as it was: a
# dashboard that is running perfectly and showing nothing, which from the sofa
# is the same thing as one that never started.
#
# `eips` IS in the firmware, so the one message that has to get through when
# there is no FBInk is the one message that can always be drawn.
if ! command -v fbink >/dev/null 2>&1; then
    echo "fbink not found. PATH=$PATH" >&2
    echo "FBInk is not part of the Kindle firmware: install it and make sure" >&2
    echo "the launcher can see it — kindle/README.md, 'It is in KUAL, but" >&2
    echo "pressing an entry does nothing'." >&2
    command -v eips >/dev/null 2>&1 &&
        eips 1 1 "ESP32 Dashboard: FBInk not found - see kual.log" 2>/dev/null
    exit 1
fi

trap cleanup INT TERM

mkdir -p "$TMP"
conf_init
conf_load
font_setup || echo "Continuing without text." >&2
prevent_sleep

# A layout BEFORE the first fetch, so no drawing path can run without one.
# RES_W and RES_H come from the collector, so this falls back to 600x800 and
# load_data() reloads it at the real resolution the moment the collector
# answers.
load_layout

if fetch_data; then
    load_data
    fetch_graph
    redraw_all "$(now_clock)"
else
    clear_screen
    refresh_screen
    redraw_offline "$(now_clock)"
fi

# ── Main loop ────────────────────────────────────────────────────────────────
# One tick a minute; the tiers decide what that tick costs.
MINUTE=0
while true; do
    nap_to_minute
    MINUTE=$((MINUTE + 1))
    # The reader's own clock, in the collector's chosen format. The format is
    # whatever the last payload said, so changing it in Settings shows up on
    # the next fetch — the same one-tick lag every other value on this page
    # has, and the clock itself is never stale because the time is local.
    NOW_TIME=$(now_clock)

    # Settings are re-read every minute rather than at startup, so a change
    # made from KUAL takes effect within a minute instead of needing Stop and
    # Start. It is one small read from a filesystem the kernel has cached, and
    # it is what makes the on-device settings menu usable at all.
    conf_load

    # settings.sh leaves this behind after any change: the settings screen it
    # painted is sitting on top of the dashboard, and whatever changed should
    # be visible now rather than at the top of the hour.
    if [ -f "$TMP/redraw" ]; then
        rm -f "$TMP/redraw"
        TIERS="full"
    else
        TIERS=$(plan_minute "$MINUTE")
    fi

    # ── Nothing to draw yet ─────────────────────────────────────────────────
    # No payload has ever parsed, so every reading is empty. Keep the reason on
    # screen and the clock current, and try again whenever the data tier comes
    # round; the first success draws the whole page.
    if [ "$HAVE_DATA" = "0" ]; then
        case " $TIERS " in
            *" full "*|*" sensors "*)
                if fetch_data && load_data; then
                    fetch_graph
                    redraw_all "$NOW_TIME"
                else
                    redraw_offline "$NOW_TIME"
                fi
                ;;
            *" clock "*) redraw_clock "$NOW_TIME" 0 ;;
        esac
        continue
    fi

    case " $TIERS " in
        *" full "*)
            fetch_data && load_data
            fetch_graph
            redraw_all "$NOW_TIME"
            continue
            ;;
    esac

    # One fetch serves however many tiers are due this minute.
    #
    # THE CHART TIER IS IN THIS LIST NOW, and it has to be. The axis labels
    # live in the payload (CH_Y0..CH_Y4 — see draw_chart_axis) and the image
    # comes from a separate fetch, so drawing one against the other's numbers
    # is a chart labelled with a scale it does not have. GRAPH_EVERY and
    # DATA_EVERY are both editable from the KUAL menu, so any pair where the
    # chart tier can fire without the data tier — 10 and 15, say — was
    # labelling a fresh image with a scale up to fifteen minutes old, and
    # nothing on the panel could show that it had happened.
    #
    # A payload fetch is one small request; the image it accompanies is fifty
    # times the size.
    case " $TIERS " in
        *" sensors "*|*" forecast "*|*" chart "*) fetch_data && load_data ;;
    esac

    case " $TIERS " in
        *" sensors "*)
            if flash_due "$MINUTE" "$DATA_EVERY" "$SENSOR_FLASH_EVERY"; then
                redraw_sensors "$NOW_TIME" 1
            else
                redraw_sensors "$NOW_TIME" 0
            fi
            ;;
    esac

    # A failed fetch leaves the previous chart in place, so redrawing it is
    # still right — the caption and the rule around it have to come back.
    case " $TIERS " in
        *" chart "*) fetch_graph || true; redraw_chart ;;
    esac

    case " $TIERS " in
        *" forecast "*) redraw_forecast ;;
    esac

    # The clock last, and only when the sensors tier has not already drawn it —
    # it lives inside that rectangle, so drawing it twice in one minute is a
    # second repaint of the same pixels.
    case " $TIERS " in
        *" sensors "*) ;;
        *" clock "*)
            if flash_due "$MINUTE" "$CLOCK_EVERY" "$CLOCK_FLASH_EVERY"; then
                redraw_clock "$NOW_TIME" 1
            else
                redraw_clock "$NOW_TIME" 0
            fi
            ;;
    esac
done
