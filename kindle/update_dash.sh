#!/bin/sh
# ============================================================================
# update_dash.sh — FBInk Kindle Weather Dashboard
#
# Fetches sensor data from an ESP32 collector and renders it directly to the
# Kindle's framebuffer using FBInk. Designed for always-on, wall-powered use.
#
# Tiered refresh strategy to balance readability vs e-ink wear:
#   Every  1 min — update clock text (partial refresh, clock zone only)
#   Every  5 min — fetch sensor data + graph, update all text (partial)
#   Every 10 min — partial clear of clock zone (anti-ghosting)
#   Every 30 min — full screen flash refresh (clear all ghosting)
#
# All temporary files land on /tmp (tmpfs) to prevent eMMC wear.
# ============================================================================

# ── Configuration ────────────────────────────────────────────────────────────
# WHERE THIS SCRIPT LIVES, not a fixed path. It was pinned to
# /mnt/us/dashboard, which is the one place a KUAL extension cannot be: KUAL
# builds its menu by scanning the subdirectories of /mnt/us/extensions and
# nowhere else, so an extension installed where the README said to put it was
# never listed — and one moved into extensions/ to be listed then found no
# layout and no icons, because those were still looked up under the old path.
#
# Deriving it removes the conflict: the folder works wherever it is copied,
# and the KUAL location is just one of them. DASH_DIR in the environment still
# wins, for anyone who has split the data off somewhere else.
if [ -z "$DASH_DIR" ]; then
    DASH_DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || DASH_DIR=$(dirname "$0")
fi
TMP="/tmp/dash"
HOST="http://192.168.1.50"       # ESP32 collector IP — change to match yours
FETCH_TIMEOUT=10                  # wget timeout in seconds

# ── Prevent sleep & screensaver ──────────────────────────────────────────────
# The device is wall-powered and must not blank the screen or suspend.
# lipc-set-prop talks to the Kindle's own power daemon.
prevent_sleep() {
    lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null
    # Some firmware versions need this too:
    lipc-set-prop com.lab126.cmd intrf enable 2>/dev/null
    # Disable the framework's own screen blanking if running:
    lipc-set-prop com.lab126.blanket unload 2>/dev/null
}

restore_sleep() {
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null
}

# ── Font selection ───────────────────────────────────────────────────────────
# System fonts first; user can drop overrides into dashboard/fonts/.
SYS_FONTS="/usr/java/lib/fonts"
USR_FONTS="$DASH_DIR/fonts"

pick_font() {
    if [ -f "$USR_FONTS/$1" ]; then echo "$USR_FONTS/$1"
    elif [ -f "$SYS_FONTS/$1" ]; then echo "$SYS_FONTS/$1"
    else echo "$1"; fi  # bare name as last resort — FBInk may find it
}

FONT_REG=$(pick_font "Bookerly-Regular.ttf")
FONT_BOLD=$(pick_font "Bookerly-Bold.ttf")

# ── Network helpers ──────────────────────────────────────────────────────────
fetch_data() {
    wget -q -T "$FETCH_TIMEOUT" -O "$TMP/data.txt" "$HOST/kindle/data" 2>/dev/null
}

fetch_graph() {
    wget -q -T 15 -O "$TMP/graph.bmp" "$HOST/kindle/graph.bmp" 2>/dev/null
}

# Read KEY="value" pairs into the environment WITHOUT executing the file.
#
# `. "$TMP/data.txt"` was doing exactly that — executing it — as root. The file
# arrives over plain HTTP from a device on the network, and one of the values in
# it is the forecast provider's free-text summary, so "the collector is
# trusted" was not enough even before considering anyone able to answer for it
# on the wire. A summary of  "; reboot; #  is a command, not a description.
#
# The collector escapes what it emits, and this refuses to run what it reads.
# Either alone would do; both means a mistake at one end is not a shell on the
# reader.
#
# Only names matching the dashboard's own convention are accepted, the value is
# taken literally up to the closing quote, and the shell's own unescaping is
# applied to nothing.
load_data() {
    [ -f "$TMP/data.txt" ] || return 1
    while IFS= read -r line; do
        key=${line%%=*}
        [ "$key" = "$line" ] && continue          # no '=' — not an assignment
        case "$key" in
            *[!A-Za-z0-9_]* | '' ) continue ;;    # not a plain variable name
        esac
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
    done < "$TMP/data.txt"

    # The layout follows the data, always — RES_W and RES_H come from the
    # collector, so the two cannot be loaded independently.
    #
    # It used to be called once, inside the branch taken when the FIRST fetch
    # succeeded. A Kindle that finished booting before the ESP32 did took the
    # other branch, and nothing in the main loop ever called it again: every
    # coordinate stayed unset for as long as the script ran, and the only cure
    # was noticing and restarting it. Calling it here also means a change to
    # the FBInk resolution on the collector is picked up on the next fetch
    # rather than at the next reboot.
    load_layout
    return 0
}

