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

conf_keys() {
    echo "HOST FETCH_TIMEOUT CLOCK_EVERY DATA_EVERY GRAPH_EVERY FORECAST_EVERY FULL_EVERY CLOCK_FLASH_EVERY SENSOR_FLASH_EVERY"
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
        Z_*|GRID_ZONES|GRID_ROWS|IN_ZONES|LBL_*|FC_*|FC[0-9]_*|WK[0-9]_*|WK_TODAY) return 0 ;;
        OUT_*|IN_*|RES_W|RES_H|LANG|DECIMALS|CLOCK_STYLE|SHOW_FLAGS) return 0 ;;
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

fetch_graph() {
    # Into a scratch file, and only into place once it is whole. wget -O
    # truncates its target the moment it opens it, so fetching straight onto
    # graph.bmp turned one WiFi hiccup into a zero-byte file — and since
    # redraw_chart clears its rectangle before drawing, that showed as a blank
    # strip where the chart was until the next successful fetch. Keeping the
    # last good chart is the better failure.
    if wget -q -T 15 -O "$TMP/graph.new" "$(host_url)/kindle/graph.bmp" 2>/dev/null \
       && [ -s "$TMP/graph.new" ]; then
        mv "$TMP/graph.new" "$TMP/graph.bmp"
        return 0
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

fb() { fbink "$@" 2>/dev/null; }

draw_text() {
    # $1=x $2=y $3=px $4=font file $5=colour $6=text
    #
    # -O/--bgless: draw the glyphs and nothing else. FBInk's OpenType renderer
    # otherwise fills the text's whole box with the background pen, which is
    # WHITE unless -B says otherwise — so white text on the inverted "today"
    # cell punched a white rectangle out of the black plate and disappeared
    # into it. Every tier clears its rectangle before drawing, so there is
    # nothing underneath that the glyphs need to cover.
    [ -n "$6" ] || return 0
    [ -n "$4" ] || return 0
    fb -q -b -O -C "$5" -t regular="$4",px="$3",left="$1",top="$2" -- "$6"
}

draw_text_bold() { draw_text "$1" "$2" "$3" "$FONT_BOLD" "$4" "$5"; }
draw_text_reg()  { draw_text "$1" "$2" "$3" "$FONT_REG"  "$4" "$5"; }

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
    [ -f "$1" ] || return 0
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
draw_clock() {
    local now_time="$1"
    fill_rect "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" WHITE
    draw_text_bold "$CL_X" "$CL_Y" "$CL_SIZE" "BLACK" "$now_time"
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
        usz=$(( sz * 42 / 100 ))
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
            # where at four tenths of the size it reads as a lower-case o.
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
baseline_y() {
    # $1=row top  $2=largest size in the row  $3=this size
    echo $(( $1 + ($2 - $3) * 80 / 100 ))
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
        draw_hline "$rx" "${IN_RULE_Y:-126}" "$rw" "GRAYA"
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
draw_chart_body() {
    draw_hline "$RULE2_X" "$RULE2_Y" "$RULE2_W" "GRAYA"
    draw_text_reg "$LAB_CHART_X" "$LAB_CHART_Y" "$LAB_SZ" "GRAY7" "$LBL_LAST24"
    draw_image "$TMP/graph.bmp" "$GR_X" "$GR_Y"
}

# ── Forecast, week strip and footer ──────────────────────────────────────────
# None of it is a sensor reading, so none of it is a slot: the forecast comes
# from an API, the week strip from the clock, and the footer is a constant.
draw_forecast_body() {
    draw_hline "$RULE3_X" "$RULE3_Y" "$RULE3_W" "GRAYA"

    if [ -n "$FC_SUMMARY" ]; then
        draw_text_reg "$LAB_FC_X" "$LAB_FC_Y" "$LAB_SZ" "GRAY7" "$LBL_FORECAST"

        local icon="$ICON_DIR/fc_${FC_CODE}_${FC_MAIN_SZ}.bmp"
        [ ! -f "$icon" ] && icon="$ICON_DIR/fc_-1_${FC_MAIN_SZ}.bmp"
        draw_image "$icon" "$FC_ICON_X" "$FC_ICON_Y"

        draw_text_reg "$FC_TEXT_X" "$FC_TEXT_Y" "$FC_TEXT_SZ" "BLACK" "$FC_SUMMARY"
        draw_text_bold "$FC_TEMP_X" "$FC_TEMP_Y" "$FC_TEMP_SZ" "BLACK" "${FC_HIGH}°/${FC_LOW}°"
        if [ -n "$FC_WIND" ] && [ "$FC_WIND" != "0" ]; then
            draw_text_reg "$FC_WIND_X" "$FC_WIND_Y" "$FC_WIND_SZ" "GRAY4" "$LBL_WIND ${FC_WIND} km/h"
        fi

        local i ol_label ol_code ol_temp ol_x ol_y ol_icon ol_icon_y ol_temp_y
        for i in 0 1 2; do
            eval "ol_label=\$FC${i}_LABEL"
            eval "ol_code=\$FC${i}_CODE"
            eval "ol_temp=\$FC${i}_TEMP"
            eval "ol_x=\$OL${i}_X"
            eval "ol_y=\$OL${i}_Y"

            [ -n "$ol_label" ] || continue
            draw_text_reg "$ol_x" "$ol_y" "$OL_LABEL_SZ" "GRAY7" "$ol_label"

            ol_icon="$ICON_DIR/fc_${ol_code}_${FC_OL_SZ}.bmp"
            [ ! -f "$ol_icon" ] && ol_icon="$ICON_DIR/fc_-1_${FC_OL_SZ}.bmp"
            ol_icon_y=$((ol_y + OL_ICON_OFFSET))
            draw_image "$ol_icon" "$ol_x" "$ol_icon_y"

            ol_temp_y=$((ol_y + OL_TEMP_OFFSET))
            draw_text_bold "$ol_x" "$ol_temp_y" "$OL_TEMP_SZ" "BLACK" "${ol_temp}°"
        done
    fi

    # Week strip
    local wk_x="$WK_X" wk_name wk_day wk_bg i
    for i in 0 1 2 3 4 5 6; do
        eval "wk_name=\$WK${i}_NAME"
        eval "wk_day=\$WK${i}_DAY"

        if [ "$i" = "$WK_TODAY" ]; then
            # Today: knocked out of a black plate.
            fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$WK_CELL_H" BLACK
            draw_text_reg "$wk_x" "$((WK_Y + WK_NAME_OFFSET))" "$WK_NAME_SZ" "WHITE" "$wk_name"
            draw_text_bold "$wk_x" "$((WK_Y + WK_DAY_OFFSET))" "$WK_DAY_SZ" "WHITE" "$wk_day"
        else
            wk_bg="GRAYE"
            [ "$i" = "5" ] || [ "$i" = "6" ] && wk_bg="GRAYD"
            fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$WK_CELL_H" "$wk_bg"
            draw_text_reg "$wk_x" "$((WK_Y + WK_NAME_OFFSET))" "$WK_NAME_SZ" "GRAY7" "$wk_name"
            draw_text_bold "$wk_x" "$((WK_Y + WK_DAY_OFFSET))" "$WK_DAY_SZ" "BLACK" "$wk_day"
        fi
        wk_x=$((wk_x + WK_CELL_W))
    done

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
    local y=$(( Z_SENS_H / 3 ))
    fill_rect "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" WHITE
    draw_text_bold "${OFF_X:-40}" "$y" "${OFF_SZ:-26}" "BLACK" \
                   "Cannot reach $(host_url)"
    draw_text_reg "${OFF_X:-40}" "$(( y + ${OFF_SZ:-26} + 12 ))" \
                  "${OFF_SUB_SZ:-16}" "GRAY5" \
                  "Check WiFi, or KUAL → Settings → Find collector"
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
    redraw_all "$(date +%H:%M)"
else
    clear_screen
    refresh_screen
    redraw_offline "$(date +%H:%M)"
fi

# ── Main loop ────────────────────────────────────────────────────────────────
# One tick a minute; the tiers decide what that tick costs.
MINUTE=0
while true; do
    nap_to_minute
    MINUTE=$((MINUTE + 1))
    NOW_TIME=$(date +%H:%M)

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
    case " $TIERS " in
        *" sensors "*|*" forecast "*) fetch_data && load_data ;;
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
