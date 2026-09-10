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
        if [ "$out" = "-" ] || [ -z "$out" ]; then cat "$FIXTURE"; else cp "$FIXTURE" "$out"; fi
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
chmod +x "$BIN"/*
PATH="$BIN:$PATH"
export PATH

FBINK_LOG="$WORK/fbink.log"
export FBINK_LOG
: > "$FBINK_LOG"

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
CH_B=174
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
EOF

# ── Load the dashboard as a library ──────────────────────────────────────────
DASH_TMP="$WORK/tmp"
DASH_CONF="$WORK/dash.conf"
export DASH_TMP DASH_CONF
mkdir -p "$DASH_TMP"
cp "$FIXTURE" "$DASH_TMP/data.txt"

DASH_LIB_ONLY=1 DASH_DIR="$KDIR" . "$KDIR/update_dash.sh"

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

lines_of() { [ -s "$1" ] && wc -l < "$1" | tr -d ' ' || echo 0; }
# grep -c always prints a count, including 0 — the `|| echo 0` this used to
# carry appended a SECOND line on no-match, so "$n" became "0\n0" and every
# numeric test on it was a syntax error rather than a comparison.
calls()    { grep -c . "$FBINK_LOG" 2>/dev/null; }
reset_log() { : > "$FBINK_LOG"; }

echo "The Kindle dashboard, drawn against a fake FBInk:"

# ── 1. Everything it draws is a command FBInk understands ────────────────────
load_kv "$DASH_TMP/data.txt"
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
  grep -q "8 мин" "$FBINK_LOG" || exit 1
  grep -q "·" "$FBINK_LOG" && exit 2
  exit 0 )
check "$?" "and with no wind to report it is the age alone, with no stray dot"

( reset_log
  FC_WIND=5 FC_AGE="" draw_forecast_body
  grep -q "5 km/h" "$FBINK_LOG" || exit 1
  grep -q "·" "$FBINK_LOG" && exit 2
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
  grep -q -- "left=$lx,top=$OL0_Y" "$FBINK_LOG" || exit 3
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
( DASH_LIB_ONLY=1 DASH_DIR="$KDIR" DASH_CONF="$DASH_CONF" DASH_TMP="$DASH_TMP" \
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
( DASH_LIB_ONLY=1 DASH_DIR="$KDIR" DASH_CONF="$DASH_CONF" DASH_TMP="$DASH_TMP" \
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
unknown=""
grep -o '"params": "kual\.sh [^"]*"' "$menu" | sed 's/.*kual\.sh //; s/"$//' |
while read -r cmd; do
    set -- $cmd
    case "$1" in
        start|stop|show|find|next|reset|profile) ;;
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