# ── Layout loading ───────────────────────────────────────────────────────────
load_layout() {
    local conf="$DASH_DIR/layout/${RES_W}x${RES_H}.conf"
    if [ -f "$conf" ]; then
        . "$conf"
    else
        # Fallback to 600x800
        . "$DASH_DIR/layout/600x800.conf"
    fi
}

# ── Drawing primitives ───────────────────────────────────────────────────────
# FBInk coordinate reference:
#   -x COL  — column (from left)
#   -y ROW  — row (from top)
#   -S SIZE — pixel size for OT/TT fonts
#   -F FONT — path to .ttf
#   -C COLOR — FBInk color name or hex
#   -p       — use pixel coordinates (not row/col)
#   -M       — partial refresh (no flash)
#   -f       — full flash refresh

draw_text() {
    # $1=x $2=y $3=size $4=font $5=color $6=text
    fbink -x "$1" -y "$2" -S "$3" -F "$4" -C "$5" -p -M -q "$6" 2>/dev/null
}

draw_text_bold() {
    draw_text "$1" "$2" "$3" "$FONT_BOLD" "$4" "$5"
}

draw_text_reg() {
    draw_text "$1" "$2" "$3" "$FONT_REG" "$4" "$5"
}

draw_hline() {
    # $1=x $2=y $3=width $4=color
    fbink -x "$1" -y "$2" -S 1 -C "$4" -p -M -q -L "$3" 2>/dev/null
}

# ── Clock rendering ──────────────────────────────────────────────────────────
draw_clock() {
    local now_time="$1"
    draw_text_bold "$CL_X" "$CL_Y" "$CL_SIZE" "BLACK" "$now_time"
}

# ── Full dashboard rendering ─────────────────────────────────────────────────
# ── The eleven places ────────────────────────────────────────────────────────
# The collector sends what is in each named place and, for the two groups that
# can close up behind an empty one, which places survived and in what order —
# and for the grid, how they break into rows. All this end does is turn that
# into pixels for one panel.
#
# NAMES, NOT COORDINATES. 600x800 and 1072x1448 want different pixel positions
# for the same design, and the reader should not have to place every value
# twice; the layout file carries the positions, this carries the drawing.

# ONE PLACE'S VALUE IS THREE DRAWS, not one.
#
# The number at its full size, then its unit as a footnote at four tenths of it,
# then the tendency arrow if it has one. FBInk draws one size per call, so each
# piece is a separate call at an x that depends on how wide the piece before it
# came out — and FBInk will not say. The collector measures them for us and
# sends the widths as Z_<PLACE>_VADVW and _UADVW, in thousandths of the type
# size; see kdAdvanceMille() in KindleDashboard.cpp.
#
# A unit at full size was what the first version of this drew, and it was wrong
# twice: "1008 hPa ↘" at 32 px is wider than the column it sits in, and a unit
# as loud as its number is not a unit, it is a second number.
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
# beside it. FBInk's y is the TOP of the text, so two sizes drawn at one y sit
# on two baselines and the row looks dropped; the ascent is about eight tenths
# of the size, which is where the 80 comes from.
baseline_y() {
    # $1=row top  $2=largest size in the row  $3=this size
    echo $(( $1 + ($2 - $3) * 80 / 100 ))
}

