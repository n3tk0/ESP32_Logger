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

# ── Power ────────────────────────────────────────────────────────────────────
# awake | wifi | suspend. See the note over net_up(): a ten-year-old Kindle
# looping a shell script with an associated radio draws 60-80 mA and lasts a
# day and a half. Each step down turns off more of that, and asks for more
# trust that the device comes back.
POWER=awake
# Seconds to wait for the radio to associate before fetching anyway.
WIFI_WAIT=15
# Stop the Amazon reader framework while the panel runs (1 = yes). Put back on
# Stop. Another 10-15 mA, at the cost of the device not being a reader.
GUI_STOP=0

# ── The screen ───────────────────────────────────────────────────────────────
# See canvas_take() and the tap menu below it. THESE HAVE TO BE HERE and not
# only in dash.conf.default: conf_load() validates every key in conf_keys()
# against whatever the running shell holds, so a key with no built-in default
# is empty on any dash.conf written before it existed — which is every one
# already on a reader. CANVAS and MENU_LBL refuse an empty value, so the
# upgrade warned once a minute and then baked the empty into dash.conf on the
# next save.
CANVAS=desktop
# ON BY DEFAULT NOW. It was off, and it is the only way back from GUI_STOP —
# which takes the launcher, and with it the Stop button, away. A setting whose
# job is to be the recovery path is a setting that has to be there before
# anybody needs it; it costs one process blocked in read(2).
TOUCH=1
TOUCH_DEV=
TOUCH_MAXX=0
TOUCH_MAXY=0
TOUCH_SWAP=0
# THE LABELS AND WHAT THEY DO, IN TWO KEYS. MENU_ACT is the authoritative one:
# it decides how many buttons the bar has and what each one runs, and MENU_LBL
# only names them. A dash.conf written before MENU_ACT existed carries three
# labels and no actions, so the built-in four would have been drawn under three
# words shifted one place along — "Hide" over the button that keeps the panel
# awake. menu_labels() takes the reader's list only when it names EVERY button
# and is not simply the shipped one; otherwise every label is derived from the
# action it sits over, so the bar is never mislabelled.
#
# THE SHIPPED WORDS ARE THEIR OWN CONSTANTS. The keys below are what dash.conf
# overwrites, so by the time the question is asked there is nothing left to
# compare against — and "these are the labels the reader chose" and "these have
# simply never been changed" are the same four words.
MENU_LBL_STD="Refresh|Awake/Sleep|Forecast|More|Exit"
#: The shipped four before `forecast`. Still in every older dash.conf, and still
#: shipped words rather than the reader's, so it is refused the same way.
MENU_LBL_OLD="Refresh|Awake/Sleep|More|Exit"
MENU_LBL2_STD="Find|Next|Battery|Info|Back"
SURE_LBL_STD="Tap the bar again to exit"
MENU_LBL="$MENU_LBL_STD"
MENU_ACT="refresh|wake|forecast|settings|quit"
#: The bar every reader had before `forecast` existed. conf_load() moves a
#: dash.conf still carrying it to the one above; a list somebody chose is theirs.
MENU_ACT_OLD="refresh|wake|settings|quit"
MENU_LBL2="$MENU_LBL2_STD"
SURE_LBL="$SURE_LBL_STD"
MODE_LBL="awake|radio off|asleep"
# ── Waking up ────────────────────────────────────────────────────────────────
# See suspend_for() and wake_interactive(). A press of the power button is the
# one wake source a suspended Kindle certainly has; WAKE_MENU decides whether
# the panel answers it with the bar, and WAKE_HOLD is how long it then listens.
WAKE_MENU=1
WAKE_HOLD=120
# ── Quiet hours ──────────────────────────────────────────────────────────────
# Hours, 0-23. Equal = off. Inside them nothing flashes and the clock can slow
# right down; leaving them spends one full flashing refresh to clear what the
# night's worth of partial updates left behind.
QUIET_FROM=0
QUIET_TO=0
QUIET_EVERY=15
# The footer's right-hand end: battery, mode, and whether the collector is
# answering. 1 = on.
STATUS=1
# Look for the collector by itself, once, when the first fetch fails against an
# address nobody has changed. 1 = on.
AUTO_FIND=1
# WHICH SHAPE THE PAGE IS DRAWN IN, and who decides.
#
#   auto        the collector says, in PAGE_MODE. It is the end that knows
#               whether a forecast is coming; this end only knows whether one
#               arrived, which is the same answer one tick later.
#   normal      always keep the forecast band, even with nothing in it.
#   standalone  always the tall-readings page: the forecast band goes and the
#               top block takes its 124 px. For a panel on a collector that is
#               its own access point and is never going to have a forecast.
LAYOUT=auto
# WHICH WAY UP THE PAGE IS DRAWN, in degrees clockwise, and who decides.
#
#   auto        the collector says, in ROTATE — the Rotation setting on its
#               Kindle page, which the browser page follows too.
#   0 90 180 270  this panel's own: 0 upright with the USB port at the
#               bottom, 90 on its side with the port on the right, 180
#               upside down, 270 on its side with the port on the left. 90
#               and 270 draw the landscape page.
#
# The framebuffer is what turns: fbdepth sets the panel's rotation and this
# script draws the page in the turned coordinates. A panel that will not turn
# says so in kual.log and keeps the upright page.
ROTATE=auto
# 1 if 90 and 270 come out the wrong way round on this model — the port on the
# left when 90 was asked for. Some Kindles number their rotations the other way.
ROTATE_SWAP=0
# HOW OFTEN THIS KINDLE SETS ITS CLOCK FROM THE COLLECTOR'S, in days.
#
#   auto        the collector says — the Clock sync setting on its Kindle page
#               (every day unless somebody changed it).
#   0           never: the Kindle keeps whatever time it has.
#   1 .. 60     every that many days.
#
# Between those the clock is drawn from the Kindle's own time, with or without
# the collector; the collector's time zone is used either way, so the panel
# says what the collector's web page says. Unless it is 0, a clock found more
# than ten minutes out is set at once, whatever the interval.
CLOCK_SYNC=auto
# WHICH ONE-TIME MOVES dash.conf HAS ALREADY HAD. A dash.conf with no CONF_VER
# line predates the marker and is 1; dash.conf.default ships the current one.
# conf_load() applies each move once and then writes the new number back, so a
# move never undoes a choice the reader makes afterwards.
#
#   2  the pre-`forecast` bar (MENU_ACT_OLD) gains the Forecast button
CONF_VER=1
CONF_VER_NOW=2

conf_keys() {
    echo "HOST FETCH_TIMEOUT CLOCK_EVERY DATA_EVERY GRAPH_EVERY FORECAST_EVERY FULL_EVERY CLOCK_FLASH_EVERY SENSOR_FLASH_EVERY TRACE POWER WIFI_WAIT GUI_STOP CANVAS TOUCH TOUCH_DEV TOUCH_MAXX TOUCH_MAXY TOUCH_SWAP MENU_LBL MENU_ACT MENU_LBL2 SURE_LBL MODE_LBL WAKE_MENU WAKE_HOLD QUIET_FROM QUIET_TO QUIET_EVERY STATUS AUTO_FIND LAYOUT ROTATE ROTATE_SWAP CLOCK_SYNC CONF_VER"
}

# THE KEYS THAT ARE NOT NUMBERS, in one place because two places drifted.
#
# strip_zeros() turns "08" into 8, which is what stops a tier written with a
# leading zero from breaking the arithmetic that reads it — and it turns "" into
# "0", which is right for a tier nobody filled in and wrong for a device path.
# It gave TOUCH_DEV a value of "0" and a warning once a minute about a setting
# the reader had deliberately left empty. conf_load() was taught the exception;
# settings.sh's cmd_set() was not, so `set TOUCH_DEV ""` still could not clear
# it. One function, asked by both.
conf_is_text() {
    case "$1" in
        HOST|POWER|CANVAS|TOUCH_DEV) return 0 ;;
        MENU_LBL|MENU_ACT|MENU_LBL2|SURE_LBL|MODE_LBL|LAYOUT|ROTATE|CLOCK_SYNC) return 0 ;;
    esac
    return 1
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
        POWER)              echo "awake | wifi (radio off between fetches) | suspend (also sleeps to RAM)" ;;
        WIFI_WAIT)          echo "Seconds to wait for the radio to associate" ;;
        GUI_STOP)           echo "Stop the Amazon reader framework while running (1 = on)" ;;
        CANVAS)             echo "desktop (draw over the reader) | blank (put its chrome away first)" ;;
        TOUCH)              echo "Tap the screen for a menu: refresh, hide, exit (1 = on)" ;;
        TOUCH_DEV)          echo "Touchscreen input device, or empty to find it" ;;
        TOUCH_MAXX)         echo "Touch panel's full scale across, or 0 if it reports screen pixels" ;;
        TOUCH_MAXY)         echo "Touch panel's full scale down, or 0 if it reports screen pixels" ;;
        TOUCH_SWAP)         echo "1 if the panel reports Y where X is expected" ;;
        LAYOUT)             echo "auto follows the collector; normal keeps the forecast band; standalone drops it and enlarges the readings" ;;
        ROTATE)             echo "auto follows the collector; 0, 90, 180 or 270 degrees clockwise (90 and 270 lie the Kindle on its side)" ;;
        ROTATE_SWAP)        echo "1 if 90 and 270 come out the wrong way round on this Kindle" ;;
        CLOCK_SYNC)         echo "auto follows the collector; 1-60 days between setting this Kindle's clock from the collector's; 0 never" ;;
        MENU_LBL)           echo "The labels on the tap menu, separated by bars" ;;
        MENU_ACT)           echo "What each button does: refresh|wake|forecast|settings|hide|quit" ;;
        MENU_LBL2)          echo "The labels on the settings bar, separated by bars" ;;
        SURE_LBL)           echo "What the bar says when it is asking to confirm Exit" ;;
        MODE_LBL)           echo "The three words for the power modes, separated by bars" ;;
        WAKE_MENU)          echo "A press of the power button opens the menu (1 = on)" ;;
        WAKE_HOLD)          echo "Seconds to stay awake and listening after such a press" ;;
        QUIET_FROM)         echo "Hour the quiet hours begin (0-23; same as QUIET_TO = off)" ;;
        QUIET_TO)           echo "Hour the quiet hours end (0-23)" ;;
        QUIET_EVERY)        echo "Minutes between clock updates during quiet hours (0 = as usual)" ;;
        STATUS)             echo "Draw battery, mode and collector state in the footer (1 = on)" ;;
        AUTO_FIND)          echo "Scan for the collector once if the first fetch fails (1 = on)" ;;
        CONF_VER)           echo "Which upgrades this file has had. Written by the dashboard; leave it alone" ;;
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
        # Which shape the page is. Only the collector can know: it is the end
        # that can see whether a forecast is coming.
        PAGE_MODE) return 0 ;;
        # Which way up the page is, in degrees, as the collector has it; and
        # whether the clock is on the page at all.
        PAGE_ROT|SHOW_CLOCK) return 0 ;;
        # The collector's time, its zone, and how often to set this clock by
        # them — see clock_sync().
        TIME_UTC|TIME_OFF|SYNC_DAYS) return 0 ;;
        # Where everything goes: the layout the collector worked out for what
        # is on the page, under the layout file's own names. Applied by
        # flow_apply(), which takes only names it lists and only digits.
        LY_*) return 0 ;;
        # Written by cache_save(), not by the collector — but the cache is
        # loaded through exactly the same path as a payload, so they are
        # allowed here. They reach one drawn string and nothing else.
        CACHED_AT|CACHED_ON) return 0 ;;
        SHOW_CHART|SHOW_WEEK|CHART_OUT|CHART_IN|KEY_OUT_ADVW|HERO_LINE) return 0 ;;
        # The wall page's band under the headline and the ink on it — pens,
        # checked where they are drawn (sub_band()).
        SUB_BAND|SUB_INK) return 0 ;;
        # The face chosen on the collector, KFACE_* 0..5 — see face_files().
        FONT_FACE) return 0 ;;
        # The week strip's look and, when it holds the forecast, its seven
        # days and which of them is today (WF_NOW); and the dividing lines'
        # thickness, pens and style. The pens are checked where they are
        # used — see rule_pen().
        WK_STYLE|WK_FC|WF_NOW|WF[0-9]_*|RULE_PX|RULE_INK|RULE_SOFT|RULE_STYLE) return 0 ;;
        # The chart's axis: the five values, the five hours, their widths, and
        # where the image's plot area is inside the image. CH_Z* are the
        # second line's values down the right, when it has a scale of its own.
        CH_Y[0-9]|CH_Y[0-9]W|CH_Z[0-9]|CH_Z[0-9]W|CH_H[0-9]|CH_H[0-9]W) return 0 ;;
        CH_L|CH_R|CH_T|CH_B|CH_NOTE) return 0 ;;
        # A newer copy of these scripts, on offer from the collector — see
        # pkg_check(). Each one is checked again where it is used.
        PKG_VER|PKG_MD5|PKG_SIZE) return 0 ;;
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
    # collector's payload that is END=1, the key payload_ok() reads to tell a
    # whole payload from the first part of one, so every fetch would have been
    # held to the older collectors' weaker test instead.
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
    # Both, because $( ) is a fork and epoch_now() is asked twice a tick. The
    # answer nobody reads costs nothing.
    STRIPPED="$v"
    printf '%s' "$v"
}

# The system clock in seconds, ready for arithmetic.
#
# $(( 08 )) IS AN ERROR IN EVERY POSIX SHELL — "value too great for base" — and
# this file already learnt that once, from a zero-padded interval in a
# hand-edited dash.conf. `date +%s` does not produce a leading zero on any real
# device, but every one of these values ends up inside $(( )), and a wait, a
# minute counter and a wake window are not places to find out.
epoch_now() {
    EPOCH=$(date +%s)
    strip_zeros "$EPOCH" >/dev/null
    EPOCH="$STRIPPED"
    return 0
}

conf_valid() {
    # $1=key $2=value → 0 if acceptable
    local k="$1" v="$2"
    case "$k" in
        POWER)
            # A word, not a number, and the only one — so it is tested before
            # the numeric arm below, which would refuse every value it has.
            case "$v" in
                awake|wifi|suspend) return 0 ;;
                *) return 1 ;;
            esac ;;
        CANVAS)
            # A word like POWER, and tested before the numeric arm for the
            # same reason.
            case "$v" in
                desktop|blank) return 0 ;;
                *) return 1 ;;
            esac ;;
        LAYOUT)
            # Three words, and `auto` is the one that does what the collector
            # says. The other two are for a reader who wants the page to stay
            # the shape they set it to whatever the network is doing.
            case "$v" in
                auto|normal|standalone) return 0 ;;
                *) return 1 ;;
            esac ;;
        ROTATE)
            case "$v" in
                auto|0|90|180|270) return 0 ;;
                *) return 1 ;;
            esac ;;
        CLOCK_SYNC)
            # Not through strip_zeros, being a text key for `auto`: so no
            # leading zero either, which $(( )) would read as octal.
            case "$v" in
                auto|0) return 0 ;;
                ''|0*|*[!0-9]*) return 1 ;;
            esac
            [ "${#v}" -le 2 ] && [ "$v" -le 60 ] ;;
        MENU_LBL|MENU_LBL2|SURE_LBL|MODE_LBL)
            # Labels separated by bars. They reach draw_text_reg_inv and
            # nothing else, so the shell metacharacters are what matter — the
            # bar itself is the separator and so is allowed.
            case "$v" in
                ''|*'"'*|*'`'*|*'$'*|*';'*|*'&'*|*'<'*|*'>'*) return 1 ;;
            esac
            return 0 ;;
        MENU_ACT)
            # Two to five of the actions the loop knows, and nothing else: this
            # key decides what a button on the bar DOES, so a word nobody
            # dispatches is a button that silently does nothing, and a word
            # carrying a metacharacter would be one that does something else
            # entirely. Checked here rather than at the tap, because a bar
            # drawn with a bad action is already a bar that lies.
            local n=0 rest="$v" one
            case "$v" in '') return 1 ;; esac
            while : ; do
                one="${rest%%|*}"
                case "$one" in
                    refresh|wake|forecast|settings|hide|quit) ;;
                    *) return 1 ;;
                esac
                n=$((n + 1))
                case "$rest" in *'|'*) rest="${rest#*|}" ;; *) break ;; esac
            done
            [ "$n" -ge 2 ] && [ "$n" -le 5 ] ;;
        TOUCH_DEV)
            # A device path, or empty to let touch_find() pick one.
            [ -z "$v" ] && return 0
            case "$v" in
                /dev/input/event[0-9]|/dev/input/event[0-9][0-9]) return 0 ;;
                *) return 1 ;;
            esac ;;
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
                TRACE|GUI_STOP|TOUCH|TOUCH_SWAP|ROTATE_SWAP) [ "$v" -le 1 ] ;;
                WAKE_MENU|STATUS|AUTO_FIND) [ "$v" -le 1 ] ;;
                # An hour of the day, and the two being equal is how the quiet
                # hours are turned off — so 0 is a value, not a refusal.
                QUIET_FROM|QUIET_TO) [ "$v" -le 23 ] ;;
                # A tier like the others, but 0 means "no different from the
                # rest of the day" rather than "never".
                QUIET_EVERY) [ "$v" -le 1440 ] ;;
                # Long enough to press a button and read the bar, short enough
                # that a panel left alone goes back down the same hour.
                WAKE_HOLD) [ "$v" -ge 10 ] && [ "$v" -le 900 ] ;;
                # A touch panel's full scale, or 0 for "it already reports
                # screen pixels". No upper tier applies.
                TOUCH_MAXX|TOUCH_MAXY) [ "$v" -le 65535 ] ;;
                # Seconds, and a wait longer than the tick it sits inside is a
                # panel that never draws.
                WIFI_WAIT) [ "$v" -le 45 ] ;;
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
        if conf_is_text "$k"; then :; else
            v=$(strip_zeros "$v"); eval "$k=\$v"
        fi
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
    # ONCE, THEN WRITTEN DOWN. Applied on every load, the move below made the
    # old four-button bar impossible to choose again: a reader who took the
    # Forecast button off got it back a minute later. So it runs while CONF_VER
    # says it has not, and the new number goes into dash.conf with it.
    if [ "${CONF_VER:-1}" -lt 2 ]; then
        # The shipped bar before `forecast`, with the shipped words or none, is
        # a bar nobody chose: the reader gets the button the new one ships
        # with. Own labels over the old four buttons mean somebody sat down and
        # named them, and both keys are left exactly as they are.
        if [ "$MENU_ACT" = "$MENU_ACT_OLD" ]; then
            case "${MENU_LBL:-}" in
                ""|"$MENU_LBL_OLD"|"$MENU_LBL_STD")
                    MENU_ACT="refresh|wake|forecast|settings|quit" ;;
            esac
        fi
    fi
    if [ "${CONF_VER:-1}" -lt "$CONF_VER_NOW" ]; then
        CONF_VER=$CONF_VER_NOW
        conf_write 2>/dev/null || echo "dash.conf: could not record CONF_VER" >&2
    fi
    return 0
}

# Is the collector address still the one the package shipped?
#
# Read out of dash.conf.default rather than written down here: the value nobody
# executes is the one that goes stale, and a first-run test comparing against
# an address the package no longer ships is a test that never fires.
host_is_default() {
    local d
    d=$(sed -n 's/^HOST=//p' "$CONF_DEFAULT" 2>/dev/null | head -1)
    [ -n "$d" ] || d="192.168.1.50"
    [ "${HOST:-}" = "$d" ]
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

# ── Power ────────────────────────────────────────────────────────────────────
#
# WHAT A DASHBOARD COSTS A TEN-YEAR-OLD KINDLE. The i.MX6SL never reaches a
# hardware suspend while this script is looping, and the radio stays associated
# whether or not anything is being fetched:
#
#   CPU awake in a shell loop      ~25-35 mA
#   WiFi associated, idle          ~30-50 mA   (beacons, DTIM, the radio itself)
#
# — 60-80 mA against a cell that left the factory at 890-1420 mAh and, ten
# years on, is likely 600-900. That is a day and a half. The panel is on a wall
# and the cable is not.
#
# Three settings, each a superset of the one before, because each is a
# different amount of trust in a ten-year-old device to wake up again:
#
#   POWER=awake     what this always did. Nothing is touched.
#   POWER=wifi      the radio is off except around a fetch. The CPU stays up.
#                   Roughly halves the draw; no way for it to fail beyond a
#                   fetch that finds no network, which the script already
#                   handles by keeping the last reading on screen.
#   POWER=suspend   the above, and the wait between ticks is a real suspend to
#                   RAM with an RTC alarm to come back. Under a milliamp while
#                   it is down, which is where the days turn into weeks — and
#                   the one that can leave a panel dark if the alarm does not
#                   take, which is why suspend_for() refuses to go down
#                   without reading the alarm back first.
#
# The tiers already say which minutes need the network: the clock is drawn
# from the reader's own clock and needs nothing.

# The nodes are variables so the tests can point them at a temp file. A test
# that writes the real /sys/power/state suspends the machine running it.
#
# AND WHICH rtc IS NOT A GUESS EITHER. This file said rtc0 and meant it. A
# reader with more than one RTC — the SoC's and the power chip's — does not
# promise that the first one is the one whose alarm the kernel will honour, and
# on the reader where it is not, suspend_for() correctly refuses to go down
# (the alarm does not read back) and the deep sleep simply never happens, with
# nothing anywhere saying why. rtc_pick() tries them and says which it took.
#
# RTC_PINNED is how a value that came from the environment is left alone:
# tests point these at a temp file, and a probe that overwrote them would
# suspend the machine running them.
RTC_PINNED=0
[ -n "${RTC_WAKEALARM:-}" ] && RTC_PINNED=1
RTC_WAKEALARM="${RTC_WAKEALARM:-/sys/class/rtc/rtc0/wakealarm}"
RTC_SINCE_EPOCH="${RTC_SINCE_EPOCH:-/sys/class/rtc/rtc0/since_epoch}"
PM_STATE="${PM_STATE:-/sys/power/state}"

# The clock the alarm is measured in — the RTC's own, not the system's. See
# suspend_for() for why the difference is the whole thing.
rtc_now() {
    RTC_NOW=$(cat "$RTC_SINCE_EPOCH" 2>/dev/null)
    case "$RTC_NOW" in
        ''|*[!0-9]*) RTC_NOW=$(date +%s) ;;
    esac
}

#: Asked once per run, the first time a suspend is actually about to be tried.
#: NOT AT STARTUP: POWER is one of the keys conf_load() re-reads every minute,
#: so a panel that starts awake and is put into deep sleep from the menu an
#: hour later would never have asked — and would then fall back to rtc0, which
#: is the guess this exists to stop making.
RTC_PICKED=0

rtc_pick() {
    RTC_PICKED=1
    [ "${RTC_PINNED:-0}" = "1" ] && return 0
    local n node want back prev chosen="" w
    for n in 0 1 2 3; do
        node="/sys/class/rtc/rtc$n/wakealarm"
        [ -w "$node" ] || continue
        RTC_WAKEALARM="$node"
        RTC_SINCE_EPOCH="/sys/class/rtc/rtc$n/since_epoch"
        rtc_now
        want=$(( RTC_NOW + 120 ))
        # WHATEVER WAS ALREADY ON THE NODE GOES BACK ON IT. This probe is not
        # only run on the way into a suspend: Settings -> Info asks it too, to
        # put the answer on the screen. An alarm somebody else armed — powerd's
        # own, or the framework's — is not this function's to throw away, and
        # without this, looking at a status page cancelled it.
        prev=$(cat "$node" 2>/dev/null)
        case "$prev" in ''|*[!0-9]*) prev=0 ;; esac
        # Cleared first, then written, then READ BACK — the same three steps
        # suspend_for() takes, because the question here is exactly the one it
        # asks: will this node hold an alarm. Cleared again afterwards: this is
        # a probe, not a request to wake up in two minutes.
        #
        # ONE WAY OUT OF THE LOOP BODY, so the restore below cannot be skipped
        # by a write that failed halfway through.
        echo 0 > "$node" 2>/dev/null || continue
        if echo "$want" > "$node" 2>/dev/null; then
            back=$(cat "$node" 2>/dev/null)
        else
            back=""
        fi
        echo 0 > "$node" 2>/dev/null
        [ "$prev" = "0" ] || echo "$prev" > "$node" 2>/dev/null
        if [ "$back" = "$want" ]; then chosen="$n"; break; fi
    done
    if [ -z "$chosen" ]; then
        RTC_WAKEALARM="/sys/class/rtc/rtc0/wakealarm"
        RTC_SINCE_EPOCH="/sys/class/rtc/rtc0/since_epoch"
        echo "RTC: no wake alarm on rtc0..rtc3; POWER=suspend will sleep" \
             "without one, which is an ordinary sleep." >&2
        return 1
    fi
    # AND IT HAS TO BE ALLOWED TO WAKE THE MACHINE. An alarm that sticks on a
    # device whose wakeup source is disabled is an alarm that fires into a
    # suspended kernel and is ignored. Written where the node exists, ignored
    # where it does not — plenty of drivers enable it themselves.
    for w in "/sys/class/rtc/rtc$chosen/device/power/wakeup" \
             "/sys/class/rtc/rtc$chosen/power/wakeup"; do
        [ -w "$w" ] && echo enabled > "$w" 2>/dev/null
    done
    echo "RTC: using rtc$chosen for the wake alarm" >&2
    return 0
}

# WHETHER WE ARE THE ONES HOLDING THE RADIO DOWN, which is not the same
# question as what POWER is set to. POWER is re-read every minute and can
# change under us; this latch is what every restore path keys on instead, so a
# radio this script turned off is a radio this script turns back on whatever
# the setting says by then.
RADIO_OFF=0

# AND A MARKER ON DISK BESIDE IT, because the shell variable dies with the
# shell. cleanup() restores both the radio and the framework on SIGTERM, but a
# dashboard that is wedged, OOM-killed or killed by hand never reaches it —
# and stop.sh sends SIGKILL ten seconds after SIGTERM to exactly those. Both
# things left behind are device-wide and outlive the process: a radio turned
# off stays off, a stopped reader framework stays stopped. The markers are what
# lets stop.sh put back precisely what was taken, rather than guessing or
# resetting a radio nobody touched.
RADIO_MARK="$TMP/radio-off"
GUI_MARK="$TMP/gui-stopped"

radio_set() {
    lipc-set-prop com.lab126.cmd wirelessEnable "$1" >/dev/null 2>&1
}

net_up() {
    [ "${POWER:-awake}" = "awake" ] && return 0
    if ! radio_set 1; then
        # THE FRAMEWORK IS WHAT SERVES THE RADIO on firmware where
        # com.lab126.cmd shares an upstart job with the reader — which is
        # exactly what GUI_STOP stops. The setting that saves ten milliamps
        # would then be the reason nothing fetches again for the rest of the
        # run, with nothing on the panel to say why. So it is put back and the
        # radio asked again: a dashboard that updates is worth more than the
        # ten milliamps, and this is the only place that can tell the two
        # firmwares apart.
        if [ "${GUI_STOPPED:-0}" != "0" ]; then
            gui_restore
            GUI_BLOCKED=1
            echo "GUI_STOP: this firmware serves the radio from the framework;" \
                 "put back so the dashboard can still fetch." >&2
            radio_set 1
        fi
    fi
    RADIO_OFF=0
    rm -f "$RADIO_MARK" 2>/dev/null
    # ASSOCIATION IS NOT INSTANT. The chip and the DHCP client want four to ten
    # seconds, and a fetch fired before that fails against a network that is
    # about to be there — which on the panel is a minute of "offline" for no
    # reason. Waited for, not slept through: a reader that associates in three
    # seconds should not pay for the one that takes nine.
    local waited=0
    while [ "$waited" -lt "${WIFI_WAIT:-15}" ]; do
        # EXACTLY CONNECTED. A `*CONNECTED*` glob also matches DISCONNECTED and
        # NOT_CONNECTED — wifid saying the opposite — so the bounded wait this
        # function exists for was skipped on the one answer it was written to
        # wait through, and the fetch fired into an interface that was still
        # coming up.
        case "$(lipc-get-prop com.lab126.wifid cmState 2>/dev/null)" in
            CONNECTED) return 0 ;;
        esac
        nap 1
        waited=$((waited + 1))
    done
    # Out of patience. The fetch is still attempted: cmState is a property of a
    # daemon, and a wrong answer from it is not a reason to skip a request that
    # might work.
    return 1
}

net_down() {
    [ "${POWER:-awake}" = "awake" ] && return 0
    radio_set 0
    RADIO_OFF=1
    : > "$RADIO_MARK" 2>/dev/null
    return 0
}

# Going back to awake has to put the radio on.
#
# POWER is one of the settings conf_load() re-reads every minute, and awake is
# the value that makes net_up(), net_down() and cleanup() all return without
# touching anything. So switching back to it — the documented way to undo a
# battery setting you did not like — left the reader with the radio off and no
# path in this script that would ever turn it on again, Stop included. The
# latch is what makes the difference visible: it says we turned it off, and
# that is still true after the setting says not to.
power_apply() {
    [ "${POWER:-awake}" = "awake" ] || return 0
    [ "${RADIO_OFF:-0}" = "1" ] || return 0
    radio_set 1
    RADIO_OFF=0
    rm -f "$RADIO_MARK" 2>/dev/null
}

# ── The Amazon framework ─────────────────────────────────────────────────────
#
# A Kindle running as a panel is still running the reader: the Java VM (cvm),
# the indexer, the search service, the touch UI. None of it is looked at and it
# costs another 10-15 mA of the 25-35 the CPU draws at idle.
#
# STOPPED, NOT KILLED. A killed framework needs a reboot; a stopped one comes
# back with `start`, and a SIGSTOPped one with SIGCONT.
#
# AND THERE IS NO KUAL WHILE IT IS STOPPED, which is the thing to know before
# turning this on. KUAL is a Kindlet, hosted by the framework this switches
# off, so "press Stop in KUAL" — the way back from every other setting here —
# is not available for this one. The ways back are:
#
#   * set GUI_STOP=0 in dash.conf over USB. gui_apply() picks it up within a
#     minute, exactly like every other key;
#   * run stop.sh, which restores the framework itself rather than relying on
#     this script's exit trap;
#   * hold the power button until the reader reboots, which clears both a
#     stopped job and a SIGSTOPped VM.
#
# Off by default, and the only setting here that makes the device stop being a
# reader while it is on.
gui_stop() {
    [ "${GUI_STOP:-0}" = "1" ] || return 0
    GUI_STOPPED=0
    if stop lab126_gui >/dev/null 2>&1; then
        GUI_STOPPED=1
    elif killall -STOP cvm 2>/dev/null; then
        GUI_STOPPED=2
    fi
    [ "$GUI_STOPPED" = "0" ] || echo "$GUI_STOPPED" > "$GUI_MARK" 2>/dev/null
    return 0
}

