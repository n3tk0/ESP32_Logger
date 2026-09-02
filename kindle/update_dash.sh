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
DASH_DIR="/mnt/us/dashboard"
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
draw_all() {
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
    draw_text_reg "$FOOT_X" "$FOOT_Y" "$FOOT_SZ" "GRAY5" "$LBL_MEASURED"
}

# ── Data-only update (partial) ───────────────────────────────────────────────
draw_data() {
    # Re-renders just the data fields without full clear.
    # Each FBInk call does a partial refresh of its own area.
    draw_all  # For now, same as full draw but without clear — FBInk handles partial
}

# ── Cleanup on exit ──────────────────────────────────────────────────────────
cleanup() {
    restore_sleep
    rm -rf "$TMP"
    exit 0
}
trap cleanup INT TERM

# ══════════════════════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════════════════════
mkdir -p "$TMP"
prevent_sleep

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
    sleep 60
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
