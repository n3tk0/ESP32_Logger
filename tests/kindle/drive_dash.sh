#!/bin/sh
# ============================================================================
# drive_dash.sh — run the Kindle dashboard's drawing and settings code with a
# fake FBInk, and assert on what it would have sent to the panel.
#
# WHY THIS EXISTS
# ---------------
# Nothing else could catch the class of bug this file was written for. The
# script's whole output is `fbink` invocations, and fbink is not installed
# anywhere in CI — so a flag that does not exist, a colour that is not in the
# palette, or a rectangle refreshed at the wrong coordinates all look exactly
# like a working dashboard from here. They looked like one on the device too:
# fbink exits non-zero and the script sends stderr to /dev/null, so the page
# simply came up blank in places.
#
# It was written after finding that the renderer used `-p` for pixel
# coordinates (it means --padded), `-M` for a partial refresh (--halfway),
# `-R WxH` and `-L W` for rectangles and lines (neither exists; -L is
# --linecountcode), `-F` with a font PATH (it names a built-in font) and
# colours GRAY10/GRAY14/GRAY15 (the palette is GRAY1..GRAY9, GRAYA..GRAYE).
#
# So the central assertion here is not "does it draw the right thing" — it is
# "would fbink have understood any of this at all", checked against FBInk's own
# option table.
#
#     sh tests/kindle/drive_dash.sh
# ============================================================================
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KDIR="$ROOT/kindle"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

FAILURES=0
CHECKS=0

check() {
    # $1=condition result (0 ok) $2=description
    CHECKS=$((CHECKS + 1))
    if [ "$1" -eq 0 ]; then
        echo "  ok   $2"
    else
        FAILURES=$((FAILURES + 1))
        echo "  FAIL $2"
    fi
}

# ── The fakes ────────────────────────────────────────────────────────────────
# fbink records its argv, one call per line, arguments tab-separated. Nothing in
# this dashboard's arguments contains a tab.
BIN="$WORK/bin"
mkdir -p "$BIN"
cat > "$BIN/fbink" <<'EOF'
#!/bin/sh
{ for a in "$@"; do printf '%s\t' "$a"; done; printf '\n'; } >> "$FBINK_LOG"
exit 0
EOF
cat > "$BIN/wget" <<'EOF'
#!/bin/sh
# Answers /kindle/data from the fixture and nothing else, so a scan finds
# exactly one collector.
out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in
        -O) out="$2"; shift 2 ;;
        -T|-t) shift 2 ;;
        -q) shift ;;
        http*) url="$1"; shift ;;
        *) shift ;;
    esac
done
# Both endpoints answer only for WGET_OK_HOST, so a test can point the
# dashboard at an address that is not there and see what it does about it.
case "$url" in
    *"$WGET_OK_HOST"*/kindle/data)
        if [ "$out" = "-" ] || [ -z "$out" ]; then cat "$FIXTURE"
        # DATA_TRUNCATE=1 is the connection that died partway through the
        # payload: a prefix of a good one, which parses perfectly and is
        # missing two thirds of its keys.
        elif [ "${DATA_TRUNCATE:-0}" = "1" ]; then head -20 "$FIXTURE" > "$out"
        else cp "$FIXTURE" "$out"; fi
        exit 0 ;;
    *"$WGET_OK_HOST"*/kindle/graph.bmp)
        # A REAL, SELF-CONSISTENT BMP: "BM", then the file's own length as a
        # 32-bit little-endian count at offset 2 (130 = \202), then bytes to
        # match. GRAPH_TRUNCATE=1 keeps the promise in the header and stops
        # writing short of it, which is what a collector that runs out of heap
        # halfway through the image leaves on the reader's disk.
        if [ -n "$out" ]; then
            if [ "${GRAPH_TRUNCATE:-0}" = "1" ]; then
                { printf 'BM\202\000\000\000'
                  dd if=/dev/zero bs=1 count=54 2>/dev/null; } > "$out"
            else
                { printf 'BM\202\000\000\000'
                  dd if=/dev/zero bs=1 count=124 2>/dev/null; } > "$out"
            fi
        fi
        exit 0 ;;
esac
# wget -O truncates its target before it knows whether the transfer will work.
[ -n "$out" ] && [ "$out" != "-" ] && : > "$out"
exit 1
EOF
cat > "$BIN/lipc-set-prop" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$BIN/ifconfig" <<'EOF'
#!/bin/sh
echo "wlan0     Link encap:Ethernet"
echo "          inet addr:10.9.9.7  Bcast:10.9.9.255  Mask:255.255.255.0"
EOF
# lipc — the Kindle's own property bus, which the power settings drive. The
# fakes record every call and answer cmState from a file the tests write, so
# association can be made to take a while, or never.
cat > "$BIN/lipc-set-prop" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$LIPC_LOG"
printf 'lipc %s\n' "$*" >> "$SYS_LOG"
# The one firmware difference nothing else can see: com.lab126.cmd is part of
# the reader framework on some readers, so with it stopped the set has nobody
# to answer it. Touching this file is how a test says "this is that reader".
[ -f "$LIPC_FAIL" ] && exit 1
exit 0
EOF
cat > "$BIN/lipc-get-prop" <<'EOF'
#!/bin/sh
printf 'get %s\n' "$*" >> "$LIPC_LOG"
# The reader's own battery, which the footer draws. A number, so that a test
# can tell "the panel did not ask" from "the panel asked and got nothing".
case "$*" in
    *battLevel*) echo "${FAKE_BATT:-62}"; exit 0 ;;
esac
[ -f "$WIFI_STATE" ] && cat "$WIFI_STATE"
exit 0
EOF
# upstart and the signal, which is how the reader framework is stopped and put
# back. Both write to the SAME log as lipc-set-prop, because the thing worth
# asserting about them is the ORDER: the framework serves the radio on the
# firmware above, so a restore that reaches the radio first restores nothing.
for t in start stop; do
    cat > "$BIN/$t" <<EOF
#!/bin/sh
printf '$t %s\n' "\$*" >> "\$SYS_LOG"
[ -f "\$GUI_NO_UPSTART" ] && exit 1
exit 0
EOF
done
cat > "$BIN/killall" <<'EOF'
#!/bin/sh
printf 'killall %s\n' "$*" >> "$SYS_LOG"
exit 0
EOF
chmod +x "$BIN"/*
PATH="$BIN:$PATH"
export PATH

FBINK_LOG="$WORK/fbink.log"
export FBINK_LOG
: > "$FBINK_LOG"

LIPC_LOG="$WORK/lipc.log"
WIFI_STATE="$WORK/wifi_state"
SYS_LOG="$WORK/sys.log"
LIPC_FAIL="$WORK/lipc_fail"
GUI_NO_UPSTART="$WORK/no_upstart"
export LIPC_LOG WIFI_STATE SYS_LOG LIPC_FAIL GUI_NO_UPSTART
export FAKE_BATT DATA_TRUNCATE
: > "$LIPC_LOG"
: > "$SYS_LOG"
rm -f "$LIPC_FAIL" "$GUI_NO_UPSTART"
echo "CONNECTED" > "$WIFI_STATE"

# The three kernel nodes suspend_for() reads and writes, pointed at files. A
# test that writes the real /sys/power/state suspends the machine running it.
# since_epoch is the RTC's own clock, which is the one the alarm is measured
# against — a test can set it to a number the system clock will never be, which
# is the only way to tell the two timebases apart.
RTC_WAKEALARM="$WORK/wakealarm"
RTC_SINCE_EPOCH="$WORK/since_epoch"
PM_STATE="$WORK/pm_state"
export RTC_WAKEALARM RTC_SINCE_EPOCH PM_STATE
: > "$RTC_WAKEALARM"
: > "$RTC_SINCE_EPOCH"
: > "$PM_STATE"

# A font, because -t/--truetype needs a file that exists.
mkdir -p "$WORK/fonts"
: > "$WORK/fonts/Bookerly-Regular.ttf"
: > "$WORK/fonts/Bookerly-Bold.ttf"

# ── The fixture ──────────────────────────────────────────────────────────────
# A trimmed /kindle/data payload, in the shape the collector emits — including
# a forecast summary that would be a command if anything ever executed it.
FIXTURE="$WORK/data.txt"
export FIXTURE
cat > "$FIXTURE" <<'EOF'
Z_GROUP_OUT="OUTSIDE"
Z_GROUP_IN="INSIDE"
Z_HERO_VALUE="-2.4"
Z_HERO_UNIT="°"
Z_HERO_ARROW=""
Z_HERO_BOLD=1
Z_HERO_ADVW=2100
Z_HERO_VADVW=2100
Z_HERO_UADVW=400
Z_HERO_INK="BLACK"
Z_BIG_VALUE="71"
Z_BIG_UNIT="%"
Z_BIG_VADVW=1200
Z_BIG_UADVW=600
Z_SUB="-2.4 to 15.3°  ·  3 min"
GRID_ZONES="PRES DEW CO2"
GRID_ROWS="3"
Z_PRES_VALUE="1008"
Z_PRES_UNIT="hPa"
Z_PRES_LABEL="PRESSURE"
Z_PRES_ARROW="↘"
Z_PRES_VADVW=2400
Z_PRES_UADVW=1900
Z_DEW_VALUE="3.1"
Z_DEW_UNIT="°"
Z_DEW_LABEL="DEW"
Z_DEW_VADVW=1600
Z_CO2_VALUE="640"
Z_CO2_UNIT="ppm"
Z_CO2_LABEL="CO2"
Z_CO2_VADVW=1800
IN_ZONES="ITEMP IHUM IAQI"
Z_ITEMP_VALUE="21.0"
Z_ITEMP_UNIT="°"
Z_ITEMP_LABEL="TEMP"
Z_ITEMP_VADVW=2000
Z_IHUM_VALUE="44"
Z_IHUM_UNIT="%"
Z_IHUM_LABEL="HUMIDITY"
Z_IHUM_VADVW=1200
Z_IAQI_VALUE="42"
Z_IAQI_LABEL="AQI"
Z_IAQI_VADVW=1200
FC_SUMMARY="Showers\"; touch $WORK/pwned; #"
FC_CODE=61
FC_HIGH=14
FC_LOW=3
FC_WIND=23
FC0_LABEL="21:00"
FC0_LABELW=2260
FC0_CODE=61
FC0_TEMP=6
FC0_TEMPW=830
FC1_LABEL="00:00"
FC1_LABELW=2260
FC1_CODE=3
FC1_TEMP=4
FC1_TEMPW=830
FC2_LABEL="03:00"
FC2_LABELW=2260
FC2_CODE=0
FC2_TEMP=2
FC2_TEMPW=830
WK0_NAME="MO"
WK0_DAY=24
WK0_NAMEW=1240
WK0_DAYW=1000
WK1_NAME="TU"
WK1_DAY=25
WK1_NAMEW=1240
WK1_DAYW=1000
WK2_NAME="WE"
WK2_DAY=26
WK2_NAMEW=1240
WK2_DAYW=1000
WK3_NAME="TH"
WK3_DAY=27
WK3_NAMEW=1240
WK3_DAYW=1000
WK4_NAME="FR"
WK4_DAY=28
WK4_NAMEW=1240
WK4_DAYW=1000
WK5_NAME="SA"
WK5_DAY=29
WK5_NAMEW=1240
WK5_DAYW=1000
WK6_NAME="SU"
WK6_DAY=30
WK6_NAMEW=1240
WK6_DAYW=1000
WK_TODAY=1
WK_MON_MONTH="MARCH"
WK_SUN_MONTH="APRIL"
OUT_BATT_WARN=1
CLOCK="12:34"
CLOCK_ADVW=2400
CLOCK_STYLE=0
TIME_FORMAT=0
DATE="24 MARCH"
SHOW_CHART=1
SHOW_WEEK=1
CHART_OUT=1
CHART_IN=1
KEY_OUT_ADVW=5200
CH_Y0="33"
CH_Y0W=1000
CH_Y1="31"
CH_Y1W=1000
CH_Y2="28"
CH_Y2W=1000
CH_Y3="26"
CH_Y3W=1000
CH_Y4="23"
CH_Y4W=1000
CH_H0="-23h"
CH_H0W=1830
CH_H1="-17h"
CH_H1W=1830
CH_H2="-11h"
CH_H2W=1830
CH_H3="-5h"
CH_H3W=1330
CH_H4="now"
CH_H4W=1500
CH_L=40
CH_R=556
CH_T=10
CH_B=194
CH_NOTE=""
LBL_KEY_OUT="outside mean"
LBL_KEY_BAND="shaded band = hourly low to high"
LBL_KEY_IN="inside"
LBL_OUTSIDE="OUTSIDE"
LBL_INSIDE="INSIDE"
LBL_LAST24="LAST 24 HOURS"
LBL_FORECAST="FORECAST"
LBL_MEASURED="Measured on site"
LBL_WIND="wind"
LBL_TO="to"
LBL_OFFLINE="Няма връзка с"
LBL_OFFLINE_HINT="Проверете WiFi"
RES_W=600
RES_H=800
END=1
EOF

# ── Load the dashboard as a library ──────────────────────────────────────────
DASH_TMP="$WORK/tmp"
DASH_CONF="$WORK/dash.conf"
export DASH_TMP DASH_CONF
mkdir -p "$DASH_TMP"
cp "$FIXTURE" "$DASH_TMP/data.txt"

# ASSIGNED, NOT PREFIXED. `VAR=x . file` is a variable assignment on a
# SPECIAL builtin, so a POSIX shell keeps it after the command returns —
# which is the whole reason the prefix spelling worked here. bash discards
# it unless it is in POSIX mode, so under `bash tests/kindle/drive_dash.sh`
# DASH_DIR came back empty the moment the source returned: everything the
# library reads AT source time still worked (CONF, USR_FONTS), everything
# it reads LATER did not — load_layout sourced "/layout/600x800.conf",
# found nothing, and 35 checks failed on empty sizes rather than on
# anything this file is testing. Not exported, because dash does not
# export them either and update_dash.sh is RUN as a child further down,
# where an inherited DASH_LIB_ONLY would stop it before it draws.
DASH_LIB_ONLY=1
DASH_DIR="$KDIR"
. "$KDIR/update_dash.sh"

# Named here so the next shell to lose it says so, rather than failing
# thirty-five drawing checks on arithmetic against an empty size.
check "$([ -n "$DASH_DIR" ] && [ -d "$DASH_DIR/layout" ] && echo 0 || echo 1)" \
      "the library still knows where it lives after it has been sourced"

USR_FONTS="$WORK/fonts"
font_setup

# ── The FBInk option table, from its own getopt definition ───────────────────
# Short flags taking no argument, taking one, and taking an optional one. If
# fbink gains an option, this list is what a new call has to be added to — and
# a call using anything absent from it is the bug this file exists to catch.
FB_NOARG="b h f c m M p r v q a e I L l V o O T H E Z z w G Q"
FB_ARG="y x Y X S F g i j J C B P A t W K d"
FB_OPTARG="s k D"

is_in() {
    case " $2 " in *" $1 "*) return 0 ;; esac
    return 1
}

# Does this recorded call parse against the table above?
# Echoes a reason on failure.
validate_call() {
    local line="$1"
    local OLDIFS="$IFS"
    IFS='	'
    # shellcheck disable=SC2086
    set -- $line
    IFS="$OLDIFS"
    local a flag
    while [ $# -gt 0 ]; do
        a="$1"; shift
        case "$a" in
            --) break ;;                       # everything after is the string
            --*) echo "long option $a not checked"; return 1 ;;
            -?) flag=${a#-}
                if is_in "$flag" "$FB_NOARG"; then
                    continue
                elif is_in "$flag" "$FB_ARG"; then
                    [ $# -gt 0 ] || { echo "-$flag needs an argument"; return 1; }
                    shift
                elif is_in "$flag" "$FB_OPTARG"; then
                    # Its suboptions, when present, are the next token.
                    case "${1:-}" in
                        top=*|left=*|width=*|height=*) shift ;;
                    esac
                else
                    echo "no such fbink option: -$flag"
                    return 1
                fi
                ;;
            -*) echo "unparsed argument $a"; return 1 ;;
            *) ;;                              # a positional string
        esac
    done
    return 0
}

FB_COLOURS="BLACK GRAY1 GRAY2 GRAY3 GRAY4 GRAY5 GRAY6 GRAY7 GRAY8 GRAY9 GRAYA GRAYB GRAYC GRAYD GRAYE WHITE"

validate_colours() {
    local line="$1" OLDIFS="$IFS" a want=0
    IFS='	'
    # shellcheck disable=SC2086
    set -- $line
    IFS="$OLDIFS"
    for a in "$@"; do
        if [ "$want" = 1 ]; then
            case "$a" in
                '#'*) ;;
                *) is_in "$a" "$FB_COLOURS" || { echo "no such colour: $a"; return 1; } ;;
            esac
            want=0
            continue
        fi
        case "$a" in -C|-B) want=1 ;; esac
    done
    return 0
}

# px= as draw_text computes it, for the assertions below.
px_of() { text_geom 0 "$1"; echo "$TX_PX"; }
# And the `top=` it lands on for a design y, which is NOT that y: draw_text()
# centres the taller box text_geom() asks FBInk for on the design's line. It
# came to the same number while the outlook label was 11 px — 11*1160/1000 is
# 12, and half of one pixel rounds to none — so an assertion written as the
# raw y passed by arithmetic accident and broke the first time that size grew.
top_of() { text_geom "$1" "$2"; echo "$TX_TOP"; }

lines_of() { [ -s "$1" ] && wc -l < "$1" | tr -d ' ' || echo 0; }
# The STRING a call drew at a given x, exactly. Asserting on the whole log
# instead was fine while one line on the page used a middle dot; the footer's
# status line uses one too, and two checks about the wind line then started
# failing on a change to the footer.
drawn_at() {
    # $1=the left= pixel  $2=the px= size, where two calls share an x — the
    # forecast summary and the wind line under it are both at FC_TEXT_X.
    # -> the text after --, or "" if nothing drew there
    local line pat="left=$1,"
    [ -n "${2:-}" ] && pat="px=$2,left=$1,"
    line=$(grep -- "$pat" "$FBINK_LOG" 2>/dev/null | head -1)
    case "$line" in
        *"	--	"*) line=${line##*"	--	"}; printf '%s' "${line%	}" ;;
        *) printf '' ;;
    esac
}
# grep -c always prints a count, including 0 — the `|| echo 0` this used to
# carry appended a SECOND line on no-match, so "$n" became "0\n0" and every
# numeric test on it was a syntax error rather than a comparison.
calls()    { grep -c . "$FBINK_LOG" 2>/dev/null; }
reset_log() { : > "$FBINK_LOG"; }

echo "The Kindle dashboard, drawn against a fake FBInk:"

# ── 1. Everything it draws is a command FBInk understands ────────────────────
# THROUGH load_data, not load_kv: it is the entry point the loop uses, and it
# is what records that these readings came off the network rather than out of
# the cache — which is what decides whether the page draws them at all.
load_data
load_layout
reset_log
redraw_all "12:34"

n=$(calls)
check "$([ "$n" -gt 20 ] && echo 0 || echo 1)" \
      "a full redraw issues $n fbink calls"

bad=""
while IFS= read -r line; do
    [ -n "$line" ] || continue
    if reason=$(validate_call "$line"); then :; else
        bad="$reason | $(printf '%s' "$line" | tr '\t' ' ')"
        break
    fi
done < "$FBINK_LOG"
check "$([ -z "$bad" ] && echo 0 || echo 1)" \
      "every option is one FBInk defines${bad:+ — $bad}"

bad=""
while IFS= read -r line; do
    [ -n "$line" ] || continue
    if reason=$(validate_colours "$line"); then :; else
        bad="$reason"
        break
    fi
done < "$FBINK_LOG"
check "$([ -z "$bad" ] && echo 0 || echo 1)" \
      "every colour is in the palette${bad:+ — $bad}"

# ── The flags this dashboard must never use again ────────────────────────────
#
# The table above only says an option EXISTS. -p, -M and -L all exist — as
# --padded, --halfway and --linecountcode — which is exactly why they were
# used by mistake for pixel coordinates, a partial refresh and a line, and
# exactly why a syntax check alone would wave the regression straight through.
# This is the list that catches it: what each one really means, and what to use
# instead.
deny_flag() {
    # $1=flag $2=what it actually is / what to use
    if grep -q "	$1	" "$FBINK_LOG" || grep -q "	$1$" "$FBINK_LOG"; then
        check 1 "never $1 — $2"
    else
        check 0 "never $1 — $2"
    fi
}
deny_flag "-p" "that is --padded, not pixel coordinates; -t left=/top= positions"
deny_flag "-M" "that is --halfway, which centres text vertically"
deny_flag "-L" "that is --linecountcode, which takes no argument; -k draws rules"
deny_flag "-R" "no such option at all; -k top=,left=,width=,height= fills"
deny_flag "-F" "that names a BUILT-IN font; -t regular=FILE takes a path"
deny_flag "-x" "character columns, not pixels; -t left= is pixels"
deny_flag "-y" "character rows, not pixels; -t top= is pixels"

# Text: TrueType, at a pixel size, with the string after --
tcalls=$(grep -c -- '-t	regular=' "$FBINK_LOG")
check "$([ "$tcalls" -gt 10 ] && echo 0 || echo 1)" \
      "$tcalls strings are drawn with -t regular=…"
check "$(grep -q -- '-t	regular=[^	]*px=' "$FBINK_LOG" && echo 0 || echo 1)" \
      "the size is given in pixels (px=), not points"
check "$(grep -q '	--	' "$FBINK_LOG" && echo 0 || echo 1)" \
      "strings are passed after --, so a value of -2.4 is not read as options"
check "$(grep -q '	--	-2.4' "$FBINK_LOG" && echo 0 || echo 1)" \
      "and the fixture's negative temperature is one of them"

# The font it names has to exist.
fontfile=$(sed -n 's/.*regular=\([^,]*\),.*/\1/p' "$FBINK_LOG" | head -1)
check "$([ -f "$fontfile" ] && echo 0 || echo 1)" \
      "the font it names exists: $(basename "${fontfile:-none}")"