gui_restore() {
    case "${GUI_STOPPED:-0}" in
        1) start lab126_gui >/dev/null 2>&1 ;;
        2) killall -CONT cvm 2>/dev/null ;;
    esac
    GUI_STOPPED=0
    rm -f "$GUI_MARK" 2>/dev/null
    return 0
}

# Applied every minute, because it is read every minute.
#
# conf_load() re-reads every key in conf_keys() each tick so that "a change
# made from KUAL takes effect within a minute" — the contract conf_write()
# prints to the reader. GUI_STOP was in that list but acted on once, before the
# loop: turning it on from Settings did nothing at all, and turning it back off
# left the framework down until Stop. The one key that quietly did not honour
# the promise the rest of them make.
gui_apply() {
    local want="${GUI_STOP:-0}"
    # Blocked only where it actually conflicts. GUI_BLOCKED is set by net_up()
    # on the firmware where stopping the framework takes the radio with it; on
    # POWER=awake nothing is asking the radio for anything, so the saving is
    # still there to be had.
    if [ "${GUI_BLOCKED:-0}" = "1" ] && [ "${POWER:-awake}" != "awake" ]; then
        want=0
    fi
    if [ "$want" = "1" ]; then
        [ "${GUI_STOPPED:-0}" = "0" ] && gui_stop
    else
        [ "${GUI_STOPPED:-0}" = "0" ] || gui_restore
    fi
    return 0
}

# Does this minute's work need the network at all?
needs_net() {
    case " $1 " in
        *" full "*|*" sensors "*|*" forecast "*|*" chart "*) return 0 ;;
    esac
    return 1
}

# Suspend to RAM for $1 seconds. Non-zero if it did not happen, and the caller
# falls back to an ordinary sleep.
#
# THE ALARM IS READ BACK BEFORE THE MACHINE GOES DOWN. Everything else here is
# recoverable — a failed fetch keeps the last reading, a failed draw comes back
# next minute — but a suspend with no alarm behind it is a panel that stays
# dark until somebody presses the power button. It is the one place in this
# script that can end the dashboard rather than degrade it.
#: 1 when the last suspend came back before its alarm was due — which means
#: something other than the alarm woke the machine, and on a Kindle that is a
#: person: the power button, or a cable. THE ONE SIGNAL THIS SCRIPT HAS THAT
#: somebody is standing in front of the panel while the CPU is down. No device
#: has to be identified for it: the alarm says when we meant to come back, and
#: the RTC says when we did.
SUSPEND_EARLY=0
#: Said once, not once a minute — see the note where it is set.
SUSPEND_WARNED=0

# The alarm goes back off wherever the sleep did not happen, or happened and
# was cut short by the button. A pending alarm on a machine that is staying
# awake is a wake that powerd's own sleep will take later, for nothing.
rtc_disarm() {
    [ -w "$RTC_WAKEALARM" ] && echo 0 > "$RTC_WAKEALARM" 2>/dev/null
    return 0
}

suspend_for() {
    local want="$1" alarm back down rdown
    SUSPEND_EARLY=0
    # Not worth the transition, and short values are where a rounding error
    # turns into an alarm in the past.
    [ "$want" -ge "${SUSPEND_MIN:-5}" ] 2>/dev/null || return 1
    [ -w "$RTC_WAKEALARM" ] && [ -w "$PM_STATE" ] || return 1

    # THE RTC'S OWN CLOCK, NOT THE SYSTEM'S. The kernel compares this node
    # against the RTC; `date +%s` reads the system clock. The two agree only
    # while the RTC runs in UTC, and on a reader whose does not, an absolute
    # alarm lands hours away — a panel dark until it comes round, or one waking
    # on every tick. The read-back below cannot tell: the digits stick either
    # way, so it would confirm a suspend that never comes back. since_epoch is
    # the same clock the alarm is measured in, and is what makes the sum mean
    # what it says.
    rtc_now
    alarm=$((RTC_NOW + want))
    # Cleared first: writing an alarm over a pending one is rejected by the
    # driver rather than replacing it, so the second write would be the one
    # that silently did nothing.
    echo 0 > "$RTC_WAKEALARM" 2>/dev/null || return 1
    echo "$alarm" > "$RTC_WAKEALARM" 2>/dev/null || return 1
    back=$(cat "$RTC_WAKEALARM" 2>/dev/null)
    [ "$back" = "$alarm" ] || return 1

    # The system clock either side of the write, which is the only way to tell
    # a machine that went down from one that did not — see below.
    epoch_now
    local went="$EPOCH"
    echo mem > "$PM_STATE" 2>/dev/null || { rtc_disarm; return 1; }
    epoch_now
    rtc_now

    # ── DID IT ACTUALLY GO DOWN? ────────────────────────────────────────────
    #
    # A write to /sys/power/state that RETURNS WITHOUT SUSPENDING is the one
    # failure this function could turn into something worse than the bug it
    # exists for. The early-wake path below answers a resume by repainting the
    # whole page and putting the bar up; a write that comes straight back would
    # do that on every pass through the main loop — a flashing panel and a
    # battery emptied in an afternoon, which is a great deal worse than a panel
    # that merely never sleeps.
    #
    # BOTH CLOCKS, AND WHICHEVER OF THEM RAN. The note below this block says
    # the system clock is not guaranteed to have been running across a suspend
    # — and asking it alone, as this did, calls a perfectly good fifteen-minute
    # sleep on such a reader "no time passed" and turns POWER=suspend into an
    # ordinary sleep for the rest of the run, with a message saying the reader
    # cannot do it. The RTC is the clock the alarm is measured in and it kept
    # running by definition; the system clock is the one that survives an RTC
    # that will not read back. Either saying time passed is time passed.
    down=$(( EPOCH - went ))
    rdown=$(( RTC_NOW - (alarm - want) ))
    [ "$rdown" -gt "$down" ] 2>/dev/null && down="$rdown"

    # No time passed on either, so the machine did not go down: say so, and let
    # the caller sleep the ordinary way. A person pressing the button within two
    # seconds of it going down loses their press and presses again; a device
    # that cannot suspend loses nothing at all.
    if [ "$down" -lt "${SUSPEND_MIN_DOWN:-2}" ]; then
        # ONCE, not once a minute. /tmp is a ramdisk on a device that runs for
        # months between reboots, and a line a minute is the same mistake
        # CONF_WARNED exists to stop: 1440 copies of one sentence, in RAM, to
        # say something that was true the first time.
        if [ "${SUSPEND_WARNED:-0}" = "0" ]; then
            SUSPEND_WARNED=1
            echo "suspend: /sys/power/state came straight back; sleeping" \
                 "instead. POWER=suspend will not save anything on this" \
                 "reader — see Settings → Info for the RTC it found." >&2
        fi
        rtc_disarm
        return 1
    fi

    # ── Past here the machine has been down and is back ─────────────────────
    # WHY it came back is measured against the alarm, not against the clock
    # above: the system clock is not guaranteed to have been running across a
    # suspend, and the alarm is the only number that says what we asked for. A
    # few seconds of slack, because an alarm and a resume do not land on the
    # same second.
    [ $(( alarm - RTC_NOW )) -ge "${SUSPEND_SLACK:-3}" ] && SUSPEND_EARLY=1
    # An early wake leaves OUR alarm still pending, and the window that follows
    # it is minutes long: cleared here so it cannot fire into the framework's
    # sleep afterwards. The next suspend arms its own.
    [ "$SUSPEND_EARLY" = "1" ] && rtc_disarm
    return 0
}

# ── Somebody pressed the button ──────────────────────────────────────────────
#
# THE PANEL HAS TO ANSWER, or the button is indistinguishable from a dead one.
# Until this, a press woke the kernel, `echo mem` returned, and the loop ran an
# ordinary tick: four minutes in five nothing was due, so nothing was drawn and
# the reader went straight back down. The press worked perfectly and looked
# like nothing at all.
#
# So an early wake buys a window: the page is repainted (whatever the framework
# put on the screen while we were away goes with it), the touchscreen is read
# for as long as somebody keeps using it, and the bar is put up so there is
# something to press. When the window runs out the panel goes back down by
# itself — nothing has to be remembered or undone.
#
# AND THE TOUCHSCREEN IS ARMED HERE EVEN WHEN TOUCH=0. That is the combination
# worth having on a wall: no process reading the panel at all while nobody is
# there, and a menu the moment the button is pressed.
AWAKE_UNTIL=0

wake_window() {
    # 0 while we are deliberately staying awake for somebody.
    [ "${AWAKE_UNTIL:-0}" -gt 0 ] 2>/dev/null || return 1
    epoch_now
    [ "$EPOCH" -lt "$AWAKE_UNTIL" ] 2>/dev/null
}

wake_extend() {
    # EVERY TAP IS SOMEBODY BEING HERE, and it opens the window as readily as
    # it pushes one out: a reader working through the settings bar should not
    # have it close under them, and on a panel with TOUCH=1 a finger is the
    # same news as the button. The one thing that closes it early is asking for
    # the sleep — see power_set().
    epoch_now
    AWAKE_UNTIL=$(( EPOCH + ${WAKE_HOLD:-120} ))
    return 0
}

wake_interactive() {
    epoch_now
    AWAKE_UNTIL=$(( EPOCH + ${WAKE_HOLD:-120} ))
    # powerd re-arms its own screensaver across a suspend on some firmware, and
    # this is the one call that has to be made again rather than once at start.
    prevent_sleep
    touch_apply
    # THE PAGE FIRST AND THE BAR ON TOP OF IT, both here rather than left to
    # the tick: redraw_all clears the screen, so a bar drawn before it would be
    # painted over by the very repaint that makes the panel look answered.
    #
    # The whole page, because the framework may have put its screensaver on the
    # screen while the CPU was down and no tier would have cleared it for up to
    # an hour. Not flashing at night: somebody pressing a button in a dark room
    # asked for the page, not for the panel to go black first.
    clock_tier
    if quiet_now; then redraw_all "$(now_clock)" 0
    else               redraw_all "$(now_clock)" 1
    fi
    [ "${TOUCH_READY:-0}" = "1" ] && menu_open main
    return 0
}

# ── Time that went missing ───────────────────────────────────────────────────
#
# A WAIT THAT TOOK FAR LONGER THAN IT ASKED FOR means the reader was asleep —
# powerd's doing, not ours: a press of the power button in POWER=awake sends
# the device to sleep by the firmware's normal path, and it comes back with
# Amazon's screensaver on the screen. Nothing in this script would have
# repainted until a tier came round, up to an hour later, and from the sofa
# that is a dashboard that has died.
#
# Cheap enough to ask on every tick, and it covers every cause at once: a
# framework repaint, a powerd suspend, a clock that was stepped.
lost_time() {
    # $1=epoch the wait began  $2=seconds it asked for
    epoch_now
    [ $(( EPOCH - $1 - $2 )) -ge "${LOST_MIN:-25}" ] 2>/dev/null || return 1
    : > "$TMP/redraw"
    prevent_sleep
    return 0
}

# ── The reader's own screen, and ours ────────────────────────────────────────
#
# FBINK WRITES TO /dev/fb0. IT DOES NOT OWN THE SCREEN.
#
# The Amazon framework still does. It repaints its library whenever it decides
# to — a cover thumbnail finishing, the status bar ticking, a sync — and every
# touch goes to it, not to us. So a dashboard drawn over the home screen is a
# dashboard that keeps being wiped by the thing underneath, and a tap on it
# opens whatever book was under your finger. The panel "coming back after a
# minute or two" is the same fault seen from the other end: the framework
# painted over us and nothing redrew until the next tick came round.
#
# CANVAS=blank asks the framework to put its chrome away — the status bar and
# the toolbars, which are the parts that repaint most often. It is NOT a fix on
# its own: only GUI_STOP makes the screen actually ours. It is the half of the
# fix that costs nothing and keeps the reader a reader.
CANVAS_MARK="$TMP/canvas"

canvas_take() {
    lipc-set-prop com.lab126.pillow disableEnablePillow 1 2>/dev/null
    : > "$CANVAS_MARK" 2>/dev/null
    return 0
}

# Applied every minute, because it is read every minute — the same contract
# gui_apply() exists for, and the same bug without it: the KUAL entry that sets
# CANVAS would have done nothing at all until the next Start, while settings.sh
# printed "the dashboard picks this up within a minute".
canvas_apply() {
    if [ "${CANVAS:-desktop}" = "blank" ]; then
        [ -f "$CANVAS_MARK" ] || canvas_take
    else
        canvas_give_back
    fi
    return 0
}

canvas_give_back() {
    [ -f "$CANVAS_MARK" ] || return 0
    lipc-set-prop com.lab126.pillow disableEnablePillow 0 2>/dev/null
    rm -f "$CANVAS_MARK" 2>/dev/null
    return 0
}

# ── The tap menu ─────────────────────────────────────────────────────────────
#
# A BAR THAT IS NOT THERE UNTIL YOU ASK FOR IT.
#
# The dashboard is a picture with no controls on it, which is right for
# something read from across a room and wrong the moment somebody is standing
# in front of it wanting it refreshed. A reader who taps the screen is asking
# the panel a question, and until this the question went through to whatever
# the framework had underneath.
#
# Tap once and the bar appears along the bottom, ruled into as many buttons as
# MENU_ACT names. Tap one and it runs. Tap anywhere above the bar and it goes
# away again.
#
# EXIT IS ON IT ON PURPOSE. It runs the same cleanup Stop does, which is the
# way back GUI_STOP otherwise takes away with the launcher it stops. IT ASKS
# FIRST, though: it is one tap away from ending the dashboard, on a panel whose
# touch calibration is the thing most likely to be wrong, so the first tap
# turns the whole bar into the confirmation and the second one is the one that
# acts. Anything else cancels it.
#
# `More` opens a second bar — find the collector, step to the next one, cycle
# the battery setting, show what the device thinks is going on. That bar is the
# reason GUI_STOP is usable at all: with the framework stopped there is no KUAL
# to change a setting from, and this is the whole of the settings menu redrawn
# where a finger can reach it.
#
# WHAT IT COSTS: one background process blocked in read(2), and the wait
# between ticks becomes a read with a timeout so a tap is acted on at once
# rather than at the top of the next minute.
TOUCH_FIFO="$TMP/touch"
TOUCH_READY=0
TOUCH_PID=""
#: 0 closed, 1 the main bar, 2 the settings bar, 3 the Exit confirmation.
MENU=0
MENU_ACTS=""
MENU_LBLS=""

# ── Bar-separated lists, without forking ─────────────────────────────────────
# Both are called once per button per repaint, so they set a variable rather
# than echoing into $( ) — see centre_in() for the arithmetic this file counts
# forks over.
list_len() {
    # $1=list -> LIST_N
    local rest="$1"
    LIST_N=0
    [ -n "$rest" ] || return 0
    while : ; do
        LIST_N=$((LIST_N + 1))
        case "$rest" in *'|'*) rest="${rest#*|}" ;; *) break ;; esac
    done
    return 0
}

list_at() {
    # $1=list $2=index, from 0 -> LIST_ITEM ("" past the end)
    local rest="$1" i=0
    LIST_ITEM=""
    while [ "$i" -lt "$2" ]; do
        case "$rest" in *'|'*) rest="${rest#*|}" ;; *) return 0 ;; esac
        i=$((i + 1))
    done
    LIST_ITEM="${rest%%|*}"
    return 0
}

# ── How wide a label is, near enough to fit it ───────────────────────────────
#
# CHARACTERS, NOT BYTES. ${#var} counts bytes — "Обнови" is twelve of them for
# six letters — which is why the labels on this bar were left-aligned in their
# thirds rather than centred. But a bar with five buttons on it has to KNOW,
# or five labels run into one another, and FBInk will not say how wide it drew
# a string.
#
# Every UTF-8 character has exactly one lead byte and its continuations are
# 0x80..0xBF, so dropping the continuations and counting what is left counts
# characters in any language. Not `wc -m`, which would count them directly:
# busybox's -m depends on a locale a Kindle does not set.
str_chars() {
    # ${#} ON WHAT IS LEFT AFTER THE CONTINUATION BYTES GO, rather than piping
    # the count out through wc: this is called once per button per repaint on a
    # ten-year-old ARM reader, and the shell can count the bytes of a string it
    # already holds. The tr is still a fork, and still the right one — the
    # shell's own length is bytes under the C locale this runs in, which is
    # exactly the miscount being corrected.
    local t
    t=$(printf '%s' "$1" | tr -d '\200-\277')
    STR_N=${#t}
    return 0
}

# Which /dev/input device is the touchscreen.
#
# WITHOUT evtest, WHICH THE KINDLE DOES NOT HAVE. The touchscreen is the input
# device that reports ABSOLUTE positions; the power button and the cover magnet
# report keys and nothing else, so a non-zero `abs` capability mask is what
# tells them apart. TOUCH_DEV overrides it for a reader where that guess is
# wrong.
touch_find() {
    local d n abs
    if [ -n "${TOUCH_DEV:-}" ]; then
        [ -r "$TOUCH_DEV" ] && { echo "$TOUCH_DEV"; return 0; }
        return 1
    fi
    for d in /dev/input/event*; do
        [ -r "$d" ] || continue
        n=${d##*/event}
        abs=$(cat "/sys/class/input/event$n/device/capabilities/abs" 2>/dev/null)
        # All-zero once the spaces and zeros are gone means it reports no axes.
        case "$(printf '%s' "$abs" | tr -d ' 0')" in
            '') continue ;;
        esac
        echo "$d"
        return 0
    done
    return 1
}

# One line of "x y" per touch, on stdout.
#
# ONE dd PER EVENT, NOT od ACROSS THE STREAM. od block-buffers when its stdout
# is a pipe: a tap produced nothing at all until four kilobytes of its output
# had piled up — about a hundred events — and then arrived as a burst. Measured,
# not guessed; the first version of this shipped that way and the menu would
# never have opened. The device is silent until a finger lands, so a fork per
# event is a fork per touch and nothing at all while nobody is touching.
#
# An input event is sixteen bytes — two 32-bit timestamps, a 16-bit type, a
# 16-bit code, a 32-bit value — so one `dd bs=16 count=1` is exactly one event
# and `od -tu2` prints it as eight numbers. The loop reads the device, not each
# dd, so the descriptor stays open across events.
#
# ONE LINE PER CONTACT. A finger produces a stream of positions; what ends a
# contact is the finger leaving, which panels say in one of two ways — BTN_TOUCH
# going to zero, or a frame carrying no coordinates at all. Both are honoured,
# because which one a reader speaks is the reader's business. A frame counter
# was the first answer and the wrong one: a real tap is three to thirty frames,
# so a budget of forty swallowed the NEXT tap — the one that presses the button
# the first tap opened.
touch_reader() {
    local rec x= y= seen=0 emitted=0
    while :; do
        rec=$(dd bs=16 count=1 2>/dev/null | od -An -tu2 -v)
        # shellcheck disable=SC2086
        set -- $rec
        [ "$#" -lt 8 ] && break                 # short read: the device is gone
        case "$5" in
            3)  case "$6" in
                    0|53) x=$7; seen=1 ;;
                    1|54) y=$7; seen=1 ;;
                esac ;;
            1)  # BTN_TOUCH. Zero is the finger leaving.
                [ "$6" = 330 ] && [ "$7" = 0 ] && emitted=0 ;;
            0)  # SYN_REPORT ONLY. SYN_MT_REPORT (2) separates the contacts
                # inside one frame and SYN_DROPPED (3) says the kernel's queue
                # overflowed and the state is not to be trusted; neither of them
                # ends a frame.
                [ "$6" = 0 ] || continue
                if [ "$seen" = 0 ]; then
                    emitted=0                   # an empty frame: the finger left
                elif [ -n "$x" ] && [ -n "$y" ] && [ "$emitted" = 0 ]; then
                    printf '%s %s\n' "$x" "$y"
                    emitted=1
                fi
                seen=0 ;;
        esac
    done < "$1"
}

touch_arm() {
    TOUCH_READY=0
    # THE WAKE WINDOW ARMS IT WHATEVER TOUCH SAYS. TOUCH=0 with WAKE_MENU=1 is
    # the setting worth having on a wall: nothing reads the panel while nobody
    # is there, and the button summons a menu when somebody is.
    [ "${TOUCH:-0}" = "1" ] || wake_window || return 0
    command -v od >/dev/null 2>&1 || {
        echo "TOUCH: no od on this reader; the tap menu needs one." >&2
        return 0
    }
    local dev
    dev=$(touch_find) || {
        echo "TOUCH: no touchscreen among /dev/input/event*; set TOUCH_DEV." >&2
        return 0
    }
    rm -f "$TOUCH_FIFO" 2>/dev/null
    mkfifo "$TOUCH_FIFO" 2>/dev/null || return 0
    # OPENED FOR BOTH, so the reader end never sees EOF when a writer closes
    # and the writer never blocks waiting for one to appear.
    exec 9<> "$TOUCH_FIFO" 2>/dev/null || return 0
    touch_reader "$dev" > "$TOUCH_FIFO" &
    TOUCH_PID=$!
    TOUCH_READY=1
    [ "${TRACE:-0}" = "1" ] && echo "TOUCH: reading $dev" >&2
    return 0
}

# And so is the menu. Arming it is not free — a background process and a fifo —
# so it is armed and disarmed to match the setting rather than at startup only.
touch_apply() {
    if [ "${TOUCH:-0}" = "1" ] || wake_window; then
        [ "${TOUCH_READY:-0}" = "1" ] || touch_arm
    else
        # And disarmed again the moment the window closes, which is what keeps
        # TOUCH=0 meaning what it says for the rest of the day.
        [ "${TOUCH_READY:-0}" = "1" ] && touch_disarm
    fi
    return 0
}

touch_disarm() {
    if [ -n "$TOUCH_PID" ]; then
        # THE CHILDREN TOO. The reader forks a dd per event and one of them is
        # blocked on the touchscreen right now; killing only the shell around
        # it leaves that dd holding the device open, one per Start/Stop cycle.
        # pgrep is not on every reader, so the fifo going away is the backstop:
        # a reader that survives this finds nothing to write to.
        for _p in $(pgrep -P "$TOUCH_PID" 2>/dev/null); do
            kill "$_p" 2>/dev/null
        done
        kill "$TOUCH_PID" 2>/dev/null
    fi
    TOUCH_PID=""
    TOUCH_READY=0
    rm -f "$TOUCH_FIFO" 2>/dev/null
    return 0
}

# The panel's coordinates are not always the screen's.
#
# Several Kindles report screen pixels and need nothing here. Where a reader
# does not, TOUCH_MAXX and TOUCH_MAXY say what its full scale is and this maps
# it; TOUCH_SWAP is for a panel mounted the other way round. TRACE=1 prints
# every tap it decoded, raw and mapped, which is the one-glance way to find
# those numbers for a reader that differs.
touch_scale() {
    local rx="$1" ry="$2" mx="${TOUCH_MAXX:-0}" my="${TOUCH_MAXY:-0}" t
    # THE MAXIMA TRAVEL WITH THEIR AXES. TOUCH_MAXX names the range of the
    # value the panel reports FIRST — which is what a reader reads off the
    # TRACE line — so on a swapped panel it has to move to the other side with
    # it. Dividing a swapped value by the other axis's maximum is how a
    # calibrated panel still lands on the wrong third.
    if [ "${TOUCH_SWAP:-0}" = "1" ]; then
        t="$rx"; rx="$ry"; ry="$t"
        t="$mx"; mx="$my"; my="$t"
    fi
    # The panel's own axes are the UPRIGHT screen's whichever way up the page
    # is drawn: the touch layer does not turn with the framebuffer. So scale
    # to the upright size, then turn the point with the page.
    local w0="${RES_W:-600}" h0="${RES_H:-800}" nx ny
    if [ "$w0" -gt "$h0" ] 2>/dev/null; then t="$w0"; w0="$h0"; h0="$t"; fi
    nx="$rx"; ny="$ry"
    [ "$mx" -gt 0 ] 2>/dev/null && nx=$(( rx * w0 / mx ))
    [ "$my" -gt 0 ] 2>/dev/null && ny=$(( ry * h0 / my ))
    case "${ROT_DEG:-0}" in
        90)  TAP_X="$ny";                TAP_Y=$(( w0 - 1 - nx )) ;;
        180) TAP_X=$(( w0 - 1 - nx ));   TAP_Y=$(( h0 - 1 - ny )) ;;
        270) TAP_X=$(( h0 - 1 - ny ));   TAP_Y="$nx" ;;
        *)   TAP_X="$nx";                TAP_Y="$ny" ;;
    esac
    [ "${TRACE:-0}" = "1" ] && echo "TOUCH: raw $1,$2 -> $TAP_X,$TAP_Y (rot ${ROT_DEG:-0})" >&2
    return 0
}

menu_geom() {
    MENU_H=$(( ${RES_H:-800} / 9 ))
    MENU_Y=$(( ${RES_H:-800} - MENU_H ))
    MENU_W=${RES_W:-600}
    # SET, NOT DEFAULTED PAST. Everything that asks which button is where has
    # to get the same answer as everything that draws one.
    [ -n "${MENU_ACTS:-}" ] || MENU_ACTS="${MENU_ACT:-refresh|wake|forecast|settings|quit}"
    list_len "$MENU_ACTS"
    MENU_N=$LIST_N
    [ "$MENU_N" -ge 1 ] 2>/dev/null || MENU_N=1
    MENU_SLOT=$(( MENU_W / MENU_N ))
    [ "$MENU_SLOT" -ge 1 ] || MENU_SLOT=1
    return 0
}

# Which button is under a tap, or `outside` for the rest of the screen.
menu_hit() {
    menu_geom
    # OUTSIDE IS THE DEFAULT, AND EVERY WAY OUT LEADS TO IT. Quit was the
    # fall-through, so a coordinate that was out of range or not a number at
    # all — an uncalibrated panel reporting 2900,3100 on a 600x800 screen, which
    # is the exact case TOUCH_MAXX exists for — failed both thirds and exited
    # the dashboard. The most destructive button is the last one that should
    # win a default.
    local i
    MENU_HIT=outside
    case "$1" in ''|*[!0-9]*) return 0 ;; esac
    case "$2" in ''|*[!0-9]*) return 0 ;; esac
    [ "$2" -lt "$MENU_Y" ] && return 0
    [ "$2" -gt "${RES_H:-800}" ] && return 0
    [ "$1" -ge "$MENU_W" ] && return 0
    # The last slot keeps the remainder of an odd division, so the bar has no
    # dead strip down its right-hand edge that a finger can land in.
    i=$(( $1 / MENU_SLOT ))
    [ "$i" -ge "$MENU_N" ] && i=$(( MENU_N - 1 ))
    list_at "$MENU_ACTS" "$i"
    [ -n "$LIST_ITEM" ] || return 0
    MENU_HIT="$LIST_ITEM"
    return 0
}

# ── Which bar is up ──────────────────────────────────────────────────────────
# The actions are this file's own, fixed words; only the labels are the
# reader's. MENU_ACT decides how many buttons the main bar has, so it is also
# what says how many labels are needed.
menu_open() {
    case "${1:-main}" in
        main) MENU=1
              MENU_ACTS="${MENU_ACT:-refresh|wake|forecast|settings|quit}"
              menu_labels "${MENU_LBL:-}" "$MENU_LBL_STD" ;;
        more) MENU=2
              MENU_ACTS="find|next|power|diag|back"
              menu_labels "${MENU_LBL2:-}" "$MENU_LBL2_STD" ;;
        # ONE BUTTON, THE WHOLE WIDTH OF THE BAR. The confirmation is not a
        # small target next to four others — it is the bar, so the second tap
        # cannot miss it and nothing else on the bar can be hit by accident
        # while it is up.
        sure) MENU=3
              MENU_ACTS="sure"
              menu_labels "${SURE_LBL:-}" "$SURE_LBL_STD" ;;
        *)    MENU=1
              MENU_ACTS="${MENU_ACT:-refresh|wake|forecast|settings|quit}"
              menu_labels "${MENU_LBL:-}" "$MENU_LBL_STD" ;;
    esac
    draw_menu
    return 0
}

# The word for slot $1, with the sleep button's two-part label resolved.
#
# `Awake/Sleep` — IT IS A TOGGLE, SO IT SAYS WHICH WAY IT WILL GO rather than
# what it controls. A button reading "Awake" that sends the panel to sleep when
# the panel is already awake is a button lying about itself, and this bar is
# read by somebody standing in front of a screen that gives no other feedback.
#
# A label with no slash in it is used exactly as it is, which is every label
# anybody has already set.
menu_label() {
    list_at "$MENU_LBLS" "$1"
    MENU_LBL_I="$LIST_ITEM"
    case "$MENU_LBL_I" in
        */*)
            list_at "$MENU_ACTS" "$1"
            if [ "$LIST_ITEM" = "wake" ]; then
                if [ "${POWER:-awake}" = "suspend" ]; then
                    MENU_LBL_I="${MENU_LBL_I%%/*}"
                else
                    MENU_LBL_I="${MENU_LBL_I#*/}"
                fi
            fi ;;
    esac
    return 0
}

# The reader's own word for the button that runs $1, or "" if the bar has none.
#
# The offline page needs it: the route it used to name goes through KUAL, and
# KUAL is exactly what is missing on the reader most likely to be looking at
# that page — the one running with GUI_STOP=1. Naming the button instead means
# naming it in whatever language the reader set it in.
#
# Locals only. MENU_ACTS and MENU_LBLS belong to the bar that is up, and this
# is asked while one is not.
menu_word() {
    local acts lbls="" want i=0
    MENU_WORD=""
    acts="${MENU_ACT:-refresh|wake|forecast|settings|quit}"
    list_len "$acts"; want=$LIST_N
    list_len "${MENU_LBL:-}"
    # The same rule the bar is drawn under — see menu_labels(). A sentence
    # naming a button has to name the word that is ON it.
    if [ -n "${MENU_LBL:-}" ] && [ "$LIST_N" = "$want" ] &&
       [ "$MENU_LBL" != "$MENU_LBL_STD" ] && [ "$MENU_LBL" != "$MENU_LBL_OLD" ]; then
        lbls="$MENU_LBL"
    fi
    while [ "$i" -lt "$want" ]; do
        list_at "$acts" "$i"
        if [ "$LIST_ITEM" = "$1" ]; then
            menu_word_of "$LIST_ITEM"
            if [ -n "$lbls" ]; then
                list_at "$lbls" "$i"
                # AN EMPTY SLOT IS NOT AN ANSWER. `MENU_LBL=Refresh||More|Exit`
                # passes conf_valid and counts four, and returning "" from here
                # with a success was a caller's `|| MENU_WORD="Awake"` never
                # firing and a screen reading "tap  to stop sleeping".
                [ -n "$LIST_ITEM" ] && MENU_WORD_DEF="$LIST_ITEM"
            fi
            # The first half of a two-part label, which is the direction a
            # sentence naming the button means: "tap Awake to stop sleeping".
            MENU_WORD="${MENU_WORD_DEF%%/*}"
            [ -n "$MENU_WORD" ] || return 1
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

# ── The buttons that change a setting ────────────────────────────────────────
#
# THROUGH dash.conf, NOT THROUGH THE VARIABLE. conf_load() re-reads the file
# every minute and would put the old value straight back; and a reader who
# taps Awake and then opens KUAL should find the menu agreeing with the panel.
# One answer, in the file both ends read.
power_set() {
    # RE-READ BEFORE WRITING. conf_write() rewrites every key from the values
    # in this shell, and conf_load() runs once a minute — so a change somebody
    # made from KUAL in the last sixty seconds has not been read yet, and
    # writing without reading would put the old value back over it.
    conf_load
    POWER="$1"
    if [ "$1" = "suspend" ]; then
        AWAKE_UNTIL=0                    # asked for the sleep: go down at once
    else
        wake_extend                      # keep the bar alive to press again
    fi
    conf_write || return 1
    return 0
}

# The one button this whole wake mechanism exists for: stop sleeping, so the
# device can be told things.
#
# NOT ALL THE WAY TO awake. `wifi` keeps the radio off between fetches — most
# of the saving — and keeps the CPU up, which is what the menu needs in order
# to exist. A reader who wants the radio up as well has POWER=awake in the
# settings bar and in KUAL.
power_toggle() {
    if [ "${POWER:-awake}" = "suspend" ]; then
        power_set wifi
    else
        power_set suspend
    fi
    return 0
}

power_cycle() {
    case "${POWER:-awake}" in
        awake) power_set wifi ;;
        wifi)  power_set suspend ;;
        *)     power_set awake ;;
    esac
    return 0
}