draw_zones() {
    # A collector running older firmware sends no Z_GROUP_OUT, and draw_all
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
        draw_text_reg "$sx" "$y" "$big_sz" "GRAY10" "/"
        draw_field "$(( sx + ${SLASH_W:-22} ))" "$y" "$big_sz" "${Z_BIG_BOLD:-0}" \
                   "$Z_BIG_VALUE" "$Z_BIG_UNIT" "$Z_BIG_ARROW" \
                   "${Z_BIG_VADVW:-0}" "${Z_BIG_UADVW:-0}" "${Z_BIG_INK:-BLACK}"
    fi

    # The 24 h low-to-high and the age, composed by the collector so that the
    # wording, the unit and the rounding are the page's and not this script's.
    # Set in the page's mid grey, not its dark one: it is context for the big
    # number above it, not a reading in its own right. The collector sends an
    # empty string when the reader has switched it off.
    [ -n "${Z_SUB:-}" ] && \
        draw_text_reg "$lx" "${SUB_Y:-128}" "${SUB_SZ:-14}" "GRAY7" "$Z_SUB"

    # ── Left column: the grid ───────────────────────────────────────────────
    # GRID_ROWS says how many cells are on each row; each row then divides its
    # own width by its own count, so two cells are two halves rather than two of
    # three thirds with the last one left white. The balancing is the
    # collector's, because the HTML page has to break the rows the same way and
    # a rule written twice is a rule written differently.
    local gy="${GRID_Y:-150}" gcols gvsz gcw gi=0
    set -- ${GRID_ZONES:-}
    for gcols in ${GRID_ROWS:-}; do
        gcw=$(( ${COL_L_W:-270} / gcols ))
        # Three across is a third of half a page, which "1008 hPa" with an arrow
        # after it does not fit at the two-across size.
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
    # Three fields or two, whichever survived, on one row with the first set
    # larger. Equal columns, so two are two halves and three are three thirds —
    # the first is bigger by TYPE, not by width, which is what keeps the row
    # aligned whichever count it is.
    n=0
    for z in ${IN_ZONES:-}; do n=$((n + 1)); done
    if [ "$n" -gt 0 ]; then
        draw_hline "$rx" "${IN_RULE_Y:-126}" "$rw" "GRAY10"
        draw_text_reg "$rx" "${IN_LAB_Y:-134}" "$lab_sz" "GRAY7" "$Z_GROUP_IN"

        # The first field gets more of the row, not an equal share: it is set
        # larger, so equal columns crowd it against its neighbour while leaving
        # the small ones space they do not need. The same split the HTML page
        # uses.
        local w1
        if [ "$n" -ge 3 ]; then w1=$(( rw * 42 / 100 ))
        elif [ "$n" -eq 2 ]; then w1=$(( rw * 58 / 100 ))
        else w1="$rw"
        fi
        cw=$(( n > 1 ? (rw - w1) / (n - 1) : rw ))

        # ALL THREE VALUES SIT ON ONE BOTTOM EDGE. The first has no caption —
        # the heading above already says which room this is, and "TEMP" under
        # it says nothing the degree sign has not — so it is half again as tall
        # and starts higher. The other two hang their captions in the space it
        # does not use, which is the space its missing caption gave back.
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
    # Drawn last, so nothing above paints over it. SEP_W=0 in the layout file
    # turns it off for a panel where it does not come out cleanly.
    if [ "${SEP_W:-1}" -gt 0 ] 2>/dev/null; then
        fbink -x "${SEP_X:-300}" -y "${TOP_Y:-20}" -C GRAY10 -p -M -q \
              -R "${SEP_W:-1}x${SEP_H:-210}" 2>/dev/null
    fi

    return 0
}