# ── 2. One refresh per zone, and the flash goes where it was asked ───────────
reset_log
redraw_clock "12:35" 1
refreshes=$(grep -c -- '	-s	' "$FBINK_LOG")
check "$([ "$refreshes" -eq 1 ] && echo 0 || echo 1)" \
      "a clock update refreshes once ($refreshes)"
check "$(grep -q -- '-f	-s	top='"$Z_CLOCK_Y"',left='"$Z_CLOCK_X"',width='"$Z_CLOCK_W"',height='"$Z_CLOCK_H" "$FBINK_LOG" && echo 0 || echo 1)" \
      "and flashes exactly the clock rectangle"
check "$(grep -q -- '-k	top='"$Z_CLOCK_Y" "$FBINK_LOG" && echo 0 || echo 1)" \
      "clearing the old time first, so a shorter one leaves nothing behind"
undeferred=0
while IFS= read -r line; do
    case "$line" in
        *"	-t	"*|*"	-k	"*|*"	-g	"*) ;;   # a drawing call
        *) continue ;;
    esac
    case "$line" in *"	-b	"*) ;; *) undeferred=$((undeferred + 1)) ;; esac
done < "$FBINK_LOG"
check "$undeferred" \
      "every drawing call defers its refresh with -b ($undeferred without)"

reset_log
redraw_sensors "12:35" 0
check "$(grep -q -- '-s	top='"$Z_SENS_Y"',left='"$Z_SENS_X"',width='"$Z_SENS_W"',height='"$Z_SENS_H" "$FBINK_LOG" && echo 0 || echo 1)" \
      "a sensor update refreshes the readings rectangle"
check "$(grep -q -- '-f	-s	top='"$Z_SENS_Y" "$FBINK_LOG" && echo 1 || echo 0)" \
      "without flashing it — a third of the screen going black every 5 min is not a fix"
refreshes=$(grep -c -- '	-s	' "$FBINK_LOG")
check "$([ "$refreshes" -le 1 ] && echo 0 || echo 1)" \
      "and refreshes once, not once per string ($refreshes)"

reset_log
redraw_forecast
check "$(grep -q -- '-s	top='"$Z_FC_Y" "$FBINK_LOG" && echo 0 || echo 1)" \
      "a forecast update refreshes the forecast rectangle"

reset_log
redraw_all "12:36"
check "$(tail -1 "$FBINK_LOG" | grep -q -- '-f	-s	' && echo 0 || echo 1)" \
      "a full redraw ends in a flashing whole-screen refresh"

# The zones must tile the screen: a gap is a strip that never gets repainted.
check "$([ "$Z_SENS_H" -eq "$Z_CHART_Y" ] && echo 0 || echo 1)" \
      "the readings zone ends where the chart zone starts"
check "$([ $((Z_CHART_Y + Z_CHART_H)) -eq "$Z_FC_Y" ] && echo 0 || echo 1)" \
      "the chart zone ends where the forecast zone starts"
check "$([ $((Z_FC_Y + Z_FC_H)) -eq "$RES_H" ] && echo 0 || echo 1)" \
      "and the forecast zone ends at the bottom of the panel"

# Both shipped panels, not just the one the fixture names: a layout whose
# dividers do not add up leaves a strip of the screen that no tier ever
# repaints, and it would only show up on the panel nobody tested with.
for res in 600x800 1072x1448; do
    RES_W=${res%x*}; RES_H=${res#*x}
    load_layout
    ok=0
    [ "$Z_SENS_H" -eq "$Z_CHART_Y" ] || ok=1
    [ $((Z_CHART_Y + Z_CHART_H)) -eq "$Z_FC_Y" ] || ok=1
    [ $((Z_FC_Y + Z_FC_H)) -eq "$RES_H" ] || ok=1
    [ "$Z_SENS_W" -eq "$RES_W" ] || ok=1
    [ "$Z_CLOCK_Y" -ge 0 ] && [ $((Z_CLOCK_Y + Z_CLOCK_H)) -le "$Z_SENS_H" ] || ok=1
    check "$ok" "the $res layout's zones tile the whole panel"
done
RES_W=600; RES_H=800; load_layout

# ── 2b. The clock rectangle is cleared and flashed, so nothing else may live
# in it ───────────────────────────────────────────────────────────────────────
# The rule above the indoor row sat six pixels inside it: drawn by
# draw_sensors_body, erased by draw_clock, on every sensor redraw and every
# minute's clock tier.
for res in 600x800 1072x1448; do
    RES_W=${res%x*}; RES_H=${res#*x}
    load_layout
    check "$([ $((Z_CLOCK_Y + Z_CLOCK_H)) -le "${IN_RULE_Y:-99999}" ] && echo 0 || echo 1)" \
          "$res: the clock rect ($Z_CLOCK_Y..$((Z_CLOCK_Y + Z_CLOCK_H))) stops above the indoor rule (${IN_RULE_Y:-none})"
    check "$([ "$Z_CLOCK_H" -gt "${CL_SIZE:-0}" ] && echo 0 || echo 1)" \
          "and is still taller than the digits in it (${CL_SIZE:-?} px)"
done
RES_W=600; RES_H=800; load_layout

# ── Text on a dark plate is knocked out with --invert ────────────────────────
#
# NOT -C WHITE -B BLACK, which draws the opposite. FBInk has a fast path for
# text whose pens are pure black and pure white — abs(fgcolor - bgcolor) ==
# 0xFF in print_ot() — where it uses stb's coverage mask directly, XORed with
# 0xFF, on the assumption that B&W text means black on white unless --invert
# says otherwise. Asking for white on black is exactly the pair that triggers
# it, so today's cell came out as a white box with a black date in it, in the
# middle of the black plate meant to contain a white one.
#
# --invert flips the mask AND the pens, so the ordinary pens through it give
# white-on-black out of both FBInk's paths — and on every Kindle, its own
# condition compensating for the legacy models' inverted colour map.
reset_log
draw_forecast_body
check "$(grep -q -- '	-h	-C	BLACK	-B	WHITE' "$FBINK_LOG" && echo 0 || echo 1)" \
      "the inverted cell is knocked out with --invert and the ordinary pens"
check "$(grep -q -- '-C	WHITE' "$FBINK_LOG" && echo 1 || echo 0)" \
      "and never asks for WHITE on BLACK, the pair that draws the opposite"
badbg=0
while IFS= read -r line; do
    case "$line" in *"	-t	"*) ;; *) continue ;; esac
    # Either bgless over a cleared rectangle, or inverted out of a plate.
    case "$line" in
        *"	-O	"*) ;;
        *"	-h	"*) ;;
        *) badbg=$((badbg + 1)) ;;
    esac
done < "$FBINK_LOG"
check "$badbg" "and every other string is bgless or inverted ($badbg neither)"

# ── The forecast icon comes reduced, so the panel never guesses ──────────────
#
# There are eleven icon files, one per range of WMO codes. This script cannot
# reduce a code to its range, so it asked for the code it was given: on a
# partly-cloudy afternoon Open-Meteo answers 2, there is no fc_2_52.bmp, and
# the panel drew fc_-1 — the circled question mark that is supposed to mean
# "no forecast at all". Three of them, in a row, next to a browser page showing
# sun and cloud. The collector reduces it now and sends FC_ICON.
( reset_log
  FC_ICON=1 FC_CODE=2 draw_forecast_body
  grep -q -- "fc_1_${FC_MAIN_SZ}.bmp" "$FBINK_LOG" || exit 1
  grep -q -- "fc_2_${FC_MAIN_SZ}.bmp" "$FBINK_LOG" && exit 2
  grep -q -- "fc_-1_${FC_MAIN_SZ}.bmp" "$FBINK_LOG" && exit 3
  exit 0 )
check "$?" "the main icon is the reduced code (FC_ICON), not the raw one"

( reset_log
  FC0_ICON=61 FC0_CODE=65 FC1_ICON=1 FC1_CODE=2 FC2_ICON=95 FC2_CODE=96 \
      draw_forecast_body
  grep -q -- "fc_61_${FC_OL_SZ}.bmp" "$FBINK_LOG" || exit 1
  grep -q -- "fc_1_${FC_OL_SZ}.bmp"  "$FBINK_LOG" || exit 2
  grep -q -- "fc_95_${FC_OL_SZ}.bmp" "$FBINK_LOG" || exit 3
  grep -q -- "fc_65_" "$FBINK_LOG" && exit 4
  exit 0 )
check "$?" "and so is each outlook column's"

# A collector too old to send FC_ICON still gets what it always got.
( reset_log
  unset FC_ICON
  FC_CODE=1 draw_forecast_body
  grep -q -- "fc_1_${FC_MAIN_SZ}.bmp" "$FBINK_LOG" || exit 1 )
check "$?" "with no FC_ICON at all it falls back to FC_CODE, as it always did"

# ── The forecast's age sits beside the wind, as it does on the page ──────────
#
# "вятър 5 km/h · 8 мин". The panel drew only the wind, so the one line that
# says whether to believe a forecast was on one screen and not the other.
( reset_log
  FC_WIND=5 FC_AGE="8 мин" draw_forecast_body
  grep -q "5 km/h · 8 мин" "$FBINK_LOG" || exit 1 )
check "$?" "the wind line carries the forecast's age"

( reset_log
  FC_WIND=0 FC_AGE="8 мин" draw_forecast_body
  got=$(drawn_at "$FC_WIND_X" "$(px_of "$FC_WIND_SZ")")
  [ "$got" = "8 мин" ] || { echo "got [$got]" >&2; exit 1; }
  exit 0 )
check "$?" "and with no wind to report it is the age alone, with no stray dot"

( reset_log
  FC_WIND=5 FC_AGE="" draw_forecast_body
  got=$(drawn_at "$FC_WIND_X" "$(px_of "$FC_WIND_SZ")")
  [ "$got" = "$LBL_WIND 5 km/h" ] || { echo "got [$got]" >&2; exit 1; }
  exit 0 )
check "$?" "and with no age, the wind alone"

# ── FBInk's px is not the design's px ────────────────────────────────────────
#
# FBInk sizes OT text with stbtt_ScaleForPixelHeight, which stb_truetype
# documents as `pixels / (ascent - descent)`: its px is the whole line height,
# not the em that CSS font-size means. The panel asked for px=88 where the page
# said font-size:88px and drew a sixth small — and, because every gap is
# computed as `size × advance-in-mille / 1000` against an em that was a sixth
# smaller than the size, pushed the headline's second value into the divider.
text_geom 200 100
check "$([ "$TX_PX" -gt 100 ] && echo 0 || echo 1)" \
      "a design size is asked of FBInk larger than itself ($TX_PX for 100)"
check "$([ "$TX_PX" -le 130 ] && echo 0 || echo 1)" \
      "  and not by more than a line height can plausibly be ($TX_PX)"
check "$([ "$TX_TOP" -lt 200 ] && echo 0 || echo 1)" \
      "the box starts higher by half its growth, so the string does not drop"