# One line across the whole bar, in place of its buttons, for an action whose
# answer is a sentence rather than a redraw.
menu_note() {
    menu_geom
    fill_rect 0 "$MENU_Y" "$MENU_W" "$MENU_H" BLACK
    local sz=$(( MENU_H * 32 / 100 ))
    [ "$sz" -lt 12 ] && sz=12
    draw_text_reg_inv $(( MENU_W / 16 )) $(( MENU_Y + (MENU_H - sz) / 2 )) "$sz" "$1"
    refresh_zone 0 "$MENU_Y" "$MENU_W" "$MENU_H" 1
    return 0
}

# Ask the collector for a fresh forecast, then draw the page with it.
#
# THE COLLECTOR ONLY QUEUES IT: the fetch runs on its own task a moment later
# and takes up to a dozen seconds (two HTTPS requests for OWM, six seconds'
# timeout each), so the page is redrawn once the collector says the fetch is
# over rather than straight away — straight away would draw the forecast it
# already had. It is asked every DASH_FC_STEP seconds, with ?w=1, which only
# answers "pending" or "done", and never for longer than DASH_FC_MAX. Anything
# else, an older collector's answer or none, ends the wait too.
# The answers are one line, see handleKindleForecast() in KindleDashboard.cpp.
forecast_now() {
    local ans waited=0
    local step="${DASH_FC_STEP:-3}" max="${DASH_FC_MAX:-45}" inc
    # A step of 0 (the tests) still counts, or the cap would never come.
    inc=$step; [ "$inc" -gt 0 ] || inc=1
    net_up
    ans=$(wget -q -T "$FETCH_TIMEOUT" -O - "$(host_url)/kindle/forecast?t=1" 2>/dev/null)
    case "$ans" in
        ok)     menu_note "Updating the forecast..."
                while [ "$waited" -lt "$max" ]; do
                    sleep "$step"
                    waited=$((waited + inc))
                    ans=$(wget -q -T "$FETCH_TIMEOUT" -O - \
                          "$(host_url)/kindle/forecast?t=1&w=1" 2>/dev/null)
                    [ "$ans" = "pending" ] || break
                done ;;
        wait*)  menu_note "Updated a moment ago. Try again in ${ans#wait } s"
                sleep "${DASH_FC_NOTE:-3}" ;;
        off)    menu_note "The forecast is off on the collector"; sleep "${DASH_FC_NOTE:-3}" ;;
        offline) menu_note "The collector is offline: no forecast to fetch"
                sleep "${DASH_FC_NOTE:-3}" ;;
        *)      menu_note "No answer from the collector"; sleep "${DASH_FC_NOTE:-3}" ;;
    esac
    : > "$TMP/redraw"
    return 0
}

settings_run() {
    # The same script KUAL runs, as its own process: one implementation of
    # "find the collector on this network", not two, and its own screen of
    # feedback while it works. It rewrites dash.conf, so the values in this
    # shell are re-read rather than assumed.
    sh "$DASH_DIR/settings.sh" "$@"
    conf_load
    : > "$TMP/redraw"
    return 0
}

# The word this script would use for an action, so a bar can always be labelled
# from what its buttons DO. One table, in the file that dispatches the actions.
menu_word_of() {
    case "$1" in
        refresh)  MENU_WORD_DEF="Refresh" ;;
        forecast) MENU_WORD_DEF="Forecast" ;;
        wake)     MENU_WORD_DEF="Awake/Sleep" ;;
        settings) MENU_WORD_DEF="More" ;;
        hide)     MENU_WORD_DEF="Hide" ;;
        quit)     MENU_WORD_DEF="Exit" ;;
        find)     MENU_WORD_DEF="Find" ;;
        next)     MENU_WORD_DEF="Next" ;;
        power)    MENU_WORD_DEF="Battery" ;;
        diag)     MENU_WORD_DEF="Info" ;;
        back)     MENU_WORD_DEF="Back" ;;
        sure)     MENU_WORD_DEF="$SURE_LBL_STD" ;;
        # Not a word this file dispatches, so there is nothing truthful to put
        # over it. The action's own name is at least not a lie about another
        # button.
        *)        MENU_WORD_DEF="$1" ;;
    esac
    return 0
}

# The reader's labels when they are demonstrably theirs, words built from the
# actions otherwise.
#
# A BAR WHOSE WORDS ARE ONE PLACE ALONG FROM ITS BUTTONS is worse than one in a
# language the reader does not read: it does not merely fail to help, it says
# the wrong thing about what a tap will do. Every dash.conf written before
# MENU_ACT existed carries three labels, and the bar it would have drawn had
# "Hide" over the button that keeps the panel awake and "Exit" over the one
# that opens the settings.
menu_labels() {
    # $1=the reader's list  $2=the shipped one, to recognise rather than to use
    local want i=0 sep=""
    list_len "$MENU_ACTS"; want=$LIST_N
    list_len "${1:-}"
    # EXACTLY AS MANY LABELS AS BUTTONS, AND NOT THE SHIPPED ONES. "Enough"
    # took a four-label default over a three-button MENU_ACT and drew
    # "Awake/Sleep" over the button that opens the settings and "More" over the
    # one that ends the dashboard: words one place along from their buttons,
    # which is worse than words in a language nobody in the room reads.
    #
    # The shipped list is refused for the same reason: a reader who changes
    # MENU_ACT and leaves MENU_LBL alone has four labels for four buttons, and
    # every one of them belongs to a button that is no longer there.
    if [ -n "${1:-}" ] && [ "$LIST_N" = "$want" ] && [ "$1" != "${2:-}" ] &&
       [ "$1" != "$MENU_LBL_OLD" ]; then
        MENU_LBLS="$1"
        return 0
    fi
    # Built from the actions, which is the same four words when the actions are
    # the shipped ones — and the right words when they are not.
    MENU_LBLS=""
    while [ "$i" -lt "$want" ]; do
        list_at "$MENU_ACTS" "$i"
        menu_word_of "$LIST_ITEM"
        MENU_LBLS="$MENU_LBLS$sep$MENU_WORD_DEF"
        sep="|"
        i=$(( i + 1 ))
    done
    return 0
}

# THE LABELS ARE THE SCRIPT'S OWN, not the collector's. The one moment this bar
# is most wanted is the one where the collector cannot be reached, so a menu
# whose words arrive over the network is a menu that is blank exactly when it
# matters. MENU_LBL carries all three, so a reader who wants them in their own
# language sets one line in dash.conf.
#
# LEFT-ALIGNED IN THEIR SLOTS, WITH THE SLOTS RULED. A ruled slot says where a
# button is without needing the width of the word in it — which is the thing
# FBInk will not report. str_chars() is only asked how many characters a label
# has, and only to keep the type from overrunning the slot it names.
draw_menu() {
    menu_geom
    fill_rect 0 "$MENU_Y" "$MENU_W" "$MENU_H" BLACK

    local pad=$(( MENU_SLOT / 8 ))
    [ "$pad" -lt 4 ] && pad=4
    local sz=$(( MENU_H * 32 / 100 ))
    [ "$sz" -lt 12 ] && sz=12

    # THE TYPE FITS THE NARROWEST SLOT, not the bar. A proportional serif
    # averages about half its size per character, so the longest label bounds
    # the size for all of them: five buttons on a 600 px panel is 120 px each,
    # and "Settings" at the height this bar used to ask for is 130 of them.
    local i=0 longest=0 room
    while [ "$i" -lt "$MENU_N" ]; do
        menu_label "$i"
        if [ -n "$MENU_LBL_I" ]; then
            str_chars "$MENU_LBL_I"
            [ "$STR_N" -gt "$longest" ] && longest="$STR_N"
        fi
        i=$((i + 1))
    done
    if [ "$longest" -gt 0 ]; then
        room=$(( (MENU_SLOT - pad * 2) * 2 / longest ))
        [ "$room" -lt "$sz" ] && sz="$room"
        [ "$sz" -lt 11 ] && sz=11
    fi
    local ty=$(( MENU_Y + (MENU_H - sz) / 2 ))

    # The dividers, in the mid grey the page uses for a hairline inside a block
    # rather than the white that would read as a gap in the bar.
    i=1
    while [ "$i" -lt "$MENU_N" ]; do
        fill_rect $(( i * MENU_SLOT )) "$MENU_Y" "${RULE_H:-1}" "$MENU_H" GRAY7
        i=$((i + 1))
    done

    i=0
    while [ "$i" -lt "$MENU_N" ]; do
        menu_label "$i"
        [ -n "$MENU_LBL_I" ] &&
            draw_text_reg_inv $(( i * MENU_SLOT + pad )) "$ty" "$sz" "$MENU_LBL_I"
        i=$((i + 1))
    done
    refresh_zone 0 "$MENU_Y" "$MENU_W" "$MENU_H" 1
    return 0
}

# ── Font selection ───────────────────────────────────────────────────────────
# -t/--truetype needs a FILE. A name that does not resolve leaves every string
# undrawn, so the candidates are tried in order and the first file that exists
# wins; the Kindle's own font directory is the fallback for a device with no
# Bookerly, and dropping any .ttf into fonts/ overrides the lot.
SYS_FONTS="${DASH_SYS_FONTS:-/usr/java/lib/fonts}"
# Overridable for the same reason DASH_TMP and the RTC nodes are: a settings
# screen that cannot resolve a font draws nothing at all, and a test with no
# way to give it one cannot tell that apart from a screen that drew badly.
USR_FONTS="${DASH_FONTS:-$DASH_DIR/fonts}"

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

# The first .ttf/.otf in fonts/ then the system's whose name has $1 in it,
# as spelt or in lower case:
# the upright face for $2=reg, skipping bold, italic and the heavier cuts, or
# the bold one for $2=bold. For the faces whose file names this script does
# not know for certain — Palatino and Baskerville differ between firmwares —
# and as a second chance for the ones it does.
pick_family() {
    # $1=family as it appears in the file name, $2=reg|bold
    local dir f base lc
    lc=$(printf '%s' "$1" | tr 'A-Z' 'a-z')
    for dir in "$USR_FONTS" "$SYS_FONTS"; do
        for f in "$dir"/*"$1"* "$dir"/*"$lc"*; do
            [ -f "$f" ] || continue
            base=${f##*/}
            case "$base" in *.ttf|*.TTF|*.otf|*.OTF) ;; *) continue ;; esac
            case "$base" in *[Ii]talic*|*[Oo]blique*) continue ;; esac
            if [ "$2" = "bold" ]; then
                case "$base" in *[Bb]old*|*_75_*) echo "$f"; return 0 ;; esac
            else
                case "$base" in *[Bb]old*|*[Hh]eavy*|*[Bb]lack*|*_75_*) continue ;; esac
                echo "$f"; return 0
            fi
        done
    done
    echo ""
}

# The face the collector's E-ink settings chose (FONT_FACE, KFACE_* 0..5):
# its files first, then the order this script has always used, so a face the
# device does not have falls back to Bookerly rather than to nothing. Into
# FACE_REG and FACE_BOLD, empty when that face has no file here.
face_files() {
    # $1=FONT_FACE
    FACE_REG=""; FACE_BOLD=""
    case "$1" in
        1) FACE_REG=$(pick_font "Caecilia_LT_65_Medium.ttf" "Caecilia_LT_55_Roman.ttf")
           FACE_BOLD=$(pick_font "Caecilia_LT_75_Bold.ttf")
           fam=Caecilia ;;
        2) fam=Palatino ;;
        3) fam=Baskerville ;;
        4) FACE_REG=$(pick_font "Helvetica_LT_65_Medium.ttf" "Helvetica_LT_55_Roman.ttf")
           FACE_BOLD=$(pick_font "Helvetica_LT_75_Bold.ttf")
           fam=Helvetica ;;
        5) FACE_REG=$(pick_font "Futura_LT_Book.ttf" "Futura_LT_Medium.ttf")
           FACE_BOLD=$(pick_font "Futura_LT_Bold.ttf" "Futura_LT_Heavy.ttf")
           fam=Futura ;;
        *) return 0 ;;
    esac
    [ -n "$FACE_REG" ]  || FACE_REG=$(pick_family "$fam" reg)
    [ -n "$FACE_BOLD" ] || FACE_BOLD=$(pick_family "$fam" bold)
    # A bold with no upright of its own would sit beside Bookerly's regular:
    # two faces on one line, which is what falling back whole avoids.
    [ -n "$FACE_REG" ] || FACE_BOLD=""
    return 0
}

FONT_FACE_SET=""
FACE_CHANGED=0
font_setup() {
    local fam
    FONT_FACE_SET="${FONT_FACE:-0}"
    face_files "$FONT_FACE_SET"
    FONT_REG="$FACE_REG"; FONT_BOLD="$FACE_BOLD"
    [ -n "$FONT_REG" ] || \
        FONT_REG=$(pick_font "Bookerly-Regular.ttf" "Caecilia_LT_65_Medium.ttf" \
                             "Helvetica_LT_65_Medium.ttf" "Futura_LT_Book.ttf")
    # The chosen face's bold with its own upright only: a Bookerly bold beside
    # a Futura regular would be two faces on one line.
    if [ -z "$FONT_BOLD" ] && [ -z "$FACE_REG" ]; then
        FONT_BOLD=$(pick_font "Bookerly-Bold.ttf" "Caecilia_LT_75_Bold.ttf" \
                              "Helvetica_LT_75_Bold.ttf" "Futura_LT_Bold.ttf")
    fi
    [ -z "$FONT_BOLD" ] && FONT_BOLD="$FONT_REG"
    [ -z "$FONT_REG" ]  && FONT_REG="$FONT_BOLD"
    if [ -z "$FONT_REG" ]; then
        echo "No TrueType font found in $USR_FONTS or $SYS_FONTS." >&2
        echo "Drop one into $USR_FONTS — text cannot be drawn without it." >&2
        return 1
    fi
    # Each face's own proportions, read out of its file — see font_metrics().
    font_metrics "$FONT_REG";  FONT_REG_SPAN="$FM_SPAN";  FONT_REG_ASC="$FM_ASC"
    font_metrics "$FONT_BOLD"; FONT_BOLD_SPAN="$FM_SPAN"; FONT_BOLD_ASC="$FM_ASC"
    # The clock is set in the bold — see clock_metrics().
    clock_metrics "$FONT_BOLD"
    clock_metrics "$FONT_REG" RW
    [ "${TRACE:-0}" = "1" ] && \
        printf 'font clock widths: 0 %s 1 %s : %s\n' "${CW_0:-?}" "${CW_1:-?}" "${CW_c:-?}" >&2
    [ "${TRACE:-0}" = "1" ] && \
        printf 'font face %s: %s span %s asc %s; %s span %s asc %s\n' "$FONT_FACE_SET" \
            "$FONT_REG" "${FONT_REG_SPAN:-?}" "${FONT_REG_ASC:-?}" \
            "$FONT_BOLD" "${FONT_BOLD_SPAN:-?}" "${FONT_BOLD_ASC:-?}" >&2
    return 0
}

# ── How big FBInk will draw a face ───────────────────────────────────────────
#
# FBInk's `px` is the face's (ascent - descent), not its em — see text_geom().
# That ratio is the face's own, and TEXT_PX_MILLE in the layout file was one
# guess for all of them: 1160. A face whose span is larger than that came out
# smaller than the browser page by the difference, the headline most visibly,
# and every value placed after another by an advance moved with it. So it is
# read from the file the panel actually draws with, the two numbers FBInk's
# stb_truetype reads itself: `hhea` ascender and descender over `head`
# unitsPerEm.
#
# Sets FM_SPAN ((ascent - descent) / em) and FM_ASC (ascent / em), both in
# thousandths — or both empty when the file is not a TrueType/OpenType font it
# can read, and the layout file's TEXT_PX_MILLE stands in as before.
FM_SPAN=""; FM_ASC=""
font_metrics() {
    # $1=font file
    FM_SPAN=""; FM_ASC=""
    [ -s "$1" ] || return 1
    command -v od >/dev/null 2>&1 || return 1
    local f="$1" n head="" hhea="" upm asc desc
    # The offset table: the version, then numTables.
    set -- $(od -A n -t u1 -N 6 "$f" 2>/dev/null)
    [ $# -eq 6 ] || return 1
    case "$1 $2 $3 $4" in
        '0 1 0 0'|'116 114 117 101'|'79 84 84 79') ;;   # 1.0, 'true', 'OTTO'
        *) return 1 ;;
    esac
    n=$(( $5 * 256 + $6 ))
    [ "$n" -ge 1 ] && [ "$n" -le 64 ] || return 1
    # The table records, sixteen bytes each: the tag, a checksum, the offset.
    set -- $(od -A n -t u1 -j 12 -N $(( n * 16 )) "$f" 2>/dev/null)
    [ $# -eq $(( n * 16 )) ] || return 1
    while [ $# -ge 16 ]; do
        case "$1 $2 $3 $4" in
            '104 101 97 100')  head=$(( (($9 * 256 + ${10}) * 256 + ${11}) * 256 + ${12} )) ;;
            '104 104 101 97')  hhea=$(( (($9 * 256 + ${10}) * 256 + ${11}) * 256 + ${12} )) ;;
        esac
        shift 16
    done
    [ -n "$head" ] && [ -n "$hhea" ] || return 1
    set -- $(od -A n -t u1 -j $(( head + 18 )) -N 2 "$f" 2>/dev/null)
    [ $# -eq 2 ] || return 1
    upm=$(( $1 * 256 + $2 ))
    set -- $(od -A n -t u1 -j $(( hhea + 4 )) -N 4 "$f" 2>/dev/null)
    [ $# -eq 4 ] || return 1
    asc=$(( $1 * 256 + $2 ));  [ "$asc" -ge 32768 ]  && asc=$(( asc - 65536 ))
    desc=$(( $3 * 256 + $4 )); [ "$desc" -ge 32768 ] && desc=$(( desc - 65536 ))
    [ "$upm" -ge 16 ] && [ "$asc" -gt 0 ] || return 1
    [ "$desc" -gt 0 ] && desc=$(( -desc ))      # a few faces store it unsigned
    FM_SPAN=$(( (asc - desc) * 1000 / upm ))
    FM_ASC=$(( asc * 1000 / upm ))
    # Nothing a text face has; a number outside it is a file misread, and the
    # layout's guess is the better one.
    if [ "$FM_SPAN" -lt 900 ] || [ "$FM_SPAN" -gt 1800 ] ||
       [ "$FM_ASC" -lt 500 ] || [ "$FM_ASC" -ge "$FM_SPAN" ]; then
        FM_SPAN=""; FM_ASC=""
        return 1
    fi
    return 0
}

# ── How wide the clock's characters are in the face it is drawn in ───────────
#
# WITH FUTURA THE CLOCK WRAPPED: "00:36" came out on two lines. FBInk wraps a
# string that runs past the screen's right edge, and the clock sits at the
# right of the page with its rectangle sized for Bookerly's figures. Futura's
# are wider — its 0 is a full circle — and the clock is the largest bold
# string on the page, so it was the one that ran out of room. Nothing the
# collector sends could have caught it: CLOCK_ADVW is measured with one
# table for every face, and it is a sample, not this minute's string.
#
# So the panel measures the clock itself, from the file it is drawn with: the
# advances of 0-9, ':' and the am/pm letters, looked up through `cmap`
# (format 4, the Windows Unicode one) and `hmtx`. Read once per face, so the
# minute-by-minute clock costs no fork. clock_fit() then sets the time smaller
# when it would not fit its rectangle, and centres it by its real width.
#
# Sets CW_<c> for each character (CW_c for ':', CW_d for '.', CW_n for '-')
# in thousandths of the em, or leaves them all empty when the file cannot be
# read that far — the clock then draws as it always has. A second argument
# names another prefix: the regular face is read into RW_ the same way, so
# draw_field() can put a unit right after the figures FBInk actually drew.
CLOCK_CHARS="0 1 2 3 4 5 6 7 8 9 c a m p d n"
clock_metrics() {
    # $1=font file  $2=prefix, CW by default
    local f="$1" p="${2:-CW}" c n off sub seg i k code gid nhm start delta ro
    for c in $CLOCK_CHARS; do eval "${p}_$c="; done
    [ -s "$f" ] || return 1
    command -v od >/dev/null 2>&1 || return 1
    set -- $(od -A n -t u1 -N 6 "$f" 2>/dev/null)
    [ $# -eq 6 ] || return 1
    n=$(( $5 * 256 + $6 ))
    [ "$n" -ge 1 ] && [ "$n" -le 64 ] || return 1
    local cmap="" hmtx="" hhea="" head=""
    set -- $(od -A n -t u1 -j 12 -N $(( n * 16 )) "$f" 2>/dev/null)
    [ $# -eq $(( n * 16 )) ] || return 1
    while [ $# -ge 16 ]; do
        off=$(( (($9 * 256 + ${10}) * 256 + ${11}) * 256 + ${12} ))
        case "$1 $2 $3 $4" in
            '99 109 97 112')   cmap=$off ;;
            '104 109 116 120') hmtx=$off ;;
            '104 104 101 97')  hhea=$off ;;
            '104 101 97 100')  head=$off ;;
        esac
        shift 16
    done
    [ -n "$cmap" ] && [ -n "$hmtx" ] && [ -n "$hhea" ] && [ -n "$head" ] || return 1
    set -- $(od -A n -t u1 -j $(( head + 18 )) -N 2 "$f" 2>/dev/null)
    [ $# -eq 2 ] || return 1
    local upm=$(( $1 * 256 + $2 ))
    [ "$upm" -ge 16 ] || return 1
    set -- $(od -A n -t u1 -j $(( hhea + 34 )) -N 2 "$f" 2>/dev/null)
    [ $# -eq 2 ] || return 1
    nhm=$(( $1 * 256 + $2 ))
    [ "$nhm" -ge 1 ] || return 1
    # The encoding records: platform, encoding, offset. Windows Unicode BMP
    # (3,1) first, then a Unicode-platform BMP one (0,0-3) — not (0,4) or
    # (0,6), which are format 12/13 and would fail the format check below.
    set -- $(od -A n -t u1 -j "$cmap" -N 4 "$f" 2>/dev/null)
    [ $# -eq 4 ] || return 1
    n=$(( $3 * 256 + $4 ))
    [ "$n" -ge 1 ] && [ "$n" -le 32 ] || return 1
    set -- $(od -A n -t u1 -j $(( cmap + 4 )) -N $(( n * 8 )) "$f" 2>/dev/null)
    [ $# -eq $(( n * 8 )) ] || return 1
    sub=""; local win=""
    while [ $# -ge 8 ]; do
        off=$(( cmap + (($5 * 256 + $6) * 256 + $7) * 256 + $8 ))
        case "$1 $2 $3 $4" in
            '0 3 0 1') win=$off ;;
            '0 0 0 0'|'0 0 0 1'|'0 0 0 2'|'0 0 0 3') [ -z "$sub" ] && sub=$off ;;
        esac
        shift 8
    done
    [ -n "$win" ] && sub=$win
    [ -n "$sub" ] || return 1
    set -- $(od -A n -t u1 -j "$sub" -N 8 "$f" 2>/dev/null)
    [ $# -eq 8 ] && [ "$1 $2" = "0 4" ] || return 1
    seg=$(( ($7 * 256 + $8) / 2 ))
    [ "$seg" -ge 1 ] || return 1
    # Segments are sorted by their last code, and ASCII is at the front: the
    # first few cover everything the clock draws. Up to 128 of each array, one
    # od apiece.
    k=$seg; [ "$k" -gt 128 ] && k=128
    local ends starts deltas ros
    ends=$(od -A n -t u1 -j $(( sub + 14 )) -N $(( k * 2 )) "$f" 2>/dev/null)
    starts=$(od -A n -t u1 -j $(( sub + 16 + seg * 2 )) -N $(( k * 2 )) "$f" 2>/dev/null)
    deltas=$(od -A n -t u1 -j $(( sub + 16 + seg * 4 )) -N $(( k * 2 )) "$f" 2>/dev/null)
    ros=$(od -A n -t u1 -j $(( sub + 16 + seg * 6 )) -N $(( k * 2 )) "$f" 2>/dev/null)
    for c in $CLOCK_CHARS; do
        case "$c" in
            c) code=58 ;; a) code=97 ;; m) code=109 ;; p) code=112 ;;
            d) code=46 ;; n) code=45 ;;
            *) code=$(( 48 + c )) ;;
        esac
        # The first segment whose last code is at or past this one.
        i=0; set -- $ends
        while [ $# -ge 2 ]; do
            [ $(( $1 * 256 + $2 )) -ge "$code" ] && break
            shift 2; i=$(( i + 1 ))
        done
        [ $# -ge 2 ] || continue
        set -- $starts;  shift $(( i * 2 )); start=$(( $1 * 256 + $2 ))
        [ "$start" -le "$code" ] || continue
        set -- $deltas;  shift $(( i * 2 )); delta=$(( $1 * 256 + $2 ))
        set -- $ros;     shift $(( i * 2 )); ro=$(( $1 * 256 + $2 ))
        if [ "$ro" -eq 0 ]; then
            gid=$(( (code + delta) & 65535 ))
        else
            # idRangeOffset is from its own slot in the array to the glyph.
            set -- $(od -A n -t u1 -j $(( sub + 16 + seg * 6 + i * 2 + ro + (code - start) * 2 )) -N 2 "$f" 2>/dev/null)
            [ $# -eq 2 ] || continue
            gid=$(( $1 * 256 + $2 ))
            [ "$gid" -eq 0 ] || gid=$(( (gid + delta) & 65535 ))
        fi
        [ "$gid" -gt 0 ] || continue
        # Glyphs past numberOfHMetrics share the last advance.
        [ "$gid" -ge "$nhm" ] && gid=$(( nhm - 1 ))
        set -- $(od -A n -t u1 -j $(( hmtx + gid * 4 )) -N 2 "$f" 2>/dev/null)
        [ $# -eq 2 ] || continue
        eval "${p}_$c=$(( ($1 * 256 + $2) * 1000 / upm ))"
    done
    # Without the figures and the colon there is nothing to measure a time by.
    for c in 0 1 2 3 4 5 6 7 8 9 c; do
        eval "[ -n \"\${${p}_$c}\" ]" || { for c in $CLOCK_CHARS; do eval "${p}_$c="; done; return 1; }
    done
    return 0
}

# How wide $1 comes out in the bold face, in thousandths of the em, into
# CLOCK_W — 0 when a character in it was not measured. No fork: it runs every
# minute.
clock_width() {
    local s="$1" c w
    CLOCK_W=0
    while [ -n "$s" ]; do
        c="${s%"${s#?}"}"; s="${s#?}"
        case "$c" in
            [0-9]) eval "w=\${CW_$c}" ;;
            :)     w="${CW_c}" ;;
            a|m|p) eval "w=\${CW_$c}" ;;
            *)     w="" ;;
        esac
        [ -n "$w" ] || { CLOCK_W=0; return 1; }
        CLOCK_W=$(( CLOCK_W + w ))
    done
    return 0
}

# How wide a reading's figures come out, in thousandths of the em, into
# VAL_ADV: measured in the face it is drawn in ($1=1 the bold, else the
# regular) when every character of $2 was read from that file, otherwise $3,
# the collector's estimate. THE ESTIMATE IS SET WIDE on purpose — figures at
# 0.62 em, so nothing is ever cut — and the unit drawn after it at that width
# stood well clear of the number: "21.6   °", and the slash and the second
# value pushed into the rule beside them.
val_adv() {
    # $1=bold $2=value $3=fallback advance
    local s="$2" p=RW c w
    [ "$1" = "1" ] && p=CW
    VAL_ADV=0
    while [ -n "$s" ]; do
        c="${s%"${s#?}"}"; s="${s#?}"
        case "$c" in
            [0-9]) eval "w=\${${p}_$c}" ;;
            .)     eval "w=\${${p}_d}" ;;
            -)     eval "w=\${${p}_n}" ;;
            *)     w="" ;;
        esac
        [ -n "$w" ] || { VAL_ADV="${3:-0}"; return 1; }
        VAL_ADV=$(( VAL_ADV + w ))
    done
    [ "$VAL_ADV" -gt 0 ] || VAL_ADV="${3:-0}"
    return 0
}

# The size the time is drawn at, $1 or smaller so that $2 fits the clock's
# rectangle with a little air at its sides — into CLOCK_SZ. Its width at that
# size into CLOCK_PX, 0 when the face could not be measured.
clock_fit() {
    # $1=design size  $2=the time
    local room
    CLOCK_SZ="$1"; CLOCK_PX=0
    clock_width "$2" || return 0
    [ "$CLOCK_W" -gt 0 ] || return 0
    # Kerning and FBInk's rounding are not in the sum; 4 % covers both.
    room=$(( ${Z_CLOCK_W:-0} * 96 / 100 ))
    [ "$room" -gt 0 ] || return 0
    if [ $(( CLOCK_SZ * CLOCK_W / 1000 )) -gt "$room" ]; then
        CLOCK_SZ=$(( room * 1000 / CLOCK_W ))
    fi
    CLOCK_PX=$(( CLOCK_SZ * CLOCK_W / 1000 ))
}