draw_all() {
    # The nine places, when the collector describes them. One running older
    # firmware sends no Z_GROUP_OUT, and the hardwired block below still draws
    # the page it knows how to describe — so the reader and the collector do not
    # have to be updated in the same minute.
    if draw_zones; then
        draw_clock "$(date +%H:%M)"
        draw_hline "$RULE2_X" "$RULE2_Y" "$RULE2_W" "GRAY10"
        draw_text_reg "$LAB_CHART_X" "$LAB_CHART_Y" "$LAB_SZ" "GRAY7" "$LBL_LAST24"
        if [ -f "$TMP/graph.bmp" ]; then
            fbink -g file="$TMP/graph.bmp",x="$GR_X",y="$GR_Y" -p -M -q 2>/dev/null
        fi
        draw_forecast
        return 0
    fi

    # Section label: OUTSIDE
    draw_text_reg "$LAB_OUT_X" "$LAB_OUT_Y" "$LAB_SZ" "GRAY7" "$LBL_OUTSIDE"

    # Battery warning icon (if active)
    if [ "$OUT_BATT_WARN" = "1" ] && [ -f "$ICON_DIR/fc_batt.bmp" ]; then
        fbink -g file="$ICON_DIR/fc_batt.bmp",x="$BATT_X",y="$BATT_Y" -p -M -q 2>/dev/null
    fi

    # Hero outdoor temperature
    draw_text_bold "$HERO_X" "$HERO_Y" "$HERO_SZ" "BLACK" "${OUT_TEMP}°"

    # Outdoor humidity
    if [ "$OUT_HUM" != "-1" ]; then
        draw_text_reg "$HUM_X" "$HUM_Y" "$HUM_SZ" "GRAY4" "/${OUT_HUM}%"
    fi

    # Subtitle: 24h range + age
    local sub=""
    if [ -n "$OUT_RANGE_LO" ] && [ -n "$OUT_RANGE_HI" ]; then
        sub="${OUT_RANGE_LO} ${LBL_TO} ${OUT_RANGE_HI}°"
    fi
    if [ "$OUT_AGE_MIN" -gt 1 ] 2>/dev/null; then
        sub="$sub  ${OUT_AGE_MIN}m"
    fi
    if [ -n "$sub" ]; then
        draw_text_reg "$SUB_X" "$SUB_Y" "$SUB_SZ" "GRAY4" "$sub"
    fi

    # Pressure
    if [ -n "$OUT_PRES" ] && [ "$OUT_PRES" != "--" ]; then
        draw_text_bold "$PRES_X" "$PRES_Y" "$PRES_SZ" "BLACK" "$OUT_PRES $OUT_PRES_UNIT"
    fi

    # Pressure tendency
    if [ -n "$OUT_TEND" ]; then
        draw_text_reg "$TEND_X" "$TEND_Y" "$TEND_SZ" "GRAY4" "$OUT_TEND_ARROW $OUT_TEND ($OUT_TEND_DELTA)"
    fi

    # Divider line
    draw_hline "$RULE1_X" "$RULE1_Y" "$RULE1_W" "GRAY10"

    # Clock area (right side of hero)
    draw_clock "$(date +%H:%M)"

    # Indoor section label
    draw_text_reg "$LAB_IN_X" "$LAB_IN_Y" "$LAB_SZ" "GRAY7" "$LBL_INSIDE"

    # Indoor temperature + humidity
    local in_str="${IN_TEMP}°"
    if [ "$IN_HUM" != "-1" ]; then
        in_str="${in_str}/${IN_HUM}%"
    fi
    if [ "$IN_AGE_MIN" -gt 1 ] 2>/dev/null; then
        in_str="$in_str  ${IN_AGE_MIN}m"
    fi
    draw_text_reg "$IN_X" "$IN_Y" "$IN_SZ" "BLACK" "$in_str"

    # Second divider
    draw_hline "$RULE2_X" "$RULE2_Y" "$RULE2_W" "GRAY10"

    # Section label: LAST 24 HOURS
    draw_text_reg "$LAB_CHART_X" "$LAB_CHART_Y" "$LAB_SZ" "GRAY7" "$LBL_LAST24"

    # Trend graph
    if [ -f "$TMP/graph.bmp" ]; then
        fbink -g file="$TMP/graph.bmp",x="$GR_X",y="$GR_Y" -p -M -q 2>/dev/null
    fi

    # Third divider
    draw_hline "$RULE3_X" "$RULE3_Y" "$RULE3_W" "GRAY10"

    draw_forecast
}