( # Every layout carries the ratio, and in a band a real font can be in.
  ok=0
  for res in 600x800 1072x1448; do
      RES_W=${res%x*}; RES_H=${res#*x}; load_layout
      [ -n "${TEXT_PX_MILLE:-}" ] || ok=1
      [ "${TEXT_PX_MILLE:-0}" -ge 1000 ] 2>/dev/null || ok=1
      [ "${TEXT_PX_MILLE:-9999}" -le 1400 ] 2>/dev/null || ok=1
  done
  exit $ok )
check "$?" "every layout names its font's (ascent - descent) / em"
RES_W=600; RES_H=800; load_layout

# ── TRACE writes down what was drawn, and changes nothing about it ───────────
#
# The panel is the one renderer nobody can watch: the tests drive it against a
# fake FBInk that records its argv, a browser has devtools, and the thing on
# the wall has neither — so "that cell does not look right" gets argued about
# from photographs. One line per call settles it.
( reset_log
  TRACE=0 draw_forecast_body 2>"$WORK/trace_off.txt"
  off=$(calls); cp "$FBINK_LOG" "$WORK/calls_off.txt"
  reset_log
  TRACE=1 draw_forecast_body 2>"$WORK/trace_on.txt"
  on=$(calls)
  # The same calls, in the same order: a switch that changed the drawing would
  # be a switch nobody could trust the output of.
  cmp -s "$WORK/calls_off.txt" "$FBINK_LOG" || exit 1
  [ "$off" = "$on" ] || exit 2
  [ ! -s "$WORK/trace_off.txt" ] || exit 3
  [ -s "$WORK/trace_on.txt" ] || exit 4
  grep -q "^fbink " "$WORK/trace_on.txt" || exit 5
  # And it is the real argv, not a summary: the pens are what a report about a
  # cell drawn in the wrong colour turns on.
  grep -q "^fbink .*-h -C BLACK -B WHITE" "$WORK/trace_on.txt" || exit 6
  exit 0 )
check "$?" "TRACE=1 logs every FBInk call and draws exactly the same panel"

# ── Power: the radio, and the suspend ────────────────────────────────────────
#
# A ten-year-old Kindle looping a shell script with an associated radio draws
# 60-80 mA against a cell that is probably down to 600-900 mAh. Most minutes on
# this panel need no network at all — the clock comes from the reader's own
# clock — so most minutes should not be paying for one.
lipc_reset() { : > "$LIPC_LOG"; }

# awake changes nothing. It is the default, and the setting exists so that
# somebody who does not want any of this is not given it.
( lipc_reset
  POWER=awake net_up
  POWER=awake net_down
  [ ! -s "$LIPC_LOG" ] || exit 1 )
check "$?" "POWER=awake touches the radio not at all"

( lipc_reset
  echo "CONNECTED" > "$WIFI_STATE"
  POWER=wifi net_up || exit 1
  grep -q "wirelessEnable 1" "$LIPC_LOG" || exit 2
  POWER=wifi net_down
  grep -q "wirelessEnable 0" "$LIPC_LOG" || exit 3
  exit 0 )
check "$?" "POWER=wifi turns the radio on for a fetch and off after it"

# Association is not instant: four to ten seconds for the chip and DHCP. Waited
# for rather than slept through, and given up on rather than waited for ever.
( lipc_reset
  echo "SEARCHING" > "$WIFI_STATE"
  start=$(date +%s)
  POWER=wifi WIFI_WAIT=2 net_up && exit 1        # it must report the timeout
  [ $(( $(date +%s) - start )) -ge 2 ] || exit 2 # and it must actually wait
  grep -q "cmState" "$LIPC_LOG" || exit 3
  exit 0 )
check "$?" "and gives the radio a bounded wait to associate, not an unbounded one"

# Which minutes need it at all — the whole of where the battery goes.
( needs_net "clock" && exit 1
  needs_net "" && exit 2
  needs_net "sensors clock" || exit 3
  needs_net "chart" || exit 4
  needs_net "forecast" || exit 5
  needs_net "full" || exit 6
  exit 0 )
check "$?" "a clock-only minute needs no network; every other tier does"

# ── The suspend, which is the one that can end the dashboard ─────────────────
#
# Everything else here degrades: a failed fetch keeps the last reading, a
# failed draw comes back next minute. A suspend with no alarm behind it is a
# panel that stays dark until somebody presses the power button.
( : > "$PM_STATE"; : > "$RTC_WAKEALARM"
  # SUSPEND_MIN_DOWN=0: the fake /sys/power/state returns at once, and that is
  # the shape of a machine that did NOT go down — which suspend_for refuses to
  # call a suspend, because the early-wake path would otherwise repaint the
  # whole page on every pass through the main loop. A test that wants the
  # suspend to have happened has to say that it did.
  SUSPEND_MIN_DOWN=0 suspend_for 45 || exit 1
  [ "$(cat "$PM_STATE")" = "mem" ] || exit 2
  # The alarm is in the future, by about what was asked for.
  now=$(date +%s); a=$(cat "$RTC_WAKEALARM")
  [ "$a" -gt "$now" ] || exit 3
  [ $(( a - now )) -le 46 ] || exit 4
  exit 0 )
check "$?" "suspend_for sets an RTC alarm ahead of itself and then suspends"

# A node that takes the write and does not keep it — the failure that leaves a
# device asleep with nothing to wake it.
#
# THE REAL FUNCTION, not a copy of it here. /dev/null is writable and reads
# back empty, which is exactly the shape of that failure, and driving the copy
# instead would be a check that passes whatever suspend_for() does.
( : > "$PM_STATE"
  RTC_WAKEALARM=/dev/null suspend_for 45 && exit 1
  [ ! -s "$PM_STATE" ] || exit 2      # and nothing was suspended
  exit 0 )
check "$?" "an alarm that does not read back stops it going down at all"

( : > "$PM_STATE"
  suspend_for 2 && exit 1             # under SUSPEND_MIN
  [ ! -s "$PM_STATE" ] || exit 2
  exit 0 )
check "$?" "and a wait too short to be worth a transition is slept, not suspended"

( : > "$PM_STATE"
  RTC_WAKEALARM="$WORK/nope/wakealarm" suspend_for 45 && exit 1
  [ ! -s "$PM_STATE" ] || exit 2
  exit 0 )
check "$?" "a device with no RTC alarm node never suspends"

# THE ALARM IS A NUMBER IN THE RTC'S TIMEBASE, NOT THE SYSTEM CLOCK'S. The
# kernel compares this node against the RTC; `date +%s` reads the system clock.
# They agree only while the RTC runs in UTC, and on a reader whose does not, an
# absolute alarm computed from `date` lands hours away — a panel dark until it
# comes round, or one waking on every tick. The read-back cannot see it: the
# digits stick either way, so the guard that exists to refuse a suspend with no
# alarm behind it would confirm exactly that suspend.
#
# since_epoch is that clock. Setting it to a number the system clock will never
# hold is the only way to prove which one the sum was taken from.
( : > "$PM_STATE"; : > "$RTC_WAKEALARM"
  echo 1000 > "$RTC_SINCE_EPOCH"
  SUSPEND_MIN_DOWN=0 suspend_for 60 || exit 1
  [ "$(cat "$RTC_WAKEALARM")" = "1060" ] || exit 2
  exit 0 )
check "$?" "the wake alarm is set in the RTC's own timebase, not the system clock's"

# A driver that does not publish since_epoch is not a driver to refuse to sleep
# on: the system clock is the best answer left, and it is the right one on
# every reader whose RTC is in UTC — which is all of the ones Amazon ships.
( : > "$PM_STATE"; : > "$RTC_WAKEALARM"; : > "$RTC_SINCE_EPOCH"
  now=$(date +%s)
  SUSPEND_MIN_DOWN=0 suspend_for 60 || exit 1
  back=$(cat "$RTC_WAKEALARM")
  [ "$back" -ge $((now + 60)) ] && [ "$back" -le $((now + 65)) ] || exit 2
  exit 0 )
check "$?" "and falls back to the system clock where the RTC does not publish one"

# Stop hands the Kindle back to its owner. With the radio off and the reader
# framework stopped, "hands it back" has to mean putting both of them right.
#
# DRIVEN, NOT GREPPED. This read cleanup()'s source for the string
# "wirelessEnable 1" and called that covered — so it went green on a cleanup()
# that reached the radio before the framework that answers for it, and it went
# RED the moment the same call moved behind a radio_set() helper. A test that
# fails on a rename and passes on a wrong order is testing the spelling.
sys_reset() { : > "$SYS_LOG"; : > "$LIPC_LOG"; }
# Which line of the shared log a pattern first appears on, or nothing.
sys_line() { grep -n -- "$1" "$SYS_LOG" 2>/dev/null | head -1 | cut -d: -f1; }

( sys_reset
  TMP="$WORK/cleanup-tmp"; RADIO_MARK="$TMP/radio-off"; GUI_MARK="$TMP/gui-stopped"
  mkdir -p "$TMP"
  POWER=wifi GUI_STOP=1 GUI_STOPPED=0 GUI_BLOCKED=0
  gui_apply                                  # the framework goes down
  net_down                                   # and the radio with it
  sys_reset
  ( DASH_PIDFILE="$WORK/cleanup.pid" cleanup ) >/dev/null 2>&1
  g=$(sys_line 'start lab126_gui'); [ -n "$g" ] || g=$(sys_line 'killall -CONT')
  r=$(sys_line 'wirelessEnable 1')
  [ -n "$g" ] || exit 1                      # the framework came back
  [ -n "$r" ] || exit 2                      # and so did the radio
  # THE ORDER IS THE POINT. The framework answers com.lab126.cmd on the reader
  # where GUI_STOP takes the radio down with it, so a radio restored first is
  # a restore with nobody to hear it.
  [ "$g" -lt "$r" ] || exit 3
  [ -d "$TMP" ] && exit 4                    # and it cleared up after itself
  exit 0 )
check "$?" "Stop puts the framework back, then the radio, then tidies up"

# And when nothing ran the trap at all. stop.sh sends SIGKILL ten seconds after
# SIGTERM, so the dashboards whose cleanup() never runs are exactly the ones it
# creates — wedged, OOM-killed, killed by hand. The markers under DASH_TMP are
# what survives that, and stop.sh is what reads them.
( sys_reset
  st="$WORK/stopsh"; rm -rf "$st"; mkdir -p "$st"
  echo 2 > "$st/gui-stopped"        # the VM was SIGSTOPped
  : > "$st/radio-off"               # and the radio turned off
  DASH_PIDFILE="$WORK/none.pid" DASH_TMP="$st" sh "$KDIR/stop.sh" >/dev/null 2>&1
  g=$(sys_line 'killall -CONT'); r=$(sys_line 'wirelessEnable 1')
  [ -n "$g" ] || exit 1
  [ -n "$r" ] || exit 2
  [ "$g" -lt "$r" ] || exit 3       # the framework first here too
  [ -d "$st" ] && exit 4
  exit 0 )
check "$?" "stop.sh puts back what a dashboard that never ran its trap left"

# From the markers, and only the markers. A reader on POWER=awake who has never
# turned GUI_STOP on does not get their radio switched on by a script they
# asked to stop.
( sys_reset
  st="$WORK/stopsh2"; rm -rf "$st"; mkdir -p "$st"
  DASH_PIDFILE="$WORK/none.pid" DASH_TMP="$st" sh "$KDIR/stop.sh" >/dev/null 2>&1
  [ -n "$(sys_line 'wirelessEnable 1')" ] && exit 1
  [ -n "$(sys_line 'start lab126_gui')$(sys_line 'killall -CONT')" ] && exit 2
  exit 0 )
check "$?" "and turns on nothing the dashboard never turned off"

# The radio this script turned off is the radio this script turns back on —
# which is not the same question as what POWER says by then. POWER is re-read
# every minute, and awake is the value that makes every path here return
# without touching anything: switching back to it left the reader with no
# network and nothing in the script that would ever restore it, Stop included.
( sys_reset
  TMP="$WORK/power-tmp"; RADIO_MARK="$TMP/radio-off"; mkdir -p "$TMP"
  POWER=wifi RADIO_OFF=0
  net_down
  grep -q "wirelessEnable 0" "$LIPC_LOG" || exit 1
  [ -f "$RADIO_MARK" ] || exit 2
  sys_reset
  POWER=awake power_apply
  grep -q "wirelessEnable 1" "$LIPC_LOG" || exit 3
  [ -f "$RADIO_MARK" ] && exit 4
  # And having put it back, it does not keep putting it back every minute.
  sys_reset
  power_apply
  [ -s "$LIPC_LOG" ] && exit 5
  exit 0 )
check "$?" "switching POWER back to awake turns the radio on again"

# wifid answers with a word, and three of the words it can answer with contain
# the one being looked for. A `*CONNECTED*` glob read DISCONNECTED as connected
# — the exact answer the bounded wait exists to wait through — and fired the
# fetch into an interface that was still coming up.
( sys_reset
  TMP="$WORK/wifi-tmp"; RADIO_MARK="$TMP/radio-off"; mkdir -p "$TMP"
  for state in DISCONNECTED NOT_CONNECTED; do
      echo "$state" > "$WIFI_STATE"
      POWER=wifi WIFI_WAIT=1 net_up && exit 1
  done
  echo "CONNECTED" > "$WIFI_STATE"
  POWER=wifi WIFI_WAIT=1 net_up || exit 2
  exit 0 )
check "$?" "DISCONNECTED is not read as CONNECTED"

# And the framework stop is reversible rather than fatal: STOP/CONT or the
# service, never a kill, because a killed cvm needs a reboot.
( body=$(sed -n '/^gui_stop() {/,/^}/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q -- "-STOP cvm" || exit 1
  printf '%s' "$body" | grep -qE "killall +-(9|KILL)" && exit 2
  body=$(sed -n '/^gui_restore() {/,/^}/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q -- "-CONT cvm" || exit 3
  exit 0 )
check "$?" "and stops the reader framework in a way Stop can undo"

# GUI_STOP is in conf_keys(), so conf_load() re-reads it every minute and
# conf_write() promises the reader "the dashboard picks this up within a
# minute". It was acted on ONCE, before the loop: turning it on from Settings
# did nothing at all, and turning it back off left the framework down until
# Stop — which, with the framework down, is a menu entry that is not on screen.
( sys_reset
  TMP="$WORK/gui-tmp"; GUI_MARK="$TMP/gui-stopped"; mkdir -p "$TMP"
  GUI_STOPPED=0 GUI_BLOCKED=0 POWER=awake
  GUI_STOP=1 gui_apply
  { sys_line 'stop lab126_gui' || sys_line 'killall -STOP'; } >/dev/null
  [ -n "$(sys_line 'stop lab126_gui')$(sys_line 'killall -STOP')" ] || exit 1
  [ -f "$GUI_MARK" ] || exit 2
  sys_reset
  GUI_STOP=0 gui_apply
  [ -n "$(sys_line 'start lab126_gui')$(sys_line 'killall -CONT')" ] || exit 3
  [ -f "$GUI_MARK" ] && exit 4
  # Idempotent: a tick that changes nothing touches nothing.
  sys_reset
  gui_apply
  [ -s "$SYS_LOG" ] && exit 5
  exit 0 )
check "$?" "GUI_STOP is applied every minute, both ways, and only on a change"

# THE ONE COMBINATION THAT CAN LEAVE THE PANEL BLANK FOR A DAY. On firmware
# where com.lab126.cmd is part of the reader framework, GUI_STOP takes the
# radio's own control interface down with it — so POWER=wifi could turn the
# radio off and never get it back, and every fetch for the rest of the run
# would fail with nothing on the panel to say why. net_up() is the only place
# that can tell the two firmwares apart, because it is the only one that asks.
( sys_reset
  TMP="$WORK/heal-tmp"; RADIO_MARK="$TMP/radio-off"; GUI_MARK="$TMP/gui-stopped"
  mkdir -p "$TMP"
  GUI_STOPPED=0 GUI_BLOCKED=0 POWER=wifi GUI_STOP=1
  gui_apply
  [ "${GUI_STOPPED:-0}" = "0" ] && exit 1
  sys_reset
  : > "$LIPC_FAIL"                        # com.lab126.cmd stops answering
  echo "CONNECTED" > "$WIFI_STATE"
  net_up >/dev/null 2>&1
  rm -f "$LIPC_FAIL"
  [ -n "$(sys_line 'start lab126_gui')$(sys_line 'killall -CONT')" ] || exit 2
  [ "${GUI_BLOCKED:-0}" = "1" ] || exit 3
  # And it stays back: re-applying the setting must not stop it again while the
  # radio is still needed, or the next minute undoes the repair.
  sys_reset
  gui_apply
  [ -n "$(sys_line 'stop lab126_gui')$(sys_line 'killall -STOP')" ] && exit 4
  # ... but with nobody asking the radio for anything, the saving is still on
  # offer, so awake gets it back.
  sys_reset
  POWER=awake gui_apply
  [ -n "$(sys_line 'stop lab126_gui')$(sys_line 'killall -STOP')" ] || exit 5
  exit 0 )
check "$?" "a framework that takes the radio with it is put back, and stays back"

# THE LOOP'S WIRING, WHICH IS THE ONE THING THIS FILE CANNOT DRIVE.
# DASH_LIB_ONLY stops the source before MAIN, so every test here calls the
# functions directly and nothing ever runs a tick. That is fine for what the
# functions do and useless for whether they are called — and a setting that is
# re-read every minute but applied only at startup looks exactly like one that
# works, which is what GUI_STOP was for two commits. So the loop is read.
( body=$(sed -n '/^while true; do/,/^done$/p' "$KDIR/update_dash.sh")
  line() { printf '%s' "$body" | grep -n "^    $1\$" | head -1 | cut -d: -f1; }
  n=$(line net_down); c=$(line conf_load)
  pw=$(line power_apply); g=$(line gui_apply)
  [ -n "$n" ] && [ -n "$c" ] && [ -n "$pw" ] && [ -n "$g" ] || exit 1
  # After the read, not before it: applying last minute's value is not applying
  # the setting.
  [ "$c" -lt "$pw" ] && [ "$c" -lt "$g" ] || exit 2
  # And the radio goes down at the TOP, before the nap — the tick body has
  # `continue` in four places, and a radio turned off after them is a radio
  # left on for the rest of the day on exactly the paths that took a shortcut.
  [ "$n" -lt "$c" ] || exit 3
  exit 0 )
check "$?" "the loop re-reads the settings and then applies them"

# ── The screen the dashboard is drawn on, and the tap menu ───────────────────
#
# FBInk writes to the framebuffer; it does not own the screen. The reader's own
# framework does — it repaints its library whenever it likes and every touch
# goes to it — which is why the dashboard kept dropping back to the home screen
# for a minute at a time, and why tapping it opened a book.

# CANVAS asks the framework to put its chrome away, and says so on disk so that
# stop.sh can put it back when this script never reaches its trap.
# DRIVEN THROUGH THE APPLIER, because that is what the loop calls. CANVAS is
# re-read every minute like every other key, so it has to be re-applied every
# minute or the KUAL entry that sets it does nothing until the next Start —
# which is the bug gui_apply() exists for, made twice.
( sys_reset
  TMP="$WORK/canvas-tmp"; CANVAS_MARK="$TMP/canvas"; mkdir -p "$TMP"
  CANVAS=desktop canvas_apply
  [ -s "$LIPC_LOG" ] && exit 1              # the default touches nothing
  [ -f "$CANVAS_MARK" ] && exit 2
  sys_reset
  CANVAS=blank canvas_apply
  grep -q "disableEnablePillow 1" "$LIPC_LOG" || exit 3
  [ -f "$CANVAS_MARK" ] || exit 4
  # Idempotent: a tick that changes nothing touches nothing.
  sys_reset; CANVAS=blank canvas_apply
  [ -s "$LIPC_LOG" ] && exit 5
  # And turning it back off hands the chrome back, within the minute.
  sys_reset; CANVAS=desktop canvas_apply
  grep -q "disableEnablePillow 0" "$LIPC_LOG" || exit 6
  [ -f "$CANVAS_MARK" ] && exit 7
  sys_reset; CANVAS=desktop canvas_apply
  [ -s "$LIPC_LOG" ] && exit 8
  exit 0 )
check "$?" "CANVAS is applied every minute, both ways, and only on a change"

# An input event is sixteen bytes: two 32-bit timestamps, a 16-bit type, a
# 16-bit code, a 32-bit value. The reader decodes them with od, because busybox
# has od and does not have evtest.
ev() {
    # $1=type $2=code $3=value — one record, little-endian, on stdout.
    printf "$(printf '\\%03o' 0 0 0 0 0 0 0 0 \
        $(( $1 & 255 )) $(( ($1 >> 8) & 255 )) \
        $(( $2 & 255 )) $(( ($2 >> 8) & 255 )) \
        $(( $3 & 255 )) $(( ($3 >> 8) & 255 )) 0 0)"
}

( : > "$WORK/ev.bin"
  # One finger: X, Y, frame — then a second frame that must NOT produce a
  # second line, because a stroke is one tap however many positions it reports.
  { ev 3 53 412; ev 3 54 690; ev 0 0 0
    ev 3 53 413; ev 3 54 691; ev 0 0 0; } > "$WORK/ev.bin"
  out=$(touch_reader "$WORK/ev.bin")
  [ "$out" = "412 690" ] || { echo "got [$out]" >&2; exit 1; }
  exit 0 )
check "$?" "a touch is decoded to one x y line per stroke, not per event"

# ABS_X/ABS_Y (0/1) as well as the multitouch pair (53/54): which of them a
# reader sends is the reader's business, and a panel that speaks the older one
# is not a panel with no touchscreen.
( { ev 3 0 120; ev 3 1 240; ev 0 0 0; } > "$WORK/ev.bin"
  [ "$(touch_reader "$WORK/ev.bin")" = "120 240" ] || exit 1
  exit 0 )
check "$?" "and the single-touch axes are read as well as the multitouch pair"

# A STROKE IS ONE TAP, AND THE NEXT TAP IS THE NEXT TAP. The first version
# counted forty frames after an emit and called that the debounce — but a real
# tap is three to thirty frames, so the budget left over from one tap swallowed
# the one after it: the tap that opens the bar ate the tap that presses the
# button it opened. What ends a contact is the finger leaving, which panels say
# either with BTN_TOUCH going to zero or with a frame carrying no coordinates.
( { ev 3 53 100; ev 3 54 700; ev 0 0 0      # tap 1, two frames
    ev 3 53 101; ev 3 54 701; ev 0 0 0
    ev 1 330 0;  ev 0 0 0                   # finger up, the way most panels say it
    ev 3 53 300; ev 3 54 750; ev 0 0 0      # tap 2
    ev 0 0 0                                # an empty frame: the other way
    ev 3 53 500; ev 3 54 760; ev 0 0 0; } > "$WORK/ev.bin"
  out=$(touch_reader "$WORK/ev.bin" | tr '\n' '/')
  [ "$out" = "100 700/300 750/500 760/" ] || { echo "got [$out]" >&2; exit 1; }
  exit 0 )
check "$?" "three taps are three lines, however each panel says the finger left"

# SYN_REPORT ONLY. SYN_MT_REPORT (code 2) separates the contacts inside one
# frame on a protocol-A panel and SYN_DROPPED (code 3) says the kernel's queue
# overflowed; treating either as the end of a frame reports a position from
# half a frame and calls the state good when the kernel has just said it is not.
( { ev 3 53 210; ev 0 2 0                   # first contact, not a frame end
    ev 3 53 999; ev 3 54 888; ev 0 3 0      # SYN_DROPPED, not a frame end
    ev 3 54 640; ev 0 0 0; } > "$WORK/ev.bin"
  out=$(touch_reader "$WORK/ev.bin")
  [ "$out" = "999 640" ] || { echo "got [$out]" >&2; exit 1; }
  exit 0 )
check "$?" "and only SYN_REPORT ends a frame, not the multitouch or dropped ones"

# THE MOST DESTRUCTIVE BUTTON IS THE LAST ONE THAT SHOULD WIN A DEFAULT. Quit
# was the fall-through, so a coordinate out of range or not a number at all —
# an uncalibrated panel reporting thousands on a 600x800 screen, which is the
# exact case TOUCH_MAXX exists for — failed both thirds and exited the
# dashboard on the reader's second tap.
( RES_W=600 RES_H=800
  menu_hit 2900 3100; [ "$MENU_HIT" = "outside" ] || exit 1
  menu_hit 900 760;   [ "$MENU_HIT" = "outside" ] || exit 2
  menu_hit "" 760;    [ "$MENU_HIT" = "outside" ] || exit 3
  menu_hit 300 "";    [ "$MENU_HIT" = "outside" ] || exit 4
  menu_hit abc 760;   [ "$MENU_HIT" = "outside" ] || exit 5
  # And a real tap on the right-hand third still quits.
  menu_hit 550 760;   [ "$MENU_HIT" = "quit" ]    || exit 6
  exit 0 )
check "$?" "an uncalibrated or nonsense coordinate dismisses the bar, never quits"

# The maxima travel with their axes: TOUCH_MAXX names the range of the value
# the panel reports FIRST, which is what the TRACE line shows. Dividing a
# swapped value by the other axis's maximum is how a calibrated panel still
# lands on the wrong third.
( RES_W=600 RES_H=800
  TOUCH_SWAP=1 TOUCH_MAXX=1024 TOUCH_MAXY=768 touch_scale 512 384
  # raw (512,384) swaps to (384,512); 384 of 768 across, 512 of 1024 down.
  [ "$TAP_X" = "300" ] || { echo "x=$TAP_X" >&2; exit 1; }
  [ "$TAP_Y" = "400" ] || { echo "y=$TAP_Y" >&2; exit 2; }
  exit 0 )
check "$?" "a swapped panel's calibration follows the axis it was measured on"

# THE MENU AND A REAL SUSPEND USED NOT TO COMBINE, and the menu won — which
# meant TOUCH=1 quietly cancelled POWER=suspend, and the two settings a reader
# most wants together were the one pair that could not be had. What decides now
# is the wake window: nothing reads the touchscreen while the CPU is down
# because nothing can, and the power button is what brings it back.
( body=$(sed -n '/^nap_to_minute() {/,/^}/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q 'TOUCH_READY' && exit 1
  printf '%s' "$body" | grep -q '! wake_window' || exit 2
  # And TAP is cleared on every path out, not only inside nap_or_tap — the
  # suspend returns before that one, so a tap taken on one tick was still in
  # TAP on the next and menu_hit ran again on stale coordinates.
  printf '%s' "$body" | grep -q '^    TAP=""' || exit 3
  exit 0 )
check "$?" "what stops a suspend is somebody being there, not the menu being armed"

# `read -t` is not POSIX. A shell without it errors at once rather than
# waiting, with the complaint swallowed — and a wait that counts reads instead
# of looking at the clock turns a minute into thirty instant iterations and the
# main loop into a spin, fetching and flashing on a battery.
( body=$(sed -n '/^nap_or_tap() {/,/^}/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q 'start=$(date +%s)' || exit 1
  printf '%s' "$body" | grep -q 'now - start )) -ge "$want"' || exit 2
  printf '%s' "$body" | grep -q 'nap 1' || exit 3
  exit 0 )
check "$?" "and the wait is bounded by the clock, not by counting the reads"

# strip_zeros() turns "" into "0", which is right for a tier nobody filled in
# and wrong for a device path. conf_load() knew; cmd_set() did not, so the
# override could never be cleared once set. One function, asked by both.
( conf_is_text HOST      || exit 1
  conf_is_text TOUCH_DEV || exit 2
  conf_is_text CANVAS    || exit 3
  conf_is_text MENU_LBL  || exit 4
  conf_is_text DATA_EVERY && exit 5
  conf_is_text TOUCH     && exit 6
  grep -q 'conf_is_text' "$KDIR/settings.sh" || exit 7
  exit 0 )
check "$?" "the keys that are not numbers are one list, and settings.sh asks it"

# Every key in conf_keys() carries a line of help, or conf_write() bakes a bare
# "# " above it and dash.conf.default's careful wording is gone the first time
# anything is saved.
( missing=""
  for k in $(conf_keys); do
      [ -n "$(conf_help "$k")" ] || missing="$missing $k"
  done
  [ -z "$missing" ] || { echo "no help for:$missing" >&2; exit 1; }
  exit 0 )
check "$?" "and every key says what it means, so a save keeps dash.conf readable"

# A key with no built-in default is empty on every dash.conf written before it
# existed — which is every one already on a reader.
( missing=""
  for k in $(conf_keys); do
      eval "v=\$$k"
      conf_valid "$k" "$v" || missing="$missing $k"
  done
  [ -z "$missing" ] || { echo "no usable default for:$missing" >&2; exit 1; }
  exit 0 )
check "$?" "every key has a built-in default its own validator accepts"

# A panel that reports its own scale rather than the screen's.
( RES_W=600 RES_H=800
  TOUCH_MAXX=0 TOUCH_MAXY=0 TOUCH_SWAP=0 touch_scale 300 400
  [ "$TAP_X" = "300" ] && [ "$TAP_Y" = "400" ] || exit 1
  TOUCH_MAXX=1200 TOUCH_MAXY=1600 TOUCH_SWAP=0 touch_scale 600 800
  [ "$TAP_X" = "300" ] && [ "$TAP_Y" = "400" ] || exit 2
  TOUCH_MAXX=0 TOUCH_MAXY=0 TOUCH_SWAP=1 touch_scale 111 222
  [ "$TAP_X" = "222" ] && [ "$TAP_Y" = "111" ] || exit 3
  exit 0 )
check "$?" "a panel with its own scale, or its axes swapped, still lands where it was touched"

# The bar is the bottom ninth, ruled into as many buttons as MENU_ACT names,
# and a tap above it dismisses.
( RES_W=600 RES_H=800 MENU_ACTS=""
  menu_geom
  [ "$MENU_H" = "88" ] || exit 1             # 800/9
  [ "$MENU_Y" = "712" ] || exit 2            # 800 - 88
  [ "$MENU_N" = "4" ] || exit 3              # the built-in four
  [ "$MENU_SLOT" = "150" ] || exit 4
  menu_hit 50 760;  [ "$MENU_HIT" = "refresh" ]  || exit 5
  menu_hit 200 760; [ "$MENU_HIT" = "wake" ]     || exit 6
  menu_hit 350 760; [ "$MENU_HIT" = "settings" ] || exit 7
  menu_hit 550 760; [ "$MENU_HIT" = "quit" ]     || exit 8
  menu_hit 300 400; [ "$MENU_HIT" = "outside" ]  || exit 9
  # The slots meet exactly: 150 belongs to the button on its right.
  menu_hit 149 760; [ "$MENU_HIT" = "refresh" ]  || exit 10
  menu_hit 150 760; [ "$MENU_HIT" = "wake" ]     || exit 11
  # And the last slot keeps the remainder of an odd division, so the bar has
  # no dead strip down its right-hand edge for a finger to land in.
  menu_hit 599 760; [ "$MENU_HIT" = "quit" ]     || exit 12
  exit 0 )
check "$?" "the bar is ruled into one slot per action, and a tap above it is outside"

# A reader who wants three buttons gets three, and the hit test agrees with
# what was drawn — the number of buttons comes from ONE list.
( RES_W=600 RES_H=800 MENU_ACTS="" MENU_ACT="refresh|hide|quit"
  menu_geom
  [ "$MENU_N" = "3" ] || exit 1
  [ "$MENU_SLOT" = "200" ] || exit 2
  menu_hit 50 760;  [ "$MENU_HIT" = "refresh" ] || exit 3
  menu_hit 250 760; [ "$MENU_HIT" = "hide" ]    || exit 4
  menu_hit 450 760; [ "$MENU_HIT" = "quit" ]    || exit 5
  exit 0 )
check "$?" "and a bar configured with three buttons is three, not four"

# ONE LIST, ASKED BY BOTH. menu_geom used to fall back to the built-in actions
# when MENU_ACTS was empty and menu_hit did not, so a hit test run before any
# bar had been opened measured four slots and found no action in any of them:
# every coordinate came back `outside`, which is a bar whose buttons all do
# nothing.
( RES_W=600 RES_H=800 MENU_ACTS=""
  menu_hit 50 760
  [ "$MENU_HIT" = "refresh" ] || exit 1
  exit 0 )
check "$?" "a hit test before any bar was opened still knows what the buttons are"

# Drawn along the bottom, in the inverted pens the today cell already uses —
# `-O` over a black plate leaves an empty black rectangle, which is the one
# place on the screen that has to be legible reading as a hole.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" MENU_LBL="" MENU_ACT=""
  FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  menu_open main
  menu_geom
  grep -q -- "-B	BLACK	-k	top=$MENU_Y,left=0,width=600,height=$MENU_H" "$FBINK_LOG" || exit 1
  for w in Refresh More Exit; do
      grep -q -- "--	$w" "$FBINK_LOG" || { echo "no $w" >&2; exit 2; }
  done
  # Knocked out of the plate, not drawn bgless over it.
  grep -q -- "-h	-C	BLACK	-B	WHITE" "$FBINK_LOG" || exit 3
  # And it refreshes only its own strip, not the whole screen.
  grep -q -- "-f	-s	top=$MENU_Y,left=0,width=600,height=$MENU_H" "$FBINK_LOG" || exit 4
  # Three dividers for four buttons, and none at either end.
  [ "$(grep -c -- "-B	GRAY7	-k" "$FBINK_LOG")" = "3" ] || exit 5
  exit 0 )
check "$?" "the menu is knocked out of a plate along the bottom, and refreshes only itself"

# The labels are the SCRIPT'S, not the collector's: the moment the bar is most
# wanted is the one where the collector cannot be reached.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  MENU_ACT="refresh|wake|settings|quit" MENU_LBL="Обнови|Буден|Още|Изход"
  menu_open main
  grep -q -- "--	Обнови" "$FBINK_LOG" || exit 1
  grep -q -- "--	Изход" "$FBINK_LOG" || exit 2
  exit 0 )
check "$?" "and are the reader's own words when they set them"

# A BAR WHOSE WORDS ARE ONE PLACE ALONG FROM ITS BUTTONS is worse than one in
# a language the reader does not read: it says the wrong thing about what a tap
# will do. Every dash.conf written before MENU_ACT existed carries exactly
# three labels, and the built-in bar has four buttons.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  MENU_ACT="refresh|wake|settings|quit" MENU_LBL="Refresh|Hide|Exit"
  menu_open main
  grep -q -- "--	Hide" "$FBINK_LOG" && exit 1
  grep -q -- "--	More" "$FBINK_LOG" || exit 2
  exit 0 )
check "$?" "a label list too short for the bar is refused, not drawn shifted along"

# THE SLEEP BUTTON SAYS WHICH WAY IT WILL GO. It is a toggle, and a button
# reading "Awake" that sends an already-awake panel to sleep lies about itself
# — on a screen that gives no other feedback at all. A label with no slash in
# it is used exactly as it is, which is every label anybody has already set.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  MENU_ACT="refresh|wake|settings|quit" MENU_LBL="Refresh|Awake/Sleep|More|Exit"
  POWER=suspend
  menu_open main
  grep -q -- "--	Awake" "$FBINK_LOG" || exit 1
  grep -q -- "--	Sleep" "$FBINK_LOG" && exit 2
  reset_log
  POWER=awake
  menu_open main
  grep -q -- "--	Sleep" "$FBINK_LOG" || exit 3
  grep -q -- "--	Awake" "$FBINK_LOG" && exit 4
  # And a one-part label is left alone, whatever the mode.
  reset_log
  MENU_LBL="Refresh|Буден|More|Exit"
  menu_open main
  grep -q -- "--	Буден" "$FBINK_LOG" || exit 5
  exit 0 )
check "$?" "the sleep button names the mode it will switch to, not the one it is in"

# And the same word wherever else it is named — the settings screen that
# explains the deep sleep has to be able to say which button gets back out.
( MENU_ACT="refresh|wake|settings|quit" MENU_LBL="Refresh|Awake/Sleep|More|Exit"
  menu_word wake || exit 1
  [ "$MENU_WORD" = "Awake" ] || { echo "got [$MENU_WORD]" >&2; exit 2; }
  menu_word settings || exit 3
  [ "$MENU_WORD" = "More" ] || exit 4
  menu_word nosuch && exit 5
  exit 0 )
check "$?" "and anything naming that button in a sentence gets the same word"

# EXIT ASKS FIRST. One tap turns the whole bar into the confirmation — one
# button, the full width, so the second tap cannot miss it and nothing else can
# be hit by accident while it is up. This panel's touch calibration is the
# thing most likely to be wrong on any given reader.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  SURE_LBL="Sure?"
  menu_open sure
  [ "$MENU" = "3" ] || exit 1
  [ "$MENU_N" = "1" ] || exit 2
  grep -q -- "--	Sure?" "$FBINK_LOG" || exit 3
  # Anywhere in the bar confirms; anywhere above it cancels.
  menu_hit 10 760;  [ "$MENU_HIT" = "sure" ]    || exit 4
  menu_hit 590 760; [ "$MENU_HIT" = "sure" ]    || exit 5
  menu_hit 300 400; [ "$MENU_HIT" = "outside" ] || exit 6
  exit 0 )
