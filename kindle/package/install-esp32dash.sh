#!/bin/sh
# ============================================================================
# install-esp32dash.sh — the update script inside the MRPI package
#
# WHAT RUNS THIS
# --------------
# MRPI (the MobileRead Package Installer, a KUAL extension) hands every .bin in
# /mnt/us/mrpackages to the Kindle's own updater. The updater unpacks the
# package into a scratch directory and SOURCES every file in it whose name ends
# in .sh — as root, with the framework paused and the working directory set to
# that scratch directory.
#
# SOURCED, not executed. Two consequences run through this whole file:
#
#   * `exit` would kill the updater in the middle of an update. Every exit here
#     is a `return`, and there is no `set -e`.
#   * every name defined here lands in the updater's shell. Everything is
#     therefore prefixed ESP32DASH_, and nothing is defined that the updater
#     might already own — logmsg included, which is borrowed when it exists
#     (NiLuJe's libotautils provides it) and stubbed only when it does not.
#
# WHY THE PAYLOAD IS A TARBALL
# ----------------------------
# The extension is nine shell scripts, and *any* file ending in .sh anywhere in
# an update package is treated by KindleTool as an install script — so shipping
# them as themselves would have the updater run start.sh, stop.sh, settings.sh
# and update_dash.sh as part of the update. They travel as esp32dash.tar.gz
# instead, which is also how the tree arrives with its layout/ and icons/
# subdirectories intact and its line endings exactly as they were built.
#
# That last part is the point of this package existing at all: nothing between
# the build and the reader gets to reinterpret a byte. No unzipping on Windows,
# no copying files by hand into the wrong folder, no CRLF.
# ============================================================================

# Everything below is written under this prefix. It is EMPTY on a reader, so
# the paths are the absolute ones the updater expects; tests/kindle/drive_package.sh
# points it at a scratch directory, which is the only way any of this can be
# exercised anywhere but on a Kindle in the middle of an update — and an
# installer that has never been run once is a coin toss with somebody's evening.
ESP32DASH_PREFIX="${ESP32DASH_PREFIX:-}"
ESP32DASH_EXTS="$ESP32DASH_PREFIX/mnt/us/extensions"
ESP32DASH_DEST="$ESP32DASH_EXTS/esp32dash"
ESP32DASH_RC=0

# The updater's logger when there is one, stderr when there is not. Same
# argument order as libotautils' logmsg, so the calls read the same either way.
if command -v logmsg > /dev/null 2>&1; then
    esp32dash_log() { logmsg "$1" "esp32dash" "" "$2"; }
else
    esp32dash_log() { echo "esp32dash: $2" >&2; }
fi

esp32dash_log "I" "installing the ESP32 Dashboard KUAL extension"

# ── /mnt/us has to be there and writable ────────────────────────────────────
# It is the FAT partition the USB cable exposes. During an update it is
# normally mounted; if it is not, there is nothing to install onto and saying
# so beats leaving half an extension behind.
if [ ! -d "$ESP32DASH_PREFIX/mnt/us" ]; then
    esp32dash_log "C" "/mnt/us is not mounted - nothing was installed"
    return 1
fi

# ── Keep what the reader typed ──────────────────────────────────────────────
# dash.conf holds the collector's address, found or entered on the device, and
# `collectors` is the scan list behind Next collector. Neither is ours to
# replace: the tarball does not contain them, and the extraction below writes
# over our own files only.
if [ -f "$ESP32DASH_DEST/dash.conf" ]; then
    esp32dash_log "I" "keeping the existing dash.conf"
fi

mkdir -p "$ESP32DASH_DEST" 2> /dev/null

# ── Unpack ──────────────────────────────────────────────────────────────────
# `gunzip -c | tar -x` rather than `tar -xz`: busybox's tar is not always built
# with the gzip applet wired into it, and gunzip is always there.
if [ ! -f ./esp32dash.tar.gz ]; then
    esp32dash_log "C" "the package is missing its payload - nothing was installed"
    return 1
fi

if gunzip -c ./esp32dash.tar.gz | tar -xf - -C "$ESP32DASH_EXTS/"; then
    esp32dash_log "I" "unpacked into $ESP32DASH_DEST"
else
    esp32dash_log "C" "could not unpack the payload - the extension may be incomplete"
    ESP32DASH_RC=1
fi

# ── Say what landed, in the log the extension itself uses ───────────────────
# kual.log is where every menu press is recorded, beside the scripts, readable
# over the USB cable. An install belongs in the same place: "which version is
# on this reader, and when did it get there" is the first question asked of a
# device that is behaving oddly.
if [ -f "$ESP32DASH_DEST/VERSION" ]; then
    esp32dash_log "I" "version $(cat "$ESP32DASH_DEST/VERSION" 2> /dev/null)"
fi
{
    echo "$(date '+%Y-%m-%d %H:%M:%S') --- installed from an MRPI package" \
         "$(cat "$ESP32DASH_DEST/VERSION" 2> /dev/null)"
} >> "$ESP32DASH_DEST/kual.log" 2> /dev/null

# ── FBInk ───────────────────────────────────────────────────────────────────
# Not shipped here: it is a separate project with its own licence, and on a
# jailbroken Kindle it is nearly always already present — the jailbreak hotfix
# installs it as /mnt/us/libkh/bin/fbink. But it is also the one missing piece
# that makes this dashboard start, keep its schedule, and draw absolutely
# nothing, so the install is the right moment to look.
ESP32DASH_FBINK=""
for ESP32DASH_D in "$ESP32DASH_PREFIX/mnt/us/libkh/bin" \
                   "$ESP32DASH_PREFIX/mnt/us/bin" \
                   "$ESP32DASH_PREFIX/mnt/us/fbink" \
                   "$ESP32DASH_EXTS/fbink/bin" "$ESP32DASH_DEST/bin" \
                   /usr/local/bin /bin /usr/bin; do
    if [ -x "$ESP32DASH_D/fbink" ]; then
        ESP32DASH_FBINK="$ESP32DASH_D/fbink"
        break
    fi
done
if [ -n "$ESP32DASH_FBINK" ]; then
    esp32dash_log "I" "FBInk found at $ESP32DASH_FBINK"
else
    esp32dash_log "W" "FBInk was not found - the dashboard cannot draw without it"
    echo "$(date '+%Y-%m-%d %H:%M:%S') FBInk not found at install time" \
        >> "$ESP32DASH_DEST/kual.log" 2> /dev/null
fi

esp32dash_log "I" "done - open KUAL and look for ESP32 Dashboard"

unset ESP32DASH_D ESP32DASH_FBINK ESP32DASH_DEST ESP32DASH_EXTS ESP32DASH_PREFIX
return $ESP32DASH_RC