# ── Forecast, week strip and footer ──────────────────────────────────────────
# Extracted from draw_all() so the slot flow and the legacy fixed layout can
# both end the page the same way. None of it is a sensor reading, so none of it
# is a slot: the forecast comes from an API, the week strip from the clock, and
# the footer is a constant.
draw_forecast() {
    # Forecast section
    if [ -n "$FC_SUMMARY" ]; then
        draw_text_reg "$LAB_FC_X" "$LAB_FC_Y" "$LAB_SZ" "GRAY7" "$LBL_FORECAST"

        # Current condition icon
        local icon="$ICON_DIR/fc_${FC_CODE}_${FC_MAIN_SZ}.bmp"
        [ ! -f "$icon" ] && icon="$ICON_DIR/fc_-1_${FC_MAIN_SZ}.bmp"
        if [ -f "$icon" ]; then
            fbink -g file="$icon",x="$FC_ICON_X",y="$FC_ICON_Y" -p -M -q 2>/dev/null
        fi

        # Current condition text
        draw_text_reg "$FC_TEXT_X" "$FC_TEXT_Y" "$FC_TEXT_SZ" "BLACK" "$FC_SUMMARY"
        draw_text_bold "$FC_TEMP_X" "$FC_TEMP_Y" "$FC_TEMP_SZ" "BLACK" "${FC_HIGH}°/${FC_LOW}°"
        if [ -n "$FC_WIND" ] && [ "$FC_WIND" != "0" ]; then
            draw_text_reg "$FC_WIND_X" "$FC_WIND_Y" "$FC_WIND_SZ" "GRAY4" "$LBL_WIND ${FC_WIND} km/h"
        fi

        # 3 outlook columns
        for i in 0 1 2; do
            eval "ol_label=\$FC${i}_LABEL"
            eval "ol_code=\$FC${i}_CODE"
            eval "ol_temp=\$FC${i}_TEMP"
            eval "ol_x=\$OL${i}_X"
            eval "ol_y=\$OL${i}_Y"

            if [ -n "$ol_label" ]; then
                # Outlook label
                draw_text_reg "$ol_x" "$ol_y" "$OL_LABEL_SZ" "GRAY7" "$ol_label"

                # Outlook icon
                local ol_icon="$ICON_DIR/fc_${ol_code}_${FC_OL_SZ}.bmp"
                [ ! -f "$ol_icon" ] && ol_icon="$ICON_DIR/fc_-1_${FC_OL_SZ}.bmp"
                local ol_icon_y=$((ol_y + OL_ICON_OFFSET))
                if [ -f "$ol_icon" ]; then
                    fbink -g file="$ol_icon",x="$ol_x",y="$ol_icon_y" -p -M -q 2>/dev/null
                fi

                # Outlook temp
                local ol_temp_y=$((ol_y + OL_TEMP_OFFSET))
                draw_text_bold "$ol_x" "$ol_temp_y" "$OL_TEMP_SZ" "BLACK" "${ol_temp}°"
            fi
        done
    fi

    # Week strip
    local wk_x="$WK_X"
    for i in 0 1 2 3 4 5 6; do
        eval "wk_name=\$WK${i}_NAME"
        eval "wk_day=\$WK${i}_DAY"

        # Background: inverted for today, grey for weekend
        if [ "$i" = "$WK_TODAY" ]; then
            # Today: inverted (black bg, white text)
            fbink -x "$wk_x" -y "$WK_Y" -S 1 -C BLACK -p -M -q -R "${WK_CELL_W}x${WK_CELL_H}" 2>/dev/null
            draw_text_reg "$wk_x" "$((WK_Y + WK_NAME_OFFSET))" "$WK_NAME_SZ" "WHITE" "$wk_name"
            draw_text_bold "$wk_x" "$((WK_Y + WK_DAY_OFFSET))" "$WK_DAY_SZ" "WHITE" "$wk_day"
        else
            local wk_bg="GRAY15"
            # Weekend slightly darker
            [ "$i" = "5" ] || [ "$i" = "6" ] && wk_bg="GRAY14"
            fbink -x "$wk_x" -y "$WK_Y" -S 1 -C "$wk_bg" -p -M -q -R "${WK_CELL_W}x${WK_CELL_H}" 2>/dev/null
            draw_text_reg "$wk_x" "$((WK_Y + WK_NAME_OFFSET))" "$WK_NAME_SZ" "GRAY7" "$wk_name"
            draw_text_bold "$wk_x" "$((WK_Y + WK_DAY_OFFSET))" "$WK_DAY_SZ" "BLACK" "$wk_day"
        fi

        wk_x=$((wk_x + WK_CELL_W))
    done

    # Footer
    draw_hline "$FOOT_RULE_X" "$FOOT_RULE_Y" "$FOOT_RULE_W" "GRAY10"
    draw_text_reg "$FOOT_X" "$FOOT_Y" "$FOOT_SZ" "GRAY5" "$LBL_MEASURED"}

}