check "$?" "Exit asks first, on a bar that is one button wide"

# The settings bar, which is the whole of the settings menu on a reader running
# with GUI_STOP=1 — there is no KUAL there to open.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  MENU_LBL2="Find|Next|Battery|Info|Back"
  menu_open more
  [ "$MENU" = "2" ] || exit 1
  [ "$MENU_N" = "5" ] || exit 2
  menu_hit 10 760;  [ "$MENU_HIT" = "find" ] || exit 3
  menu_hit 590 760; [ "$MENU_HIT" = "back" ] || exit 4
  exit 0 )
check "$?" "and the settings bar reaches find, next, battery and info"

# CHARACTERS, NOT BYTES, or five labels on a 600 px panel run into each other.
# ${#var} counts bytes: "Обнови" is twelve of them for six letters.
( str_chars "Refresh";  [ "$STR_N" = "7" ] || exit 1
  str_chars "Обнови";   [ "$STR_N" = "6" ] || exit 2
  str_chars "";         [ "$STR_N" = "0" ] || exit 3
  exit 0 )
check "$?" "a label's width is counted in characters, in any language"

# And the type is sized to the slot it has to fit inside.
( reset_log
  RES_W=600 RES_H=800 MENU_ACTS="" FONT_REG="$WORK/fonts/Bookerly-Regular.ttf"
  MENU_ACT="refresh|wake|settings|hide|quit"
  MENU_LBL="Refresh|Stay awake|Settings|Hide|Exit"
  menu_open main
  # Five slots of 120 px, and "Stay awake" is ten characters: at the 28 px this
  # bar asks for by default that is 140 px of type in a 120 px slot.
  big=$(grep -o 'px=[0-9]*' "$FBINK_LOG" | sort -t= -k2 -n | tail -1 | cut -d= -f2)
  [ -n "$big" ] || exit 1
  [ "$big" -le 30 ] || { echo "px=$big in a 120px slot" >&2; exit 2; }
  exit 0 )
check "$?" "and a bar with five buttons shrinks its type to fit them"

# THE WIRING, which DASH_LIB_ONLY means this file cannot drive.
#
# THE MINUTE IS READ OFF THE CLOCK, NOT COUNTED. The comment over the tap
# handling promised that "a reader who taps four times should not fast-forward
# the chart", and a counter incremented once per pass through the loop did
# exactly that: each tap ran a tick and pushed the hourly full refresh a minute
# further out. So did every early wake from a suspend. And a counter cannot
# account for a suspend that slept through fourteen empty minutes, which is the
# whole of what turns POWER=suspend into days.
( body=$(sed -n '/^while true; do/,/^done$/p' "$KDIR/update_dash.sh")
  line() { printf '%s' "$body" | grep -n -- "$1" | head -1 | cut -d: -f1; }
  printf '%s' "$body" | grep -q 'MINUTE=\$((MINUTE + 1))' && exit 1
  printf '%s' "$body" | grep -q 'MINUTE=\$(( EPOCH / 60 - START_MIN ))' || exit 2
  t=$(line 'if \[ -n "${TAP:-}" \]; then')
  m=$(line 'MINUTE=\$(( EPOCH / 60 - START_MIN ))')
  [ -n "$t" ] && [ -n "$m" ] && [ "$t" -lt "$m" ] || exit 3
  # Exit runs the same cleanup Stop does — the way back GUI_STOP takes away —
  # and asks before it does.
  printf '%s' "$body" | grep -q 'quit)     menu_open sure' || exit 4
  printf '%s' "$body" | grep -q 'sure)     cleanup' || exit 5
  # And a second pass inside one minute draws nothing, rather than repainting
  # the tiers that minute has already had.
  printf '%s' "$body" | grep -q 'MINUTE" = "$LAST_MINUTE"' || exit 6
  exit 0 )
check "$?" "the minute comes off the clock, a tap does not spend one, and Exit asks"

