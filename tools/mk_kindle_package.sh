#!/bin/sh
# ============================================================================
# mk_kindle_package.sh — build the Kindle dashboard's installable artifacts
#
# Produces two things from the same staged tree:
#
#   Update_esp32dash_<version>_install.bin   drop into /mnt/us/mrpackages,
#                                            then KUAL -> Helper -> Install MR
#                                            Packages. One file, one button.
#   esp32dash-kindle-<version>.zip           unpack at the USB root instead,
#                                            for a reader without MRPI.
#
# WHY A PACKAGE AND NOT "COPY THE FOLDER"
# ---------------------------------------
# Copying the folder is where the installs went wrong. A checkout on Windows
# rewrites every line ending, and busybox ash cannot run a script whose lines
# end in a carriage return; a half-finished copy leaves a menu that launches
# nothing; and the folder has to land in exactly one place. All three failures
# look identical on the device — KUAL closes, the home screen comes back,
# nothing is said. A build artifact removes the question: the bytes here are
# the bytes that reach the reader.
#
# THE .bin IS A KINDLE UPDATE PACKAGE
# -----------------------------------
# MRPI feeds it to the Kindle's own updater, so it has to be a signed OTA V2
# package. KindleTool builds and signs those with the jailbreak key a hacked
# Kindle already trusts — no secret of ours is involved, and nothing here needs
# one. The invocation mirrors the universal jailbreak hotfix's own, which is
# the current, actively-maintained proof that these settings install across the
# whole FW 5.x range:
#
#     kindletool create ota2 -d kindle5 -s min -t max -O -C . out.bin
#
#   -d kindle5   every FW 5.x device, Touch through Scribe.
#   -s min -t max  no firmware-version gate; without them -O picks a source
#                  revision of FW 5.5.0 and readers below it refuse the package.
#   -O           the versioned OTA bundle type (FC04). Dropping it gives FD04
#                for a kindle5 target, which is the first thing to try if a
#                device ever rejects the package outright.
#   -C .         store paths relative to the staging directory rather than as
#                the absolute paths they were passed as.
#
# Usage:
#   tools/mk_kindle_package.sh [--version V] [--out DIR] [--stage-only]
#
#   --stage-only   build the staging tree and the zip, skip KindleTool. This is
#                  the half a test can run anywhere, and it is where every
#                  property that matters on the reader is decided.
# ============================================================================
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/kindle"
VERSION=""
STAGE_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --out)     OUT="$2"; shift 2 ;;
        --stage-only) STAGE_ONLY=1; shift ;;
        -h|--help) sed -n '2,50p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# A version for the file name and for the VERSION file the extension carries.
# `git describe` when this is a checkout, the tag CI was built from otherwise,
# and "dev" when it is neither — never an empty string, which would produce
# "Update_esp32dash__install.bin" and a VERSION file nobody can read.
if [ -z "$VERSION" ]; then
    VERSION=$(cd "$ROOT" && git describe --tags --always --dirty 2>/dev/null || true)
fi
[ -z "$VERSION" ] && VERSION="dev"

STAGE="$OUT/stage"          # what goes into the .bin: script + payload tarball
TREE="$OUT/tree"            # the extension as it will exist on the reader
rm -rf "$OUT"
mkdir -p "$STAGE" "$TREE/esp32dash"

# ── The payload: every file the repository tracks under kindle/ ─────────────
#
# Asked of git rather than listed here, because a list here is a list that goes
# stale — a new layout file or icon would be missing from the package and from
# nothing else, and the reader would fall back to a 600x800 layout on a
# 1072x1448 panel with no error anywhere. kindle/package/ is the exception: it
# is the packaging itself, and does not belong on the device.
FILES=$(cd "$ROOT" && git ls-files kindle/ 2>/dev/null | grep -v '^kindle/package/' || true)
if [ -z "$FILES" ]; then
    # Not a checkout (a release tarball, say). Take what is there instead.
    FILES=$(cd "$ROOT" && find kindle -type f ! -path 'kindle/package/*' \
            ! -name dash.conf ! -name collectors ! -name kual.log | sort)
fi

COUNT=0
for f in $FILES; do
    rel=${f#kindle/}
    mkdir -p "$TREE/esp32dash/$(dirname "$rel")"
    cp "$ROOT/$f" "$TREE/esp32dash/$rel"
    COUNT=$((COUNT + 1))
done
echo "$VERSION" > "$TREE/esp32dash/VERSION"
echo "staged $COUNT file(s) as esp32dash/ (version $VERSION)"

# ── The tarball ─────────────────────────────────────────────────────────────
# Reproducible on purpose: same input, same bytes, so two builds of the same
# commit can be compared. The extension's own scripts must NOT arrive as .sh
# files in the package root — KindleTool treats every .sh in an update package
# as something for the updater to run, which would have the Kindle execute
# start.sh and update_dash.sh as part of the update itself.
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime='1970-01-01 00:00:00 UTC' \
    -czf "$STAGE/esp32dash.tar.gz" -C "$TREE" esp32dash

cp "$ROOT/kindle/package/install-esp32dash.sh" "$STAGE/"

# ── The zip, for a reader without MRPI ──────────────────────────────────────
# Rooted at extensions/ so it unpacks straight onto the USB volume. A zip
# stores bytes; no unpacker on any platform rewrites a line ending.
ZIPTREE="$OUT/zip"
mkdir -p "$ZIPTREE/extensions"
cp -R "$TREE/esp32dash" "$ZIPTREE/extensions/"
mkdir -p "$ZIPTREE/mrpackages"
cat > "$ZIPTREE/mrpackages/README.txt" <<'TXT'
Drop Update_esp32dash_*_install.bin in this folder, eject the Kindle, then
open KUAL -> Helper -> Install MR Packages.
TXT
ZIP="$OUT/esp32dash-kindle-$VERSION.zip"
(cd "$ZIPTREE" && zip -q -r -X "$ZIP" extensions mrpackages)
echo "wrote $ZIP"

if [ "$STAGE_ONLY" = "1" ]; then
    echo "stage-only: $STAGE"
    exit 0
fi

# ── The update package ──────────────────────────────────────────────────────
KINDLETOOL="${KINDLETOOL:-kindletool}"
if ! command -v "$KINDLETOOL" > /dev/null 2>&1; then
    echo "" >&2
    echo "kindletool not found." >&2
    echo "Build it from https://github.com/NiLuJe/KindleTool (zlib1g-dev," >&2
    echo "libarchive-dev, nettle-dev, then make), or set KINDLETOOL=/path." >&2
    echo "Everything above this line was still produced; only the .bin was not." >&2
    exit 3
fi

BIN="$OUT/Update_esp32dash_${VERSION}_install.bin"
(cd "$STAGE" && "$KINDLETOOL" create ota2 -d kindle5 -s min -t max -O -C . "$BIN")
echo "wrote $BIN"