# ── Network helpers ──────────────────────────────────────────────────────────
#
#: HH:MM of the last payload that parsed, and whether one has this run. The
#: readings carry their own age from the collector; these say whether the
#: collector is being reached at all, which nothing on the page could show.
LAST_OK=""
DATA_FRESH=0
#: Has ANY fetch worked this run — which is not the same question as whether
#: the last one did, and is what tells a page drawn from the cache apart from
#: one drawn from the network.
EVER_FRESH=0
#: Fetches that have failed in a row. ONE IS A WIFI HICCUP and the page should
#: not flinch at it: the numbers are five minutes old and the collector will
#: almost certainly answer the next tier. Several in a row is a collector that
#: is not there, and numbers nobody has said anything about are then a lie the
#: page is telling on its own behalf.
FAILS=0
#: The stamp on the payload the cache already holds, so an hour that brought
#: nothing new does not rewrite it — see cache_save().
CACHE_STAMP=""
CACHE="${DASH_CACHE:-$DASH_DIR/last.txt}"
# The addresses a scan found, beside dash.conf for the same reason the cache is
# — /tmp is a ramdisk and stop.sh empties it. Named HERE, in the file
# settings.sh sources, because settings.sh is the half that writes it and the
# dashboard is the half that asks whether it exists: two spellings of one path
# is a scan whose results the other half cannot see.
SCAN_LIST="${DASH_SCAN_LIST:-$DASH_DIR/collectors}"

# Are the readings on screen old enough that drawing them without saying so
# would be dishonest?
data_stale() {
    [ "${DATA_FRESH:-0}" = "1" ] && return 1
    [ "${EVER_FRESH:-0}" = "1" ] || return 0
    [ "${FAILS:-0}" -ge "${STALE_AFTER:-2}" ]
}

# Is this a WHOLE payload, or the first part of one?
#
# THE SAME QUESTION graph_ok() ASKS ABOUT THE IMAGE, and it went unasked here
# for longer. The collector streams this while it is also serving the web UI
# and taking readings, the client is a ten-year-old reader on wifi, and busybox
# wget does not always call a short read an error — so half a payload lands on
# disk looking exactly like a whole one. It parses. Every key past the cut is
# simply absent, and the page comes up with a third of its values blank and
# nothing to say why. `wget -O` had already truncated the good copy by then.
#
# END=1 is the collector's last line. A collector too old to send one is held
# to RES_H instead, which is in the metadata block two thirds of the way in —
# not proof, but it catches the cut that lands in the readings, which is most
# of them.
payload_ok() {
    [ -s "$1" ] || return 1
    grep -q '^END=1' "$1" 2>/dev/null && return 0
    grep -q '^RES_H=' "$1" 2>/dev/null
}

# ── Which way up ─────────────────────────────────────────────────────────────
# THE FRAMEBUFFER TURNS, NOT THE DRAWING. FBInk draws in whatever orientation
# the framebuffer is in, so once the panel is turned every coordinate this
# script already has is right as it stands: the collector sends the landscape
# page in landscape coordinates (RES_W=800, RES_H=600) and nothing below knows
# it is on its side. Only the touch layer does not turn — see touch_scale().
#
# Turned with FBInk's fbdepth when there is one (-r, the panel's own number),
# else by writing the kernel's rotate node; and if that does not come out the
# right shape, with fbdepth -R, the Linux canonical numbering. Then CHECKED: a Kindle that ignores the
# request, or turns the wrong way, would otherwise draw an 800 px page onto a
# 600 px screen for as long as the dashboard ran. What does not check out is
# undone, logged, and the upright page asked for instead.

#: Degrees the panel is turned to now, by this script. 0 until it turns it.
ROT_DEG=0
#: 1 once the panel has refused a turn. The upright page from then on.
ROT_FAIL=0
#: The rotation the framebuffer had before this script touched it.
ROT_ORIG=""
#: The rotation that draws the page upright: ROT_ORIG, unless the reader was
#: already on its side when the dashboard started, then the panel's own 0.
ROT_BASE=""
ROT_NODE="${ROT_NODE:-/sys/class/graphics/fb0/rotate}"

rot_fbdepth() {
    local c
    for c in fbdepth /mnt/us/koreader/fbdepth /mnt/us/libkh/bin/fbdepth; do
        if command -v "$c" >/dev/null 2>&1; then ROT_BIN="$c"; return 0; fi
        [ -x "$c" ] && { ROT_BIN="$c"; return 0; }
    done
    return 1
}

# The framebuffer's rotation now, in the panel's own numbering.
rot_read() {
    local v
    v=$(cat "$ROT_NODE" 2>/dev/null | tr -dc '0-9')
    case "$v" in [0-3]) echo "$v" ;; *) echo 0 ;; esac
}

# Whether the screen is wider than it is tall, from FBInk's own view of it.
# 2 when FBInk will not say, which lets the turn through unchecked.
rot_is_wide() {
    local e w h
    e=$(fbink -e 2>/dev/null) || { echo 2; return; }
    w=$(printf '%s' "$e" | tr ';' '\n' | sed -n 's/^viewWidth=\([0-9]*\).*/\1/p' | head -n 1)
    h=$(printf '%s' "$e" | tr ';' '\n' | sed -n 's/^viewHeight=\([0-9]*\).*/\1/p' | head -n 1)
    case "$w$h" in ''|*[!0-9]*) echo 2; return ;; esac
    [ "$w" -gt "$h" ] && echo 1 || echo 0
}

# Turn the panel to $1 degrees clockwise from where it started.
rot_apply() {
    local deg="$1" q
    q=$(( deg / 90 ))
    rot_origin
    ROT_TOUCHED=1
    # The two quarter turns the other way round, on a model that numbers them
    # so — ROTATE_SWAP=1.
    if [ "${ROTATE_SWAP:-0}" = "1" ]; then
        case "$q" in 1) q=3 ;; 3) q=1 ;; esac
    fi
    local want=$(( (ROT_BASE + q) % 4 ))
    rot_set_raw "$want" && rot_check "$deg" && return 0
    # The panel's own numbering did not come out that shape: FBInk's canonical
    # one, which is the Linux convention whatever the model.
    if [ "$deg" != "0" ] && rot_fbdepth && "$ROT_BIN" -R "$q" >/dev/null 2>&1; then
        rot_check "$deg" && return 0
    fi
    return 1
}

# Where the framebuffer started, once. A reader that was left in landscape
# (its own UI turned) is not "upright" for this page: the panel's 0 is.
rot_origin() {
    [ -n "$ROT_ORIG" ] && return 0
    ROT_ORIG=$(rot_read)
    ROT_BASE="$ROT_ORIG"
    [ "$(rot_is_wide)" = "1" ] && ROT_BASE=0
    # For stop.sh, which has to put it back after a dashboard that died.
    echo "$ROT_ORIG" > "$TMP/rota" 2>/dev/null
}

# CHECKED BY ITS SHAPE. Half a turn cannot be told from none this way, and a
# quarter turn the wrong way round can only be told by eye — which is what
# ROTATE_SWAP is for.
rot_check() {
    case "$1" in
        90|270) [ "$(rot_is_wide)" != "0" ] ;;
        0)      [ "$(rot_is_wide)" != "1" ] ;;
        *)      return 0 ;;
    esac
}

rot_set_raw() {
    if rot_fbdepth; then
        "$ROT_BIN" -r "$1" >/dev/null 2>&1 && return 0
    fi
    [ -w "$ROT_NODE" ] && echo "$1" > "$ROT_NODE" 2>/dev/null
}

# Back to how the reader was, on the way out.
rot_restore() {
    # Whether or not a turn ever succeeded: one that failed half way can
    # still have left the panel somewhere else.
    [ -n "$ROT_ORIG" ] || return 0
    [ "${ROT_TOUCHED:-0}" = "1" ] && rot_set_raw "$ROT_ORIG"
    ROT_DEG=0
    rm -f "$TMP/rota" 2>/dev/null
}

# This panel's own answer, if it has one: ROTATE in dash.conf, or 0 once the
# panel has refused to turn. Sets ROT_LOCAL; non-zero for "the collector's".
rot_local() {
    if [ "${ROT_FAIL:-0}" = "1" ]; then ROT_LOCAL=0; return 0; fi
    case "${ROTATE:-auto}" in
        0|90|180|270) ROT_LOCAL="$ROTATE"; return 0 ;;
    esac
    return 1
}

# Turn the panel to the page in hand. The page says which way up it was laid
# out for (PAGE_ROT); a collector too old to say lays out the upright page,
# and the panel stays upright for it whatever dash.conf asks.
rot_sync() {
    local d="${PAGE_ROT:-0}"
    case "$d" in 0|90|180|270) ;; *) d=0 ;; esac
    # Already there — except the first time, when the reader may have been
    # left on its side and even the upright page needs a turn.
    local first=0
    [ -n "$ROT_ORIG" ] || { rot_origin; first=1; }
    if [ "$d" = "${ROT_DEG:-0}" ]; then
        [ "$first" = "0" ] && return 0
        [ "$ROT_BASE" = "$ROT_ORIG" ] && return 0
    fi
    if rot_apply "$d"; then
        ROT_DEG="$d"
        # Every pixel on the panel is somewhere else now.
        [ "${HAVE_DATA:-0}" = "1" ] && : > "$TMP/redraw"
        return 0
    fi
    echo "$(date '+%H:%M') rotation: the panel did not turn to $d degrees, drawing the upright page" >&2
    ROT_FAIL=1
    # Back to upright, whatever the failed turn left behind — and to where the
    # reader was if even that does not check out.
    if [ "$d" = "0" ] || ! rot_apply 0; then
        [ -n "$ROT_ORIG" ] && rot_set_raw "$ROT_ORIG"
    fi
    ROT_DEG=0
    [ "${HAVE_DATA:-0}" = "1" ] && : > "$TMP/redraw"
    return 1
}

# ── Whose time ───────────────────────────────────────────────────────────────
# THE CLOCK ON THE PANEL IS THIS KINDLE'S, drawn every minute from `date`, and
# it goes on being drawn while the collector is away. What the collector gives
# it is a reference: every CLOCK_SYNC days (or SYNC_DAYS, the collector's
# setting, under auto) the Kindle's clock is set from TIME_UTC, the system
# clock and — so it survives a sleep or a reboot — the hardware one. And
# TIME_OFF, the collector's zone, is what the clock is shown in, so the panel
# and the web page agree whatever zone the Kindle itself was set to.

#: Where the last setting is remembered, across restarts: epoch seconds.
CLOCK_SYNC_FILE="${DASH_CLOCK_SYNC:-$DASH_DIR/clocksync}"
#: A clock this far out is set at once, whatever the interval.
CLOCK_SYNC_FAR=600
#: The file's value, read once: it changes only when this script writes it.
CLOCK_SYNC_LAST=""
CLOCK_SYNC_READ=0
#: 1 when the last clock_sync() moved the clock.
CLOCK_JUMPED=0

# A POSIX TZ for an offset EAST of UTC in seconds. The sign is POSIX's, the
# other way round: UTC+2 is "UTC-2".
tz_from_off() {
    local off="$1" sign="-" h m
    case "$off" in ''|-|*[!0-9-]*|*?-*) return 1 ;; esac
    [ "$off" -lt 0 ] && { sign="+"; off=$(( -off )); }
    h=$(( off / 3600 )); m=$(( off % 3600 / 60 ))
    if [ "$m" -eq 0 ]; then echo "UTC$sign$h"
    else printf 'UTC%s%d:%02d\n' "$sign" "$h" "$m"; fi
}

tz_apply() {
    local tz
    tz=$(tz_from_off "${TIME_OFF:-}") || return 0
    [ "$tz" = "${TZ:-}" ] && return 0
    TZ="$tz"; export TZ
}

# Days between settings: this panel's own, or the collector's. 0 is never.
clock_sync_days() {
    case "${CLOCK_SYNC:-auto}" in
        auto) SYNC_EVERY="${SYNC_DAYS:-1}" ;;
        *)    SYNC_EVERY="$CLOCK_SYNC" ;;
    esac
    case "$SYNC_EVERY" in ''|*[!0-9]*) SYNC_EVERY=1 ;; esac
}

# Whether to set the clock now. $1 the Kindle's time, $2 the collector's, $3
# when it was last set (empty for never), $4 the days between.
clock_sync_due() {
    local now="$1" utc="$2" last="$3" days="$4" d
    [ "${days:-0}" -gt 0 ] 2>/dev/null || return 1
    d=$(( utc - now )); [ "$d" -lt 0 ] && d=$(( -d ))
    [ "$d" -gt "$CLOCK_SYNC_FAR" ] && return 0
    case "$last" in ''|*[!0-9]*) return 0 ;; esac
    # A remembered setting in the future is a clock that has since gone back.
    [ "$last" -gt "$(( now + 3600 ))" ] && return 0
    [ $(( now - last )) -ge $(( days * 86400 )) ]
}

# Epoch seconds to "YYYY-MM-DD hh:mm:ss" UTC, in shell arithmetic — for a
# busybox date that will not take -s @seconds.
epoch_utc() {
    local t="$1" z era doe yoe y doy mp d m
    z=$(( t / 86400 + 719468 )); era=$(( z / 146097 ))
    doe=$(( z - era * 146097 ))
    yoe=$(( (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365 ))
    y=$(( yoe + era * 400 )); doy=$(( doe - (365 * yoe + yoe / 4 - yoe / 100) ))
    mp=$(( (5 * doy + 2) / 153 )); d=$(( doy - (153 * mp + 2) / 5 + 1 ))
    if [ "$mp" -lt 10 ]; then m=$(( mp + 3 )); else m=$(( mp - 9 )); fi
    [ "$m" -le 2 ] && y=$(( y + 1 ))
    printf '%04d-%02d-%02d %02d:%02d:%02d\n' "$y" "$m" "$d" \
        $(( t % 86400 / 3600 )) $(( t % 3600 / 60 )) $(( t % 60 ))
}

clock_set() {
    date -u -s "@$1" >/dev/null 2>&1 ||
        date -u -s "$(epoch_utc "$1")" >/dev/null 2>&1 || return 1
    # And the hardware clock, or the next suspend or reboot undoes it — in
    # UTC, which is what the kernel reads it as: a plain -w writes local time,
    # and TZ is the collector's zone by now.
    hwclock -u -w >/dev/null 2>&1 || TZ=UTC0 hwclock -w >/dev/null 2>&1
    return 0
}

clock_sync() {
    local utc="${TIME_UTC:-}" now last
    case "$utc" in ''|*[!0-9]*) return 0 ;; esac
    [ "$utc" -gt 1000000000 ] || return 0
    clock_sync_days
    [ "$SYNC_EVERY" -gt 0 ] 2>/dev/null || return 0
    if [ "$CLOCK_SYNC_READ" = "0" ]; then
        CLOCK_SYNC_LAST=$(tr -dc '0-9' < "$CLOCK_SYNC_FILE" 2>/dev/null)
        CLOCK_SYNC_READ=1
    fi
    now=$(date +%s)
    last="$CLOCK_SYNC_LAST"
    clock_sync_due "$now" "$utc" "$last" "$SYNC_EVERY" || return 0
    if [ $(( utc - now )) -ge 2 ] || [ $(( now - utc )) -ge 2 ]; then
        if ! clock_set "$utc"; then
            echo "$(date '+%H:%M') clock: could not set the time from the collector" >&2
            return 1
        fi
        echo "$(date '+%H:%M') clock: set from the collector ($(( utc - now )) s)" >&2
        # Every tier is measured against this clock, and a wake window held
        # open to an absolute time moves with it — or setting the clock back
        # an hour reopens a window that had closed, for an hour.
        [ "${AWAKE_UNTIL:-0}" -gt 0 ] 2>/dev/null &&
            AWAKE_UNTIL=$(( AWAKE_UNTIL + utc - now ))
        EPOCH="$utc"
        CLOCK_JUMPED=1
    fi
    CLOCK_SYNC_LAST="$utc"
    echo "$utc" > "$CLOCK_SYNC_FILE" 2>/dev/null
    return 0
}

fetch_data() {
    # Into a scratch file and only into place once it is whole — the same shape
    # as fetch_graph, for the same reason: keeping the last good payload is a
    # better failure than replacing it with part of a new one.
    # The page shape the reader has chosen, if it has: the collector works the
    # layout out for the page this panel will draw, and only this end knows.
    local q="" sep="?"
    case "${LAYOUT:-auto}" in
        normal|standalone) q="?shape=$LAYOUT"; sep="&" ;;
    esac
    # And which way up, when this panel has its own answer — or cannot turn.
    rot_local && { q="$q${sep}rot=$ROT_LOCAL"; sep="&"; }
    # This script draws three indoor readings as two columns (IN_COL); a
    # collector sends that layout only to a reader that says so.
    q="$q${sep}col=1"
    # And the wall page (WALL, IN_VCOL, the clock beside the forecast), which
    # a collector sends only to a reader that says it can draw it.
    q="$q&wall=1"
    # Which version of these scripts this is, so the collector can offer a
    # newer one — and, while the last one it offered was refused, why.
    # docs/KINDLE_UPDATE.md §3.
    [ -n "${DASH_VER:-}" ] || pkg_version
    q="$q&pkg=$DASH_VER_Q"
    [ -n "${PKG_ERR:-}" ] && q="$q&pkgerr=$PKG_ERR&pkgfor=$PKG_TRIED"
    if wget -q -T "$FETCH_TIMEOUT" -O "$TMP/data.new" "$(host_url)/kindle/data$q" \
            2>/dev/null && payload_ok "$TMP/data.new"; then
        mv "$TMP/data.new" "$TMP/data.txt"
        # The time in it is this minute's, which a cached payload's is not.
        TIME_FRESH=1
        # And so is the offer in it: a cached page must never start an update.
        PKG_FRESH=1
        FAILS=0
        TRUNC_WARNED=0
        return 0
    fi
    FAILS=$(( FAILS + 1 ))
    # ONCE PER RUN OF BAD LUCK, not once per fetch. A link that consistently
    # cuts the payload — the exact case payload_ok() exists for — is 288 copies
    # of one sentence a day in a ramdisk, which is the mistake CONF_WARNED and
    # SUSPEND_WARNED already exist to stop. The latch is dropped by the next
    # whole payload, so a fault that comes back is reported again.
    if [ -s "$TMP/data.new" ] && [ "${TRUNC_WARNED:-0}" = "0" ]; then
        TRUNC_WARNED=1
        echo "$(date '+%H:%M') data: incomplete payload ($(wc -c < "$TMP/data.new" | tr -dc '0-9') bytes), keeping the last good one; further cuts are not logged until a whole one arrives" >&2
    fi
    rm -f "$TMP/data.new"
    DATA_FRESH=0
    return 1
}

# ── The last page, kept where a reboot cannot take it ────────────────────────
#
# A PANEL THAT HAS JUST BOOTED KNOWS NOTHING. /tmp is a ramdisk, so after a
# reboot or a Stop the dashboard comes up with no payload at all: no chart, no
# forecast, no week strip, and the one message it can draw is in English
# because the language is a value the collector sends. If the collector is also
# down — which is the same power cut, most of the time — that is the whole page
# until it comes back.
#
# So the payload is kept beside dash.conf, written on the full tier: once an
# hour, not once a fetch, because this is FAT on the eMMC and not tmpfs.
cache_save() {
    # ONLY A PAYLOAD THIS RUN ACTUALLY FETCHED. The full tier calls this every
    # hour whether the fetch worked or not, and what is in data.txt when it did
    # not is the cache itself, loaded at startup — so an offline reader rewrote
    # the same bytes to the eMMC once an hour and appended another CACHED_AT to
    # them every time. A week away is 168 duplicate lines and 168 rewrites of a
    # file on FAT, to save what was already saved.
    [ "${EVER_FRESH:-0}" = "1" ] || return 1
    # AND NOT THE SAME PAYLOAD TWICE. Once the collector goes down, every hour
    # after that would rewrite the last good payload with the same stamp on it:
    # the duplicate lines are gone but the writes are not, and this is eMMC.
    # The stamp is the payload's own identity here — it is the minute a fetch
    # last succeeded, so an unchanged one means nothing new to keep.
    [ "${LAST_OK:-}" != "${CACHE_STAMP:-}" ] || return 1
    [ -s "$TMP/data.txt" ] || return 1
    cp "$TMP/data.txt" "$CACHE.tmp$$" 2>/dev/null || return 1
    # WHEN, or the page drawn from this file cannot say how old it is — and a
    # panel showing yesterday's numbers with no date on them is the one
    # failure worse than a blank one. Written as a payload key so that loading
    # the cache is exactly loading a payload.
    #
    # AND WHICH DAY, which the clock alone cannot carry: a reader switched off
    # on Monday evening and switched on on Thursday drew "(17:40)" under
    # readings three days old, and 17:40 on a page whose own clock says 09:12
    # reads as yesterday at worst. The day is only put on screen when it is not
    # today's — see cache_load().
    printf 'CACHED_AT="%s"\nCACHED_ON="%s"\n' "${LAST_OK:-}" "$(date '+%d.%m')" \
        >> "$CACHE.tmp$$" 2>/dev/null
    mv "$CACHE.tmp$$" "$CACHE" 2>/dev/null || { rm -f "$CACHE.tmp$$"; return 1; }
    CACHE_STAMP="${LAST_OK:-}"
    return 0
}

cache_load() {
    [ -s "$CACHE" ] || return 1
    cp "$CACHE" "$TMP/data.txt" 2>/dev/null || return 1
    # An offer is only ever acted on from a payload fetched this run.
    PKG_FRESH=0
    load_data || return 1
    # load_data believes what it loaded is current, because every other caller
    # has just fetched it. Here it is not.
    LAST_OK="${CACHED_AT:-}"
    # The day in front of the time when the cache is not from today. Two
    # numbers rather than a formatted date: this is a stamp in brackets beside
    # a clock, it has to be unmistakably not the clock, and it must not depend
    # on a `date -d` busybox may not have.
    if [ -n "${CACHED_ON:-}" ] && [ -n "$LAST_OK" ] &&
       [ "$CACHED_ON" != "$(date '+%d.%m')" ]; then
        LAST_OK="$CACHED_ON $LAST_OK"
    fi
    EVER_FRESH=0
    # NOT FRESH, and the page says so: the ages in this payload were computed
    # at the collector when it was written, so its "3 min" is a lie by however
    # long the reader was off. The readings zone draws the offline notice over
    # them until a fetch succeeds; the chart, forecast and week strip below are
    # worth having whatever their age.
    DATA_FRESH=0
    return 0
}

# ── Updating these scripts from the collector ────────────────────────────────
#
# THE SAME MODEL AS THE SENSOR NODES' FIRMWARE: the collector holds one copy of
# this extension, the person offers it on the collector's E-ink dashboard page,
# and every reader PULLS it. docs/KINDLE_UPDATE.md is the contract.
#
#   1. Every fetch says which version this is (?pkg=, from VERSION).
#   2. While a package is on offer and this reader runs something else, the
#      payload carries PKG_VER, PKG_MD5 and PKG_SIZE.
#   3. pkg_install() downloads /kindle/pkg.tar, checks it, and only then
#      touches this folder. Any refusal leaves the folder exactly as it was,
#      is logged, and is reported on every fetch (?pkgerr=, with ?pkgfor= the
#      MD5 it was about) until the next start — a package that failed once is
#      not retried every five minutes.
#   4. pkg_restart() hands the device back and becomes the new copy.
#
# EVERYTHING HERE RUNS AS ROOT, on a file that came over plain HTTP. The
# collector refuses anything that is not a package before offering it
# (src/web/KindlePkgTar.h), but that is what stops a wrong upload, not what
# makes this safe: the checks below are the ones that count, and they are made
# before a single byte lands outside $TMP.

# This copy's version, and the same spelled for a query string. "none" for a
# folder copied from a checkout, which has no VERSION: it can still update, it
# just cannot say what it runs.
pkg_version() {
    local v=""
    [ -f "$DASH_DIR/VERSION" ] && IFS= read -r v < "$DASH_DIR/VERSION"
    v=${v%"$(printf '\r')"}
    case "$v" in ''|*[!A-Za-z0-9._+-]*) v=none ;; esac
    DASH_VER="$v"
    # '+' is a space in a query string.
    DASH_VER_Q=$(printf '%s' "$v" | sed 's/+/%2B/g')
    return 0
}

#: The MD5 of the last package tried this run, and why it was refused (empty
#: when it was not). Only the dashboard's own process acts on an offer:
#: settings.sh sources this file and fetches too.
PKG_TRIED=""
PKG_ERR=""
PKG_MAIN=0

pkg_check() {
    [ "${PKG_MAIN:-0}" = "1" ] || return 0
    [ -n "${PKG_VER:-}" ] || return 0
    [ -n "${DASH_VER:-}" ] || pkg_version
    [ "$PKG_VER" != "$DASH_VER" ] || return 0
    # Once per package per run. A new upload is a new MD5, and is tried.
    [ "$PKG_TRIED" = "${PKG_MD5:-}" ] && return 0
    PKG_TRIED="${PKG_MD5:-}"
    PKG_ERR=""
    echo "$(date '+%H:%M') update: the collector offers $PKG_VER (this is $DASH_VER)" >&2
    if pkg_install; then
        pkg_log "updated from $DASH_VER to $PKG_VER by the collector"
        echo "$(date '+%H:%M') update: installed $PKG_VER, restarting into it" >&2
        pkg_restart
        return 0
    fi
    PKG_ERR="$PKG_WHY"
    pkg_log "update to $PKG_VER refused: $PKG_WHY - nothing was changed"
    echo "$(date '+%H:%M') update: $PKG_VER refused ($PKG_WHY), nothing was changed" >&2
    return 0
}

# One line in kual.log, beside the scripts: "which version is on this reader,
# and when did it get there" is the first question asked of one that behaves
# oddly, and /tmp/dash.log does not survive a reboot.
pkg_log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') --- $1" >> "$DASH_DIR/kual.log" 2>/dev/null
    return 0
}

#: Why pkg_install() refused, as one word the collector shows.
PKG_WHY=""

pkg_fail() {
    PKG_WHY="$1"
    rm -rf "$TMP/pkg" "$TMP/pkg.tar" "$TMP/pkg.list" 2>/dev/null
    return 1
}