# The trap gives back everything the run took, in the order that works.
( body=$(sed -n '/^cleanup() {/,/^}/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q 'touch_disarm'     || exit 1
  printf '%s' "$body" | grep -q 'canvas_give_back' || exit 2
  exit 0 )
check "$?" "and Stop disarms the screen and hands the chrome back"


# ── The button, which is the only way into a suspended Kindle ────────────────
#
# A SUSPENDED KINDLE WAKES FROM THE POWER BUTTON AND NOT FROM THE TOUCHSCREEN:
# the touch controller has no power while the CPU is down. So the button is the
# way in — and until this the panel did not notice it had been used. It woke,
# ran an ordinary tick, found nothing due that minute (four minutes in five
# there is nothing), drew nothing, and went straight back down. The press
# worked perfectly and was indistinguishable from a dead button.
#
# Nothing has to identify the wake source for that: the alarm says when we
# meant to come back and the RTC says when we did.
( : > "$PM_STATE"; : > "$RTC_WAKEALARM"; : > "$RTC_SINCE_EPOCH"
  SUSPEND_MIN_DOWN=0 suspend_for 45 || exit 1
  # Back with the whole of the wait still to run: a button, not the alarm.
  [ "$SUSPEND_EARLY" = "1" ] || exit 2
  exit 0 )
check "$?" "a resume with the wait still to run is reported as somebody waking it"

# WHICH rtc, ASKED WHERE THE ANSWER IS FIRST NEEDED. A reader with more than
# one RTC does not promise the first one holds the alarm the kernel honours,
# and where it does not, suspend_for() correctly refuses to go down and the
# deep sleep simply never happens with nothing saying why. Asked lazily rather
# than at startup, because POWER is re-read every minute: a panel that starts
# awake and is put into deep sleep from the menu an hour later has to ask too.
( body=$(sed -n '/^nap_to_minute() {/,/^}/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q 'rtc_pick' || exit 1
  # And the environment's own nodes are never overwritten by the probe — a
  # test that let it loose would suspend the machine running it.
  [ "$RTC_PINNED" = "1" ] || exit 2
  before="$RTC_WAKEALARM"
  rtc_pick || exit 3
  [ "$RTC_WAKEALARM" = "$before" ] || exit 4
  exit 0 )
check "$?" "the wake alarm's RTC is probed when a suspend needs it, not assumed"

# And one that lands on its alarm is not, or every ordinary tick would put the
# menu up. SUSPEND_SLACK is the tolerance between the two.
( : > "$PM_STATE"; : > "$RTC_WAKEALARM"; : > "$RTC_SINCE_EPOCH"
  SUSPEND_SLACK=100 SUSPEND_MIN_DOWN=0 suspend_for 45 || exit 1
  [ "$SUSPEND_EARLY" = "0" ] || exit 2
  exit 0 )

# ── A WRITE THAT COMES STRAIGHT BACK IS NOT A SUSPEND ───────────────────────
#
# This is the one failure the early-wake path could have turned into something
# worse than the bug it exists for: a /sys/power/state that returns without
# suspending would be answered with a full repaint and the bar, on every pass
# through the main loop — a flashing panel and a battery emptied in an
# afternoon, rather than a panel that merely never sleeps.
( : > "$PM_STATE"; : > "$RTC_WAKEALARM"; : > "$RTC_SINCE_EPOCH"
  SUSPEND_WARNED=0
  # INTO A FILE, not through $( ) or a pipe: both are subshells, and the latch
  # that makes this message a one-off is a variable the subshell would take
  # away with it — so the second call would warn again and the check would be
  # testing the harness rather than the script.
  suspend_for 45 2>"$WORK/s1.err" && exit 1     # it must report the failure
  grep -q "straight back" "$WORK/s1.err" || exit 2
  [ "$SUSPEND_EARLY" = "0" ] || exit 3          # and claim nobody woke it
  # ONCE, not once a minute: /tmp is a ramdisk on a device that runs for
  # months, and a line a minute is 1440 copies of one sentence in RAM.
  suspend_for 45 2>"$WORK/s2.err"
  [ -s "$WORK/s2.err" ] && exit 4
  exit 0 )
check "$?" "a suspend that returns with no time passed is refused, not answered"
check "$?" "and a resume near enough to its alarm is the alarm, not a person"

# THE WAKE WINDOW IS WHAT LETS THE TWO SETTINGS COEXIST. TOUCH=0 with
# WAKE_MENU=1 is the combination worth having on a wall: nothing reads the
# panel while nobody is there, and the button summons a menu when somebody is.
( TOUCH=0 AWAKE_UNTIL=0 TOUCH_DEV=/dev/input/event9
  touch_arm 2>"$WORK/ta.err"
  [ "$TOUCH_READY" = "0" ] || exit 1
  [ -s "$WORK/ta.err" ] && exit 2          # it did not even look
  # Inside the window it looks, whatever TOUCH says.
  AWAKE_UNTIL=$(( $(date +%s) + 60 ))
  touch_arm 2>"$WORK/ta.err"
  grep -q "touchscreen" "$WORK/ta.err" || exit 3
  exit 0 )
check "$?" "the touchscreen is read during a wake window even with TOUCH=0"

#
# ASSIGNED, NOT PREFIXED, wherever the function writes the variable back:
# `VAR=x func` is a temporary scope in dash, so an assignment the function
# makes to that same name is discarded when it returns. See the note over the
# library load above — the same shell difference, from the other side.
( AWAKE_UNTIL=0
  wake_window && exit 1
  AWAKE_UNTIL=$(( $(date +%s) + 60 )); wake_window || exit 2
  AWAKE_UNTIL=$(( $(date +%s) - 1 ));  wake_window && exit 3
  # A tap is somebody being here, so it opens the window as readily as it
  # pushes one out.
  AWAKE_UNTIL=0
  WAKE_HOLD=90
  wake_extend
  [ "$AWAKE_UNTIL" -gt "$(( $(date +%s) + 80 ))" ] || exit 4
  wake_window || exit 5
  exit 0 )
check "$?" "the window opens, closes by itself, and every tap pushes it out"

# ── Sleeping through the minutes with nothing in them ────────────────────────
#
# THE SAVING IS IN NOT WAKING UP. POWER=suspend with CLOCK_EVERY=1 suspends and
# comes back sixty times an hour, and every one of those is a resume, a draw
# and a flashing refresh of the clock — 1440 of each a day, for a setting whose
# whole promise is days of battery. The tiers already say which minutes have
# work in them.
( CLOCK_EVERY=1 DATA_EVERY=5 GRAPH_EVERY=15 FORECAST_EVERY=30 FULL_EVERY=60
  CLOCK_NOW=1
  next_due_in 0;  [ "$NEXT_DUE" = "1" ] || exit 1
  next_due_in 37; [ "$NEXT_DUE" = "1" ] || exit 2
  exit 0 )
check "$?" "at CLOCK_EVERY=1 every minute has work, so the sleep is one minute"

( CLOCK_EVERY=5 DATA_EVERY=15 GRAPH_EVERY=30 FORECAST_EVERY=60 FULL_EVERY=120
  CLOCK_NOW=5
  minute_busy 5  || exit 1
  minute_busy 7  && exit 2
  next_due_in 0; [ "$NEXT_DUE" = "5" ] || { echo "got $NEXT_DUE" >&2; exit 3; }
  next_due_in 7; [ "$NEXT_DUE" = "3" ] || { echo "got $NEXT_DUE" >&2; exit 4; }
  exit 0 )
check "$?" "at the battery-saver intervals it sleeps past the four empty minutes"

# Capped, because a panel should come back and look at itself now and again
# whatever the intervals say — a fetch that has been failing for half an hour
# is worth finding out about.
( CLOCK_EVERY=1440 DATA_EVERY=1440 GRAPH_EVERY=1440 FORECAST_EVERY=1440
  FULL_EVERY=1440 CLOCK_NOW=1440
  next_due_in 1; [ "$NEXT_DUE" = "30" ] || { echo "got $NEXT_DUE" >&2; exit 1; }
  exit 0 )
check "$?" "and never for longer than SLEEP_MAX_MIN, whatever the intervals say"

# ── Quiet hours ──────────────────────────────────────────────────────────────
# A flashing refresh is a black frame, and at three in the morning in a bedroom
# it is the brightest thing in the room.
( date() { echo "03"; }
  QUIET_FROM=22 QUIET_TO=7 quiet_eval || exit 1          # over midnight
  QUIET_FROM=1  QUIET_TO=6 quiet_eval || exit 2          # inside a plain range
  QUIET_FROM=8  QUIET_TO=17 quiet_eval && exit 3         # outside one
  QUIET_FROM=0  QUIET_TO=0 quiet_eval && exit 4          # equal: switched off
  exit 0 )
check "$?" "quiet hours are read off the clock, and wrap round midnight"

( QUIET_IS=1
  flash_due 50 5 10 && exit 1                            # would flash otherwise
  QUIET_IS=0
  flash_due 50 5 10 || exit 2
  exit 0 )
check "$?" "and nothing flashes inside them"

( date() { echo "03"; }
  CLOCK_EVERY=1 QUIET_FROM=22 QUIET_TO=7 QUIET_EVERY=15
  clock_tier
  [ "$CLOCK_NOW" = "15" ] || exit 1
  QUIET_EVERY=0 clock_tier
  [ "$CLOCK_NOW" = "1" ] || exit 2
  exit 0 )
check "$?" "the clock slows down inside them, which is what makes the sleep long"

# The full tier still redraws everything at night; it just does not go black
# doing it, and the morning is paid for in one flashing refresh.
( reset_log
  redraw_all "12:34" 0
  grep -q -- '-f	-s	$' "$FBINK_LOG" && exit 1
  grep -q -- '	-s	$' "$FBINK_LOG" || exit 2
  exit 0 )
check "$?" "a full redraw can be asked for without the flash"

( body=$(sed -n '/^while true; do/,/^done$/p' "$KDIR/update_dash.sh")
  printf '%s' "$body" | grep -q 'QUIET_WAS' || exit 1
  exit 0 )
check "$?" "and leaving them spends one, for the night's worth of ghosting"

# ── Half a payload is not a payload ──────────────────────────────────────────
#
# THE SAME QUESTION graph_ok() ASKS ABOUT THE IMAGE, unasked here for longer.
# The collector streams this while it is also serving the web UI, to a
# ten-year-old reader on wifi, and busybox wget does not always call a short
# read an error — so half a payload lands on disk looking exactly like a whole
# one. It parses. Every key past the cut is simply absent, and the page comes
# up with a third of its values blank and nothing to say why.
( printf 'A=1\nEND=1\n' > "$WORK/p1"; payload_ok "$WORK/p1" || exit 1
  printf 'A=1\nRES_H=800\n' > "$WORK/p2"; payload_ok "$WORK/p2" || exit 2
  printf 'A=1\nZ_HERO_VALUE="4"\n' > "$WORK/p3"; payload_ok "$WORK/p3" && exit 3
  : > "$WORK/p4"; payload_ok "$WORK/p4" && exit 4
  exit 0 )
check "$?" "a payload is whole if it ends where the collector ends it"

# And the good one on disk is kept, rather than replaced by the part.
( DASH_TMP_OLD="$TMP"
  cp "$FIXTURE" "$TMP/data.txt"
  DATA_TRUNCATE=1 WGET_OK_HOST=10.9.9.42 HOST=10.9.9.42 fetch_data 2>/dev/null && exit 1
  # Untouched: the same size it was, not the twenty lines the fake sent.
  [ "$(wc -c < "$TMP/data.txt")" = "$(wc -c < "$FIXTURE")" ] || exit 2
  [ -f "$TMP/data.new" ] && exit 3
  exit 0 )
check "$?" "a truncated one keeps the last good payload instead of replacing it"

( cp "$FIXTURE" "$TMP/data.txt"
  FAILS=0
  DATA_TRUNCATE=1 WGET_OK_HOST=10.9.9.42 HOST=10.9.9.42 fetch_data 2>/dev/null
  [ "$FAILS" = "1" ] || exit 1
  DATA_TRUNCATE=0 WGET_OK_HOST=10.9.9.42 HOST=10.9.9.42 fetch_data || exit 2
  [ "$FAILS" = "0" ] || exit 3
  exit 0 )
check "$?" "and a run of failures is counted, so one hiccup is not a verdict"

# ── A place that stops being sent stops being drawn ──────────────────────────
#
# load_kv() ONLY EVER ASSIGNS. A sensor whose node went flat, a group switched
# off in the web UI: the value it last reported stayed on the panel, with no
# age against it and nothing to tell it from a live reading.
( load_data || exit 1
  [ -n "$Z_CO2_VALUE" ] || exit 2
  grep -v 'CO2' "$FIXTURE" > "$TMP/data.txt"
  load_data || exit 3
  [ -z "${Z_CO2_VALUE:-}" ] || { echo "kept [$Z_CO2_VALUE]" >&2; exit 4; }
  # And the places that ARE still sent are still there.
  [ -n "$Z_PRES_VALUE" ] || exit 5
  cp "$FIXTURE" "$TMP/data.txt"
  exit 0 )
check "$?" "a reading the collector stops sending is forgotten, not kept on screen"

# ── Numbers nobody has confirmed are not drawn as if they had been ───────────
( DATA_FRESH=1 data_stale && exit 1
  DATA_FRESH=0 EVER_FRESH=0 data_stale || exit 2   # nothing has ever arrived
  DATA_FRESH=0 EVER_FRESH=1 FAILS=1 STALE_AFTER=2 data_stale && exit 3
  DATA_FRESH=0 EVER_FRESH=1 FAILS=2 STALE_AFTER=2 data_stale || exit 4
  exit 0 )
check "$?" "one failed fetch is a hiccup; several in a row make the page stale"

( reset_log
  load_data
  DATA_FRESH=0 EVER_FRESH=1 FAILS=3 LAST_OK="09:12" \
      redraw_sensors "12:35" 0
  # The readings zone carries the reason, and says when it last worked.
  grep -q -- "--	.*09:12" "$FBINK_LOG" || exit 1
  grep -q -- "--	-2.4" "$FBINK_LOG" && exit 2
  exit 0 )
check "$?" "a stale readings zone draws the reason and the time it last worked"

# ── The last page, kept where a reboot cannot take it ────────────────────────
( CACHE="$WORK/last.txt"
  cp "$FIXTURE" "$TMP/data.txt"
  LAST_OK="07:45" cache_save || exit 1
  grep -q '^CACHED_AT="07:45"' "$CACHE" || exit 2
  rm -f "$TMP/data.txt"
  cache_load || exit 3
  [ "$HAVE_DATA" = "1" ] || exit 4
  # Loaded, and NOT claimed as current: its ages were computed before the
  # reader was switched off.
  [ "$DATA_FRESH" = "0" ] || exit 5
  [ "$EVER_FRESH" = "0" ] || exit 6
  [ "$LAST_OK" = "07:45" ] || { echo "got [$LAST_OK]" >&2; exit 7; }
  data_stale || exit 8
  cp "$FIXTURE" "$TMP/data.txt"
  exit 0 )
check "$?" "the cached page comes back after a reboot, and is not passed off as current"

# ── The footer says what the panel knows about itself ────────────────────────
# The battery badge on this page belongs to the outdoor NODE. The reader's own
# battery — the one that decides whether the panel is on the wall next week —
# appeared nowhere at all.
( reset_log
  load_data
  FAKE_BATT=62 POWER=suspend AWAKE_UNTIL=0 STATUS=1 MODE_LBL="awake|radio off|asleep"
  draw_status
  got=$(drawn_at "$STAT_X" "$(px_of "$STAT_SZ")")
  [ "$got" = "62% · asleep" ] || { echo "got [$got]" >&2; exit 1; }
  exit 0 )
check "$?" "the footer carries the reader's own battery and the mode it is in"

# THE MODE IT IS IN, NOT THE ONE POWER NAMES: a panel inside its wake window is
# awake whatever the setting says, and that is the one thing somebody standing
# in front of it wants confirmed before they start tapping.
( reset_log
  load_data
  FAKE_BATT=62 POWER=suspend STATUS=1 MODE_LBL="awake|radio off|asleep"
  AWAKE_UNTIL=$(( $(date +%s) + 60 ))
  draw_status
  got=$(drawn_at "$STAT_X" "$(px_of "$STAT_SZ")")
  [ "$got" = "62% · awake" ] || { echo "got [$got]" >&2; exit 1; }
  exit 0 )
check "$?" "and says awake while it is staying awake for somebody"

( reset_log
  load_data
  STATUS=0 draw_status
  [ -s "$FBINK_LOG" ] && exit 1
  exit 0 )
check "$?" "and draws nothing at all when it is switched off"

# ── The buttons that change a setting write it where both ends read it ───────
# conf_load() re-reads dash.conf every minute and would put the old value
# straight back; and a reader who taps Awake and then opens KUAL should find
# the menu agreeing with the panel.
( CONF="$WORK/power.conf"
  POWER=suspend
  AWAKE_UNTIL=0
  power_toggle
  [ "$POWER" = "wifi" ] || exit 1
  grep -q '^POWER=wifi' "$CONF" || exit 2
  power_toggle
  [ "$POWER" = "suspend" ] || exit 3
  [ "$AWAKE_UNTIL" = "0" ] || exit 4          # asked for the sleep: go down
  POWER=awake;   power_cycle; [ "$POWER" = "wifi" ]    || exit 5
  POWER=wifi;    power_cycle; [ "$POWER" = "suspend" ] || exit 6
  POWER=suspend; power_cycle; [ "$POWER" = "awake" ]   || exit 7
  exit 0 )
check "$?" "Awake stops the sleeping, writes it to dash.conf, and puts it back"

# ── Time that went missing ───────────────────────────────────────────────────
# A press of the power button in POWER=awake sends the reader to sleep by the
# firmware's own path, and it comes back with Amazon's screensaver on the
# screen. Nothing here would have repainted until a tier came round, up to an
# hour later — which from the sofa is a dashboard that has died.
( rm -f "$TMP/redraw"
  lost_time "$(date +%s)" 60 && exit 1        # the wait has not even run
  [ -f "$TMP/redraw" ] && exit 2
  lost_time "$(( $(date +%s) - 600 ))" 60 || exit 3
  [ -f "$TMP/redraw" ] || exit 4
  rm -f "$TMP/redraw"
  exit 0 )
check "$?" "a wait that took far longer than it asked for repaints the page"

# $(( 08 )) is an error in every POSIX shell, and every one of these values
# ends up inside $(( )).
( date() { echo "08"; }
  epoch_now
  [ "$EPOCH" = "8" ] || exit 1
  exit 0 )
check "$?" "the clock is read as a decimal number, not as bad octal"

# ── 3. The payload is data, never a command ──────────────────────────────────
check "$([ ! -f "$WORK/pwned" ] && echo 0 || echo 1)" \
      "a forecast summary containing a shell command did not run it"
case "$FC_SUMMARY" in
    *'touch'*) got=0 ;;
    *) got=1 ;;
esac
check "$got" "and it survived as text: $(printf '%.28s…' "$FC_SUMMARY")"

# ── 3b. The payload may not choose which variables it sets ───────────────────
#
# "A plain variable name" is not a safe thing to let the network pick: PATH is
# one, and so are TMP, DASH_DIR and SLEEP_PID. Verified against the real
# parser, not against a promise in a comment.
cat > "$DASH_TMP/hostile.txt" <<'EOF'
PATH=/pwned
TMP=/mnt/us
DASH_DIR=/evil
SLEEP_PID=1
IFS=:
RES_W=../../../../etc
Z_HERO_VALUE=8.4
EOF
( real_path="$PATH"
  load_kv "$DASH_TMP/hostile.txt" PAYLOAD
  [ "$PATH" = "$real_path" ] || exit 1
  [ "$TMP" != "/mnt/us" ] || exit 2
  [ "$DASH_DIR" != "/evil" ] || exit 3
  [ "$SLEEP_PID" != "1" ] || exit 4
  [ "$Z_HERO_VALUE" = "8.4" ] || exit 5 )
check "$?" "a payload cannot set PATH, TMP, DASH_DIR or SLEEP_PID — but its own keys still land"

# RES_W and RES_H are interpolated into a path that gets EXECUTED with `.`.
( RES_W="../../../../tmp"; RES_H="800"
  load_layout
  [ "$RES_W" = "600" ] || exit 1 )
check "$?" "a traversal in RES_W falls back to a real layout instead of sourcing it"

# ── 3c. A file with no trailing newline keeps its last line ──────────────────
printf 'A_ONE=1\nRES_H=1448' > "$DASH_TMP/nonl.txt"
( load_kv "$DASH_TMP/nonl.txt" PAYLOAD
  [ "$RES_H" = "1448" ] || exit 1 )
check "$?" "the last line of a file with no trailing newline is not dropped"

# ── 3d. The chart on disk is only ever replaced by a whole one ──────────────
#
# THE REPORTED SYMPTOM WAS "the chart disappears sometimes, and comes back".
# The image is streamed off an ESP32 that is also serving a web UI, to a
# ten-year-old reader on wifi; when the connection dies mid-image, what lands
# on disk is a BMP header promising a size and a file that stops short of it.
# It is not empty, so the old "is it non-empty" test promoted it over the good
# chart, and then FBInk refused to draw it — a blank strip until some later
# fetch happened to succeed.
printf 'GOOD' > "$DASH_TMP/graph.bmp"
( WGET_OK_HOST=nowhere.invalid HOST=203.0.113.9 fetch_graph
  [ "$(cat "$DASH_TMP/graph.bmp")" = "GOOD" ] || exit 1
  [ ! -f "$DASH_TMP/graph.new" ] || exit 2 )
check "$?" "a failed graph fetch leaves the previous chart intact"

( GRAPH_TRUNCATE=1 WGET_OK_HOST=10.9.9.42 HOST=10.9.9.42 fetch_graph && exit 1
  [ "$(cat "$DASH_TMP/graph.bmp")" = "GOOD" ] || exit 2
  [ ! -f "$DASH_TMP/graph.new" ] || exit 3 )
check "$?" "a half-written image does not replace the chart that works"

( WGET_OK_HOST=10.9.9.42 HOST=10.9.9.42 fetch_graph || exit 1
  [ "$(wc -c < "$DASH_TMP/graph.bmp" | tr -dc 0-9)" = "130" ] || exit 2 )
check "$?" "and a whole one does"

# The check has to be sure before it refuses. A device with no `od` — or a
# header this shell cannot read as a number — must keep working, or the fix
# for a chart that sometimes vanishes is a chart that never arrives.
( od() { return 127; }
  printf 'BM....some bytes' > "$DASH_TMP/graph.odd"
  graph_ok "$DASH_TMP/graph.odd" || exit 1 )
check "$?" "with no od to read the header, a plausible BMP is still accepted"
( printf 'not a bmp at all' > "$DASH_TMP/graph.odd"
  graph_ok "$DASH_TMP/graph.odd" && exit 1
  : > "$DASH_TMP/graph.odd"
  graph_ok "$DASH_TMP/graph.odd" && exit 2
  exit 0 )
check "$?" "but something that is not a BMP, or is empty, never is"

# ── 3d2. A chart that is not there says so ──────────────────────────────────
#
# The caption and the rule above the chart are drawn whatever happens, so an
# empty rectangle under them reads as "no readings" — a sensor problem, which
# is where anyone would then go looking. It is the image that is missing.
( reset_log
  rm -f "$DASH_TMP/graph.bmp"
  HOST=10.9.9.42
  draw_chart_body && exit 1                       # must report that it failed
  grep -q "No chart yet" "$FBINK_LOG" || exit 2
  grep -q "http://10.9.9.42" "$FBINK_LOG" || exit 3
  grep -q -- "-g	file=" "$FBINK_LOG" && exit 4   # and no blit was attempted
  exit 0 )
check "$?" "with no image, the chart tier writes a reason where the chart goes"
( reset_log
  printf 'BM\202\000\000\000' > "$DASH_TMP/graph.bmp"
  draw_chart_body || exit 1
  grep -q "No chart yet" "$FBINK_LOG" && exit 2
  grep -q -- "file=$DASH_TMP/graph.bmp" "$FBINK_LOG" || exit 3
  exit 0 )
