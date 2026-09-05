#!/bin/sh
# ============================================================================
# kual.sh — the one thing KUAL runs, and the only file here that has to work
#           on a device where nothing else does.
#
# WHY THIS FILE EXISTS AT ALL
# ---------------------------
# KUAL shows no output. It runs `action params`, the menu closes, and the
# reader goes back to the home screen whatever happened — a script that was
# never found, a shell that died on the first line, and a dashboard that
# started perfectly all look identical from the sofa. "Every entry does
# nothing" was reported from a device that could not be logged into, and there
# was no way to tell which of those three it was.
#
# So every menu entry now runs THIS, and this hands over to kual-run.sh, which
# writes what happened to a log next to itself — on /mnt/us, where it can be
# read over the USB cable with no shell and no network.
#
# WHY IT IS ONE LINE, AND WHY THAT LINE LOOKS LIKE THAT
# ----------------------------------------------------
# The first suspect for "nothing runs" is CRLF: these files are edited and
# copied on Windows, and busybox ash cannot run a script whose lines end in a
# carriage return — `then\r` is not `then`. A launcher that heals that cannot
# itself be a script that CRLF breaks.
#
# One command, and the only CR a CRLF copy can leave is at the very end of it,
# where it becomes a trailing character on the last argument. kual-run.sh
# strips carriage returns from its arguments for exactly that reason. Nothing
# else in this file is syntax that a CR can spoil.
#
# It is also why the real work is in a second file: that one is filtered
# through `tr` before it runs, so it can be written like ordinary shell.
# ============================================================================
exec /bin/sh -c 'd=${0%/*}; [ "$d" = "$0" ] && d=.; r=/tmp/kual-run.$$; tr -d "\r" < "$d/kual-run.sh" > "$r" 2>/dev/null || cp "$d/kual-run.sh" "$r"; exec /bin/sh "$r" "$d" "$@"' "$0" "$@"