# Download, check, unpack into $TMP, check again, then put every file in place.
pkg_install() {
    local tar="$TMP/pkg.tar" d="$TMP/pkg" n sum mode rest name f rel v
    PKG_WHY=""
    rm -rf "$d" "$tar" "$TMP/pkg.list" 2>/dev/null

    # The offer itself, before it is used for anything.
    case "${PKG_SIZE:-}" in ''|*[!0-9]*) pkg_fail bad_offer; return 1 ;; esac
    case "${PKG_MD5:-}" in *[!0-9a-f]*) pkg_fail bad_offer; return 1 ;; esac
    [ "${#PKG_MD5}" -eq 32 ] || { pkg_fail bad_offer; return 1; }
    case "$PKG_VER" in *[!A-Za-z0-9._+-]*) pkg_fail bad_offer; return 1 ;; esac
    [ "$PKG_SIZE" -le 1572864 ] 2>/dev/null || { pkg_fail too_big; return 1; }

    # Whole, and the file that was offered: the size and the MD5 the payload
    # named. wget does not always call a short read an error.
    wget -q -T 60 -O "$tar" "$(host_url)/kindle/pkg.tar" 2>/dev/null ||
        { pkg_fail download; return 1; }
    n=$(wc -c < "$tar" 2>/dev/null | tr -dc '0-9')
    [ "$n" = "$PKG_SIZE" ] || { pkg_fail size; return 1; }
    command -v md5sum >/dev/null 2>&1 || { pkg_fail no_md5sum; return 1; }
    sum=$(md5sum "$tar" 2>/dev/null | cut -d' ' -f1)
    [ "$sum" = "$PKG_MD5" ] || { pkg_fail md5; return 1; }

    # WHAT IS IN IT, BEFORE ANY OF IT IS UNPACKED — unpacking is where a link
    # in the archive becomes a write through that link. Regular files and
    # directories under esp32dash/, plain names, none of this reader's own
    # files. A tar that cannot list modes (-v) fails here, which is the safe
    # way to fail.
    tar -tvf "$tar" > "$TMP/pkg.list" 2>/dev/null || { pkg_fail not_package; return 1; }
    [ -s "$TMP/pkg.list" ] || { pkg_fail not_package; return 1; }
    while read -r mode rest; do
        name=${rest##* }
        case "$mode" in -*|d*) ;; *) pkg_fail bad_entry; return 1 ;; esac
        case "$name" in esp32dash/*) ;; *) pkg_fail bad_path; return 1 ;; esac
        case "$name" in *[!A-Za-z0-9._/-]*|*//*|*/./*|*/.|*/../*|*/..) pkg_fail bad_path; return 1 ;; esac
        case "$name" in
            esp32dash/dash.conf|esp32dash/collectors|esp32dash/kual.log|esp32dash/last.txt)
                pkg_fail bad_path; return 1 ;;
        esac
    done < "$TMP/pkg.list"
    # And again WITHOUT REGARD TO CASE: /mnt/us is FAT, where DASH.CONF is
    # this reader's dash.conf.
    if tr 'A-Z' 'a-z' < "$TMP/pkg.list" |
            grep -qE ' esp32dash/(dash\.conf|collectors|kual\.log|last\.txt)$'; then
        pkg_fail bad_path
        return 1
    fi

    mkdir -p "$d" && tar -xf "$tar" -C "$d" 2>/dev/null || { pkg_fail unpack; return 1; }
    rm -f "$tar"
    # And what landed, in case the listing and the unpacking disagreed.
    [ -z "$(find "$d" -type l 2>/dev/null)" ] || { pkg_fail bad_entry; return 1; }

    # Complete, and the version it was offered as.
    for f in update_dash.sh start.sh stop.sh menu.json VERSION; do
        [ -s "$d/esp32dash/$f" ] || { pkg_fail missing_files; return 1; }
    done
    v=""
    IFS= read -r v < "$d/esp32dash/VERSION"
    v=${v%"$(printf '\r')"}
    [ "$v" = "$PKG_VER" ] || { pkg_fail version; return 1; }
    # Every script at least parses. A package that cannot is one this reader
    # would restart into and never come back from on its own.
    for f in "$d"/esp32dash/*.sh; do
        [ -f "$f" ] || continue
        sh -n "$f" 2>/dev/null || { pkg_fail syntax; return 1; }
    done

    # INTO PLACE IN TWO PASSES. Every file is copied beside its target as
    # .new first — the step that can fail on a full volume — and only when
    # all of them are there is anything renamed over the old one. So a
    # failure leaves the old copy whole, and the renames are quick enough that
    # nothing in between is a real window. Renamed rather than written in
    # place: this script is itself one of the files, and the shell running it
    # still has the old one open.
    (cd "$d/esp32dash" && find . -type d) > "$TMP/pkg.list" 2>/dev/null
    while read -r rel; do
        mkdir -p "$DASH_DIR/$rel" 2>/dev/null || { pkg_fail write; return 1; }
    done < "$TMP/pkg.list"
    (cd "$d/esp32dash" && find . -type f) > "$TMP/pkg.list" 2>/dev/null
    while read -r rel; do
        if ! cp "$d/esp32dash/$rel" "$DASH_DIR/$rel.new" 2>/dev/null; then
            while read -r rel; do rm -f "$DASH_DIR/$rel.new"; done < "$TMP/pkg.list"
            pkg_fail write
            return 1
        fi
    done < "$TMP/pkg.list"
    while read -r rel; do
        mv -f "$DASH_DIR/$rel.new" "$DASH_DIR/$rel" 2>/dev/null
    done < "$TMP/pkg.list"
    rm -rf "$d" "$TMP/pkg.list"
    sync 2>/dev/null
    return 0
}

# The device handed back exactly as Stop would, then this process becomes the
# new copy — the same PID, so the pidfile and stop.sh stay right. The new copy
# starts as a fresh Start does: cached page first, then its own fetch.
pkg_restart() {
    handback
    exec sh "$DASH_DIR/update_dash.sh"
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

# Is the image on disk as tall as the chart the layout has room for? The
# collector's layout sizes the chart, and a fetch for the new height can fail
# after the page has already moved: the kept image, drawn at the new GR_Y,
# would run over the key and the band below it. Not drawn then — the line
# saying there is no chart yet goes there instead, until the fetch comes in.
# The BMP's height is at offset 22, negative for a top-down image; a reader
# with no od, or a header that says 0, is let through as before.
graph_fits() {
    [ "${LAYOUT_FLOW:-0}" = "1" ] || return 0
    local h w
    h=$(od -An -td4 -j22 -N4 "$1" 2>/dev/null | tr -dc '0-9')
    case "$h" in ''|0) return 0 ;; esac
    [ "$h" = "${GR_H:-}" ] || return 1
    # And as wide, on the landscape page, where the chart is beside the
    # readings: an upright page's image there would run off the screen.
    [ "${LAND:-0}" = "1" ] || return 0
    w=$(od -An -td4 -j18 -N4 "$1" 2>/dev/null | tr -dc '0-9')
    case "$w" in ''|0) return 0 ;; esac
    [ "$w" = "${GR_W:-}" ]
}

# Is the chart switched on? Consulted before FETCHING as well as before
# drawing: a reader who turns the chart off in Settings should not have the
# Kindle keep downloading a 56 KB image over WiFi every GRAPH_EVERY minutes
# for a section nothing draws.
chart_wanted() { [ "${SHOW_CHART:-1}" = "1" ]; }

# A 24 h line drawn behind figures on the wall page: the outdoor one behind
# the headline (HERO_LINE, the chart's switch on a page with no chart) and a
# grid place's own (Z_<PLACE>_LINE). Kept in a file named by its place, size
# and pens — white on black for a place set that way — so a page laid out
# afresh, or a place turned inverse, never draws one that no longer fits; the
# last good one of the same shape stays when a fetch fails.
line_file() {
    # $1=place $2=w $3=h -> LINE_F
    local inv
    eval "inv=\${Z_$1_INV:-0}"
    LINE_F="$TMP/line_$1_$2x$3_${inv:-0}.bmp"
}
fetch_line() {
    # $1=place $2=query prefix ("" or "z=g1&") $3=w $4=h
    line_file "$1" "$3" "$4"
    local q="line=1&$2w=$3&h=$4"
    case "$LINE_F" in *_1.bmp) q="$q&inv=1" ;; esac
    if wget -q -T 15 -O "$TMP/line.new" "$(host_url)/kindle/graph.bmp?$q" \
         2>/dev/null && [ -s "$TMP/line.new" ]; then
        rm -f "$TMP/line_$1_"*.bmp
        mv "$TMP/line.new" "$LINE_F"
    else
        rm -f "$TMP/line.new"
    fi
}
# Drawn on the place's plate, if it has one, and under its figures.
draw_line() {
    # $1=place $2=x $3=y $4=w $5=h
    line_file "$1" "$4" "$5"
    [ -s "$LINE_F" ] && draw_image "$LINE_F" "$2" "$3"
    return 0
}

# The headline's line stops where its plate does: 8 px short of the band, so
# the white between the two stays white under an inverted headline too.
hero_line_h() {
    LINE_H=$(( ${SUB_Y:-180} - ${HERO_Y:-40} ))
    sub_band && LINE_H=$(( LINE_H - 8 ))
    return 0
}

fetch_lines() {
    if [ "${LAYOUT_FLOW:-0}" != "1" ]; then rm -f "$TMP"/line_*.bmp; return 0; fi
    if [ "${HERO_LINE:-0}" = "1" ]; then
        hero_line_h
        fetch_line HERO "" "${HEAD_W:-564}" "$LINE_H"
    else
        rm -f "$TMP"/line_HERO_*.bmp
    fi
    # Files from before lines were named by their shape.
    rm -f "$TMP/heroline.bmp" "$TMP"/zline_*.bmp
    local gcols gvsz gi z f keep=" HERO "
    set --
    if [ "${WALL:-0}" = "1" ]; then set -- ${GRID_ZONES:-}; fi
    for gcols in ${LY_GRID_ROWS:-}; do
        [ -n "${1:-}" ] || break
        if [ "$gcols" -ge 3 ]; then gvsz="${GRID_VAL_SZ_3:-26}"; else gvsz="${GRID_VAL_SZ:-31}"; fi
        gi=0
        while [ "$gi" -lt "$gcols" ] && [ -n "${1:-}" ]; do
            z="$1"; shift; gi=$((gi + 1))
            eval "[ \"\${Z_${z}_LINE:-}\" = 1 ]" || continue
            keep="$keep$z "
            fetch_line "$z" "z=$(echo "$z" | tr 'A-Z' 'a-z')&" \
                       "$(( ${COL_L_W:-270} / gcols - 14 ))" "$gvsz"
        done
    done
    # A place that no longer has a line, or a page that is no longer the
    # wall, leaves none behind.
    for f in "$TMP"/line_*.bmp; do
        [ -e "$f" ] || continue
        z="${f#"$TMP"/line_}"; z="${z%%_*}"
        case "$keep" in *" $z "*) ;; *) rm -f "$f" ;; esac
    done
    return 0
}

fetch_graph() {
    fetch_lines
    chart_wanted || return 1
    # Into a scratch file, and only into place once it is whole. wget -O
    # truncates its target the moment it opens it, so fetching straight onto
    # graph.bmp turned one WiFi hiccup into a zero-byte file — and since
    # redraw_chart clears its rectangle before drawing, that showed as a blank
    # strip where the chart was until the next successful fetch. Keeping the
    # last good chart is the better failure.
    # As tall as the collector's layout left the chart. Without a layout the
    # fixed image, which is what the layout file's GR_H was measured against.
    local q=""
    [ "${LAYOUT_FLOW:-0}" = "1" ] && q="?h=${GR_H}"
    [ "${LAYOUT_FLOW:-0}" = "1" ] && [ "${LAND:-0}" = "1" ] && q="$q&w=${GR_W}"
    if wget -q -T 15 -O "$TMP/graph.new" "$(host_url)/kindle/graph.bmp$q" 2>/dev/null \
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

# What the previous payload put in the shell, taken back out.
#
# load_kv() ONLY EVER ASSIGNS. A place that stops being sent — a sensor whose
# node went flat, a group switched off in the web UI, a forecast module turned
# off — kept the value it had when it was last heard from, on screen, with no
# age against it and nothing to distinguish it from a live reading. Silently
# wrong beats loudly wrong on most pages; on this one it is the whole point of
# the page.
#
# The names come from the lists the LAST payload sent, because those are the
# only ones this end can enumerate: Z_<PLACE>_* is a family the collector
# names, not a fixed set.
zones_forget() {
    local z s
    for z in ${GRID_ZONES:-} ${IN_ZONES:-} HERO BIG; do
        for s in VALUE UNIT LABEL ARROW BOLD INK VADVW UADVW ADVW LADVW CAPUNIT HEAVY INV BAR LINE; do
            unset "Z_${z}_${s}" 2>/dev/null
        done
    done
    unset Z_GROUP_OUT Z_GROUP_IN Z_GROUP_IN_ADVW Z_SUB Z_SUB_ADVW GRID_ZONES GRID_ROWS IN_ZONES 2>/dev/null
    # The forecast is a section that can be switched off, so its heading key
    # has to be able to go away too — draw_forecast_body draws the whole block
    # only when FC_SUMMARY is set.
    unset FC_SUMMARY FC0_LABEL FC1_LABEL FC2_LABEL 2>/dev/null
    # AND THE SHAPE OF THE PAGE, for the same reason: a collector downgraded to
    # a firmware that does not send it would otherwise leave the panel drawing
    # the standalone layout for ever, because load_kv only ever assigns.
    unset PAGE_MODE SUB_BAND SUB_INK 2>/dev/null
    # And which way up, and whether there is a clock: a collector that no
    # longer says lays out the upright page with one.
    unset PAGE_ROT SHOW_CLOCK FC3_LABEL FC4_LABEL 2>/dev/null
    unset TIME_UTC TIME_OFF SYNC_DAYS FONT_FACE 2>/dev/null
    # And the layout, for the same reason again: a collector downgraded to a
    # firmware that works none out has to take the panel back to the file's.
    for z in $FLOW_KEYS GRID_ROWS CH_T CH_B CH_L CH_R; do unset "LY_$z" 2>/dev/null; done
    unset CACHED_AT CACHED_ON 2>/dev/null
    # And the offer: a collector that stops offering a package stops sending
    # the keys, and load_kv only ever assigns.
    unset PKG_VER PKG_MD5 PKG_SIZE 2>/dev/null
    return 0
}

# The place names the payload lists go into eval as variable names (Z_G1_…),
# so they are upper-case letters, digits and _ or the lists are dropped:
# payload_key_ok() checks a key's NAME, and these are values.
zones_check() {
    case " ${GRID_ZONES:-} ${IN_ZONES:-} " in
        *[!A-Z0-9_\ ]*) GRID_ZONES=""; IN_ZONES="" ;;
    esac
}

load_data() {
    [ -f "$TMP/data.txt" ] || return 1
    zones_forget
    load_kv "$TMP/data.txt" PAYLOAD || return 1
    zones_check
    # The face chosen on the collector, picked up the moment it changes — and
    # the whole page redrawn in it, not one zone at a time over half an hour.
    if [ "${FONT_FACE:-0}" != "$FONT_FACE_SET" ]; then
        font_setup || true
        FACE_CHANGED=1
    fi

    # The layout follows the data, always — RES_W and RES_H come from the
    # collector, so the two cannot be loaded independently.
    #
    # It used to be called once, inside the branch taken when the FIRST fetch
    # succeeded. A Kindle that finished booting before the ESP32 did took the
    # other branch, and nothing in the main loop ever called it again: every
    # coordinate stayed unset for as long as the script ran.
    load_layout
    # LAID OUT FOR A TURN THIS PANEL WOULD NOT MAKE. load_layout() tried, the
    # framebuffer refused, and the page in hand is the landscape one: ask again
    # for the upright page, once — fetch_data() says rot=0 from now on.
    if [ "${ROT_FAIL:-0}" = "1" ] && [ "${PAGE_ROT:-0}" != "0" ] && fetch_data; then
        zones_forget
        load_kv "$TMP/data.txt" PAYLOAD || return 1
        zones_check
        load_layout
    fi
    # The collector's zone, from any payload, the cached one included; its
    # time only from one fetched just now.
    local tz_was="${TZ:-}"
    tz_apply
    CLOCK_JUMPED=0
    if [ "${TIME_FRESH:-0}" = "1" ]; then
        TIME_FRESH=0
        clock_sync
    fi
    # The time the caller is about to draw was read before either: read it
    # again, so this tick draws the new one rather than the next clock tier.
    if [ "${TZ:-}" != "$tz_was" ] || [ "$CLOCK_JUMPED" = "1" ]; then
        NOW_TIME=$(now_clock)
    fi
    HAVE_DATA=1
    DATA_FRESH=1
    EVER_FRESH=1
    LAST_OK=$(now_clock)
    # LAST, once everything this payload says has been taken in: an update
    # that installs does not come back here — it restarts into the new copy.
    if [ "${PKG_FRESH:-0}" = "1" ]; then
        PKG_FRESH=0
        pkg_check
    fi
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
# Is this the standalone page — no forecast band, readings a sixth larger?
#
# THE READER'S SETTING WINS, and `auto` is what asks the collector. PAGE_MODE
# arrives in the payload and is unset by zones_forget() before every load, so a
# collector too old to send it, or one downgraded to a firmware without it,
# takes this back to the ordinary page rather than leaving the panel in a shape
# nothing is sending any more.
page_standalone() {
    case "${LAYOUT:-auto}" in
        normal)     return 1 ;;
        standalone) return 0 ;;
    esac
    [ "${PAGE_MODE:-normal}" = "standalone" ]
}

#: 1 when the standalone overlay is the layout currently loaded — not merely
#: when one was asked for. See load_layout().
LAYOUT_SA=0

# Is the forecast band drawn at all?
#
# KEYED ON THE LAYOUT THAT LOADED, not on what was asked for. A panel with no
# overlay of its own is still holding coordinates with a forecast band in them,
# and hiding the block on that page is 124 px of white with a week strip under
# it — the exact hole this whole change exists to close. The two questions
# cannot be allowed to disagree, so only one of them decides.
fc_wanted() {
    if [ "${LAYOUT_FLOW:-0}" = "1" ]; then [ "${FC_BAND:-1}" = "1" ]; return; fi
    [ "${LAYOUT_SA:-0}" != "1" ]
}

# ── The layout the collector worked out ──────────────────────────────────────
# A COLLECTOR THAT KNOWS WHAT IS ON THE PAGE SAYS WHERE IT GOES. The layout
# file is one fixed page: switch the week strip off and its 88 px stay white;
# put two readings in the grid and they sit on one row with an empty one under
# them. The collector is the end that knows which sections are on, how many
# places have a reading and how wide each can get, so it works the page out —
# src/web/KindleFlow.h — and sends it as LY_<name> for the names below, already
# at this panel's size. They are laid over the file, which stays the
# description of everything that does not move.
#
# THESE NAMES AND NO OTHERS, AND DIGITS ONLY. The values are assigned with
# eval into the variables every draw reads, so a name outside this list or a
# value that is not a number is dropped rather than trusted.
#
# A collector too old to send a layout sends no LY_GR_H, and the panel draws
# the file's page exactly as it always has.
FLOW_KEYS="GROUP_LAB_SZ HERO_Y HERO_SZ BIG_SZ HEAD_GAP SLASH_W SUB_Y SUB_SZ
 GRID_Y GRID_ROW_H GRID_LAB_SZ GRID_VAL_SZ GRID_VAL_SZ_3 SEP_H
 CL_SIZE CL_H CL_SZ_BOXED CL_SZ_RULED CL_RULED_PAD CL_SZ_DATED CL_DATE_SZ CL_DATE_GAP
 IN_RULE_Y IN_LAB_Y IN_VAL_Y IN_VAL2_Y IN_VAL_SZ IN_VAL_SZ_1 IN_W1 IN_STACK
 IN_COL IN_VAL3_Y
 RULE2_Y LAB_CHART_Y GR_Y GR_H KEY_Y RULE3_Y
 LAB_FC_Y FC_ICON_Y FC_TEXT_Y FC_TEMP_Y FC_WIND_Y OL0_Y OL1_Y OL2_Y
 WK_HDG_RULE_Y WK_HDG_Y WK_Y FC_BAND
 COL_L_W BATT_X
 LAND TOP_Y COL_L_X COL_R_X COL_R_W SEP_X CL_X CL_Y CL_W TOPROW_Y
 RULE2_X RULE2_W LAB_CHART_X GR_X GR_W KEY_IN_X KEY_BAND
 OL_N OL0_X OL1_X OL2_X OL3_X OL4_X OL3_Y OL4_Y RULE3_W
 WK_X WK_HDG_X WK_CELL_W WK_HDG_RULE_W FOOT_RULE_Y FOOT_RULE_W FOOT_Y
 STAT_X STAT_Y BATT_Y
 WALL HEAD_W HEAD_RULE_Y IN_VCOL SEP_Y SEP2_X SEP2_Y SEP2_H
 FC_ICON_X FC_TEXT_X FC_TEXT_W FC_TEXT_SZ FC_TEMP_X FC_TEMP_SZ FC_WIND_X FC_WIND_SZ"

#: 1 when the collector's layout is the one loaded. See flow_apply().
LAYOUT_FLOW=0
#: Every value it set, in order — how load_layout tells a page that moved.
FLOW_SIG=""

flow_apply() {
    local k v sig=""
    LAYOUT_FLOW=0
    # The keys no layout file carries, which would otherwise outlive the page
    # that sent them — a landscape page's LAND=1 on the upright one after it.
    unset IN_W1 IN_STACK IN_COL IN_VAL3_Y IN_VAL2_Y FC_BAND LAND TOPROW_Y KEY_BAND OL_N \
          OL3_X OL4_X OL3_Y OL4_Y \
          WALL HEAD_W HEAD_RULE_Y IN_VCOL SEP_Y SEP2_X SEP2_Y SEP2_H FC_TEXT_W 2>/dev/null
    LAB_INK=GRAY7
    SLASH_INK=GRAYA
    case "${LY_GR_H:-}" in ''|*[!0-9]*) FLOW_SIG=""; return 0 ;; esac
    # The grid's rows are a list of counts, each divided into the column's
    # width: 1..6 each, separated by single spaces, or the grid is not drawn
    # at all rather than divided by zero.
    local r
    for r in ${LY_GRID_ROWS:-}; do
        case "$r" in [1-6]) ;; *) LY_GRID_ROWS=""; break ;; esac
    done
    for k in $FLOW_KEYS; do
        eval "v=\${LY_$k:-}"
        case "$v" in ''|*[!0-9]*) continue ;; esac
        eval "$k=\$v"
        sig="$sig$v,"
    done
    # The chart's axis, for the image this layout asks for rather than the
    # fixed one: /kindle/graph.bmp?h=GR_H is as tall as the chart now is.
    case "${LY_CH_T:-}" in ''|*[!0-9]*) ;; *) CH_T="$LY_CH_T" ;; esac
    case "${LY_CH_B:-}" in ''|*[!0-9]*) ;; *) CH_B="$LY_CH_B" ;; esac
    case "${LY_CH_L:-}" in ''|*[!0-9]*) ;; *) CH_L="$LY_CH_L" ;; esac
    case "${LY_CH_R:-}" in ''|*[!0-9]*) ;; *) CH_R="$LY_CH_R" ;; esac
    FLOW_SIG="$sig${LY_GRID_ROWS:-}"
    LAYOUT_FLOW=1
    # THE WALL PAGE IS READ FROM ACROSS A ROOM: its captions and the line
    # under the headline in black, where the desk page sets them in grey as
    # context for the numbers beside them. Grey at three metres is nothing —
    # and the slash between the two headline values with them, or the two
    # run together into one number.
    [ "${WALL:-0}" = "1" ] && { LAB_INK=BLACK; SLASH_INK=BLACK; }
    return 0
}

load_layout() {
    local conf
    case "${RES_W:-}" in ''|*[!0-9]*) RES_W=600 ;; esac
    case "${RES_H:-}" in ''|*[!0-9]*) RES_H=800 ;; esac
    [ "$RES_W" -ge 100 ] && [ "$RES_W" -le 4000 ] 2>/dev/null || RES_W=600
    [ "$RES_H" -ge 100 ] && [ "$RES_H" -le 4000 ] 2>/dev/null || RES_H=800

    # Turn the panel first, if the page asks for it — see rot_sync().
    rot_sync
    # ON ITS SIDE, THE UPRIGHT PANEL'S FILE: the landscape page is that panel
    # turned, drawn at the same sizes, and the collector's layout moves every
    # x and y that differs. There is no 800x600.conf to find.
    if [ "$RES_W" -gt "$RES_H" ]; then
        conf="$DASH_DIR/layout/${RES_H}x${RES_W}.conf"
    else
        conf="$DASH_DIR/layout/${RES_W}x${RES_H}.conf"
    fi
    if [ -f "$conf" ]; then
        . "$conf"
    else
        conf="$DASH_DIR/layout/600x800.conf"
        . "$conf"
    fi
    # ── AND THE STANDALONE PAGE ON TOP OF IT ────────────────────────────────
    # An OVERLAY, not a second full layout: it carries only the numbers that
    # move, so the base file stays the one description of this panel and a key
    # added there cannot be missing here. Sourced after, so its numbers win.
    #
    # A panel with no overlay of its own keeps the ORDINARY page, forecast band
    # and all: falling back to the 600 px overlay would be the wrong page drawn
    # at the right size, and hiding the band without moving anything would be
    # the hole this change exists to close. LAYOUT_SA is what the rest of the
    # script reads, so the two cannot disagree.
    local was="${LAYOUT_SA:-0}" was_sig="${FLOW_SIG:-}"
    LAYOUT_SA=0
    if page_standalone; then
        conf="${conf%.conf}-standalone.conf"
        if [ -f "$conf" ]; then
            . "$conf"
            LAYOUT_SA=1
        fi
    fi
    # ── A PAGE THAT CHANGED SHAPE HAS TO BE REPAINTED WHOLE ─────────────────
    # The router goes down, the collector comes up as its own AP, and the next
    # payload says standalone: every coordinate on the page has just moved, and
    # the tiers repaint their own rectangles only. Without this the chart stays
    # where it was, the new one is drawn 124 px lower, and the panel carries
    # both until the next full tier — up to an hour of a page with two charts
    # on it. The same going back the other way.
    #
    # Skipped on the first load of a run, where `was` and the new value can
    # only differ because nothing had been drawn yet.
    #
    # AND THE COLLECTOR'S LAYOUT ON TOP OF BOTH, which moves things for more
    # reasons than the forecast: a section switched off, a reading that came
    # or went. Any change to it is the same case as the one above.
    flow_apply
    if [ -n "${HAVE_DATA:-}" ] && [ "$HAVE_DATA" = "1" ] &&
       { [ "$was" != "$LAYOUT_SA" ] || [ "$was_sig" != "$FLOW_SIG" ]; }; then
        : > "$TMP/redraw"
    fi
    # Derived once here rather than per string: both depend only on the layout,
    # and a fork per draw is a fork this script has spent years avoiding.
    #
    # THE FACE'S OWN NUMBERS WHEN font_setup() COULD READ THEM, the layout
    # file's guess only when it could not: the guess was 1160 for every face,
    # and a face with a taller span drew that much smaller than the page.
    TEXT_PX_MILLE="${FONT_REG_SPAN:-${TEXT_PX_MILLE:-1160}}"
    BASELINE_MILLE=$(( ${FONT_REG_ASC:-$(( 800 * TEXT_PX_MILLE / 1000 ))} - (TEXT_PX_MILLE - 1000) / 2 ))
    if [ -n "${FONT_BOLD_SPAN:-}" ] && [ -n "${FONT_BOLD_ASC:-}" ]; then
        BASELINE_MILLE_BOLD=$(( FONT_BOLD_ASC - (FONT_BOLD_SPAN - 1000) / 2 ))
    else
        BASELINE_MILLE_BOLD="$BASELINE_MILLE"
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

    # Where the offline notice goes: the readings' rectangle.
    Z_OFF_X=0; Z_OFF_Y=0; Z_OFF_W=$ZW; Z_OFF_H=$Z_SENS_H
    Z_TOP_H=0

    # ── ON ITS SIDE the chart is BESIDE the readings, not under them ─────────
    # The top row (the clock and the week strip, to the rule under them) is a
    # rectangle of its own, redrawn with the readings; the readings are the
    # left column to the hairline, the chart the rest of the middle band.
    if [ "${LAND:-0}" = "1" ]; then
        local top="${TOPROW_Y:-0}" mid
        Z_TOP_H=0
        [ "$top" -gt 0 ] 2>/dev/null && { rule_h; Z_TOP_H=$(( top + RULE_T )); }
        mid=$(( ${RULE3_Y:-440} - Z_TOP_H ))
        Z_SENS_Y=$Z_TOP_H
        Z_SENS_H=$mid
        Z_CHART_Y=$Z_TOP_H
        if [ "${GR_H:-0}" -gt 0 ] 2>/dev/null; then
            Z_SENS_W=$(( ${SEP_X:-328} + ${SEP_W:-1} ))
            Z_CHART_X=$Z_SENS_W
            Z_CHART_W=$(( ZW - Z_SENS_W ))
            Z_CHART_H=$mid
        else
            Z_CHART_H=0
        fi
        # Across the whole band, because the notice is a sentence and the
        # left column is a third of the page. The chart tier leaves it alone
        # while it stands — see redraw_chart().
        Z_OFF_Y=$Z_TOP_H; Z_OFF_H=$mid
    fi
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
# That ratio, (ascent - descent) / unitsPerEm in thousandths, is read from
# each face's own file by font_metrics() into FONT_REG_SPAN / FONT_BOLD_SPAN.
# TEXT_PX_MILLE in the layout file is only the fallback for a file it cannot
# read: as the one number for every face it drew a taller-spanned face small.
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
    # $1=design top  $2=design size  [$3=font file — the bold face's own span
    # when it is FONT_BOLD; the regular one's otherwise]
    local m="${FONT_REG_SPAN:-${TEXT_PX_MILLE:-1160}}"
    [ -n "${3:-}" ] && [ "$3" = "${FONT_BOLD:-}" ] && m="${FONT_BOLD_SPAN:-$m}"
    TX_PX=$(( $2 * m / 1000 ))
    TX_TOP=$(( $1 - (TX_PX - $2) / 2 ))
    [ "$TX_TOP" -lt 0 ] && TX_TOP=0
}

# The other direction: the design `top` that puts a string's BOX at $1.
# For anything positioned by where its box has to sit rather than by where the
# layout tuned its top — the week strip centres its two rows in the cell.
box_top() {
    # $1=wanted box top  $2=design size
    BOX_TOP=$(( $1 + ($2 * ${FONT_REG_SPAN:-${TEXT_PX_MILLE:-1160}} / 1000 - $2) / 2 ))
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
    local txt="$6"
    # NO ARROW REACHES FBINK. The Kindle's book faces have none, and FBInk
    # draws a missing glyph as an empty box — the tendency arrow was one, and
    # so was every "→" in the offline hint, which the collector sends and the
    # reader may have typed into a label. The tendency is drawn by draw_arrow();
    # an arrow inside a sentence becomes the ASCII the font does have. A case
    # first, so the strings that carry none — all but one or two — cost no fork.
    case "$txt" in
        *'→'*|*'←'*) txt=$(printf '%s' "$txt" | sed 's/→/>/g; s/←/</g') ;;
    esac
    text_geom "$2" "$3" "$4"
    # Inside a place set white on black (zone_plate), every string is.
    if [ -n "$7" ] || [ -n "${DRAW_INV:-}" ]; then
        fb -q -b -h -C BLACK -B WHITE -t regular="$4",px="$TX_PX",left="$1",top="$TX_TOP" -- "$txt"
    elif [ -n "${8:-}" ]; then
        # $8, a ground the box is filled with — the band's own shade, so the
        # box does not show. See draw_text_on().
        fb -q -b -C "$5" -B "$8" -t regular="$4",px="$TX_PX",left="$1",top="$TX_TOP" -- "$txt"
    else
        fb -q -b -O -C "$5" -t regular="$4",px="$TX_PX",left="$1",top="$TX_TOP" -- "$txt"
    fi
}

draw_text_bold() { draw_text "$1" "$2" "$3" "$FONT_BOLD" "$4" "$5"; }
draw_text_reg()  { draw_text "$1" "$2" "$3" "$FONT_REG"  "$4" "$5"; }

# The same two, knocked out of a dark plate. No colour: --invert decides it,
# and passing one would only be a colour that is ignored.
draw_text_bold_inv() { draw_text "$1" "$2" "$3" "$FONT_BOLD" "" "$4" INV; }
draw_text_reg_inv()  { draw_text "$1" "$2" "$3" "$FONT_REG"  "" "$4" INV; }

# On a ground of a shade of its own ($4 ink on $5): FBInk fills the text's
# box with the ground's pen, so the box is the band and does not show. White
# on black is the knocked-out pair above, for the reason draw_text() gives;
# black on white is the ordinary bgless call.
draw_text_on() {
    # $1=x $2=y $3=px $4=ink $5=ground $6=text
    if [ "$4" = WHITE ] && [ "$5" = BLACK ]; then draw_text_reg_inv "$1" "$2" "$3" "$6"
    elif [ "$5" = WHITE ]; then draw_text_reg "$1" "$2" "$3" "$4" "$6"
    else draw_text "$1" "$2" "$3" "$FONT_REG" "$4" "$6" "" "$5"
    fi
}

# The wall page's band under the headline (SUB_BAND, SUB_INK), when the
# collector sent one and there is a 24 h range to put on it: a pen name, or
# nothing — anything else is not drawn rather than handed to FBInk.
sub_band() {
    [ "${WALL:-0}" = "1" ] && [ -n "${Z_SUB:-}" ] || return 1
    case "${SUB_BAND:-}" in BLACK|WHITE|GRAY[1-9A-E]) ;; *) return 1 ;; esac
    case "${SUB_INK:-}" in BLACK|WHITE|GRAY[1-9A-E]) ;; *) return 1 ;; esac
}
fill_rect() {
    # $1=x $2=y $3=w $4=h $5=colour
    #
    # NOTHING FOR AN EMPTY RECTANGLE. The layout sends 0 for a rule that is not
    # on the page — the chart's rule and the week strip's on the landscape
    # page, the hairline between the columns with nothing to separate — and
    # FBInk reads a zero width or height as "to the edge of the screen".
    [ "${3:-0}" -gt 0 ] 2>/dev/null && [ "${4:-0}" -gt 0 ] 2>/dev/null || return 0
    local pen="$5"
    # Inside a place set white on black, the pens swap: an arrow or a bar
    # drawn in black comes out white on the plate, and its white in black.
    if [ -n "${DRAW_INV:-}" ]; then
        case "$pen" in WHITE) pen=BLACK ;; *) pen=WHITE ;; esac
    fi
    fb -q -b -B "$pen" -k top="$2",left="$1",width="$3",height="$4"
}

# ── A place's own style: extra bold, white on black ─────────────────────────
# zone_style PLACE reads the two the collector sent for it (Z_<PLACE>_HEAVY
# and _INV, each only when set); draw_field() thickens the value from them,
# and zone_plate() lays the black plate. zone_done puts the pens back, so the
# next place is drawn as it asked to be.
ZHEAVY=""; ZINV=""
zone_style() {
    eval "ZHEAVY=\${Z_$1_HEAVY:-}; ZINV=\${Z_$1_INV:-}"
}
zone_plate() {
    # $1=x $2=y $3=w $4=h — the place's rectangle, black when it asked for it
    [ "$ZINV" = "1" ] || return 0
    fill_rect "$1" "$2" "$3" "$4" BLACK
    DRAW_INV=1
}
zone_done() { ZHEAVY=""; ZINV=""; DRAW_INV=""; }

# An indoor reading's caption, over its value at $2 — and the place's plate
# first, from the caption's top to the value's foot, when it asked for one.
in_caption() {
    # $1=x $2=value top $3=cell width $4=value size $5=caption [$6=caption x]
    local ls="${GRID_LAB_SZ:-10}"
    zone_plate "$(( $1 - 6 ))" "$(( $2 - ls - 10 ))" "$3" "$(( ls + 4 + $4 + 12 ))"
    draw_text_reg "${6:-$1}" "$(( $2 - ls - 4 ))" "$ls" "${LAB_INK:-GRAY7}" "$5"
}

draw_hline() {
    # $1=x $2=y $3=width $4=colour — a rectangle one pixel tall, because FBInk
    # has no line primitive and -L is --linecountcode, which took this script's
    # width as a string to print.
    #
    # THE PAGE'S RULES ARE THE READER'S TO SET: GRAYA is "a rule" and GRAYD
    # "the soft rule under the clock", and both are drawn in the collector's
    # RULE_INK / RULE_SOFT, RULE_PX thick and RULE_STYLE — see rule_style().
    # Anything else is drawn as asked.
    local pen="$4"
    case "$pen" in
        GRAYA) rule_pen "${RULE_INK:-}" GRAYA; pen="$RULE_PEN" ;;
        GRAYD) rule_pen "${RULE_SOFT:-}" GRAYD; pen="$RULE_PEN" ;;
        *) fill_rect "$1" "$2" "$3" "${RULE_H:-1}" "$pen"; return ;;
    esac
    rule_h
    draw_rule "$1" "$2" "$3" "$RULE_T" "$pen"
}

# The vertical twin, for the hairline between the columns: $1=x $2=y $3=height
# $4=width from the layout (SEP_W), 0 for none.
draw_vline() {
    [ "${4:-0}" -gt 0 ] 2>/dev/null || return 0
    rule_pen "${RULE_INK:-}" GRAYA
    rule_h
    draw_rule "$1" "$2" "$RULE_T" "$3" "$RULE_PEN"
}

# The pen a rule is drawn in: $1 as the collector sent it, or $2 when it sent
# nothing or something that is not a pen. Into RULE_PEN, not echoed — every
# rule on the page asks, and a subshell each is a fork each.
RULE_PEN=GRAYA
rule_pen() {
    case "$1" in
        BLACK|GRAY[1-9A-E]) RULE_PEN="$1" ;;
        *)                  RULE_PEN="$2" ;;
    esac
}

# How thick a rule is at this panel's size: the collector's RULE_PX, or the
# layout file's RULE_H for a collector that sends none. Into RULE_T.
RULE_T=1
rule_h() {
    RULE_T="${RULE_PX:-${RULE_H:-1}}"
    case "$RULE_T" in ''|*[!0-9]*) RULE_T="${RULE_H:-1}" ;; esac
    [ "$RULE_T" -ge 1 ] 2>/dev/null || RULE_T=1
    [ "$RULE_T" -le 12 ] || RULE_T=12
}

# A rule, solid or not: $1=x $2=y $3=w $4=h $5=pen.
#
# DASHED AND DOTTED ARE AN IMAGE, NOT A ROW OF RECTANGLES. FBInk draws one
# rectangle per process, and a dotted rule across the page is ninety of them —
# seconds of the reader's time on every repaint, for six rules. So the pattern
# is written once as a small BMP, kept in $TMP by its size, pen and style,
# and blitted like the chart. Anything that goes wrong there draws it solid.
draw_rule() {
    local st="${RULE_STYLE:-0}" f
    case "$st" in 1|2) ;; *) fill_rect "$1" "$2" "$3" "$4" "$5"; return ;; esac
    [ "${3:-0}" -gt 0 ] 2>/dev/null && [ "${4:-0}" -gt 0 ] 2>/dev/null || return 0
    f="$TMP/rule_${st}_${5}_${3}x${4}.bmp"
    if [ -s "$f" ] || rule_bmp "$f" "$3" "$4" "$5" "$st"; then
        draw_image "$f" "$1" "$2" && return 0
    fi
    fill_rect "$1" "$2" "$3" "$4" "$5"
}

# Writes a dashed ($5=1) or dotted ($5=2) rule $2 x $3 in pen $4 to $1, as a
# 24-bit BMP: the format the chart already arrives in, so it is one FBInk
# certainly reads. The pattern runs along the rule's length and its steps are
# counted in its thickness, so a heavier rule has longer dashes.
rule_bmp() {
    local f="$1" w="$2" h="$3" pen="$4" st="$5"
    local g on off per row pad size i j p px bg t
    case "$pen" in
        BLACK) g=0 ;;
        GRAY[1-9]) g=$(( ${pen#GRAY} * 17 )) ;;
        GRAYA) g=170 ;; GRAYB) g=187 ;; GRAYC) g=204 ;; GRAYD) g=221 ;; GRAYE) g=238 ;;
        *) return 1 ;;
    esac
    px=$(printf '\\%03o\\%03o\\%03o' "$g" "$g" "$g")
    bg='\377\377\377'
    t="$h"; [ "$w" -lt "$h" ] && t="$w"            # the thickness
    if [ "$st" = "1" ]; then on=$(( t * 5 )); off=$(( t * 3 ))
                        [ "$on" -lt 6 ] && on=6; [ "$off" -lt 4 ] && off=4
    else                on="$t"; off=$(( t * 2 )); [ "$off" -lt 2 ] && off=2
    fi
    per=$(( on + off ))
    pad=$(( (4 - (w * 3) % 4) % 4 ))
    size=$(( 54 + (w * 3 + pad) * h ))
    {
        printf 'BM'; le32 "$size"; le32 0; le32 54
        le32 40; le32 "$w"; le32 "$h"; printf '\001\000\030\000'
        le32 0; le32 $(( size - 54 )); le32 2835; le32 2835; le32 0; le32 0
        if [ "$w" -ge "$h" ]; then
            # Across: one row, the same for every line of the thickness.
            row=""; i=0
            while [ "$i" -lt "$w" ]; do
                if [ $(( i % per )) -lt "$on" ]; then row="$row$px"; else row="$row$bg"; fi
                i=$(( i + 1 ))
            done
            p=0; while [ "$p" -lt "$pad" ]; do row="$row\000"; p=$(( p + 1 )); done
            j=0; while [ "$j" -lt "$h" ]; do printf "$row"; j=$(( j + 1 )); done
        else
            # Down: each line all ink or all paper. BMP rows run bottom up,
            # which for a pattern that starts with ink only moves where the
            # last dash is cut.
            local ink="" paper=""
            i=0; while [ "$i" -lt "$w" ]; do ink="$ink$px"; paper="$paper$bg"; i=$(( i + 1 )); done
            p=0; while [ "$p" -lt "$pad" ]; do ink="$ink\000"; paper="$paper\000"; p=$(( p + 1 )); done
            j=0
            while [ "$j" -lt "$h" ]; do
                if [ $(( j % per )) -lt "$on" ]; then printf "$ink"; else printf "$paper"; fi
                j=$(( j + 1 ))
            done
        fi
    } > "$f.tmp" 2>/dev/null && mv "$f.tmp" "$f" && return 0
    rm -f "$f.tmp"
    return 1
}

# A 32-bit little-endian number, as the four bytes a BMP header wants.
le32() {
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' \
        $(( $1 & 255 )) $(( ($1 >> 8) & 255 )) $(( ($1 >> 16) & 255 )) $(( ($1 >> 24) & 255 )))"
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

# The same, without the flash — what the full tier spends during quiet hours,
# where the whole point is that the panel does not go black at four in the
# morning. Ghosting accumulates instead, and is cleared in one go when the
# quiet hours end.
refresh_screen_plain() { fb -q -s; }

clear_screen()   { fb -q -b -B WHITE -k; }

# ── What the panel knows about itself ────────────────────────────────────────
#
# THE BATTERY BADGE ON THIS PAGE IS THE OUTDOOR NODE'S. The reader's own
# battery — the one that decides whether the panel is on the wall next week —
# appears nowhere at all, on a page whose entire power section exists to make
# it last. One lipc call, on the tier that draws the footer.
BATT=""
batt_read() {
    BATT=$(lipc-get-prop com.lab126.powerd battLevel 2>/dev/null | tr -dc '0-9')
    return 0
}

# The right-hand end of the footer: how full the reader is, which power mode it
# is actually in, and — when the collector is not answering — when it last did.
#
# THE MODE IT IS IN, NOT THE ONE POWER NAMES. A panel inside its wake window is
# awake whatever the setting says, and that is the one thing somebody standing
# in front of it wants confirmed before they start tapping.
#
# ONLY UNDER POWER=suspend, THOUGH: the window is what holds the machine up,
# and it holds nothing else. Under POWER=wifi the radio is still cut between
# fetches during the window, so reporting "awake" for two minutes after every
# tap had the one line whose job is to say which mode the panel is in saying
# the wrong one.
#
# LEFT-ALIGNED AT STAT_X, like everything else here: FBInk will not say how
# wide it drew a string, so a right-aligned footer would be aligned on a guess.
# The layout leaves room for it.
draw_status() {
    [ "${STATUS:-1}" = "1" ] || return 0
    [ "${STAT_X:-0}" -gt 0 ] 2>/dev/null || return 0
    local s="" i=0
    batt_read
    [ -n "$BATT" ] && s="$BATT%"
    if [ "${POWER:-awake}" = "suspend" ] && wake_window; then
        i=0
    else
        case "${POWER:-awake}" in
            wifi)    i=1 ;;
            suspend) i=2 ;;
            *)       i=0 ;;
        esac
    fi
    list_at "${MODE_LBL:-awake|radio off|asleep}" "$i"
    if [ -n "$LIST_ITEM" ]; then
        if [ -n "$s" ]; then s="$s · $LIST_ITEM"; else s="$LIST_ITEM"; fi
    fi
    # In brackets, and only when it is not current: a second time on a page
    # that already has a clock has to be unmistakably not the clock.
    if data_stale && [ -n "${LAST_OK:-}" ]; then
        s="$s ($LAST_OK)"
    fi
    [ -n "$s" ] || return 0
    draw_text_reg "$STAT_X" "${STAT_Y:-${FOOT_Y:-0}}" \
                  "${STAT_SZ:-${FOOT_SZ:-12}}" "GRAY7" "$s"
    return 0
}

# THE READER'S OWN WIFI, on the wall page's footer: five bars, as full as its
# signal, and the network's name — this end's link, the one that decides
# whether the page keeps up, so read here and nothing from the collector.
# The name from wifid, or wpa_supplicant where wifid has none; the signal from
# /proc/net/wireless, whose level is dBm (-65.), dBm + 256 (191.) or a
# percentage, by driver.
WIFI_PROC=/proc/net/wireless
WIFI_SSID=""
WIFI_BARS=0
wifi_read() {
    WIFI_SSID=$(lipc-get-prop com.lab126.wifid currentEssid 2>/dev/null | head -1)
    [ -n "$WIFI_SSID" ] || \
        WIFI_SSID=$(wpa_cli status 2>/dev/null | sed -n 's/^ssid=//p' | head -1)
    # Printable; draw_wifi() cuts it to the room it has.
    WIFI_SSID=$(printf '%s' "$WIFI_SSID" | tr -d '\000-\037')
    local lvl
    lvl=$(awk 'NR > 2 { sub(/\.$/, "", $4); print $4; exit }' "$WIFI_PROC" 2>/dev/null)
    WIFI_BARS=0
    case "$lvl" in
        -[0-9]*) lvl=${lvl#-} ;;
        [0-9]*)  if [ "$lvl" -gt 100 ] 2>/dev/null; then lvl=$(( 256 - lvl ))
                 else WIFI_BARS=$(( (lvl + 19) / 20 )); return 0; fi ;;
        *)       return 0 ;;
    esac
    # dBm as a magnitude: -55 and stronger is all five.
    [ "$lvl" -ge 0 ] 2>/dev/null || return 0
    if   [ "$lvl" -le 55 ]; then WIFI_BARS=5
    elif [ "$lvl" -le 62 ]; then WIFI_BARS=4
    elif [ "$lvl" -le 70 ]; then WIFI_BARS=3
    elif [ "$lvl" -le 78 ]; then WIFI_BARS=2
    elif [ "$lvl" -le 86 ]; then WIFI_BARS=1
    fi
    return 0
}

# The first $2 letters of $1, counted as letters rather than bytes: busybox
# cut counts bytes, and a name cut inside a two-byte letter draws as a box.
utf8_head() {
    printf '%s' "$1" | LC_ALL=C awk -v n="$2" 'BEGIN { RS = "\001" } {
        o = ""; k = 0
        for (i = 1; i <= length($0); i++) {
            c = substr($0, i, 1)
            if (!(c >= "\200" && c < "\300")) { if (k == n) break; k++ }
            o = o c
        }
        printf "%s", o }'
}

# How wide a caption is drawn, in percent of the collector's estimate. The
# estimate is the browser's serif; the Kindle's Bookerly sets a caption 15 %
# wider, which is what the grid's arrow has always been placed by. Centring
# on the wall page goes by the same, so its two columns agree.
cap_pct() { if [ "${WALL:-0}" = "1" ]; then echo 115; else echo 100; fi; }

# Left of the status line, ten type sizes before STAT_X: the bars stand on
# the footer's baseline, filled to the signal and light grey past it, and the
# name is cut to the room left before the status. Not on a collector's own
# access point: the note at the footer's left already names that network,
# and is long enough to run into this.
draw_wifi() {
    [ "${WALL:-0}" = "1" ] && [ "${STAT_X:-0}" -gt 0 ] 2>/dev/null || return 0
    case "${LBL_MEASURED:-}" in "AP "*) return 0 ;; esac
    wifi_read
    local sz="${STAT_SZ:-${FOOT_SZ:-12}}" y="${STAT_Y:-${FOOT_Y:-0}}"
    local x i=1 h pen bw gap base tx n
    x=$(( STAT_X - sz * 10 ))
    bw=$(( sz / 4 )); [ "$bw" -lt 2 ] && bw=2
    gap=$(( sz / 6 )); [ "$gap" -lt 1 ] && gap=1
    base=$(( y + sz * ${BASELINE_MILLE:-848} / 1000 ))
    while [ "$i" -le 5 ]; do
        h=$(( sz * (i + 1) / 6 ))
        pen=GRAYC; [ "$i" -le "$WIFI_BARS" ] && pen=BLACK
        fill_rect "$(( x + (i - 1) * (bw + gap) ))" "$(( base - h ))" "$bw" "$h" "$pen"
        i=$((i + 1))
    done
    # About half a type size a letter, and half a size clear of the status.
    tx=$(( x + 5 * (bw + gap) + sz / 3 ))
    n=$(( (STAT_X - sz / 2 - tx) * 2 / sz ))
    [ -n "$WIFI_SSID" ] && [ "$n" -gt 0 ] && \
        draw_text_reg "$tx" "$y" "$sz" "GRAY7" "$(utf8_head "$WIFI_SSID" "$n")"
    return 0
}

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
#
# When the face could be measured, clock_fit() has the real width of this
# minute's string in CLOCK_PX, and that is what it is centred by instead.
clock_centre_x() {
    # $1=type size  -> CENTRE_X
    local sz="$1" w
    w=$(( sz * ${CLOCK_ADVW:-0} / 1000 ))
    [ "${CLOCK_PX:-0}" -gt 0 ] 2>/dev/null && w="$CLOCK_PX"
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
    # SWITCHED OFF, NOTHING — not even the white fill: upright, the indoor row
    # has moved up into the clock's rectangle.
    [ "${SHOW_CLOCK:-1}" = "1" ] || return 0
    # The clearing fill is per-style, not up front: the boxed clock covers the
    # whole rectangle in black anyway, so a white fill before it was a second
    # fbink process a minute — 1440 forks a day on a ten-year-old ARM device —
    # painting something nothing would ever see.
    case "${CLOCK_STYLE:-0}" in
        1)  # BOXED — knocked out of a black plate, the treatment the current
            # weekday already gets in the week strip. On a screen with no
            # colour a filled block is the one mark that survives dithering
            # unambiguously, which is why the style exists at all.
            clock_fit "${CL_SZ_BOXED:-$CL_SIZE}" "$now_time"; sz="$CLOCK_SZ"
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
            clock_fit "${CL_SZ_RULED:-$CL_SIZE}" "$now_time"; sz="$CLOCK_SZ"
            draw_hline "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" BLACK
            # Set smaller to fit, it keeps its middle where the full size had it.
            cy=$(( Z_CLOCK_Y + ${CL_RULED_PAD:-15} + (${CL_SZ_RULED:-$CL_SIZE} - sz) / 2 ))
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
            clock_fit "${CL_SZ_DATED:-$CL_SIZE}" "$now_time"; sz="$CLOCK_SZ"
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
                text_geom "$CL_Y" "$sz" "$FONT_BOLD"
                draw_text_reg "$CL_X" "$(( TX_TOP + TX_PX + ${CL_DATE_GAP:-6} ))" \
                    "${CL_DATE_SZ:-14}" "GRAY4" "$DATE"
            fi
            ;;
        *)  fill_rect "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" WHITE
            clock_fit "$CL_SIZE" "$now_time"
            draw_text_bold "$CL_X" "$(( CL_Y + (CL_SIZE - CLOCK_SZ) / 2 ))" \
                "$CLOCK_SZ" "BLACK" "$now_time" ;;
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

    [ "$ZHEAVY" = "1" ] && bold=1
    if [ "$bold" = "1" ]; then
        draw_text_bold "$x" "$y" "$sz" "$ink" "$val"
        # Extra bold: the bold face again, a hair to the right — the
        # reader's faces stop at bold. Not on a black plate: that pass paints
        # its own box (draw_text()), which would rub out the first, so a
        # place set white on black stays plain bold on the panel.
        [ "$ZHEAVY" = "1" ] && [ -z "${DRAW_INV:-}" ] && \
            draw_text_bold "$(( x + (sz / 40 > 1 ? sz / 40 : 1) ))" "$y" "$sz" "$ink" "$val"
    else
        draw_text_reg  "$x" "$y" "$sz" "$ink" "$val"
    fi
    val_adv "$bold" "$val" "$vadv"
    ux=$(( x + sz * VAL_ADV / 1000 ))

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
            draw_text_reg "$ux" "$(baseline_y "$y" "$sz" "$usz" "$bold" 0)" "$usz" "GRAY4" "$unit"
        fi
        ux=$(( ux + usz * uadv / 1000 ))
    fi

    if [ -n "$arrow" ]; then
        asz=$(( sz * 50 / 100 ))
        [ "$asz" -lt 10 ] && asz=10
        local vb="${BASELINE_MILLE:-848}"
        [ "$bold" = "1" ] && vb="${BASELINE_MILLE_BOLD:-$vb}"
        draw_arrow "$(( ux + sz / 10 ))" "$(( y + sz * vb / 1000 ))" \
                   "$asz" "$arrow" "GRAY4"
        ux=$(( ux + sz / 10 + asz * 62 / 100 ))
    fi
}

# How wide draw_field() will draw a field, into FIELD_W: the value, its unit
# and the gaps draw_field() leaves, and the arrow. The same arithmetic, so a
# centred field is centred on what is actually drawn.
FIELD_W=0
field_w() {
    # $1=size $2=value $3=unit $4=arrow $5=value advance $6=unit advance $7=bold
    local sz="$1" usz
    val_adv "${7:-0}" "$2" "${5:-0}"
    FIELD_W=$(( sz * VAL_ADV / 1000 ))
    if [ -n "$3" ]; then
        if [ "$3" = "°" ]; then usz=$(( sz * 34 / 100 ))
        else                    usz=$(( sz * 42 / 100 ))
        fi
        [ "$usz" -lt 9 ] && usz=9
        case "$3" in '°'|'%') ;; *) FIELD_W=$(( FIELD_W + sz / 12 )) ;; esac
        FIELD_W=$(( FIELD_W + usz * ${6:-0} / 1000 ))
    fi
    if [ -n "$4" ]; then
        usz=$(( sz * 50 / 100 )); [ "$usz" -lt 10 ] && usz=10
        FIELD_W=$(( FIELD_W + sz / 10 + usz * 62 / 100 ))
    fi
}

# ── The tendency arrow, drawn rather than typeset ────────────────────────────
# THE PANEL'S FONTS HAVE NO ARROWS. Bookerly, Caecilia and the rest of what a
# Kindle ships are book faces: U+2191..U+2198 are not in them, and FBInk draws
# a missing glyph as an empty box — which is what the pressure tendency was on
# the panel, while the browser page, with a system font to fall back on, drew
# it fine. So the five arrows the collector sends are drawn from rectangles:
# a shaft and a head, sitting on the baseline of the value beside them, at the
# size that value's arrow would have been set in.
#
# A diagonal shaft is a stair of small squares; its head is the two sides of
# the corner it points into. A straight one is one bar with a chevron of
# squares for its head. Something the collector sends that is not one of the
# five is typeset as before.
draw_arrow() {
    # $1=x $2=baseline y $3=size $4=the arrow $5=colour
    local x="$1" base="$2" sz="$3" a="$4" col="${5:-GRAY4}"
    local s t st hl top cx cy i
    s=$(( sz * 62 / 100 ));  [ "$s" -lt 7 ] && s=7
    t=$(( s / 7 ));          [ "$t" -lt 2 ] && t=2
    st=$(( t / 2 ));         [ "$st" -lt 1 ] && st=1
    hl=$(( s * 45 / 100 ))                  # the head's arms
    top=$(( base - s ))
    cx=$(( x + (s - t) / 2 )); cy=$(( top + (s - t) / 2 ))
    case "$a" in
        '↘')
            i=0
            while [ "$i" -le $(( s - t )) ]; do
                fill_rect $(( x + i )) $(( top + i )) "$t" "$t" "$col"
                i=$(( i + st ))
            done
            fill_rect $(( x + s - hl )) $(( top + s - t )) "$hl" "$t" "$col"
            fill_rect $(( x + s - t )) $(( top + s - hl )) "$t" "$hl" "$col" ;;
        '↗')
            i=0
            while [ "$i" -le $(( s - t )) ]; do
                fill_rect $(( x + i )) $(( top + s - t - i )) "$t" "$t" "$col"
                i=$(( i + st ))
            done
            fill_rect $(( x + s - hl )) "$top" "$hl" "$t" "$col"
            fill_rect $(( x + s - t )) "$top" "$t" "$hl" "$col" ;;
        '→')
            fill_rect "$x" "$cy" "$s" "$t" "$col"
            i=0
            while [ "$i" -le "$hl" ]; do
                fill_rect $(( x + s - t - i )) $(( cy - i )) "$t" "$t" "$col"
                fill_rect $(( x + s - t - i )) $(( cy + i )) "$t" "$t" "$col"
                i=$(( i + st ))
            done ;;
        '↑')
            fill_rect "$cx" "$top" "$t" "$s" "$col"
            i=0
            while [ "$i" -le "$hl" ]; do
                fill_rect $(( cx - i )) $(( top + i )) "$t" "$t" "$col"
                fill_rect $(( cx + i )) $(( top + i )) "$t" "$t" "$col"
                i=$(( i + st ))
            done ;;
        '↓')
            fill_rect "$cx" "$top" "$t" "$s" "$col"
            i=0
            while [ "$i" -le "$hl" ]; do
                fill_rect $(( cx - i )) $(( top + s - t - i )) "$t" "$t" "$col"
                fill_rect $(( cx + i )) $(( top + s - t - i )) "$t" "$t" "$col"
                i=$(( i + st ))
            done ;;
        *)
            draw_text_reg "$x" $(( base - sz * ${BASELINE_MILLE:-848} / 1000 )) \
                          "$sz" "$col" "$a" ;;
    esac
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
    # [$4=1 when the larger is bold  $5=1 when this one is] — the two faces'
    # baselines are not quite at one fraction of the size.
    local bl="${BASELINE_MILLE:-848}" bs="${BASELINE_MILLE:-848}"
    [ "${4:-0}" = "1" ] && bl="${BASELINE_MILLE_BOLD:-$bl}"
    [ "${5:-0}" = "1" ] && bs="${BASELINE_MILLE_BOLD:-$bs}"
    echo $(( $1 + ($2 * bl - $3 * bs) / 1000 ))
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
    local z val unit lab arrow bold vadv uadv ink n i cx cw vsz y ladv lcx vcx capu capa

    # ── Left column: the outdoor headline ───────────────────────────────────
    # White on black, when the headline asked for it: the whole of its row,
    # heading and the line under it included.
    zone_style HERO
    local hb=$(( ${SUB_Y:-128} + ${SUB_SZ:-14} + 6 )) band=""
    [ "${HEAD_RULE_Y:-0}" -gt 0 ] 2>/dev/null && hb=$(( HEAD_RULE_Y - 4 ))
    # Down to the band under it, when there is one, with white between.
    sub_band && band=1 && hb=$(( ${SUB_Y:-128} - 8 ))
    zone_plate "$(( lx - 6 ))" "$(( ${TOP_Y:-20} - 6 ))" \
               "$(( ${HEAD_W:-${COL_L_W:-270}} + 12 ))" "$(( hb - ${TOP_Y:-20} + 6 ))"
    # The outdoor line behind it, on the plate, so the figures are drawn over it.
    [ "${HERO_LINE:-0}" = "1" ] && hero_line_h && \
        draw_line HERO "$lx" "${HERO_Y:-38}" "${HEAD_W:-564}" "$LINE_H"
    draw_text_reg "$lx" "${TOP_Y:-20}" "$lab_sz" "${LAB_INK:-GRAY7}" "$Z_GROUP_OUT"

    local hero_sz="${HERO_SZ:-84}" hero_y="${HERO_Y:-38}" hx="$lx" sx0="$lx"
    # NOTHING BESIDE IT, SO IT IS CENTRED in the column, and the line under it
    # with it — as the page's .head.ctr and .sub.ctr. The widths are the
    # collector's; an older one sends none and both stay at the left edge.
    if [ -z "${Z_BIG_VALUE:-}" ]; then
        field_w "$hero_sz" "$Z_HERO_VALUE" "$Z_HERO_UNIT" "$Z_HERO_ARROW" \
                "${Z_HERO_VADVW:-0}" "${Z_HERO_UADVW:-0}" "${Z_HERO_BOLD:-0}"
        [ "${Z_HERO_VADVW:-0}" -gt 0 ] 2>/dev/null && \
            centre_in "$lx" "${HEAD_W:-${COL_L_W:-270}}" "$FIELD_W" && hx="$CENTRE_X"
        centre_in "$lx" "${HEAD_W:-${COL_L_W:-270}}" \
                  "$(( ${SUB_SZ:-14} * ${Z_SUB_ADVW:-0} / 1000 ))"
        sx0="$CENTRE_X"
    fi
    draw_field "$hx" "$hero_y" "$hero_sz" "${Z_HERO_BOLD:-0}" \
               "$Z_HERO_VALUE" "$Z_HERO_UNIT" "$Z_HERO_ARROW" \
               "${Z_HERO_VADVW:-0}" "${Z_HERO_UADVW:-0}" "${Z_HERO_INK:-BLACK}"

    # The slash and the second value, after the whole of the headline. The
    # collector measured that width for us — see kdAdvanceMille() — because
    # FBInk will not say how wide it drew something and ${#var} counts bytes,
    # which makes "8.4°" five characters long.
    if [ -n "${Z_BIG_VALUE:-}" ]; then
        local big_sz="${BIG_SZ:-44}"
        # As draw_field() drew it: the figures as wide as they came out and
        # the unit at its own small size. Z_HERO_ADVW counted the degree at the
        # headline's full size, which put the slash a long way after it; it is
        # only what an older collector, with no Z_HERO_VADVW, leaves to go by.
        local hw=$(( hero_sz * ${Z_HERO_ADVW:-0} / 1000 ))
        if [ "${Z_HERO_VADVW:-0}" -gt 0 ] 2>/dev/null; then
            field_w "$hero_sz" "$Z_HERO_VALUE" "$Z_HERO_UNIT" "$Z_HERO_ARROW" \
                    "$Z_HERO_VADVW" "${Z_HERO_UADVW:-0}" "${Z_HERO_BOLD:-0}"
            hw="$FIELD_W"
        fi
        local sx=$(( lx + hw + ${HEAD_GAP:-8} ))
        # The slash is always the regular face, the value may be bold: each
        # on the headline's baseline by its own face's numbers.
        draw_text_reg "$sx" "$(baseline_y "$hero_y" "$hero_sz" "$big_sz" "${Z_HERO_BOLD:-0}" 0)" \
                      "$big_sz" "${SLASH_INK:-GRAYA}" "/"
        y=$(baseline_y "$hero_y" "$hero_sz" "$big_sz" "${Z_HERO_BOLD:-0}" "${Z_BIG_BOLD:-0}")
        ZHEAVY="${Z_BIG_HEAVY:-}"
        draw_field "$(( sx + ${SLASH_W:-22} ))" "$y" "$big_sz" "${Z_BIG_BOLD:-0}" \
                   "$Z_BIG_VALUE" "$Z_BIG_UNIT" "$Z_BIG_ARROW" \
                   "${Z_BIG_VADVW:-0}" "${Z_BIG_UADVW:-0}" "${Z_BIG_INK:-BLACK}"
    fi

    # The 24 h low-to-high and the age, composed by the collector so that the
    # wording, the unit and the rounding are the page's and not this script's.
    [ -n "${Z_SUB:-}" ] && [ -z "$band" ] && \
        draw_text_reg "$sx0" "${SUB_Y:-128}" "${SUB_SZ:-14}" "${LAB_INK:-GRAY7}" "$Z_SUB"
    zone_done
    if [ -n "$band" ]; then
        # On the wall page's band instead: across the page in its shade, the
        # range centred on it — which is the line under the headline's row.
        fill_rect "$lx" "$(( ${SUB_Y:-128} - 6 ))" "${HEAD_W:-564}" "$(( ${SUB_SZ:-14} + 12 ))" "$SUB_BAND"
        centre_in "$lx" "${HEAD_W:-564}" "$(( ${SUB_SZ:-14} * ${Z_SUB_ADVW:-0} / 1000 ))"
        draw_text_on "$CENTRE_X" "${SUB_Y:-128}" "${SUB_SZ:-14}" "$SUB_INK" "$SUB_BAND" "$Z_SUB"
    # The wall page's rule under the headline's row, which runs the width of
    # the page: the grid and the indoor readings are under it, side by side.
    elif [ "${HEAD_RULE_Y:-0}" -gt 0 ] 2>/dev/null; then
        draw_hline "$lx" "$HEAD_RULE_Y" "${HEAD_W:-564}" "GRAYA"
    fi

    # ── Left column: the grid ───────────────────────────────────────────────
    # GRID_ROWS says how many cells are on each row; each row then divides its
    # own width by its own count, so two cells are two halves rather than two of
    # three thirds with the last one left white.
    local gy="${GRID_Y:-150}" gcols gvsz gcw gi=0 grows="${GRID_ROWS:-}"
    # The collector's layout may break them differently — two one under the
    # other rather than side by side, when that sets them larger — and sizes
    # every row alike. GRID_ROWS stays the fixed page's answer for a reader
    # that has no layout to follow.
    [ "${LAYOUT_FLOW:-0}" = "1" ] && grows="${LY_GRID_ROWS:-}"
    set -- ${GRID_ZONES:-}
    local gcell
    for gcols in $grows; do
        gcw=$(( ${COL_L_W:-270} / gcols ))
        if [ "$gcols" -ge 3 ]; then gvsz="${GRID_VAL_SZ_3:-26}"
        else                        gvsz="${GRID_VAL_SZ:-31}"
        fi
        # The wall page's hairlines between the places: across at the middle
        # of the air above every row but the first, and down 8 px left of
        # each cell after the first — the page's appendWallBody().
        gcell=$(( ${GRID_LAB_SZ:-10} + 4 + gvsz ))
        if [ "${WALL:-0}" = "1" ] && [ "$gy" -gt "${GRID_Y:-150}" ]; then
            draw_hline "$lx" "$(( gy - (${GRID_ROW_H:-46} - gcell) / 2 ))" \
                       "$(( ${COL_L_W:-270} - 8 ))" GRAYA
        fi
        gi=0
        while [ "$gi" -lt "$gcols" ] && [ -n "${1:-}" ]; do
            z="$1"; shift
            if [ "${WALL:-0}" = "1" ] && [ "$gi" -gt 0 ]; then
                draw_vline "$(( lx + gi * gcw - 8 ))" "$(( gy - 2 ))" "$(( gcell + 4 ))" 1
            fi
            eval "val=\$Z_${z}_VALUE; unit=\$Z_${z}_UNIT; lab=\$Z_${z}_LABEL"
            eval "arrow=\$Z_${z}_ARROW; bold=\$Z_${z}_BOLD; ink=\${Z_${z}_INK:-BLACK}"
            eval "vadv=\${Z_${z}_VADVW:-0}; uadv=\${Z_${z}_UADVW:-0}"
            # The wall page's unit, on the caption's line rather than after
            # the figures — the collector has already taken it off the value.
            eval "capu=\${Z_${z}_CAPUNIT:-}"
            [ -n "$capu" ] && lab="$lab / $capu"
            # ...and its tendency arrow after the caption, which the collector
            # left out of the figures' size on this page.
            capa=""
            if [ "${WALL:-0}" = "1" ] && [ -n "$arrow" ]; then capa="$arrow"; arrow=""; fi
            cx=$(( lx + gi * gcw ))
            # A row of one is centred, caption and value each on their own —
            # the page's .grid td.c1 — and every cell is on the wall page.
            # Widths of 0 (an older collector) leave both at the cell's left
            # edge. The caption's arrow, after it, is counted in with it.
            lcx="$cx"; vcx="$cx"
            if [ "$gcols" = "1" ] || [ "${WALL:-0}" = "1" ]; then
                eval "ladv=\${Z_${z}_LADVW:-0}"
                # A wall caption is measured as the arrow is placed, with
                # Bookerly's 15 % (CAP_PCT); the desk's row of one as it was.
                local lw=$(( ${GRID_LAB_SZ:-10} * ladv * $(cap_pct) / 100000 ))
                [ -n "$capa" ] && lw=$(( lw + ${GRID_LAB_SZ:-10} * 2 ))
                [ "$ladv" -gt 0 ] 2>/dev/null || lw=0
                centre_in "$cx" "$gcw" "$lw"
                lcx="$CENTRE_X"
                if [ -n "$val" ] && [ "$vadv" -gt 0 ] 2>/dev/null; then
                    field_w "$gvsz" "$val" "$unit" "$arrow" "$vadv" "$uadv" "$bold"
                    centre_in "$cx" "$gcw" "$FIELD_W"
                    vcx="$CENTRE_X"
                fi
            fi
            zone_style "$z"
            zone_plate "$(( cx - 6 ))" "$(( gy - 6 ))" "$(( gcw - 6 ))" \
                       "$(( ${GRID_LAB_SZ:-10} + 4 + gvsz + 12 ))"
            # Its own 24 h line behind the figures, on the plate, as the
            # headline's.
            if [ "${WALL:-0}" = "1" ] && eval "[ \"\${Z_${z}_LINE:-}\" = 1 ]"; then
                draw_line "$z" "$cx" "$(( gy + ${GRID_LAB_SZ:-10} + 4 ))" "$(( gcw - 14 ))" "$gvsz"
            fi
            draw_text_reg "$lcx" "$gy" "${GRID_LAB_SZ:-10}" "${LAB_INK:-GRAY7}" "$lab"
            if [ -n "$capa" ]; then
                eval "ladv=\${Z_${z}_LADVW:-0}"
                # After the caption by its estimate and 15 % more, and half
                # its size of air: the estimate is the browser's serif, and
                # the Kindle's Bookerly sets a caption with a unit wider —
                # by a third of its size the arrow touched "hPa".
                draw_arrow "$(( lcx + ${GRID_LAB_SZ:-10} * ladv * 115 / 100000 + ${GRID_LAB_SZ:-10} / 2 ))" \
                           "$(( gy + ${GRID_LAB_SZ:-10} * ${BASELINE_MILLE:-848} / 1000 ))" \
                           "$(( ${GRID_LAB_SZ:-10} * 3 / 2 ))" "$capa" "${LAB_INK:-GRAY7}"
            fi
            draw_field "$vcx" "$(( gy + ${GRID_LAB_SZ:-10} + 4 ))" "$gvsz" \
                       "$bold" "$val" "$unit" "$arrow" "$vadv" "$uadv" "$ink"
            zone_done
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
        # Upright without the clock there is nothing above the row to rule
        # it off from: it starts at the top, level with the outdoor heading.
        # On the wall page the row is a column of its own beside the grid,
        # under the rule across the page, and needs none either.
        if { [ "${SHOW_CLOCK:-1}" = "1" ] || [ "${LAND:-0}" = "1" ]; } &&
           [ "${WALL:-0}" != "1" ]; then
            draw_hline "$rx" "${IN_RULE_Y:-126}" "$rw" "GRAYD"
        fi
        # Centred over the column on the wall page, as everything in it is.
        CENTRE_X="$rx"
        [ "${WALL:-0}" = "1" ] && centre_in "$rx" "$rw" "$(( lab_sz * ${Z_GROUP_IN_ADVW:-0} * $(cap_pct) / 100000 ))"
        draw_text_reg "$CENTRE_X" "${IN_LAB_Y:-134}" "$lab_sz" "${LAB_INK:-GRAY7}" "$Z_GROUP_IN"

        # The first field gets more of the row, not an equal share: it is set
        # larger, so equal columns crowd it against its neighbour while leaving
        # the small ones space they do not need.
        local w1
        if [ -n "${IN_W1:-}" ]; then
            # The collector's layout: what the first field needs of the row,
            # in thousandths, rather than a fixed share.
            w1=$(( rw * IN_W1 / 1000 ))
        elif [ "$n" -ge 3 ]; then w1=$(( rw * 42 / 100 ))
        elif [ "$n" -eq 2 ]; then w1=$(( rw * 58 / 100 ))
        else w1="$rw"
        fi
        cw=$(( n > 1 ? (rw - w1) / (n - 1) : rw ))

        # ALL THREE VALUES SIT ON ONE BOTTOM EDGE. The first has no caption —
        # the heading above already says which room this is — so it is half
        # again as tall and starts higher.
        local big="${IN_VAL_SZ_1:-52}" small="${IN_VAL_SZ:-28}"
        local big_y="${IN_VAL_Y:-158}"
        local small_y="${IN_VAL2_Y:-$(( big_y + big - small ))}"
        # OR THE FIRST ON A LINE OF ITS OWN, the others under it sharing the
        # whole width — the collector's choice, when it sets the first larger.
        local stack=0
        if [ "${IN_STACK:-0}" = "1" ] && [ "$n" -gt 1 ]; then
            stack=1
            w1="$rw"
            cw=$(( rw / (n - 1) ))
        fi
        # OR THREE AS TWO COLUMNS: the first on the left, the other two one
        # above the other beside it, the lower one on the first one's bottom
        # line — the collector's IN_COL.
        local col=0 low_y
        if [ "${IN_COL:-0}" = "1" ] && [ "$n" -eq 3 ] && [ "$stack" = "0" ]; then
            col=1
            low_y="${IN_VAL3_Y:-$(( big_y + big - small ))}"
        fi
        # OR ONE UNDER THE OTHER, each at the column's left edge with its
        # caption over it — the wall page's IN_VCOL.
        local vcol=0
        if [ "${IN_VCOL:-0}" = "1" ]; then
            vcol=1; col=0; stack=0
            low_y="${IN_VAL3_Y:-$small_y}"
        fi

        i=0
        for z in $IN_ZONES; do
            eval "val=\$Z_${z}_VALUE; unit=\$Z_${z}_UNIT; lab=\$Z_${z}_LABEL"
            eval "arrow=\$Z_${z}_ARROW; bold=\$Z_${z}_BOLD; ink=\${Z_${z}_INK:-BLACK}"
            eval "vadv=\${Z_${z}_VADVW:-0}; uadv=\${Z_${z}_UADVW:-0}"
            zone_style "$z"
            if [ "$i" = "0" ]; then
                cx="$rx"; vsz="$big"; y="$big_y"
                # Alone, it stands in the middle of the column.
                if [ "$n" -eq 1 ] && [ -n "$val" ] && [ "$vadv" -gt 0 ] 2>/dev/null; then
                    field_w "$big" "$val" "$unit" "$arrow" "$vadv" "$uadv" "$bold"
                    centre_in "$rx" "$rw" "$FIELD_W"
                    cx="$CENTRE_X"
                fi
                zone_plate "$(( rx - 6 ))" "$(( y - 6 ))" \
                           "$(( (vcol || stack || n == 1 ? rw : w1) ))" "$(( vsz + 12 ))"
            elif [ "$vcol" = "1" ]; then
                cx="$rx"; vsz="$small"
                if [ "$i" = "1" ]; then y="$small_y"; else y="$low_y"; fi
                CENTRE_X="$rx"
                if [ "${WALL:-0}" = "1" ]; then
                    eval "ladv=\${Z_${z}_LADVW:-0}"
                    centre_in "$rx" "$rw" "$(( ${GRID_LAB_SZ:-10} * ladv * $(cap_pct) / 100000 ))"
                fi
                in_caption "$cx" "$y" "$rw" "$vsz" "$lab" "$CENTRE_X"
            elif [ "$col" = "1" ]; then
                cx=$(( rx + w1 )); vsz="$small"
                if [ "$i" = "1" ]; then y="$small_y"; else y="$low_y"; fi
                in_caption "$cx" "$y" "$(( rw - w1 ))" "$vsz" "$lab"
            elif [ "$stack" = "1" ]; then
                cx=$(( rx + (i - 1) * cw )); vsz="$small"; y="$small_y"
                in_caption "$cx" "$y" "$cw" "$vsz" "$lab"
            else
                cx=$(( rx + w1 + (i - 1) * cw )); vsz="$small"; y="$small_y"
                in_caption "$cx" "$y" "$cw" "$vsz" "$lab"
            fi
            # The wall page's column centres each value under its caption.
            if [ "$vcol" = "1" ] && [ "${WALL:-0}" = "1" ] && [ -n "$val" ] &&
               [ "$vadv" -gt 0 ] 2>/dev/null; then
                field_w "$vsz" "$val" "$unit" "$arrow" "$vadv" "$uadv" "$bold"
                centre_in "$rx" "$rw" "$FIELD_W"
                cx="$CENTRE_X"
            fi
            draw_field "$cx" "$y" "$vsz" "$bold" "$val" "$unit" "$arrow" \
                       "$vadv" "$uadv" "$ink"
            zone_done
            i=$((i + 1))
        done
    fi

    # ── The hairline between the columns ────────────────────────────────────
    if [ "${SEP_W:-1}" -gt 0 ] 2>/dev/null; then
        draw_vline "${SEP_X:-300}" "${SEP_Y:-${TOP_Y:-20}}" "${SEP_H:-210}" "${SEP_W:-1}"
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
        local tx="$TEND_X"
        if [ -n "$OUT_TEND_ARROW" ]; then
            draw_arrow "$TEND_X" $(( TEND_Y + TEND_SZ * ${BASELINE_MILLE:-848} / 1000 )) \
                       "$TEND_SZ" "$OUT_TEND_ARROW" "GRAY4"
            tx=$(( TEND_X + TEND_SZ ))
        fi
        draw_text_reg "$tx" "$TEND_Y" "$TEND_SZ" "GRAY4" "$OUT_TEND ($OUT_TEND_DELTA)"
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
        # Not on the landscape page's narrower chart, which has no room for it.
        if [ -n "${LBL_KEY_BAND:-}" ] && [ "${KEY_BAND:-1}" = "1" ] &&
           [ "${KEY_OUT_ADVW:-0}" -gt 0 ] 2>/dev/null; then
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
        y=$(( GR_Y + CH_T + (CH_B - CH_T) * k / 4 + base - sz * ${BASELINE_MILLE:-848} / 1000 ))
        if [ -n "$lab" ]; then
            x=$(( GR_X + CH_L - gap - sz * w / 1000 ))
            draw_text_reg "$x" "$y" "$sz" "GRAY7" "$lab"
        fi
        # The second line's own value on the same rule, left-aligned right
        # of the plot — sent only when it has a scale of its own (CH_Z*).
        eval "lab=\${CH_Z${k}:-}"
        [ -n "$lab" ] && draw_text_reg "$(( GR_X + CH_R + gap ))" "$y" "$sz" "GRAY7" "$lab"
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

    # NOTHING RECORDED YET IS NOT THE SAME AS NOTHING HAPPENING, so an empty
    # record is drawn as the chart it will become — the grid and the hour axis,
    # no scale down the side (the collector sends none) — with the sentence the
    # page prints inside the plot. CH_NOTE carries it, so the wording and the
    # language are the page's. Without the image the sentence stands alone.
    if graph_fits "$TMP/graph.bmp" && draw_image "$TMP/graph.bmp" "$GR_X" "$GR_Y"; then
        draw_chart_axis
        if [ -n "${CH_NOTE:-}" ]; then
            # At the axis size plus a little, so the Bulgarian sentence still
            # fits the narrower chart beside the readings on a turned page.
            local nsz=$(( ${AX_SZ:-11} + 2 ))
            draw_text_reg "$(( GR_X + ${CH_L:-40} + 8 ))" \
                "$(( GR_Y + (${CH_T:-10} + ${CH_B:-174}) / 2 - nsz / 2 ))" \
                "$nsz" "GRAY5" "$CH_NOTE"
        fi
        # Names no line on an empty record: the collector sends CHART_OUT and
        # CHART_IN as 0 there, and the key draws only what those name.
        draw_chart_key
        return 0
    fi
    if [ -n "${CH_NOTE:-}" ]; then
        draw_text_reg "${GR_X:-20}" "$(( ${GR_Y:-278} + ${GR_H:-200} / 3 ))" \
                      "${LAB_SZ:-16}" "GRAY5" "$CH_NOTE"
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
    # NOT EVEN THE RULE on the standalone page: RULE3_Y is the week heading's
    # own rule there, and a second hairline one pixel above it is a two-pixel
    # line nobody asked for. The week strip and footer below are drawn by the
    # rest of this function, which is why the return is here and not at the top.
    # The wall page's band holds the clock too, so it is ruled off with or
    # without a forecast in it.
    { fc_wanted || [ "${WALL:-0}" = "1" ]; } && draw_hline "$RULE3_X" "$RULE3_Y" "$RULE3_W" "GRAYA"
    # ...and the hairline between the clock and the forecast.
    [ "${SEP2_H:-0}" -gt 0 ] 2>/dev/null && \
        draw_vline "$SEP2_X" "$SEP2_Y" "$SEP2_H" "${SEP_W:-1}"

    if fc_wanted && [ -n "$FC_SUMMARY" ]; then
        local lsz="$LAB_SZ"
        [ "${WALL:-0}" = "1" ] && lsz="${GROUP_LAB_SZ:-$LAB_SZ}"
        # No heading on the wall page: the condition word stands in its place.
        [ "${WALL:-0}" = "1" ] || \
            draw_text_reg "$LAB_FC_X" "$LAB_FC_Y" "$lsz" "${LAB_INK:-GRAY7}" "$LBL_FORECAST"

        # FC_ICON, NOT FC_CODE. There are eleven icon files, one per range of
        # WMO codes, and this script cannot reduce a code to its range — so it
        # asked for fc_2_52.bmp on a partly-cloudy day, did not find it, and
        # drew fc_-1, the circled question mark that is supposed to mean "no
        # forecast". The collector reduces it now (weatherIconCode), which is
        # also where the browser page's ranges live, so the two cannot disagree.
        # FC_CODE is the fallback for a collector too old to send FC_ICON.
        # The wall page's is larger (FC_WALL_SZ): it is what reads across a
        # room. A layout from before it has none, and keeps the desk's size.
        local isz="$FC_MAIN_SZ"
        [ "${WALL:-0}" = "1" ] && [ -n "${FC_WALL_SZ:-}" ] && isz="$FC_WALL_SZ"
        local icon="$ICON_DIR/fc_${FC_ICON:-$FC_CODE}_${isz}.bmp"
        [ ! -f "$icon" ] && icon="$ICON_DIR/fc_-1_${isz}.bmp"
        draw_image "$icon" "$FC_ICON_X" "$FC_ICON_Y"

        # On the wall page the word is set as large as its column allows, up
        # to FC_TEXT_SZ, and kept on the icon's middle: FC_TEXT_W is that
        # column and FC_SUMMARY_ADVW the word's width, measured by the
        # collector because FBInk will not say.
        local tsz="$FC_TEXT_SZ" ty="$FC_TEXT_Y" fit
        if [ "${FC_TEXT_W:-0}" -gt 0 ] 2>/dev/null && [ "${FC_SUMMARY_ADVW:-0}" -gt 0 ] 2>/dev/null; then
            fit=$(( FC_TEXT_W * 1000 / FC_SUMMARY_ADVW ))
            [ "$fit" -lt "$tsz" ] && tsz="$fit"
            [ "$tsz" -lt $(( FC_TEXT_SZ * 2 / 5 )) ] && tsz=$(( FC_TEXT_SZ * 2 / 5 ))
            ty=$(( FC_TEXT_Y + (FC_TEXT_SZ - tsz) / 2 ))
        fi
        # Bold on the wall page, where it is read from across the room.
        # And centred over the forecast's column there, as the page's .ctr:
        # by the regular face's measure and a twentieth more for the bold.
        if [ "${WALL:-0}" = "1" ]; then
            centre_in "$FC_TEXT_X" "${FC_TEXT_W:-0}" "$(( tsz * ${FC_SUMMARY_ADVW:-0} * 105 / 100000 ))"
            draw_text_bold "$CENTRE_X" "$ty" "$tsz" "BLACK" "$FC_SUMMARY"
        else
            draw_text_reg "$FC_TEXT_X" "$ty" "$tsz" "BLACK" "$FC_SUMMARY"
        fi
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
        if [ "${WALL:-0}" = "1" ]; then
            # Beside the icon, one to a line: "вятър", "4 km/h", the age.
            local wy="$FC_WIND_Y" wl wn=""
            [ -n "$FC_WIND" ] && [ "$FC_WIND" != "0" ] && wn=1
            for wl in "${wn:+$LBL_WIND}" "${wn:+$FC_WIND km/h}" "$FC_AGE"; do
                [ -n "$wl" ] || continue
                draw_text_reg "$FC_WIND_X" "$wy" "$FC_WIND_SZ" "GRAY4" "$wl"
                wy=$(( wy + FC_WIND_SZ * 4 / 3 ))
            done
        elif [ -n "$fc_sub" ]; then
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
        local plate_w="${OL_PLATE_W:-0}" ol_w ol_n="${OL_N:-3}"
        # Three upright, five on its side — OL_N, from the collector's layout.
        # None on the wall page, where the clock has the band's other half.
        case "$ol_n" in [0-5]) ;; *) ol_n=3 ;; esac
        i=0
        while [ "$i" -lt "$ol_n" ]; do
            eval "ol_label=\$FC${i}_LABEL"
            eval "ol_code=\${FC${i}_ICON:-\$FC${i}_CODE}"
            eval "ol_temp=\$FC${i}_TEMP"
            eval "ol_x=\$OL${i}_X"
            eval "ol_y=\$OL${i}_Y"

            if [ -z "$ol_label" ] || [ -z "$ol_x" ]; then i=$((i + 1)); continue; fi

            # NOT AN OPTIONAL SHAPE, whatever this guard suggests: ol_centre
            # below centres all three of the label, the icon and the
            # temperature inside OL_PLATE_W, so a layout without it stacks them
            # at ol_x - width/2 — and the icons carry the plate's grey as their
            # own ground, so they would be three grey squares on white. Both
            # layouts set it and drive_dash.sh fails a layout that does not.
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
            i=$((i + 1))
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
    # Upright it is part of this band; on its side it is beside the clock,
    # in the top row — see draw_top_row().
    if [ "${SHOW_WEEK:-1}" = "1" ] && [ "${LAND:-0}" != "1" ]; then
        draw_week
    fi

    draw_hline "$FOOT_RULE_X" "$FOOT_RULE_Y" "$FOOT_RULE_W" "GRAYA"
    draw_text_reg "$FOOT_X" "$FOOT_Y" "$FOOT_SZ" "GRAY5" "$LBL_MEASURED"
    draw_wifi
    draw_status
}

# ── The week strip ───────────────────────────────────────────────────────────
draw_week() {
    # WHAT THE STRIP HOLDS: the calendar week, or — WK_FC=1 — seven
    # days' forecast in the same cells. The collector decides, because it is
    # the end that knows whether there is a fresh forecast to put there.
    if [ "${WK_FC:-0}" = "1" ]; then
        draw_week_fc
        return
    fi

    # Week heading (month)
    if [ -n "${WK_MON_MONTH:-}" ]; then
        draw_hline "${WK_HDG_RULE_X:-$FOOT_RULE_X}" "${WK_HDG_RULE_Y:-$WK_Y}" \
                   "${WK_HDG_RULE_W:-$FOOT_RULE_W}" "GRAYA"
        local wk_heading="$WK_MON_MONTH"
        [ -n "${WK_SUN_MONTH:-}" ] && wk_heading="$wk_heading – $WK_SUN_MONTH"
        draw_text_reg "${WK_HDG_X:-$WK_X}" "${WK_HDG_Y:-$WK_Y}" \
                      "${WK_HDG_SZ:-$LAB_SZ}" "GRAY7" "$wk_heading"
    fi

    # HOW THE CELLS ARE DRAWN: WK_STYLE, the page's .wd rules. 0 filled grey
    # (the weekend darker), 1 an outline round each, 2 no cells at all, 3 no
    # cells and today underlined rather than inverted. Without a filled cell
    # the weekend says so with a darker name.
    local st="${WK_STYLE:-0}" wk_npen wk_t
    case "$st" in 0|1|2|3) ;; *) st=0 ;; esac
    rule_h; wk_t="$RULE_T"; mark_t
    rule_pen "${RULE_INK:-}" GRAYA

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

        # The outline, shared between neighbours as the page's collapsed
        # borders are: each cell draws its left, top and bottom, and the last
        # one its right as well.
        if [ "$st" = "1" ]; then
            fill_rect "$wk_x" "$WK_Y" "$wk_t" "$WK_CELL_H" "$RULE_PEN"
            fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$wk_t" "$RULE_PEN"
            fill_rect "$wk_x" "$(( WK_Y + WK_CELL_H - wk_t ))" "$WK_CELL_W" "$wk_t" "$RULE_PEN"
            [ "$i" = "6" ] && \
                fill_rect "$(( wk_x + WK_CELL_W - wk_t ))" "$WK_Y" "$wk_t" "$WK_CELL_H" "$RULE_PEN"
        fi

        if [ "$i" = "$WK_TODAY" ] && [ "$st" != "3" ]; then
            # Today: knocked out of a black plate — and drawn ON that
            # plate, not bgless over it. Bgless left an empty black
            # rectangle where the date should be: the one cell on the
            # screen that has to be legible, reading as a hole. See
            # draw_text().
            fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$WK_CELL_H" BLACK
            draw_text_reg_inv "$wk_nx" "$wk_ny" "$WK_NAME_SZ" "$wk_name"
            draw_text_reg_inv "$wk_dx" "$wk_dy" "$WK_DAY_SZ" "$wk_day"
        elif [ "$i" = "$WK_TODAY" ]; then
            # Minimal: today's name black, its date bold, a bar under it.
            fill_rect "$wk_x" "$(( WK_Y + WK_CELL_H - WK_MARK_T ))" \
                      "$WK_CELL_W" "$WK_MARK_T" BLACK
            draw_text_reg "$wk_nx" "$wk_ny" "$WK_NAME_SZ" "BLACK" "$wk_name"
            draw_text_bold "$wk_dx" "$wk_dy" "$WK_DAY_SZ" "BLACK" "$wk_day"
        else
            wk_npen="GRAY7"
            if [ "$st" = "0" ]; then
                wk_bg="GRAYE"
                { [ "$i" = "5" ] || [ "$i" = "6" ]; } && wk_bg="$WK_WE_PEN"
                fill_rect "$wk_x" "$WK_Y" "$WK_CELL_W" "$WK_CELL_H" "$wk_bg"
            elif [ "$i" = "5" ] || [ "$i" = "6" ]; then
                wk_npen="GRAY4"
            fi
            draw_text_reg "$wk_nx" "$wk_ny" "$WK_NAME_SZ" "$wk_npen" "$wk_name"
            draw_text_reg "$wk_dx" "$wk_dy" "$WK_DAY_SZ" "BLACK" "$wk_day"
        fi
        wk_x=$((wk_x + WK_CELL_W))
    done
}

# The filled style's weekend cells, a step darker than the weekdays' GRAYE,
# in the calendar and the forecast alike. The forecast's weekend icons carry
# this grey as their ground, fc_<code>_<size>d.bmp — tools/check_kindle_icons.py
# reads it from here.
WK_WE_PEN=GRAYD

# How thick the mark on the current day is — the frame round it in the
# forecast, the bar under it in the minimal calendar: three design pixels at
# this panel's size. Into WK_MARK_T; set by mark_t() before either is drawn.
WK_MARK_T=3
mark_t() {
    local short="${RES_W:-600}"
    [ "${RES_H:-800}" -lt "$short" ] 2>/dev/null && short="$RES_H"
    WK_MARK_T=$(( (3 * short + 300) / 600 ))
    [ "$WK_MARK_T" -ge 2 ] || WK_MARK_T=2
}

# ── The forecast in the week strip ───────────────────────────────────────────
# Tomorrow and the six days after it, where the calendar would be: the
# weekday, the condition, the high and, grey after it, the low. The cells take
# the calendar's and its month heading's height together, so nothing else on
# the page moves when the reader switches between the two.
#
# The weekend (WF<i>_WE=1) is darker, as in the calendar: a GRAYD cell with
# the icons grounded on it, fc_<code>_<size>d.bmp, in the filled style, and a
# darker weekday name in the others.
#
# WF_NOW names the cell that is today, to frame: -1 from a collector whose
# strip starts tomorrow, and 0 when it is not sent, from an older one whose
# strip started today. TODAY IS FRAMED, NOT INVERTED: a weather icon on a
# black plate is a white square with nothing in it.
#
# The icons carry their own ground (see tools/check_kindle_icons.py), so a
# filled cell takes the outlook's GRAYE icons (its weekend the GRAYD ones) and
# every other style the white-ground ones, fc_<code>_<size>w.bmp.
draw_week_fc() {
    local st="${WK_STYLE:-0}" top="${WK_HDG_Y:-$WK_Y}" x="$WK_X" i now="${WF_NOW:-0}"
    local h name nw icon hi lo hiw low tsz nh th pad y nx ix hx lx w f sfx="" we csfx npen
    case "$st" in 0|1|2|3) ;; *) st=0 ;; esac
    [ "$st" = "0" ] || sfx="w"
    h=$(( WK_Y + WK_CELL_H - top ))
    tsz=$(( WK_NAME_SZ * 8 / 7 ))
    mark_t; rule_h; rule_pen "${RULE_INK:-}" GRAYA

    # Name, icon and temperatures as one block, centred in the cell's height.
    nh=$(( WK_NAME_SZ * ${TEXT_PX_MILLE:-1160} / 1000 ))
    th=$(( tsz * ${TEXT_PX_MILLE:-1160} / 1000 ))
    pad=$(( (h - nh - FC_OL_SZ - th - 4) / 2 ))
    [ "$pad" -ge 0 ] || pad=0

    draw_hline "${WK_HDG_RULE_X:-$FOOT_RULE_X}" "${WK_HDG_RULE_Y:-$WK_Y}" \
               "${WK_HDG_RULE_W:-$FOOT_RULE_W}" "GRAYA"
    for i in 0 1 2 3 4 5 6; do
        eval "name=\${WF${i}_NAME:-}; nw=\${WF${i}_NAMEW:-0}; icon=\${WF${i}_ICON:-}"
        eval "hi=\${WF${i}_HI:-}; lo=\${WF${i}_LO:-}; hiw=\${WF${i}_HIW:-0}; low=\${WF${i}_LOW:-0}"
        eval "we=\${WF${i}_WE:-0}"
        csfx="$sfx"; npen=GRAY7

        case "$st" in
            0) if [ "$we" = "1" ]; then
                   fill_rect "$x" "$top" "$WK_CELL_W" "$h" "$WK_WE_PEN"; csfx="d"
               else
                   fill_rect "$x" "$top" "$WK_CELL_W" "$h" GRAYE
               fi ;;
            1) fill_rect "$x" "$top" "$RULE_T" "$h" "$RULE_PEN"
               fill_rect "$x" "$top" "$WK_CELL_W" "$RULE_T" "$RULE_PEN"
               fill_rect "$x" "$(( top + h - RULE_T ))" "$WK_CELL_W" "$RULE_T" "$RULE_PEN"
               [ "$i" = "6" ] && \
                   fill_rect "$(( x + WK_CELL_W - RULE_T ))" "$top" "$RULE_T" "$h" "$RULE_PEN" ;;
        esac
        [ "$st" != "0" ] && [ "$we" = "1" ] && npen=GRAY4
        if [ "$i" = "$now" ]; then
            if [ "$st" = "3" ]; then
                fill_rect "$x" "$(( top + h - WK_MARK_T ))" "$WK_CELL_W" "$WK_MARK_T" BLACK
            else
                w="$WK_MARK_T"
                fill_rect "$x" "$top" "$w" "$h" BLACK
                fill_rect "$(( x + WK_CELL_W - w ))" "$top" "$w" "$h" BLACK
                fill_rect "$x" "$top" "$WK_CELL_W" "$w" BLACK
                fill_rect "$x" "$(( top + h - w ))" "$WK_CELL_W" "$w" BLACK
            fi
        fi

        y=$(( top + pad ))
        centre_in "$x" "$WK_CELL_W" "$(( WK_NAME_SZ * nw / 1000 ))"; nx="$CENTRE_X"
        if [ "$i" = "$now" ]; then draw_text_reg "$nx" "$y" "$WK_NAME_SZ" BLACK "$name"
        else                    draw_text_reg "$nx" "$y" "$WK_NAME_SZ" "$npen" "$name"
        fi
        y=$(( y + nh + 2 ))
        if [ -n "$icon" ]; then
            f="$ICON_DIR/fc_${icon}_${FC_OL_SZ}${csfx}.bmp"
            [ -f "$f" ] || f="$ICON_DIR/fc_-1_${FC_OL_SZ}${csfx}.bmp"
            centre_in "$x" "$WK_CELL_W" "$FC_OL_SZ"; ix="$CENTRE_X"
            draw_image "$f" "$ix" "$y"
        fi
        y=$(( y + FC_OL_SZ + 2 ))
        if [ -n "$hi" ]; then
            centre_in "$x" "$WK_CELL_W" "$(( tsz * (hiw + low) / 1000 ))"; hx="$CENTRE_X"
            lx=$(( hx + tsz * hiw / 1000 ))
            draw_text_bold "$hx" "$y" "$tsz" BLACK "$hi"
            [ -n "$lo" ] && draw_text_reg "$lx" "$y" "$tsz" GRAY7 "$lo"
        fi
        x=$(( x + WK_CELL_W ))
    done
}

# ── The four repaints ────────────────────────────────────────────────────────
# Each one clears its own rectangle, draws into the framebuffer, and refreshes
# that rectangle exactly once. Clearing first is not optional: e-ink does not
# erase what it is drawn over, so "21.0" replaced by "9.8" leaves the "0"
# standing where nothing wrote.

redraw_clock() {
    # $1=HH:MM  $2=1 to flash this rectangle
    [ "${SHOW_CLOCK:-1}" = "1" ] || return 0
    draw_clock "$1"
    refresh_zone "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" "${2:-0}"
}

redraw_sensors() {
    # $1=HH:MM  $2=1 to flash
    #
    # NUMBERS NOBODY HAS CONFIRMED DO NOT GET DRAWN AS IF THEY HAD BEEN. A
    # failed fetch used to leave the previous readings in place and repaint
    # them, unchanged, every five minutes for as long as the collector stayed
    # down: the only thing on the panel that could have said otherwise was the
    # age inside Z_SUB, which is part of the payload and was frozen with it. A
    # dead collector and a calm afternoon looked identical.
    if data_stale; then
        redraw_offline "$1"
        return 0
    fi
    fill_rect "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" WHITE
    draw_sensors_body
    if [ "${LAND:-0}" = "1" ] && [ "${Z_TOP_H:-0}" -gt 0 ] 2>/dev/null; then
        # The clock is in the top row on its side, with the week strip — and
        # ONE refresh for both, so a flashing tier flashes once.
        fill_rect 0 0 "$ZW" "$Z_TOP_H" WHITE
        draw_top_row "$1"
        refresh_zone 0 0 "$ZW" $(( Z_SENS_Y + Z_SENS_H )) "${2:-0}"
        return 0
    fi
    # Inside this rectangle, so it goes with it — except on its side, where
    # it is in the top row, and on the wall page, where it is in the band at
    # the foot and redraw_forecast() draws it.
    [ "${LAND:-0}" = "1" ] || [ "${WALL:-0}" = "1" ] || draw_clock "$1"
    refresh_zone "$Z_SENS_X" "$Z_SENS_Y" "$Z_SENS_W" "$Z_SENS_H" "${2:-0}"
}

# The landscape page's top row: the clock, the week strip, the rule under them.
draw_top_row() {
    # $1=HH:MM, or empty for the row without the clock (the offline page,
    # where the readings the clock stands with are not drawn)
    [ "${LAND:-0}" = "1" ] || return 0
    [ "${Z_TOP_H:-0}" -gt 0 ] 2>/dev/null || return 0
    [ -n "$1" ] && draw_clock "$1"
    [ "${SHOW_WEEK:-1}" = "1" ] && draw_week
    draw_hline "${FOOT_RULE_X:-18}" "$TOPROW_Y" "${FOOT_RULE_W:-764}" "GRAYA"
}

redraw_top_row() {
    [ "${Z_TOP_H:-0}" -gt 0 ] 2>/dev/null || return 0
    fill_rect 0 0 "$ZW" "$Z_TOP_H" WHITE
    draw_top_row "$1"
    refresh_zone 0 0 "$ZW" "$Z_TOP_H" "${2:-0}"
}

redraw_chart() {
    # NO RECTANGLE AT ALL when the collector's layout has no chart: the
    # readings above run straight into what is under them, and a refresh of a
    # zero-height region is not a refresh FBInk promises to keep small.
    [ "${Z_CHART_H:-0}" -gt 0 ] 2>/dev/null || return 0
    # On its side the offline notice runs across the chart's half of the band
    # too, and it stays until the collector answers.
    [ "${LAND:-0}" = "1" ] && data_stale && return 0
    fill_rect "$Z_CHART_X" "$Z_CHART_Y" "$Z_CHART_W" "$Z_CHART_H" WHITE
    draw_chart_body
    refresh_zone "$Z_CHART_X" "$Z_CHART_Y" "$Z_CHART_W" "$Z_CHART_H" 0
}

redraw_forecast() {
    fill_rect "$Z_FC_X" "$Z_FC_Y" "$Z_FC_W" "$Z_FC_H" WHITE
    draw_forecast_body
    # The wall page's clock is inside this rectangle, beside the forecast:
    # cleared with it, so drawn again with it.
    [ "${WALL:-0}" = "1" ] && draw_clock "$(now_clock)"
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
    local y=$(( Z_OFF_Y + Z_OFF_H / 3 ))
    fill_rect "$Z_OFF_X" "$Z_OFF_Y" "$Z_OFF_W" "$Z_OFF_H" WHITE
    draw_text_bold "${OFF_X:-40}" "$y" "${OFF_SZ:-26}" "BLACK" \
                   "${LBL_OFFLINE:-Cannot reach} $(host_url)"
    local y2=$(( y + ${OFF_SZ:-26} + 12 ))
    draw_text_reg "${OFF_X:-40}" "$y2" \
                  "${OFF_SUB_SZ:-16}" "GRAY5" \
                  "${LBL_OFFLINE_HINT:-Check WiFi, or KUAL > Settings > Find collector}"

    # ── And the two things that hint could not say ──────────────────────────
    # WHEN IT LAST WORKED, which is the difference between a collector that
    # went down a minute ago and a panel that has been showing the same
    # numbers since Tuesday — and the route to the search that does NOT go
    # through the launcher, named with the reader's own word for the button,
    # because GUI_STOP takes KUAL away and this is the page that reader sees.
    local y3=$(( y2 + ${OFF_SUB_SZ:-16} + 8 )) hint=""
    if [ "${TOUCH_READY:-0}" = "1" ] && menu_word settings; then
        hint="$MENU_WORD"
        list_at "${MENU_LBL2:-Find|Next|Battery|Info|Back}" 0
        [ -n "$LIST_ITEM" ] && hint="$hint > $LIST_ITEM"
    fi
    if [ -n "${LAST_OK:-}" ]; then
        if [ -n "$hint" ]; then hint="$hint   ($LAST_OK)"
        else                    hint="($LAST_OK)"
        fi
    fi
    [ -n "$hint" ] &&
        draw_text_reg "${OFF_X:-40}" "$y3" "${OFF_SUB_SZ:-16}" "GRAY7" "$hint"

    draw_clock "$1"
    refresh_zone "$Z_OFF_X" "$Z_OFF_Y" "$Z_OFF_W" "$Z_OFF_H" 0
    # On its side the clock is above this rectangle, in the top row; on the
    # wall page, below it in the band at the foot.
    { [ "${LAND:-0}" = "1" ] || [ "${WALL:-0}" = "1" ]; } && [ "${SHOW_CLOCK:-1}" = "1" ] &&
        refresh_zone "$Z_CLOCK_X" "$Z_CLOCK_Y" "$Z_CLOCK_W" "$Z_CLOCK_H" 0
    return 0
}

# The whole page, one flashing refresh at the end. This is the tier that
# actually resets ghosting, and it is why the others do not have to.
redraw_all() {
    # $1=HH:MM  $2=0 for a full redraw that does not flash
    FACE_CHANGED=0
    clear_screen
    # A COLD START MAY HAVE NOTHING TO PUT IN THE READINGS. cache_load leaves a
    # page with a chart, a forecast and a week strip and readings whose ages
    # were computed before the reader was switched off; drawing those as if
    # they were current is the one thing this page must not do.
    if ! data_stale; then
        draw_sensors_body
        if [ "${LAND:-0}" = "1" ]; then draw_top_row "$1"; else draw_clock "$1"; fi
    fi
    # The week strip is the date, and stands without the readings — with the
    # rule under the row, which is drawn with it.
    data_stale && [ "${LAND:-0}" = "1" ] && draw_top_row ""
    draw_chart_body
    draw_forecast_body
    if ! data_stale; then
        if [ "${2:-1}" = "1" ]; then refresh_screen; else refresh_screen_plain; fi
        return 0
    fi
    # The readings zone carries the reason, and issues its own refresh — so the
    # screen-wide one is spent first and the zone repaint lands on top of it.
    if [ "${2:-1}" = "1" ]; then refresh_screen; else refresh_screen_plain; fi
    redraw_offline "$1"
    return 0
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
    due "$m" "${CLOCK_NOW:-$CLOCK_EVERY}" && out="$out clock"
    echo "${out# }"
}

# Did one of the tiers in $1 already draw the clock this minute? It lives in
# the readings' rectangle, so the sensors tier draws it — except on the wall
# page, where it is in the band at the foot and the forecast tier does.
# Drawing it twice is a second repaint of the same pixels; not drawing it at
# all leaves the wall clock a minute behind every time the readings update.
clock_covered() {
    if [ "${WALL:-0}" = "1" ]; then
        case " $1 " in *" forecast "*) return 0 ;; esac
    else
        case " $1 " in *" sensors "*) return 0 ;; esac
    fi
    return 1
}

# Is there anything at all to do in minute $1?
#
# THE SAME QUESTION plan_minute ANSWERS, asked without forking. A suspend is
# worth taking for as long as there is nothing to come back for, and the tiers
# already know when that is: at the battery-saver intervals, fourteen minutes
# in fifteen are empty, and the panel was waking for every one of them —
# resume, look, find nothing due, go back down. `$(plan_minute)` is a subshell
# and next_due_in() asks up to thirty times in a row, so it asks this instead.
minute_busy() {
    due "$1" "$FULL_EVERY"     && return 0
    due "$1" "$DATA_EVERY"     && return 0
    due "$1" "$GRAPH_EVERY"    && return 0
    due "$1" "$FORECAST_EVERY" && return 0
    due "$1" "${CLOCK_NOW:-$CLOCK_EVERY}" && return 0
    return 1
}

# How many minutes until the next one with work in it — 1 whenever the very
# next minute has some, which is every minute at CLOCK_EVERY=1.
#
# CAPPED, because the panel should come back and look at itself now and again
# whatever the intervals say: a fetch that has been failing for half an hour is
# worth finding out about, and a suspend is also the state this script cannot
# observe itself in.
next_due_in() {
    local n=1
    while [ "$n" -lt "${SLEEP_MAX_MIN:-30}" ]; do
        minute_busy $(( $1 + n )) && break
        n=$((n + 1))
    done
    NEXT_DUE=$n
    return 0
}

# ── Quiet hours ──────────────────────────────────────────────────────────────
#
# A FLASHING REFRESH IS A BLACK FRAME, and in a bedroom at three in the morning
# it is the brightest thing in the room. It is also the most expensive thing
# this panel does, on the hours when nobody is reading it.
#
# So between QUIET_FROM and QUIET_TO nothing flashes and the clock can slow
# right down — and the morning is paid for in one go: leaving the quiet hours
# spends a whole flashing refresh to clear what a night of partial updates left
# behind, which is the one moment it buys the most.
#: The answer for this tick. `date +%H` is a fork, and four things ask.
QUIET_IS=0
quiet_now() { [ "${QUIET_IS:-0}" = "1" ]; }
quiet_check() {
    if quiet_eval; then QUIET_IS=1; else QUIET_IS=0; fi
    return 0
}

quiet_eval() {
    local f="${QUIET_FROM:-0}" t="${QUIET_TO:-0}" h
    [ "$f" = "$t" ] && return 1                   # equal: switched off
    h=$(date +%H)
    h=$(strip_zeros "$h")
    if [ "$f" -lt "$t" ] 2>/dev/null; then
        [ "$h" -ge "$f" ] && [ "$h" -lt "$t" ]
    else
        # Over midnight, which is what anybody setting this actually wants.
        [ "$h" -ge "$f" ] || [ "$h" -lt "$t" ]
    fi
}

# The clock tier for the hour we are in. Computed once a tick into a global
# rather than asked per call: plan_minute() and minute_busy() both want it, and
# minute_busy() is asked about up to thirty minutes ahead.
CLOCK_NOW=""
clock_tier() {
    quiet_check
    CLOCK_NOW="${CLOCK_EVERY:-1}"
    if [ "${QUIET_EVERY:-0}" -gt 0 ] 2>/dev/null && quiet_now; then
        CLOCK_NOW="$QUIET_EVERY"
    fi
    return 0
}

flash_due() {
    # $1=minute counter $2=tier interval $3=flash every N of those tiers
    quiet_now && return 1
    [ -n "$3" ] && [ "$3" -gt 0 ] 2>/dev/null || return 1
    [ "$2" -gt 0 ] 2>/dev/null || return 1
    [ $(( ($1 / $2) % $3 )) -eq 0 ]
}

# ── Cleanup on exit ──────────────────────────────────────────────────────────
cleanup() {
    handback
    rm -f "${DASH_PIDFILE:-/tmp/dash.pid}"
    exit 0
}

# Everything cleanup() gives back, without the exit — pkg_restart() hands the
# device back like this and then becomes the new copy of this script, in the
# same process, so the pidfile stays true and stop.sh still finds it.
handback() {
    # The sleep is a child process and does not get the signal we did. Left
    # alone it holds the script here for the rest of the minute, which is what
    # made Stop look like it had failed.
    [ -n "$SLEEP_PID" ] && kill "$SLEEP_PID" 2>/dev/null
    restore_sleep
    # THE FRAMEWORK FIRST. On the firmware where GUI_STOP takes the radio down
    # with it, the framework is what answers com.lab126.cmd — so restoring the
    # radio before the service that answers for it is a restore that quietly
    # does nothing.
    touch_disarm
    # The page upright again before the reader gets the screen back.
    rot_restore
    canvas_give_back
    gui_restore
    # And the radio goes back if WE are the ones who turned it off. Keyed on
    # the latch and not on POWER: POWER may have been set back to awake in the
    # meantime, and awake is the value that would skip this. Stop hands the
    # device back to its owner, and handing it back with no network is a Kindle
    # that looks broken.
    [ "${RADIO_OFF:-0}" = "1" ] && radio_set 1
    rm -rf "$TMP"
    return 0
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
    # A real suspend where it is asked for and possible, an ordinary sleep
    # where it is not. suspend_for() returns non-zero rather than risking a
    # machine that does not come back, so this is the fallback and not an
    # error path.
    # CLEARED HERE, WHERE EVERY PATH OUT OF THIS FUNCTION PASSES. It used to be
    # cleared only inside nap_or_tap, which the suspend below returns before
    # reaching — so a tap taken on one tick was still in TAP on the next, and
    # with the bar open menu_hit ran again on the stale coordinates. A tap in
    # the right-hand third then chose `quit` on a tick nobody had touched, and
    # the dashboard exited by itself.
    TAP=""
    WOKE_UP=0
    local start deep
    epoch_now
    start="$EPOCH"

    # ── WHAT DECIDES WHETHER THE MACHINE GOES DOWN ──────────────────────────
    #
    # It used to be whether the tap menu was armed, and so the two settings a
    # reader most wants together — a panel that sleeps for days, and a panel
    # that answers a finger — were the one pair that could not be had. The menu
    # won, which meant TOUCH=1 quietly cancelled POWER=suspend.
    #
    # It is the wake window now. Nothing reads the touchscreen while the CPU is
    # down because nothing can; the button is what brings it back, and for the
    # couple of minutes after that the panel stays up and listens. Outside that
    # window there is nobody to serve and the machine sleeps.
    if [ "${POWER:-awake}" = "suspend" ] && ! wake_window; then
        # AND IT SLEEPS PAST THE MINUTES WITH NOTHING IN THEM. The tiers say
        # which minutes have work: at the battery-saver intervals fourteen in
        # fifteen have none, and the panel was waking for every one of them to
        # find that out. The clock is the tier that decides how long this can
        # be — which is why the quiet hours can slow the clock down, and why
        # doing so is worth whole days.
        next_due_in "${MINUTE:-0}"
        # THE EXTENSION BELONGS TO THE SUSPEND, NOT TO THE FALLBACK. It is
        # bought by the machine being down: a reader whose /sys/power/state
        # refuses, or that has no RTC alarm to come back on, would otherwise
        # take the half hour it was asked to sleep for and spend it awake —
        # with TOUCH=0 that is a panel that answers nothing and repaints
        # nothing for thirty minutes, which is far worse than the minute the
        # fallback promises.
        deep=$(( delay + (NEXT_DUE - 1) * 60 ))
        # Which RTC holds an alarm the kernel will honour, asked here because
        # here is where the answer is first needed.
        [ "${RTC_PICKED:-0}" = "1" ] || rtc_pick
        if suspend_for "$deep"; then
            # Back before the alarm was due: somebody pressed something.
            if [ "${SUSPEND_EARLY:-0}" = "1" ] && [ "${WAKE_MENU:-1}" = "1" ]; then
                wake_interactive
                # SAID OUT LOUD TO THE LOOP, because the loop cannot tell this
                # from a wait that simply ran out — and what it does with one
                # of those is dismiss the bar and repaint. See the main loop.
                WOKE_UP=1
            fi
            return 0
        fi
    fi
    nap_or_tap "$delay"
    # However that wait went, if far more time passed than was asked for then
    # the reader was asleep — powerd's doing — and something else has been on
    # the screen since.
    lost_time "$start" "$delay"
    return 0
}

# The wait, with an ear open for the screen.
#
# TAP is the tap that arrived, or empty if the wait ran out. Without the menu
# armed this is the plain interruptible sleep it has always been; with it, the
# wait is a read on the touch FIFO so a finger is acted on the moment it lands
# instead of at the top of the next minute.
#
# SLICED INTO TWO-SECOND READS so a signal is still noticed promptly. `nap` is
# a background sleep the trap can kill, which is what made Stop feel immediate;
# a single sixty-second read would hand that back.
nap_or_tap() {
    local want="$1" start now last=""
    TAP=""
    if [ "${TOUCH_READY:-0}" != "1" ]; then
        nap "$want"
        return 1
    fi
    # BOUNDED BY THE CLOCK, NOT BY COUNTING THE READS. `read -t` is not POSIX:
    # a shell without it errors immediately rather than waiting, with the
    # complaint swallowed by the redirect — and crediting each read with two
    # seconds it never spent turned a minute's wait into thirty instant
    # iterations and the main loop into a spin, fetching and flashing the panel
    # as fast as the CPU allows, on a battery.
    start=$(date +%s)
    while :; do
        if read -t 2 -r TAP <&9 2>/dev/null && [ -n "$TAP" ]; then
            return 0
        fi
        TAP=""
        now=$(date +%s)
        [ $(( now - start )) -ge "$want" ] && return 1
        # The clock has not moved, so that read did not wait. Make it.
        [ "$now" = "$last" ] && nap 1
        last="$now"
    done
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
# This process, and not settings.sh sourcing the same file, is the one that
# acts on an update the collector offers — see pkg_check().
PKG_MAIN=1
pkg_version

mkdir -p "$TMP"
conf_init
conf_load
clock_tier
font_setup || echo "Continuing without text." >&2
prevent_sleep
canvas_apply
gui_apply
touch_apply

# A layout BEFORE the first fetch, so no drawing path can run without one.
# RES_W and RES_H come from the collector, so this falls back to 600x800 and
# load_data() reloads it at the real resolution the moment the collector
# answers.
load_layout

# THE LAST PAGE THIS PANEL DREW, before asking the network for a new one. /tmp
# is a ramdisk, so a reader that has just been switched on knows nothing at
# all: no chart, no forecast, and the offline notice in English because the
# language is one of the things the collector sends. The cache is a page to
# put the notice ON, in the right language, while the first fetch is tried.
cache_load

net_up
if fetch_data && load_data; then
    fetch_graph
    cache_save
    redraw_all "$(now_clock)"
else
    # ── NOBODY HAS TOLD IT WHERE THE COLLECTOR IS ───────────────────────────
    # The address it is trying is the one the package shipped, nothing is
    # answering there, and no scan has ever been run. That is not a fault to
    # report — it is the first-run state, and the reader is standing in front
    # of a device with no keyboard. So look, once, and say what was found.
    # AUTO_FIND=0 for anybody who would rather it did not.
    if [ "${AUTO_FIND:-1}" = "1" ] && host_is_default &&
       [ ! -s "$SCAN_LIST" ]; then
        echo "first run: no collector at $HOST, looking for one" >&2
        settings_run find
        rm -f "$TMP/redraw"
        conf_load
        net_up
        fetch_data && load_data && fetch_graph && cache_save
    fi
    if [ "$HAVE_DATA" = "1" ]; then
        redraw_all "$(now_clock)"
    else
        clear_screen
        refresh_screen
        redraw_offline "$(now_clock)"
    fi
fi

# ── Main loop ────────────────────────────────────────────────────────────────
# One tick a minute; the tiers decide what that tick costs.
#
# MINUTE IS READ OFF THE CLOCK, NOT COUNTED. It was a counter incremented once
# per pass through this loop, which quietly made every extra pass a minute the
# schedule had not spent: the comment over the tap handling below promised that
# "a reader who taps four times should not fast-forward the chart", and four
# taps did exactly that — each one ran a tick and pushed the hourly full
# refresh a minute further out. An early wake from a suspend did the same. And
# it made sleeping through the empty minutes impossible to account for, which
# is the one thing that turns POWER=suspend into days.
#
# Counted from the minute the dashboard started, so every interval still means
# what dash.conf says it means and minute 0 is the page drawn above.
epoch_now
START_MIN=$(( EPOCH / 60 ))
MINUTE=0
LAST_MINUTE=0
while true; do
    # AT THE TOP, not after the work. The tick body below has `continue` in it
    # in four places, and a radio turned off after them is a radio left on for
    # the rest of the day on exactly the paths that took a shortcut.
    net_down
    nap_to_minute

    # SOMEBODY PRESSED THE BUTTON AND IT HAS ALREADY BEEN ANSWERED:
    # wake_interactive() repainted the page and put the bar up. Straight back
    # to listening for the tap that bar exists to catch.
    #
    # THE LOOP CANNOT TELL THIS FROM A WAIT THAT RAN OUT, and what it does with
    # one of those is two lines below: dismiss the bar and repaint. That erased
    # the bar within milliseconds of drawing it — the press worked, the panel
    # flashed twice, and the button still looked dead, which is the whole thing
    # this mechanism exists to fix. The tick would then have painted zones over
    # what was left of it.
    if [ "${WOKE_UP:-0}" = "1" ]; then
        WOKE_UP=0
        continue
    fi

    # A TAP IS ANSWERED BEFORE THE TICK IT INTERRUPTED, and does not spend a
    # minute: the clock counter is what decides which tier comes round next,
    # and a reader who taps four times should not fast-forward the chart.
    if [ -n "${TAP:-}" ]; then
        touch_scale $TAP
        # Every tap is also a reason to stay up a little longer: a reader
        # working through the settings bar should not have the window close
        # under them halfway.
        wake_extend
        if [ "${MENU:-0}" = "0" ]; then
            menu_open main
            continue
        fi
        menu_hit "$TAP_X" "$TAP_Y"
        case "$MENU_HIT" in
            # A full tick, through the same file settings.sh leaves behind:
            # one way for "draw everything now", not two.
            refresh)  MENU=0; : > "$TMP/redraw" ;;
            # The collector fetches, then the whole page is drawn with it —
            # the bar has to come off the screen anyway, and a redraw is what
            # takes it off.
            forecast) MENU=0; forecast_now ;;
            # THE BUTTON THIS WHOLE MECHANISM EXISTS FOR. Written to dash.conf,
            # so KUAL and settings.sh agree with the panel afterwards.
            wake)     MENU=0; power_toggle; : > "$TMP/redraw" ;;
            settings) menu_open more; continue ;;
            # The settings bar. Each of these runs the same settings.sh that
            # KUAL runs, which paints its own answer on the way past.
            find)     MENU=0; settings_run find ;;
            next)     MENU=0; settings_run next ;;
            power)    MENU=0; power_cycle; : > "$TMP/redraw" ;;
            diag)     MENU=0; settings_run diag ;;
            # IT ASKS BEFORE IT ENDS THE DASHBOARD. One tap turns the bar into
            # the confirmation; the next one inside it is the one that acts.
            # This panel's touch calibration is the thing most likely to be
            # wrong on any given reader, and Exit is the one button where being
            # wrong cannot be undone from the sofa.
            quit)     menu_open sure; continue ;;
            sure)     cleanup ;;
            # hide, back, and every coordinate that was out of range or not a
            # number at all. THE SAME FILE, NOT A REDRAW OF ITS OWN:
            # redraw_all() ends with its own refresh_screen, so calling one
            # after it flashed the whole panel twice for one dismissal.
            *)        MENU=0; : > "$TMP/redraw" ;;
        esac
    elif [ "${MENU:-0}" != "0" ]; then
        # THE WAIT RAN OUT WITH THE BAR STILL UP, so it is dismissed rather
        # than drawn through: the tick below repaints zones, and a zone
        # repainted over half a bar is a smear nobody asked for. It also means
        # a bar left up by accident goes away on its own within the minute —
        # and an Exit that was asked about and not answered is an Exit that
        # does not happen.
        MENU=0
        : > "$TMP/redraw"
    fi

    epoch_now
    MINUTE=$(( EPOCH / 60 - START_MIN ))
    # A CLOCK THAT WENT BACKWARDS, which on this device is ordinary rather than
    # exotic: the time is set from the network once WiFi associates, and what
    # the RTC held until then can be minutes or hours out. Re-based on the spot,
    # because the alternative is a panel that draws nothing at all until the
    # clock has caught up with a minute it already counted.
    if [ "$MINUTE" -lt "$LAST_MINUTE" ] 2>/dev/null; then
        START_MIN=$(( EPOCH / 60 - LAST_MINUTE - 1 ))
        MINUTE=$(( LAST_MINUTE + 1 ))
    fi
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
    # Both of these are settings like any other, so they are applied where
    # every other setting is: after the read, every minute. power_apply() puts
    # the radio back if POWER has gone to awake, and gui_apply() starts or
    # stops the framework to match GUI_STOP.
    power_apply
    gui_apply
    canvas_apply
    touch_apply
    # Which hour we are in, and therefore how often the clock is drawn and
    # whether anything is allowed to flash.
    clock_tier
    # LEAVING THE QUIET HOURS IS WORTH A WHOLE FLASHING REFRESH. Nothing has
    # flashed since they began, so a night of partial updates is sitting on the
    # panel — and this is the one moment when clearing it costs nothing anybody
    # is awake to mind.
    if quiet_now; then
        QUIET_WAS=1
    elif [ "${QUIET_WAS:-0}" = "1" ]; then
        QUIET_WAS=0
        : > "$TMP/redraw"
    fi

    # settings.sh leaves this behind after any change: the settings screen it
    # painted is sitting on top of the dashboard, and whatever changed should
    # be visible now rather than at the top of the hour.
    if [ -f "$TMP/redraw" ]; then
        rm -f "$TMP/redraw"
        TIERS="full"
    elif [ "$MINUTE" = "$LAST_MINUTE" ]; then
        # A SECOND PASS INSIDE ONE MINUTE, which is what a tap or an early wake
        # produces: the tiers due in this minute have already been drawn, and
        # drawing them again is a second repaint of the same pixels — the exact
        # cost the schedule is arranged to avoid. Nothing is due; the wait
        # below is what this pass is for.
        TIERS=""
    else
        TIERS=$(plan_minute "$MINUTE")
    fi
    LAST_MINUTE="$MINUTE"

    # The clock is drawn from the reader's own clock, so most minutes need no
    # network at all — which is the whole of where the battery goes.
    needs_net "$TIERS" && net_up

    # ── Nothing to draw yet ─────────────────────────────────────────────────
    # No payload has ever parsed, so every reading is empty. Keep the reason on
    # screen and the clock current, and try again whenever the data tier comes
    # round; the first success draws the whole page.
    if [ "$HAVE_DATA" = "0" ]; then
        case " $TIERS " in
            *" full "*|*" sensors "*)
                if fetch_data && load_data; then
                    fetch_graph
                    # The first page of the run, kept at once rather than at
                    # the top of the hour: a panel that has just been told
                    # where its collector is should survive the next reboot
                    # knowing it.
                    cache_save
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
            # Once an hour, onto the volume a reboot does not clear — so the
            # next cold start has a page to draw. On the full tier and nowhere
            # else: this is FAT on the eMMC, not tmpfs.
            cache_save
            if quiet_now; then redraw_all "$NOW_TIME" 0
            else               redraw_all "$NOW_TIME" 1
            fi
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
    # A new face from the collector: every zone at once, see load_data().
    if [ "${FACE_CHANGED:-0}" = "1" ]; then
        if quiet_now; then redraw_all "$NOW_TIME" 0
        else               redraw_all "$NOW_TIME" 1
        fi
        continue
    fi

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

    # The clock last, and only when the tier whose rectangle it lives in has
    # not already drawn it — see clock_covered().
    case " $TIERS " in
        *" clock "*)
            clock_covered "$TIERS" && continue
            if flash_due "$MINUTE" "$CLOCK_EVERY" "$CLOCK_FLASH_EVERY"; then
                redraw_clock "$NOW_TIME" 1
            else
                redraw_clock "$NOW_TIME" 0
            fi
            ;;
    esac
done