check "$?" "and when the image is there it is blitted, with nothing written over it"
rm -f "$DASH_TMP/graph.odd"
reset_log

# ── 3d3. The chart's key, and the two switches over the sections ────────────
#
# WHY THIS IS TESTED AND NOT JUST LOOKED AT
#
# This screen and the browser page at /kindle are one design rendered twice,
# from one set of settings, by two pieces of code that cannot see each other.
# Everything they disagree about is invisible from either side: the reader
# turns the chart off, the browser stops drawing it, and the panel on the wall
# — the one anybody is actually looking at — carries on. There is no error
# anywhere in that.
#
# The key is the case that was found. The page has drawn one under its chart
# since the chart existed; this script never had one, so the panel showed two
# lines and said nothing about which was which.
( reset_log
  printf 'BM\202\000\000\000' > "$DASH_TMP/graph.bmp"
  draw_chart_body || exit 1
  # The wording is the collector's, in the collector's language.
  grep -q "outside mean" "$FBINK_LOG" || exit 2
  grep -q "inside" "$FBINK_LOG" || exit 3
  grep -q "shaded band = hourly low to high" "$FBINK_LOG" || exit 4
  # THE SWATCHES AT THE WEIGHTS OF THE LINES THEY STAND FOR — 3 px solid for
  # the outdoor mean, 2 px dashed for the indoor. A key drawn in some other
  # weight describes a chart the reader is not looking at.
  grep -q -- "-k	top=[0-9]*,left=$GR_X,width=$KEY_SW_W,height=$KEY_SW_H" "$FBINK_LOG" || exit 5
  # The dashed one is segments, because FBInk has no dashed rule: more than
  # one fill of KEY_SW_H_IN, none of them the full swatch width.
  segs=$(grep -c -- "height=$KEY_SW_H_IN	*$" "$FBINK_LOG")
  [ "${segs:-0}" -ge 2 ] || exit 6
  grep -q -- "width=$KEY_SW_W,height=$KEY_SW_H_IN" "$FBINK_LOG" && exit 7
  exit 0 )
check "$?" "the chart's key names both lines, at the weights the page draws them"

# Only when there is a line for it to name. A key over an empty grid says the
# chart is showing something.
( reset_log
  CHART_OUT=0 CHART_IN=0 draw_chart_body || exit 1
  grep -q "outside mean" "$FBINK_LOG" && exit 2
  grep -q "inside" "$FBINK_LOG" && exit 3
  grep -q -- "file=$DASH_TMP/graph.bmp" "$FBINK_LOG" || exit 4   # chart still drawn
  exit 0 )
check "$?" "with no series in it, the chart gets no key"

# An older collector sends neither flag. Drawing nothing is what this script
# did before it had a key at all, so the reader is never told about lines the
# collector has not said are there.
( reset_log
  unset CHART_OUT CHART_IN
  draw_chart_body || exit 1
  grep -q "outside mean" "$FBINK_LOG" && exit 2
  exit 0 )
check "$?" "and an older collector, which sends neither flag, gets none either"
load_kv "$DASH_TMP/data.txt" PAYLOAD

# The two section switches. The page tests KSHOW_CHART and KSHOW_WEEK; this
# tested neither, so switching a section off in Settings hid it on the browser
# and left it on the panel.
( reset_log
  SHOW_CHART=0 draw_chart_body || exit 1
  [ "$(calls)" = "0" ] || exit 2 )
check "$?" "the chart switched off draws nothing at all — not even its rule"

( reset_log
  SHOW_WEEK=0 draw_forecast_body
  # No week cells: the strip's plates are WK_CELL_W wide and there are seven.
  grep -q -- "width=$WK_CELL_W,height=$WK_CELL_H" "$FBINK_LOG" && exit 1
  # But the footer under it is not part of the strip and still appears.
  grep -q "Measured on site" "$FBINK_LOG" || exit 2
  grep -q -- "-k	top=$FOOT_RULE_Y" "$FBINK_LOG" || exit 3
  exit 0 )
check "$?" "the week strip switched off takes the strip and leaves the footer"

( reset_log
  draw_forecast_body
  grep -q -- "width=$WK_CELL_W,height=$WK_CELL_H" "$FBINK_LOG" || exit 1 )
check "$?" "and switched on it is still there"
reset_log

# ── 3d3b. The chart's numbers, its plates, and everything the page centres ──
#
# All of these were found by holding a photograph of the browser page next to a
# photograph of the panel. None of them is an error anywhere: the panel drew
# what it was told to draw, and what it was told left out every string the page
# renders with CSS the panel does not have.
( reset_log
  printf 'BM\202\000\000\000' > "$DASH_TMP/graph.bmp"
  draw_chart_body || exit 1
  # The five values down the side and the five hours along the bottom. The
  # image is a 4-bit BMP with no font in it; without these the panel showed a
  # bare grid beside a browser page showing the same grid with numbers on it.
  for v in 33 31 28 26 23 -23h -17h -11h -5h now; do
      grep -q -- "	--	$v	" "$FBINK_LOG" || exit 2
  done
  # Right-aligned on the axis: "33" is 1000 mille at 11 px = 11 px wide, so it
  # starts at GR_X + CH_L - AX_GAP - 11.
  grep -q -- "left=$(( GR_X + CH_L - AX_GAP - AX_SZ * 1000 / 1000 ))," "$FBINK_LOG" || exit 3
  # And "now" is set against the right-hand end of the axis, not past it.
  grep -q -- "left=$(( GR_X + CH_R - AX_SZ * 1500 / 1000 ))," "$FBINK_LOG" || exit 4
  exit 0 )
check "$?" "the chart's axis is labelled, the way the page labels it"

# An empty record is a sentence, not a grid. A grid with no line in it reads as
# a sensor that has stopped, which is the one thing it does not mean.
( reset_log
  CH_NOTE="The 24 hour record fills as readings arrive." draw_chart_body || exit 1
  grep -q "24 hour record fills" "$FBINK_LOG" || exit 2
  grep -q -- "-g	file=" "$FBINK_LOG" && exit 3      # and no empty image under it
  grep -q -- "	--	33	" "$FBINK_LOG" && exit 4      # nor an axis for nothing
  exit 0 )
check "$?" "with nothing recorded yet it says so instead of drawing an empty grid"

# An older collector sends no axis at all. Drawing the image alone is what this
# script did before, and is still better than placing labels it has not been
# given.
( reset_log
  unset CH_L CH_R CH_T CH_B
  draw_chart_body || exit 1
  grep -q -- "file=$DASH_TMP/graph.bmp" "$FBINK_LOG" || exit 2
  grep -q -- "	--	33	" "$FBINK_LOG" && exit 3
  exit 0 )
check "$?" "and an older collector, which sends no axis, still gets its chart"
load_kv "$DASH_TMP/data.txt" PAYLOAD
load_layout

# The week strip: centred in its cells, and the number set regular. Seven
# identical boxes are the one place a misalignment cannot hide, and the panel
# drew all seven hard against the left edge in bold.
( reset_log
  draw_forecast_body
  # WK0: name 1240 mille at WK_NAME_SZ, day 1000 at WK_DAY_SZ, in an
  # WK_CELL_W cell starting at WK_X.
  nx=$(( WK_X + (WK_CELL_W - WK_NAME_SZ * 1240 / 1000) / 2 ))
  dx=$(( WK_X + (WK_CELL_W - WK_DAY_SZ * 1000 / 1000) / 2 ))
  # The pair is centred in the cell now, not hung off the layout's two offsets:
  # text_geom() made the day's box taller than its design size, which left the
  # two rows 5 px from the top and 2 from the bottom.
  wk_gap=$(( WK_DAY_OFFSET - WK_NAME_OFFSET ))
  wk_dh=$(( WK_DAY_SZ * TEXT_PX_MILLE / 1000 ))
  wk_ny_want=$(( WK_Y + (WK_CELL_H - wk_gap - wk_dh) / 2 ))
  wk_dy_want=$(( wk_ny_want + wk_gap ))
  # Really centred: the space above the first box equals the space under the
  # second, to a pixel.
  wk_nh=$(( WK_NAME_SZ * TEXT_PX_MILLE / 1000 ))
  above=$(( wk_ny_want - WK_Y ))
  below=$(( WK_Y + WK_CELL_H - wk_dy_want - wk_dh ))
  [ "$above" -ge 0 ] && [ "$below" -ge 0 ] || exit 6
  [ $(( above - below )) -le 1 ] && [ $(( below - above )) -le 1 ] || exit 7
  [ "$nx" -gt "$WK_X" ] || exit 1                  # it really is inset
  grep -q -- "left=$nx,top=$wk_ny_want" "$FBINK_LOG" || exit 2
  grep -q -- "left=$dx,top=$wk_dy_want" "$FBINK_LOG" || exit 3
  # .wd-d carries no font-weight on the page, so the numeral is set in the
  # REGULAR face. -t names its file as `regular=` whichever face it is, so the
  # assertion has to be on the path — the bold one must not appear at this size.
  grep -q -- "regular=$FONT_REG,px=$(px_of "$WK_DAY_SZ")," "$FBINK_LOG" || exit 4
  grep -q -- "regular=$FONT_BOLD,px=$(px_of "$WK_DAY_SZ")," "$FBINK_LOG" && exit 5
  exit 0 )
check "$?" "the week strip is centred in its cells and set regular, as the page sets it"

# BOTH PANELS. The 600x800 numbers happen to divide evenly; the check that
# matters is that the derivation centres whatever sizes a layout carries — the
# 1072 one has a 20 px name where 600 has 11, and the two offsets it was tuned
# with are not the ones that centre it now.
( ok=0
  for res in 600x800 1072x1448; do
      RES_W=${res%x*}; RES_H=${res#*x}; load_layout
      gap=$(( WK_DAY_OFFSET - WK_NAME_OFFSET ))
      dh=$(( WK_DAY_SZ * TEXT_PX_MILLE / 1000 ))
      top=$(( (WK_CELL_H - gap - dh) / 2 ))
      below=$(( WK_CELL_H - top - gap - dh ))
      [ "$top" -ge 0 ] && [ "$below" -ge 0 ] || ok=1
      [ $(( top - below )) -le 1 ] && [ $(( below - top )) -le 1 ] || ok=1
  done
  exit $ok )
check "$?" "and centred on both panels, not only the one whose numbers divide"

# ── The tiling clears itself, fills the panel, and does not overrun it ───────
# THE GUARD RAIL FOR EVERY Y IN A LAYOUT FILE. The panel is the one renderer
# nobody can watch: a section drawn over its neighbour, or past the bottom
# edge, comes back as a photograph days later. So the order the design fixes is
# asserted here — from the layout files themselves, for both panels, through
# the same text_geom() the renderer positions with, because a size is not the
# height of the box FBInk draws it in.
#
# THE LAST CLAUSE IS THE ONE THAT KEEPS THE TYPE LARGE, and it is the reason
# this exists. The tiling this replaced stopped at 701 of 800 and every size on
# the panel had been chosen to fit inside that; the captions came out at 10 px,
# which is a millimetre of cap height at 167 ppi and unreadable from across a
# room. A layout that leaves more than a twentieth of the screen blank under
# its footer has room it is not spending, and this fails until it spends it.
( bad=0
  # The bottom edge of the box FBInk actually draws for a design y and size.
  bot() { text_geom "$1" "$2"; echo $(( TX_TOP + TX_PX )); }
  #
  # AGAINST THE NEIGHBOUR'S DESIGN LINE, NOT ITS BOX. FBInk's box is a sixth
  # taller than the type in it and centred on the line, so a quarter of it is
  # the face's leading above the caps — and this design shares that leading on
  # purpose, exactly as the page's line-height does: a 14 px caption sits over
  # an 88 px number whose box legitimately starts above the caption's. Compared
  # box against box, five places in the shipped layout "fail" and none of them
  # is visible. What this loop is for is the coarse order — a section drawn
  # into the next, a footer past the bottom edge, a chart over its own key —
  # which is what re-tiling gets wrong and what no photograph can diagnose.
  le() { [ "$1" -le "$2" ] || { echo "  $4: $3 ends at $1, past $2" >&2; bad=1; }; }

  for res in 600x800 1072x1448; do
      RES_W=${res%x*}; RES_H=${res#*x}; load_layout

      # ── The legacy block, which is the one nobody looks at ──────────────
      # What update_dash.sh draws when the collector is running older firmware
      # and sends no named places. It is the one arrangement the preview tool
      # never renders and no photograph is ever taken of — and growing LAB_SZ,
      # LEGACY_SUB_SZ and TEND_SZ by three px each put its heading two px
      # inside the hero, its subtitle against the pressure line and, on the
      # Paperwhite, its section label a pixel inside the indoor row. This loop
      # covered only the zone layout, so all of it passed.
      le "$(bot "$LAB_OUT_Y" "$LAB_SZ")"  "$LEGACY_HERO_Y" "the legacy heading" "$res"
      le "$(bot "$LEGACY_HERO_Y" "$LEGACY_HERO_SZ")" "$LEGACY_SUB_Y" \
         "the legacy hero" "$res"
      le "$(bot "$LEGACY_SUB_Y" "$LEGACY_SUB_SZ")" "$PRES_Y" \
         "the legacy subtitle" "$res"
      le "$(bot "$PRES_Y" "$PRES_SZ")"    "$TEND_Y"   "the legacy pressure" "$res"
      le "$(bot "$TEND_Y" "$TEND_SZ")"    "$RULE1_Y"  "the legacy tendency" "$res"
      le "$RULE1_Y" "$LAB_IN_Y" "the legacy rule" "$res"
      le "$(bot "$LAB_IN_Y" "$LAB_SZ")"   "$IN_Y"     "the legacy indoor heading" "$res"
      le "$(bot "$IN_Y" "$IN_SZ")"        "$RULE2_Y"  "the legacy indoor row" "$res"

      # The top block: left column, then right, both above the chart's rule.
      le "$(bot "$TOP_Y" "$GROUP_LAB_SZ")" "$HERO_Y"   "the group label" "$res"
      le "$(bot "$SUB_Y" "$SUB_SZ")"       "$GRID_Y"   "the 24 h line"   "$res"
      gy=$(( GRID_Y + GRID_ROW_H + GRID_LAB_SZ + 4 ))
      le "$(bot "$gy" "$GRID_VAL_SZ")"     "$RULE2_Y"  "the second grid row" "$res"
      le "$(( CL_Y + CL_H ))"              "$IN_RULE_Y" "the clock's rectangle" "$res"
      le "$(bot "$IN_LAB_Y" "$GROUP_LAB_SZ")" \
         "$(( IN_VAL_Y + IN_VAL_SZ_1 - IN_VAL_SZ - GRID_LAB_SZ - 4 ))" \
         "the indoor heading" "$res"
      le "$(bot "$IN_VAL_Y" "$IN_VAL_SZ_1")" "$RULE2_Y" "the indoor row" "$res"
      # The hairline between the columns runs the height of the block, so it
      # must not outrun it into the chart's rule.
      le "$(( TOP_Y + SEP_H ))" "$RULE2_Y" "the column hairline" "$res"

      # The chart, its caption and its key.
      le "$RULE2_Y" "$LAB_CHART_Y" "the chart's rule" "$res"
      le "$(bot "$LAB_CHART_Y" "$LAB_SZ")" "$GR_Y"    "the chart's caption" "$res"
      le "$(( GR_Y + GR_H ))"              "$KEY_Y"   "the chart" "$res"
      le "$(bot "$KEY_Y" "$KEY_SZ")"       "$RULE3_Y" "the chart's key" "$res"

      # The forecast: the left stack, the icon beside it, and the three plates.
      le "$RULE3_Y" "$LAB_FC_Y" "the forecast's rule" "$res"
      le "$(bot "$LAB_FC_Y" "$LAB_SZ")" "$FC_TEXT_Y"  "the forecast's caption" "$res"
      le "$(bot "$LAB_FC_Y" "$LAB_SZ")" "$FC_ICON_Y"  "the forecast's caption" "$res"
      le "$(bot "$FC_TEXT_Y" "$FC_TEXT_SZ")" "$FC_TEMP_Y" "the condition" "$res"
      le "$(bot "$FC_TEMP_Y" "$FC_TEMP_SZ")" "$FC_WIND_Y" "the high and low" "$res"
      le "$(bot "$FC_WIND_Y" "$FC_WIND_SZ")" "$WK_HDG_RULE_Y" "the wind line" "$res"
      le "$(( FC_ICON_Y + FC_MAIN_SZ ))" "$WK_HDG_RULE_Y" "the forecast icon" "$res"

      plate_top=$(( OL0_Y - OL_PLATE_TOP ))
      plate_bot=$(( plate_top + OL_PLATE_H ))
      le "$RULE3_Y"  "$plate_top"      "the forecast's rule" "$res"
      le "$plate_bot" "$WK_HDG_RULE_Y" "the outlook plates"  "$res"
      le "$(bot "$OL0_Y" "$OL_LABEL_SZ")" "$(( OL0_Y + OL_ICON_OFFSET ))" \
         "the outlook label" "$res"
      le "$(( OL0_Y + OL_ICON_OFFSET + FC_OL_SZ ))" \
         "$(( OL0_Y + OL_TEMP_OFFSET ))" "the outlook icon" "$res"
      le "$(bot "$(( OL0_Y + OL_TEMP_OFFSET ))" "$OL_TEMP_SZ")" "$plate_bot" \
         "the outlook temperature" "$res"
      # AND THE ICON HAS TO FIT ITS PLATE, sideways as well as down the page.
      # It is an opaque blit carrying the plate's own grey as its ground — that
      # is what stops it reading as a white card inside the grey one — so an
      # icon wider than the card it sits on would hang a strip of GRAYE over
      # the page's white on both sides of it.
      #
      # OL_PLATE_W CHECKED FIRST, and for its own sake. ol_centre() centres the
      # label, the icon and the temperature inside it, so a layout that leaves
      # it out does not merely lose the plate: it stacks all three at
      # ol_x - width/2, and the icons then carry a grey for a card nobody drew.
      # Testing the width against an empty string would also have made `[`
      # print "integer expression expected" and this file report an icon
      # "34px wide on a px plate" — a hole in a message, for a missing key.
      case "${OL_PLATE_W:-}" in
          ''|*[!0-9]*|0)
              echo "  $res: OL_PLATE_W is unset or zero, and the whole outlook column is centred in it" >&2
              bad=1 ;;
          *)  [ "$FC_OL_SZ" -le "$OL_PLATE_W" ] || {
                  echo "  $res: the outlook icon is ${FC_OL_SZ}px wide on a ${OL_PLATE_W}px plate" >&2
                  bad=1; } ;;
      esac

      # The week strip and the footer.
      le "$WK_HDG_RULE_Y" "$WK_HDG_Y" "the week's rule" "$res"
      le "$(bot "$WK_HDG_Y" "$WK_HDG_SZ")" "$WK_Y" "the month heading" "$res"
      le "$(( WK_Y + WK_CELL_H ))" "$FOOT_RULE_Y" "the week strip" "$res"
      le "$FOOT_RULE_Y" "$FOOT_Y" "the footer's rule" "$res"

      foot_bot=$(bot "$FOOT_Y" "$FOOT_SZ")
      le "$foot_bot" "$RES_H" "the footer" "$res"
      # ... and it has to come close to the bottom, not stop short of it.
      le "$(( RES_H - foot_bot ))" "$(( RES_H / 20 ))" \
         "$(( RES_H - foot_bot )) px of unspent screen under the footer" "$res"
  done
  exit $bad )
check "$?" "every section clears the next and the tiling spends the panel"

RES_W=600; RES_H=800; load_layout
load_kv "$DASH_TMP/data.txt" PAYLOAD

