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
        [ -n "$out" ] && printf 'BM fake' > "$out"
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
FC0_CODE=61
FC0_TEMP=6
FC1_LABEL="00:00"
FC1_CODE=3
FC1_TEMP=4
FC2_LABEL="03:00"
FC2_CODE=0
FC2_TEMP=2
WK0_NAME="MO"
WK0_DAY=24
WK1_NAME="TU"
WK1_DAY=25
WK2_NAME="WE"
WK2_DAY=26
WK3_NAME="TH"
WK3_DAY=27
WK4_NAME="FR"
WK4_DAY=28
WK5_NAME="SA"
WK5_DAY=29
WK6_NAME="SU"
WK6_DAY=30
WK_TODAY=1
OUT_BATT_WARN=1
LBL_OUTSIDE="OUTSIDE"
LBL_INSIDE="INSIDE"
LBL_LAST24="LAST 24 HOURS"
LBL_FORECAST="FORECAST"
LBL_MEASURED="Measured on site"
LBL_WIND="wind"
LBL_TO="to"
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

# White text on the inverted "today" cell has to be drawn WITHOUT a background
# box, or FBInk's OT renderer fills that box white and the glyphs vanish.
reset_log
draw_forecast_body
check "$(grep -q -- '-O	-C	WHITE' "$FBINK_LOG" && echo 0 || echo 1)" \
      "white text is drawn bgless (-O), not as white-on-white"
nobg=0
while IFS= read -r line; do
    case "$line" in *"	-t	"*) ;; *) continue ;; esac
    case "$line" in *"	-O	"*) ;; *) nobg=$((nobg + 1)) ;; esac
done < "$FBINK_LOG"
check "$nobg" "and so is every other string ($nobg with a background box)"

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

# ── 3d. A failed chart fetch keeps the chart that is there ───────────────────
printf 'GOOD' > "$DASH_TMP/graph.bmp"
( WGET_OK_HOST=nowhere.invalid HOST=203.0.113.9 fetch_graph
  [ "$(cat "$DASH_TMP/graph.bmp")" = "GOOD" ] || exit 1
  [ ! -f "$DASH_TMP/graph.new" ] || exit 2 )
check "$?" "a failed graph fetch leaves the previous chart intact"

# ── 3e. With no data at all, the page says why ───────────────────────────────
( HAVE_DATA=0
  unset Z_GROUP_OUT OUT_TEMP IN_TEMP IN_HUM OUT_HUM
  : > "$FBINK_LOG"
  draw_sensors_body && exit 1                      # must refuse
  redraw_offline "12:34"
  grep -q -- 'Cannot reach' "$FBINK_LOG" || exit 2
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

echo ""
if [ "$FAILURES" -gt 0 ]; then
    echo "FAIL: $FAILURES of $CHECKS check(s) failed"
    exit 1
fi
echo "OK: $CHECKS checks — the dashboard speaks FBInk, and its settings hold"