# ── Data-only update (partial) ───────────────────────────────────────────────
draw_data() {
    # Re-renders just the data fields without full clear.
    # Each FBInk call does a partial refresh of its own area.
    draw_all  # For now, same as full draw but without clear — FBInk handles partial
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
trap cleanup INT TERM

# A sleep that can be interrupted.
#
# `sleep 60` in the foreground cannot: a shell running a foreground command
# does not act on a trapped signal until that command returns, so pressing Stop
# in KUAL did nothing visible for up to a minute — and stop.sh had already
# removed the pidfile by then, so a second Start would launch another copy
# alongside the one still winding down, two processes drawing to one
# framebuffer.
#
# Backgrounding it and waiting is the POSIX way round that: `wait` returns as
# soon as a trapped signal arrives, and the handler runs immediately.
SLEEP_PID=""
nap() {
    sleep "$1" &
    SLEEP_PID=$!
    wait "$SLEEP_PID" 2>/dev/null
    SLEEP_PID=""
}

# ══════════════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════════════
mkdir -p "$TMP"
prevent_sleep

# A layout BEFORE the first fetch, so no drawing path can run without one.
#
# RES_W and RES_H come from the collector, so load_layout() falls back to
# 600x800 here and load_data() reloads it at the real resolution the moment the
# collector answers. Without this the clock still ran on its own timer while the
# collector was unreachable, emitting `fbink -x  -y  -S` with every coordinate
# empty — rejected by FBInk, so the "Cannot reach" message stayed on screen by
# luck rather than by design.
load_layout

# Initial fetch and full draw
if fetch_data; then
    load_data          # also loads the layout, which follows RES_W/RES_H
    fetch_graph
    fbink -c                    # clear full screen
    draw_all
    draw_clock "$(date +%H:%M)"
    fbink -s -f                 # full refresh (flash)
else
    fbink -c
    draw_text_reg 50 400 24 "BLACK" "Cannot reach $HOST — check WiFi"
    fbink -s -f
fi

# Main loop
CYCLE=0
while true; do
    nap 60
    CYCLE=$((CYCLE + 1))
    NOW_TIME=$(date +%H:%M)

    if [ $((CYCLE % 30)) -eq 0 ]; then
        # ═══ FULL REFRESH (every 30 min) ═══
        # Clears ghosting with a black flash, then redraws everything.
        fetch_data && load_data
        fetch_graph
        fbink -c                        # full clear (flash to black)
        draw_all
        draw_clock "$NOW_TIME"
        fbink -s -f                     # full e-ink refresh

    elif [ $((CYCLE % 10)) -eq 0 ]; then
        # ═══ PARTIAL CLEAR (every 10 min) ═══
        # Clears just the clock zone to reduce ghosting buildup.
        fetch_data && load_data
        # Clear clock area
        fbink -x "$CL_X" -y "$CL_Y" -C WHITE -p -M -q \
              -R "${CL_W}x${CL_H}" 2>/dev/null
        draw_clock "$NOW_TIME"
        draw_data

    elif [ $((CYCLE % 5)) -eq 0 ]; then
        # ═══ DATA UPDATE (every 5 min) ═══
        # Fresh sensor data + new graph.
        fetch_data && load_data
        fetch_graph
        draw_data
        draw_clock "$NOW_TIME"

    else
        # ═══ CLOCK ONLY (every 1 min) ═══
        # Clear just the clock text and redraw.
        fbink -x "$CL_X" -y "$CL_Y" -C WHITE -p -M -q \
              -R "${CL_W}x${CL_H}" 2>/dev/null
        draw_clock "$NOW_TIME"
    fi
done