# An older collector measures nothing. Falling back to the left edge is what
# this always did, and is better than centring on a width of zero.
( reset_log
  unset WK0_NAMEW WK0_DAYW WK1_NAMEW WK1_DAYW WK2_NAMEW WK2_DAYW \
        WK3_NAMEW WK3_DAYW WK4_NAMEW WK4_DAYW WK5_NAMEW WK5_DAYW \
        WK6_NAMEW WK6_DAYW
  draw_forecast_body
  wk_gap=$(( WK_DAY_OFFSET - WK_NAME_OFFSET ))
  wk_dh=$(( WK_DAY_SZ * TEXT_PX_MILLE / 1000 ))
  wk_ny_want=$(( WK_Y + (WK_CELL_H - wk_gap - wk_dh) / 2 ))
  grep -q -- "left=$WK_X,top=$wk_ny_want" "$FBINK_LOG" || exit 1 )
check "$?" "with no widths it falls back to the left edge rather than to nonsense"
load_kv "$DASH_TMP/data.txt" PAYLOAD

# The three outlook columns: a grey plate each, with the label, the icon and
# the temperature centred on it.
( reset_log
  draw_forecast_body
  grep -q -- "-B	GRAYE	-k	top=$(( OL0_Y - OL_PLATE_TOP )),left=$OL0_X,width=$OL_PLATE_W,height=$OL_PLATE_H" "$FBINK_LOG" || exit 1
  # The icon is FC_OL_SZ wide and the plate OL_PLATE_W, so it is inset by half
  # the difference — not drawn at the column's left edge.
  ix=$(( OL0_X + (OL_PLATE_W - FC_OL_SZ) / 2 ))
  grep -q -- "x=$ix,y=$(( OL0_Y + OL_ICON_OFFSET ))" "$FBINK_LOG" || exit 2
  lx=$(( OL0_X + (OL_PLATE_W - OL_LABEL_SZ * 2260 / 1000) / 2 ))
  grep -q -- "left=$lx,top=$(top_of "$OL0_Y" "$OL_LABEL_SZ")" "$FBINK_LOG" || exit 3
  exit 0 )
check "$?" "each outlook column sits on a plate with its contents centred"

# The hairline between the clock and the indoor row is the light one. Drawn at
# a section rule's weight it reads as a third section break.
( reset_log
  draw_sensors_body
  grep -q -- "-B	GRAYD	-k	top=$IN_RULE_Y" "$FBINK_LOG" || exit 1 )
check "$?" "the indoor hairline is #d8d8d8, not a section rule"

# A degree is a footnote, not a unit. The page sets .unit at 0.42em and
# .unit-d at 0.34em; one size for both put a circle the height of a lower-case
# o where the superscript should be, and pushed everything after it right.
( reset_log
  draw_field 10 20 100 0 "21.0" "°" "" 2000 330 BLACK
  grep -q -- "px=$(px_of 34)," "$FBINK_LOG" || exit 1
  reset_log
  draw_field 10 20 100 0 "1008" "hPa" "" 2000 1600 BLACK
  grep -q -- "px=$(px_of 42)," "$FBINK_LOG" || exit 2
  exit 0 )
check "$?" "the degree is set smaller than a spelt-out unit, as the page sets it"
reset_log

# ── 3d4. The clock: the reader's time, the collector's format and style ─────
#
# The time is the Kindle's own — it redraws every minute and the collector is
# fetched every few at best. The FORMAT and the STYLE are settings, made once
# on the same page as everything else, and they used to reach the browser and
# stop there: a reader who chose the twelve-hour clock got it on the web page
# and 24-hour on the panel, from one setting on one device.
#
# now_clock() is kdFmtTime() written in shell, so these are its three cases.
( date() { echo "09:05"; }
  [ "$(TIME_FORMAT=0 now_clock)" = "09:05" ] || exit 1
  [ "$(TIME_FORMAT=1 now_clock)" = "9:05" ]  || exit 2
  [ "$(TIME_FORMAT=2 now_clock)" = "9:05am" ] || exit 3
  [ "$(now_clock)" = "09:05" ] || exit 4 )        # unset is 24-hour
check "$?" "the three clock formats are the three the page offers"

# The two the twelve-hour clock is always got wrong on: noon is 12pm, not 0pm,
# and midnight is 12am, not 0am or 12pm.
( date() { echo "12:00"; }
  [ "$(TIME_FORMAT=2 now_clock)" = "12:00pm" ] || exit 1 )
check "$?" "noon is 12pm"
( date() { echo "00:30"; }
  [ "$(TIME_FORMAT=2 now_clock)" = "12:30am" ] || exit 1
  [ "$(TIME_FORMAT=1 now_clock)" = "0:30" ] || exit 2 )
check "$?" "and midnight is 12am"
( date() { echo "13:07"; }
  [ "$(TIME_FORMAT=2 now_clock)" = "1:07pm" ] || exit 1
  [ "$(TIME_FORMAT=0 now_clock)" = "13:07" ] || exit 2 )
check "$?" "an afternoon hour counts down from twelve, not up from zero"

# 08 and 09 are the pair that breaks a shell doing arithmetic: $((08)) is
# "value too great for base" in every POSIX shell, and this one is inside the
# clock, which is drawn every minute.
( date() { echo "08:09"; }
  [ "$(TIME_FORMAT=2 now_clock)" = "8:09am" ] || exit 1
  [ "$(TIME_FORMAT=1 now_clock)" = "8:09" ] || exit 2 )
check "$?" "with 08 and 09 read as decimal, not as bad octal"

# The four styles. Each one is what kdSkinCss draws in CSS, with the primitives
# a framebuffer has.
( reset_log
  CLOCK_STYLE=0 draw_clock "12:34"
  grep -q -- "px=$(px_of "$CL_SIZE")," "$FBINK_LOG" || exit 1
  grep -q -- "-B	BLACK" "$FBINK_LOG" && exit 2       # no plate
  exit 0 )
check "$?" "the plain clock is the time at its full size and nothing else"

( reset_log
  CLOCK_STYLE=1 draw_clock "12:34"
  # A black plate filling the clock rectangle, with the time knocked out of it.
  grep -q -- "-B	BLACK	-k	top=$Z_CLOCK_Y,left=$Z_CLOCK_X,width=$Z_CLOCK_W,height=$Z_CLOCK_H" "$FBINK_LOG" || exit 1
  grep -q -- "-h	-C	BLACK	-B	WHITE" "$FBINK_LOG" || exit 2
  grep -q -- "px=$(px_of "$CL_SZ_BOXED")," "$FBINK_LOG" || exit 3
  # Centred, not against the left edge — CLOCK_ADVW is what centres it.
  left=$(sed -n 's/.*-h	-C	BLACK	-B	WHITE	-t	[^	]*left=\([0-9]*\),.*/\1/p' "$FBINK_LOG" | head -1)
  [ -n "$left" ] && [ "$left" -gt "$CL_X" ] || exit 4 )
check "$?" "the boxed clock is knocked out of a plate and centred on it"

( reset_log
  CLOCK_STYLE=2 draw_clock "12:34"
  grep -q -- "-k	top=$Z_CLOCK_Y,left=$Z_CLOCK_X,width=$Z_CLOCK_W,height=$RULE_H" "$FBINK_LOG" || exit 1
  grep -q -- "px=$(px_of "$CL_SZ_RULED")," "$FBINK_LOG" || exit 2 )
check "$?" "the ruled clock has a hairline over it and is set smaller"

( reset_log
  CLOCK_STYLE=3 draw_clock "12:34"
  grep -q -- "px=$(px_of "$CL_SZ_DATED")," "$FBINK_LOG" || exit 1
  grep -q -- "px=$(px_of "$CL_DATE_SZ")," "$FBINK_LOG" || exit 2
  grep -q "24 MARCH" "$FBINK_LOG" || exit 3 )
check "$?" "the dated clock takes its room from the time and puts the date under it"

# An older collector sends no CLOCK_STYLE. The plain clock is what this script
# has always drawn, so that is what it keeps drawing.
( reset_log
  unset CLOCK_STYLE
  draw_clock "12:34"
  grep -q -- "px=$(px_of "$CL_SIZE")," "$FBINK_LOG" || exit 1
  grep -q -- "-B	BLACK" "$FBINK_LOG" && exit 2
  exit 0 )
check "$?" "and with no style at all it is the plain one, as it always was"
load_kv "$DASH_TMP/data.txt" PAYLOAD

# Every style, and the key, through the option table — the check section 1 runs
# over the default page. A style is drawn on a timer nobody watches; a flag it
# gets wrong is a clock that silently stops appearing.
bad=""
for style in 0 1 2 3; do
    reset_log
    CLOCK_STYLE=$style redraw_all "12:34"
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        if reason=$(validate_call "$line"); then :; else
            bad="style $style: $reason"; break
        fi
        if reason=$(validate_colours "$line"); then :; else
            bad="style $style: $reason"; break
        fi
    done < "$FBINK_LOG"
    [ -z "$bad" ] || break
done
check "$([ -z "$bad" ] && echo 0 || echo 1)" \
      "all four clock styles speak FBInk too${bad:+ — $bad}"
load_kv "$DASH_TMP/data.txt" PAYLOAD
load_layout
reset_log

# ── 3d5. The language is the collector's, including when it is unreachable ──
#
# Every string on this panel comes from /kindle/data, so the language follows
# the collector's setting with no reflash and nothing to configure here. The
# offline message is the one that cannot: it is drawn precisely because the
# collector cannot be reached. It uses whatever the last successful fetch left
# behind, which covers every outage after first contact.
( reset_log
  HOST=10.9.9.42
  redraw_offline "12:34"
  grep -q "Няма връзка с" "$FBINK_LOG" || exit 1
  grep -q "Проверете WiFi" "$FBINK_LOG" || exit 2
  grep -q "Cannot reach" "$FBINK_LOG" && exit 3
  exit 0 )
check "$?" "the offline message is in the language the collector last sent"

# And before first contact there is nothing to have kept, so it says what it
# has always said rather than nothing at all.
( reset_log
  unset LBL_OFFLINE LBL_OFFLINE_HINT
  HOST=10.9.9.42
  redraw_offline "12:34"
  grep -q "Cannot reach" "$FBINK_LOG" || exit 1
  grep -q "Find collector" "$FBINK_LOG" || exit 2
  exit 0 )
check "$?" "and English before the collector has ever answered"
load_kv "$DASH_TMP/data.txt" PAYLOAD
reset_log

# ── 3d6. The axis and the image come from the same minute ───────────────────
#
# The five temperatures down the side of the chart are in the PAYLOAD and the
# grid they label is in a separately fetched image. GRAPH_EVERY and DATA_EVERY
# are both editable from the KUAL menu, so a pair like 10 and 15 gives minutes
# where the chart tier fires alone — and a fresh image then gets a scale up to
# fifteen minutes old drawn down its side, which nothing on the panel could
# show had happened.
CLOCK_EVERY=1; DATA_EVERY=15; GRAPH_EVERY=10; FORECAST_EVERY=30; FULL_EVERY=60
check "$(case " $(plan_minute 10) " in *" chart "*) echo 0 ;; *) echo 1 ;; esac)" \
      "minute 10 is a chart tier with no data tier ($(plan_minute 10))"
( . /dev/null
  # The loop's own fetch list, as the script writes it: the chart tier has to
  # be one of the tiers that pulls a payload.
  grep -q '\*" sensors "\*|\*" forecast "\*|\*" chart "\*) fetch_data' \
       "$KDIR/update_dash.sh" || exit 1 )
check "$?" "and the chart tier fetches the payload its axis labels come from"
CLOCK_EVERY=1; DATA_EVERY=5; GRAPH_EVERY=15; FORECAST_EVERY=30; FULL_EVERY=60

# ── 3e. With no data at all, the page says why ───────────────────────────────
( HAVE_DATA=0
  unset Z_GROUP_OUT OUT_TEMP IN_TEMP IN_HUM OUT_HUM
  : > "$FBINK_LOG"
  draw_sensors_body && exit 1                      # must refuse
  redraw_offline "12:34"
  # Whatever the offline wording currently is — the payload carries it now, so
  # a literal here would only be asserting the fixture's language.
  grep -q -- "${LBL_OFFLINE:-Cannot reach}" "$FBINK_LOG" || exit 2
  grep -q -- '	--	°$' "$FBINK_LOG" && exit 3     # no punctuation-only fields
  # and the message lands in the readings zone, not under the chart
  grep -q -- "-s	top=$Z_SENS_Y,left=$Z_SENS_X" "$FBINK_LOG" || exit 4 )
check "$?" "with no payload it draws a reason in the readings zone, not '°  /%'"
reset_log
load_kv "$DASH_TMP/data.txt" PAYLOAD
load_layout

# ── 4. The schedule ──────────────────────────────────────────────────────────
# Defaults: clock 1, data 5, chart 15, forecast 30, full 60.
CLOCK_EVERY=1; DATA_EVERY=5; GRAPH_EVERY=15; FORECAST_EVERY=30; FULL_EVERY=60
check "$([ "$(plan_minute 1)" = "clock" ] && echo 0 || echo 1)" \
      "minute 1 is the clock alone ($(plan_minute 1))"
check "$([ "$(plan_minute 5)" = "sensors clock" ] && echo 0 || echo 1)" \
      "minute 5 adds the readings ($(plan_minute 5))"
check "$([ "$(plan_minute 15)" = "sensors chart clock" ] && echo 0 || echo 1)" \
      "minute 15 adds the chart ($(plan_minute 15))"
check "$([ "$(plan_minute 30)" = "sensors chart forecast clock" ] && echo 0 || echo 1)" \
      "minute 30 adds the forecast ($(plan_minute 30))"
check "$([ "$(plan_minute 60)" = "full" ] && echo 0 || echo 1)" \
      "minute 60 is the full refresh, and stands in for the rest ($(plan_minute 60))"

# A tier switched off must never come round, and must not divide by zero.
GRAPH_EVERY=0
check "$([ "$(plan_minute 15)" = "sensors clock" ] && echo 0 || echo 1)" \
      "an interval of 0 disables its tier instead of failing"
GRAPH_EVERY=15

# The flash counter counts TIERS, not minutes: every 10th clock update at a
# 5-minute clock interval is every 50 minutes, not every 10.
CLOCK_EVERY=5; CLOCK_FLASH_EVERY=10
check "$(flash_due 50 5 10 && echo 0 || echo 1)" "minute 50 flashes (10th clock tier)"
check "$(flash_due 10 5 10 && echo 1 || echo 0)" "minute 10 does not (2nd)"
CLOCK_EVERY=1; CLOCK_FLASH_EVERY=1
check "$(flash_due 7 1 1 && echo 0 || echo 1)" "with the defaults every minute flashes"
check "$(flash_due 7 1 0 && echo 1 || echo 0)" "and 0 turns flashing off"

# A zero-padded interval is not octal. $((08)) is an error in every POSIX
# shell, and this one is evaluated once a minute.
CLOCK_EVERY=1; DATA_EVERY=$(strip_zeros "08"); GRAPH_EVERY=15
check "$([ "$DATA_EVERY" = "8" ] && echo 0 || echo 1)" \
      "a zero-padded interval is normalised ($DATA_EVERY), not read as octal"
check "$(plan_minute 8 >/dev/null 2>&1 && echo 0 || echo 1)" \
      "and the schedule computes with it instead of dying on base 8"
DATA_EVERY=5

echo ""
echo "Settings, edited the way KUAL edits them:"

# ── 5. settings.sh ───────────────────────────────────────────────────────────
run_settings() {
    DASH_DIR="$KDIR" DASH_TMP="$DASH_TMP" DASH_CONF="$DASH_CONF" \
        DASH_FONTS="$WORK/fonts" DASH_CACHE="$WORK/settings-last.txt" \
        DASH_SCAN_LIST="$WORK/collectors" sh "$KDIR/settings.sh" "$@" 2>&1
}

rm -f "$DASH_CONF"
run_settings show > "$WORK/show.txt"
check "$([ -f "$DASH_CONF" ] && echo 0 || echo 1)" \
      "a missing dash.conf is seeded from dash.conf.default"
check "$(grep -q '^HOST' "$WORK/show.txt" && echo 0 || echo 1)" \
      "show lists the settings"

run_settings set HOST 10.9.9.42 >/dev/null
check "$([ "$(run_settings get HOST)" = "10.9.9.42" ] && echo 0 || echo 1)" \
      "set HOST is written and read back"
check "$(grep -q '^HOST=10.9.9.42' "$DASH_CONF" && echo 0 || echo 1)" \
      "and lands in dash.conf as a plain key=value"

# The battery settings, as three named choices. `set POWER wifi` has always
# worked; what did not was reaching it — kual-run.sh forwarded a fixed list of
# commands and menu.json listed a fixed set of entries, and neither had grown
# any of the three keys. The docs said "try wifi first", which on the device
# meant a USB cable.
( run_settings power wifi >/dev/null
  [ "$(run_settings get POWER)" = "wifi" ] || exit 1
  [ "$(run_settings get GUI_STOP)" = "0" ] || exit 2
  run_settings power deep >/dev/null
  [ "$(run_settings get POWER)" = "suspend" ] || exit 3
  [ "$(run_settings get GUI_STOP)" = "1" ] || exit 4
  # And back out again in one step, which is the half that has to work: it is
  # the setting whose menu entry stops being on screen once it is on.
  run_settings power awake >/dev/null
  [ "$(run_settings get POWER)" = "awake" ] || exit 5
  [ "$(run_settings get GUI_STOP)" = "0" ] || exit 6
  exit 0 )
check "$?" "power names the battery settings, and names a way back out of them"

# The one that stops the reader framework says so before it does it, because
# the menu it was pressed in goes with it.
( out=$(run_settings power deep)
  printf '%s' "$out" | grep -qi "reboot" || exit 1
  run_settings power awake >/dev/null
  exit 0 )
check "$?" "and the deep setting says how to get the reader back"

check "$(run_settings power nonsense >/dev/null 2>&1 && echo 1 || echo 0)" \
      "an unknown battery setting is refused rather than written"

# ── THE ONE THAT ACTUALLY BUYS DAYS ─────────────────────────────────────────
# POWER=suspend on its own does not, and the menu entry that set it said it
# did: with CLOCK_EVERY=1 the panel suspends and comes back sixty times an
# hour, and every one of those is a resume, a draw and a flashing refresh of
# the clock. The saving is in not waking up, so the mode that promises days has
# to slow the clock down as well.
( run_settings profile normal >/dev/null
  [ "$(run_settings get CLOCK_EVERY)" = "1" ] || exit 1
  run_settings power suspend >/dev/null
  [ "$(run_settings get POWER)" = "suspend" ] || exit 2
  [ "$(run_settings get CLOCK_EVERY)" = "1" ] || exit 3   # suspend alone: as asked
  out=$(run_settings power days)
  [ "$(run_settings get POWER)" = "suspend" ] || exit 4
  [ "$(run_settings get CLOCK_EVERY)" -ge 10 ] || exit 5
  [ "$(run_settings get DATA_EVERY)" -ge 10 ] || exit 6
  [ "$(run_settings get GUI_STOP)" = "0" ] || exit 7
  # And it says how to get back out, which is the half that has to work.
  printf '%s' "$out" | grep -qi "power button" || exit 8
  run_settings profile normal >/dev/null
  run_settings power awake >/dev/null
  exit 0 )
check "$?" "the deep-sleep mode sets the intervals too, and names the way back"

( run_settings profile days >/dev/null
  [ "$(run_settings get CLOCK_EVERY)" -ge 10 ] || exit 1
  run_settings profile normal >/dev/null
  [ "$(run_settings get CLOCK_EVERY)" = "1" ] || exit 2
  exit 0 )
