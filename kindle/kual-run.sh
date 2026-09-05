#!/bin/sh
# ============================================================================
# kual-run.sh — what a KUAL menu entry actually does, and a record of it
#
# Reached only through kual.sh, which passes the extension's own directory as
# the first argument and the command after it:
#
#     sh kual-run.sh /mnt/us/extensions/esp32dash start
#
# It exists to turn "nothing happened" into a sentence. KUAL discards whatever
# a menu entry prints, so the three things that go wrong on a fresh install —
# FBInk not installed, files copied with Windows line endings, a folder copied
# incompletely — were all invisible, and identical: the menu closed, the home
# screen came back, and the dashboard did not start.
#
# Everything here writes to kual.log NEXT TO THE SCRIPTS rather than into /tmp:
# /mnt/us is the volume that appears when the Kindle is plugged into a computer,
# so the log can be read with no shell, no network and no SSH. That is the only
# channel a reader has when it is not working.
# ============================================================================

# ── Where we are, and what we were asked for ─────────────────────────────────
DIR="${1:-.}"
shift 2>/dev/null

# kual.sh is written so that a CRLF copy of it can only damage the LAST
# argument, which is where the carriage return ends up. Strip it here, from all
# of them, before anything compares one against a word. The re-split is
# deliberate and safe: every command this dispatcher takes is a bare word.
ARGS=$(printf '%s ' "$@" | tr -d '\r')
# shellcheck disable=SC2086
set -- $ARGS

[ -d "$DIR" ] || DIR=.
cd "$DIR" 2>/dev/null || true
DIR=$(pwd)

# ── FBInk has to be findable, not merely installed ───────────────────────────
# The dashboard draws with FBInk, which is not part of the Kindle firmware and
# is not on the PATH KUAL hands to an extension even when it is installed. A
# missing binary is why a dashboard that "started" can leave the screen exactly
# as it was: every draw fails, the script keeps its schedule, and nothing ever
# appears. Look in the places people put it before deciding it is absent.
#
# /mnt/us/libkh/bin FIRST, because that is not a guess: the universal jailbreak
# hotfix installs FBInk there as part of setting the device up, so on most
# jailbroken Kindles the binary is already present and merely unreachable from
# the PATH a menu entry is given. That one line is the difference between "the
# dashboard does nothing" and a working panel.
for d in /mnt/us/libkh/bin "$DIR" "$DIR/bin" /mnt/us/bin /mnt/us/fbink \
         /mnt/us/extensions/fbink /mnt/us/extensions/fbink/bin \
         /mnt/us/extensions/kterm/bin \
         /usr/local/bin /usr/local/mnt/us/bin; do
    [ -d "$d" ] && PATH="$PATH:$d"
done
export PATH

# ── The log ──────────────────────────────────────────────────────────────────
LOG="$DIR/kual.log"
# The probe is in a SUBSHELL on purpose: a redirection that fails on a special
# built-in (`:` is one) is allowed to end a non-interactive shell outright, and
# a read-only /mnt/us must degrade to /tmp, not kill the launcher.
( : >> "$LOG" ) 2>/dev/null || LOG=/tmp/kual.log
# FAT and a decade of launches: keep it small enough to open on a phone.
if [ "$(wc -c < "$LOG" 2>/dev/null | tr -dc '0-9')" -gt 65536 ] 2>/dev/null; then
    tail -n 200 "$LOG" > "$LOG.trim" 2>/dev/null && mv "$LOG.trim" "$LOG"
fi

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null; }

# ── Saying something on the panel ────────────────────────────────────────────
# FBInk when it is there, and `eips` when it is not: eips ships with the Kindle
# firmware, so the one message that matters most — "FBInk is missing" — is the
# one message that can always be drawn.
say() {
    if command -v fbink >/dev/null 2>&1; then
        fbink -q -- "$1" 2>/dev/null && return 0
    fi
    if command -v eips >/dev/null 2>&1; then
        eips 1 1 "$1" 2>/dev/null && return 0
    fi
    return 1
}

# ── Windows line endings, healed in place ────────────────────────────────────
# Not a diagnosis printed for someone else to act on: a CR in these files is
# never intentional, the fix is mechanical, and the person reading this is on a
# reader with no editor. Only text this extension owns is touched — the icons
# are BMPs and a substitution inside one draws noise.
heal_crlf() {
    local f healed=""
    # dash.conf.default is named explicitly: it is not a *.conf. conf_init
    # copies it verbatim to dash.conf on first run and update_dash.sh SOURCES
    # the result, so a CR there gives HOST a trailing carriage return, every
    # wget fails against "http://192.168.1.50\r", and nothing anywhere says
    # why. dash.conf itself is healed for the same reason — it is the file
    # somebody may have edited over USB, from Windows.
    for f in "$DIR"/*.sh "$DIR"/*.conf "$DIR"/*.json "$DIR"/*.xml \
             "$DIR"/dash.conf.default "$DIR"/dash.conf \
             "$DIR"/layout/*.conf; do
        [ -f "$f" ] || continue
        if tr -d '\r' < "$f" | cmp -s - "$f"; then continue; fi
        if tr -d '\r' < "$f" > "$f.lf" 2>/dev/null && mv "$f.lf" "$f" 2>/dev/null
        then
            healed="$healed $(basename "$f")"
        else
            rm -f "$f.lf"
        fi
    done
    [ -n "$healed" ] && log "healed Windows line endings in:$healed"
    return 0
}

# ── Running one thing, and recording how it went ─────────────────────────────
run() {
    # $1=what to call it in the log, then the command
    local label="$1" out rc
    shift
    log "run: $label ($*)"
    out=$("$@" 2>&1)
    rc=$?
    [ -n "$out" ] && echo "$out" >> "$LOG" 2>/dev/null
    log "exit $rc"
    if [ "$rc" -ne 0 ]; then
        say "ESP32 Dashboard: $label failed (exit $rc). See kual.log"
    fi
    return $rc
}

# ── What KUAL asked for ──────────────────────────────────────────────────────
# The version goes in every time, not once at install: the first question asked
# of a reader behaving oddly is which build is on it, and the answer has to be
# in the same file as the symptom.
log "--- $* (dir $DIR, version $(cat "$DIR/VERSION" 2>/dev/null || echo unknown))"
heal_crlf

if ! command -v fbink >/dev/null 2>&1; then
    log "FBInk is not installed, or not in PATH: $PATH"
    log "The dashboard cannot draw anything without it. See kindle/README.md."
    say "ESP32 Dashboard: FBInk not found. See kual.log"
fi

case "${1:-}" in
    start)   run "Start Dashboard" sh "$DIR/start.sh" ;;
    stop)    run "Stop Dashboard"  sh "$DIR/stop.sh" ;;
    show|find|next|reset)
             run "Settings $1"     sh "$DIR/settings.sh" "$1" ;;
    profile) run "Settings profile ${2:-}" sh "$DIR/settings.sh" profile "${2:-normal}" ;;
    "")      log "no command given"; say "ESP32 Dashboard: no command given"; exit 2 ;;
    *)       log "unknown command: $1"
             say "ESP32 Dashboard: unknown command $1"
             exit 2 ;;
esac
exit $?