check "$?" "and the same intervals are a profile in their own right"

# Quiet hours are two keys, and nobody should set them one at a time from a
# menu that can only step numbers.
( run_settings quiet night >/dev/null
  [ "$(run_settings get QUIET_FROM)" = "22" ] || exit 1
  [ "$(run_settings get QUIET_TO)" = "7" ] || exit 2
  run_settings quiet off >/dev/null
  [ "$(run_settings get QUIET_FROM)" = "$(run_settings get QUIET_TO)" ] || exit 3
  run_settings quiet 1 6 >/dev/null
  [ "$(run_settings get QUIET_FROM)" = "1" ] || exit 4
  [ "$(run_settings get QUIET_TO)" = "6" ] || exit 5
  # 24 is not an hour, and a refused value leaves the pair as it was.
  run_settings quiet 1 24 >/dev/null 2>&1
  [ "$(run_settings get QUIET_TO)" = "6" ] || exit 6
  run_settings quiet off >/dev/null
  exit 0 )
check "$?" "quiet hours are set as one named choice, and a bad hour is refused"

# THE SAME FACTS AS kual.log, ON THE SCREEN. Every fault reported against this
# extension so far has been an installation one, and every one of them was
# diagnosed by plugging the reader into a computer and reading a log.
( out=$(run_settings diag)
  for w in fbink collector touch power refresh; do
      printf '%s' "$out" | grep -q "$w" || { echo "no $w" >&2; exit 1; }
  done
  exit 0 )
check "$?" "diag says where FBInk is, whether the collector answers, and what is set"

# THE FIRST-RUN ADDRESS IS READ OUT OF dash.conf.default, not written down in
# the script: the copy nothing executes is the one that goes stale, and a
# first-run test comparing against an address the package no longer ships is a
# test that never fires.
( d=$(sed -n 's/^HOST=//p' "$KDIR/dash.conf.default" | head -1)
  [ -n "$d" ] || exit 1
  HOST="$d";        host_is_default || exit 2
  HOST="10.9.9.42"; host_is_default && exit 3
  exit 0 )
check "$?" "the shipped collector address is recognised without being repeated"

# A page of settings has outgrown one column on a 600x800 panel, and what a
# screen does not mention is a setting nobody can check.
( reset_log
  run_settings show >/dev/null
  n=$(grep -c -- '-t	regular=' "$FBINK_LOG" 2>/dev/null)
  [ "${n:-0}" -gt 0 ] || exit 1
  # EVERY LINE IS ON THE SCREEN. There are more keys in conf_keys() than a
  # 600x800 panel fits at the step this page was written with, and the ones
  # past the bottom edge were drawn where FBInk clips them: a settings screen
  # that silently does not mention a setting. One line per key, full width, so
  # a long value is not clipped either.
  [ "$n" -ge "$(conf_keys | wc -w)" ] || { echo "$n lines" >&2; exit 2; }
  over=$(grep -o 'top=[0-9]*' "$FBINK_LOG" | cut -d= -f2 | sort -n | tail -1)
  [ "${over:-0}" -lt 800 ] || { echo "a line at top=$over" >&2; exit 3; }
  exit 0 )
check "$?" "and show fits every key on the screen, values and all"

# strip_zeros() turns "" into "0", which conf_valid TOUCH_DEV refuses — so the
# touchscreen override could be set from KUAL and never cleared again. cmd_set()
# now asks conf_is_text(), the same list conf_load() asks.
( run_settings set TOUCH_DEV /dev/input/event3 >/dev/null
  [ "$(run_settings get TOUCH_DEV)" = "/dev/input/event3" ] || exit 1
  run_settings set TOUCH_DEV "" >/dev/null
  [ -z "$(run_settings get TOUCH_DEV)" ] || { echo "[$(run_settings get TOUCH_DEV)]" >&2; exit 2; }
  exit 0 )
check "$?" "the touchscreen override can be cleared again, not only set"

run_settings set DATA_EVERY 7 >/dev/null
check "$([ "$(run_settings get DATA_EVERY)" = "7" ] && echo 0 || echo 1)" \
      "an interval can be changed"

before=$(run_settings get DATA_EVERY)
run_settings set DATA_EVERY 0 >/dev/null 2>&1
check "$([ "$(run_settings get DATA_EVERY)" = "$before" ] && echo 0 || echo 1)" \
      "an interval of 0 is refused — the loop would divide by it"
run_settings set DATA_EVERY "5; reboot" >/dev/null 2>&1
check "$([ "$(run_settings get DATA_EVERY)" = "$before" ] && echo 0 || echo 1)" \
      "and so is a value carrying a command"
run_settings set HOST 'evil"; reboot; #' >/dev/null 2>&1
check "$([ "$(run_settings get HOST)" = "10.9.9.42" ] && echo 0 || echo 1)" \
      "a collector address with shell metacharacters is refused"
check "$([ "$(run_settings set NOPE 1 >/dev/null 2>&1; echo $?)" != "0" ] && echo 0 || echo 1)" \
      "an unknown setting is refused"

run_settings profile saver >/dev/null
check "$([ "$(run_settings get FULL_EVERY)" = "120" ] && echo 0 || echo 1)" \
      "the battery-saver profile stretches the full refresh to 120 min"
run_settings profile fast >/dev/null
check "$([ "$(run_settings get GRAPH_EVERY)" = "5" ] && echo 0 || echo 1)" \
      "the fast profile redraws the chart every 5 min"
run_settings profile normal >/dev/null
check "$([ "$(run_settings get DATA_EVERY)" = "5" ] && echo 0 || echo 1)" \
      "and normal puts it back"

# A change has to reach a RUNNING dashboard. It re-reads dash.conf every minute
# and repaints when it finds this file, which is what lets the menu say
# "applies within a minute" instead of "stop and start it again".
rm -f "$DASH_TMP/redraw"
run_settings set FORECAST_EVERY 45 >/dev/null
check "$([ -f "$DASH_TMP/redraw" ] && echo 0 || echo 1)" \
      "a change asks the running dashboard to repaint"
( DASH_LIB_ONLY=1; DASH_DIR="$KDIR"; DASH_CONF="$DASH_CONF"; DASH_TMP="$DASH_TMP"
  . "$KDIR/update_dash.sh"
  conf_load
  [ "$FORECAST_EVERY" = "45" ] || exit 1 )
check "$?" "and the loop reads the new value on its next tick"
run_settings set FORECAST_EVERY 30 >/dev/null

# ── 6. Finding the collector without a keyboard ──────────────────────────────
# The fake ifconfig says 10.9.9.7/24 and the fake wget answers for one host, so
# the scan has exactly one right answer.
WGET_OK_HOST=10.9.9.42
export WGET_OK_HOST
SCAN_BATCH=64 SCAN_TIMEOUT=1 run_settings find > "$WORK/find.txt" 2>&1
check "$([ "$(run_settings get HOST)" = "10.9.9.42" ] && echo 0 || echo 1)" \
      "find scans the Kindle's own subnet and saves what answered"
check "$(grep -q '10.9.9.42' "$WORK/find.txt" && echo 0 || echo 1)" \
      "and says which address it took"

# The scan result has to outlive Stop, which deletes /tmp/dash — and a reboot,
# which is a ramdisk. It lives beside dash.conf for that reason.
check "$(grep -q 'DASH_SCAN_LIST:-\$SELF_DIR/collectors' "$KDIR/settings.sh" && echo 0 || echo 1)" \
      "the scan list defaults to beside dash.conf, not under /tmp/dash"
rm -rf "$DASH_TMP"
mkdir -p "$DASH_TMP"
run_settings next > "$WORK/next.txt" 2>&1
check "$([ "$(run_settings get HOST)" = "10.9.9.42" ] && echo 0 || echo 1)" \
      "so Next collector still works after the dashboard was stopped"

run_settings reset >/dev/null
check "$([ "$(run_settings get HOST)" = "192.168.1.50" ] && echo 0 || echo 1)" \
      "reset restores dash.conf.default"

# The redraw flag is dropped AFTER the settings screen is painted. say_lines
# clears and flashes the whole panel; with the flag already down, a tick
# landing in that window repaints the dashboard and then has the settings page
# drawn on top of it, with nothing left to undo that until the next full
# refresh an hour later.
check "$(awk '
    { line[NR] = $0 }
    END {
        for (i = 1; i <= NR; i++) {
            if (line[i] !~ /^[ \t]*ask_redraw[ \t]*$/) continue
            for (j = i + 1; j <= i + 4 && j <= NR; j++)
                if (line[j] ~ /say_lines/) { bad = 1 }
        }
        exit (bad ? 1 : 0)
    }' "$KDIR/settings.sh" && echo 0 || echo 1)" \
      "and it is set after the screen is painted, not before"

# ── 7. A hand-edited dash.conf cannot break the loop ─────────────────────────
printf 'HOST=10.0.0.5\nDATA_EVERY=0\nFULL_EVERY=notanumber\n' > "$DASH_CONF"
( DASH_LIB_ONLY=1; DASH_DIR="$KDIR"; DASH_CONF="$DASH_CONF"; DASH_TMP="$DASH_TMP"
  . "$KDIR/update_dash.sh"
  conf_load 2>/dev/null
  [ "$HOST" = "10.0.0.5" ] || exit 1
  [ "$(host_url)" = "http://10.0.0.5" ] || exit 4
  [ "$DATA_EVERY" -ge 1 ] || exit 2
  [ "$FULL_EVERY" -ge 1 ] || exit 3 ) 
check "$?" "a bad value in dash.conf falls back to the default, and the address gains its scheme only where it is used"

# ── 8. The tick aims at the minute, not at "sixty seconds from now" ──────────
# A tick costs time — a fetch, a dozen draws, a refresh — so a fixed 60 makes
# every cycle 60+N and the clock walks away from the minute it is showing.
( date() { echo "07"; }
  nap()  { echo "$1" > "$WORK/slept"; }
  nap_to_minute
  [ "$(cat "$WORK/slept")" = "53" ] || exit 1 )
check "$?" "at :07 it sleeps 53 seconds, landing on the minute"
( date() { echo "00"; }
  nap()  { echo "$1" > "$WORK/slept"; }
  nap_to_minute
  [ "$(cat "$WORK/slept")" = "60" ] || exit 1 )
check "$?" "and at :00 it waits a whole minute rather than ticking twice for it"
( date() { echo "08"; }
  nap()  { echo "$1" > "$WORK/slept"; }
  nap_to_minute
  [ "$(cat "$WORK/slept")" = "52" ] || exit 1 )
check "$?" "with :08 and :09 handled as decimal, not as bad octal"

# ── 9. KUAL can actually launch what menu.json names ─────────────────────────
#
# KUAL SHOWS NOTHING. It runs `action params`, closes the menu, and the home
# screen comes back — whether the dashboard started, the script was never
# found, or the shell died on its first line. "Every entry does nothing" was
# reported from a device with no shell access, and there was no way to tell
# those apart, so every entry now goes through kual.sh, which records what
# happened next to itself on /mnt/us where a USB cable can read it.
menu="$KDIR/menu.json"
check "$([ -f "$menu" ] && echo 0 || echo 1)" "menu.json is present"

# Paths are relative ON PURPOSE, and this is the assertion that says so: KUAL
# runs an action in the directory the menu.json it came from lives in (it is
# how KOReader's own extension gets away with "./bin/koreader-ext.sh"), so a
# relative path is the one spelling that does not care what the folder was
# named when it was copied. An absolute /mnt/us/extensions/esp32dash/… breaks
# every entry the moment somebody unzips it as esp32dash-main.
abs=$(grep -o '"params": "/[^"]*"' "$menu")
check "$([ -z "$abs" ] && echo 0 || echo 1)" \
      "no entry hardcodes an install path${abs:+ ($abs)}"

notdispatched=$(grep -o '"params": "[^"]*"' "$menu" | grep -v '"params": "kual\.sh ' || true)
check "$([ -z "$notdispatched" ] && echo 0 || echo 1)" \
      "every entry goes through kual.sh, so every launch is logged${notdispatched:+ ($notdispatched)}"

for f in kual.sh kual-run.sh start.sh stop.sh settings.sh update_dash.sh; do
    check "$([ -f "$KDIR/$f" ] && echo 0 || echo 1)" "  $f ships in kindle/"
done

# Every command the menu can send has to be one the dispatcher answers. A
# renamed entry that falls through to "unknown command" is the same silence
# this whole file exists to end.
#
# THE LIST IS READ OUT OF THE DISPATCHER, not written down here. This carried
# its own copy of the seven commands kual-run.sh knew, which made it a second
# place to remember — and the copy nothing executes is the one that goes stale.
# It did, the moment the battery settings were given menu entries: the check
# reported "power" as a command the dispatcher does not know, at a point where
# the dispatcher had just learnt it.
known=$(sed -n '/^case "\${1:-}" in$/,/^esac$/p' "$KDIR/kual-run.sh" |
        sed -n 's/^    \([a-z|"]*\)).*/\1/p' | tr '|\n' '  ')
case " $known " in
    *" start "*|*" stop "*) ;;
    *) echo "the dispatcher's command list did not parse: [$known]" >&2
       known="" ;;
esac
unknown=""
grep -o '"params": "kual\.sh [^"]*"' "$menu" | sed 's/.*kual\.sh //; s/"$//' |
while read -r cmd; do
    set -- $cmd
    case " $known " in
        *" $1 "*) ;;
        *) echo "$1" >> "$WORK/unknown" ;;
    esac
done
[ -f "$WORK/unknown" ] && unknown=$(cat "$WORK/unknown")
check "$([ -z "$unknown" ] && echo 0 || echo 1)" \
      "and every command it sends is one kual-run.sh knows${unknown:+ ($unknown)}"

# ── 9b. The launcher survives the thing it is here to fix ────────────────────
#
# CRLF is the first suspect whenever nothing runs: busybox ash cannot execute a
# script whose lines end in a carriage return, and these files are edited and
# copied on Windows. So the launcher is written to survive being CRLF itself —
# one command, where the only CR that can land is a trailing character on the
# last argument — and to heal the rest of the extension on the way past.
EXT="$WORK/ext"
mkdir -p "$EXT/layout"
for f in kual.sh kual-run.sh; do
    sed 's/$/\r/' "$KDIR/$f" > "$EXT/$f"        # deliberately Windows-mangled
done
cat > "$EXT/start.sh" <<'EOF'
#!/bin/sh
echo "start.sh ran in $(pwd)"
EOF
sed 's/$/\r/' "$EXT/start.sh" > "$EXT/start.crlf" && mv "$EXT/start.crlf" "$EXT/start.sh"
printf 'GR_X=20\r\nGR_Y=278\r\n' > "$EXT/layout/600x800.conf"
# Not a *.conf, and it is the one conf_init copies to dash.conf on first
# run — a CR here gives HOST a trailing carriage return and every fetch
# fails against "http://192.168.1.50\r" with nothing anywhere saying why.
printf 'HOST=192.168.1.50\r\n' > "$EXT/dash.conf.default"
printf '\211PNG\r\n\032\n' > "$EXT/icons.bmp"   # not ours: must not be touched
icons_before=$(wc -c < "$EXT/icons.bmp" | tr -dc 0-9)

( cd "$EXT" && PATH="$BIN:$PATH" sh kual.sh start > "$WORK/kual.out" 2>&1 )
rc=$?
check "$rc" "a CRLF kual.sh still launches, from a CRLF kual-run.sh"
check "$(grep -q "start.sh ran in" "$EXT/kual.log" 2>/dev/null && echo 0 || echo 1)" \
      "  and the script it launched ran, with its output in kual.log"
check "$(grep -q "healed Windows line endings" "$EXT/kual.log" 2>/dev/null && echo 0 || echo 1)" \
      "  the CRs it found were fixed in place, not just reported"
check "$(tr -d '\r' < "$EXT/start.sh" | cmp -s - "$EXT/start.sh" && echo 0 || echo 1)" \
      "  start.sh has no CR left in it"
check "$(tr -d '\r' < "$EXT/layout/600x800.conf" | cmp -s - "$EXT/layout/600x800.conf" && echo 0 || echo 1)" \
      "  and neither does a layout file, whose GR_X=20\\r FBInk would reject"
check "$(tr -d '\r' < "$EXT/dash.conf.default" | cmp -s - "$EXT/dash.conf.default" && echo 0 || echo 1)" \
      "  nor dash.conf.default, which first run copies into the live config"
check "$([ "$(wc -c < "$EXT/icons.bmp" | tr -dc 0-9)" = "$icons_before" ] && echo 0 || echo 1)" \
      "  while a file that is not ours keeps every byte it had"

# The log is the point of all of it: next to the scripts, on the volume that
# appears over USB, not in /tmp where a reboot takes it.
check "$([ -f "$EXT/kual.log" ] && echo 0 || echo 1)" \
      "the log is written beside the scripts, where a USB cable can read it"

# An entry that fails has to say so on the panel — KUAL will not.
cat > "$EXT/stop.sh" <<'EOF'
#!/bin/sh
echo "this is what went wrong" >&2
exit 3
EOF
: > "$FBINK_LOG"
( cd "$EXT" && PATH="$BIN:$PATH" sh kual.sh stop > /dev/null 2>&1 )
check "$([ "$?" = "3" ] && echo 0 || echo 1)" "a failing entry exits non-zero"
check "$(grep -q "exit 3" "$EXT/kual.log" && echo 0 || echo 1)" \
      "  with the exit code and the error text in the log"
check "$(grep -q "this is what went wrong" "$EXT/kual.log" && echo 0 || echo 1)" \
      "  including what the script itself printed"
check "$(grep -qi "failed" "$FBINK_LOG" && echo 0 || echo 1)" \
      "  and a line on the panel, because nothing else would ever show one"

# FBInk is not part of the Kindle firmware, and KUAL's PATH does not include
# where people put it. Without it every draw fails and the panel simply does
# not change — a dashboard that is running and invisible.
: > "$FBINK_LOG"
( cd "$EXT" && PATH="/usr/bin:/bin" sh kual.sh start > /dev/null 2>&1 )
check "$(grep -q "FBInk is not installed" "$EXT/kual.log" && echo 0 || echo 1)" \
      "a missing FBInk is named in the log rather than being drawn around"

# And the dashboard itself refuses to run blind: it would otherwise keep its
# schedule for days against a framebuffer it cannot write to.
out=$(PATH="/usr/bin:/bin" DASH_DIR="$KDIR" DASH_TMP="$WORK/t2" \
      /bin/sh "$KDIR/update_dash.sh" 2>&1)
check "$([ "$?" != "0" ] && echo 0 || echo 1)" \
      "update_dash.sh stops when FBInk is missing instead of drawing nothing"
check "$(printf '%s' "$out" | grep -q "fbink not found" && echo 0 || echo 1)" \
      "  and says which binary it wanted"

# A CR anywhere in these files is the other way the menu dies silently: KUAL's
# JSON reader rejects the file and lists nothing, and busybox ash cannot run a
# script whose shebang ends in one. .gitattributes pins them to LF; this is
# what notices when that stops being true.
crlf=""
for f in "$menu" "$KDIR"/*.sh "$KDIR/config.xml"; do
    [ -f "$f" ] || continue
    if od -c "$f" 2>/dev/null | grep -q '\\r'; then crlf="$crlf $(basename "$f")"; fi
done
check "$([ -z "$crlf" ] && echo 0 || echo 1)" \
      "no CRLF in what the Kindle parses${crlf:+ —$crlf}"

echo ""
if [ "$FAILURES" -gt 0 ]; then
    echo "FAIL: $FAILURES of $CHECKS check(s) failed"
    exit 1
fi
echo "OK: $CHECKS checks — the dashboard speaks FBInk, and its settings hold"
