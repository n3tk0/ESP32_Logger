#!/bin/sh
# ============================================================================
# drive_package.sh — build the MRPI package and install it into a fake Kindle
#
# WHY
# ---
# The installer is sourced by the Kindle's own updater, as root, with the
# framework paused. There is no console, no shell and no second chance: if it
# is wrong, the reader says "Your Kindle did not update" — or worse, says
# nothing and leaves half an extension behind — and the person holding it has
# no way to find out why. It is the least debuggable code in this repository
# and it had better be the most tested.
#
# So this stages the package, asserts the properties that decide what the
# updater will do with it, and then RUNS the installer against a scratch tree
# that stands in for /mnt/us.
#
#     sh tests/kindle/drive_package.sh
# ============================================================================
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

FAILURES=0
CHECKS=0
check() {
    CHECKS=$((CHECKS + 1))
    if [ "$1" -eq 0 ]; then
        echo "  ok   $2"
    else
        FAILURES=$((FAILURES + 1))
        echo "  FAIL $2"
    fi
}

echo "The package, as it is built:"

sh "$ROOT/tools/mk_kindle_package.sh" --stage-only --version t-e-s-t --out "$WORK/out" \
    > "$WORK/build.log" 2>&1
check "$?" "tools/mk_kindle_package.sh --stage-only runs"
STAGE="$WORK/out/stage"

check "$([ -f "$STAGE/install-esp32dash.sh" ] && echo 0 || echo 1)" \
      "the update script is in the package root"
check "$([ -f "$STAGE/esp32dash.tar.gz" ] && echo 0 || echo 1)" \
      "and the extension travels beside it as one tarball"

# THE PROPERTY THIS PACKAGING EXISTS FOR. KindleTool marks *every* file whose
# name ends in .sh as something for the updater to run — not just the ones in
# the root, and not just the one we meant. Ship the extension as itself and the
# Kindle executes start.sh, stop.sh, settings.sh, kual.sh and update_dash.sh as
# part of the update, in whatever order they happen to be listed.
shs=$(find "$STAGE" -name '*.sh' | wc -l | tr -dc 0-9)
check "$([ "$shs" = "1" ] && echo 0 || echo 1)" \
      "exactly one .sh in the whole package ($shs) — the updater runs all of them"

# The one that is there must be sourceable, not executable-with-exit: `exit` in
# a sourced script ends the UPDATER, mid-update.
bad=$(sed 's/#.*//' "$STAGE/install-esp32dash.sh" \
      | grep -nE '(^|[[:space:];&|(])exit([[:space:]]|$)' || true)
check "$([ -z "$bad" ] && echo 0 || echo 1)" \
      "it never calls exit — it is sourced, and exit would end the updater${bad:+ ($bad)}"
check "$(grep -q '^return ' "$STAGE/install-esp32dash.sh" && echo 0 || echo 1)" \
      "  it returns instead"
# A CR in this one is not a dashboard that misbehaves — it is an update that
# fails, on a device whose only report is "Your Kindle did not update".
check "$(od -c "$STAGE/install-esp32dash.sh" | grep -q '\\r' && echo 1 || echo 0)" \
      "  and carries no carriage return into the updater"

# Everything the repository ships for the Kindle has to be in the tarball. A
# missing layout file is a 1072x1448 panel drawn to a 600x800 layout, with no
# error anywhere; a missing icon is a blank square in the forecast.
tracked=$(cd "$ROOT" && git ls-files kindle/ | grep -v '^kindle/package/' | sed 's|^kindle/||' | sort)
packed=$(tar tzf "$STAGE/esp32dash.tar.gz" | sed -n 's|^esp32dash/||p' \
         | grep -v '/$' | grep -v '^VERSION$' | grep -v '^$' | sort)
echo "$packed" > "$WORK/packed.txt"
missing=$(echo "$tracked" | comm -23 - "$WORK/packed.txt")
check "$([ -z "$missing" ] && echo 0 || echo 1)" \
      "every tracked kindle/ file is in the payload${missing:+ — missing: $(echo $missing)}"
check "$(echo "$packed" | grep -q '^update_dash.sh$' && echo 0 || echo 1)" \
      "  including the renderer itself"
check "$(echo "$packed" | grep -q '^layout/1072x1448.conf$' && echo 0 || echo 1)" \
      "  and both layouts"
check "$(tar tzf "$STAGE/esp32dash.tar.gz" | grep -q '^esp32dash/VERSION$' && echo 0 || echo 1)" \
      "a VERSION file rides along, so a reader can say what it is running"

# The whole reason for a build artifact: nothing between here and the reader
# gets to reinterpret a byte.
mkdir -p "$WORK/unpacked"
tar xzf "$STAGE/esp32dash.tar.gz" -C "$WORK/unpacked"
crlf=""
for f in $(find "$WORK/unpacked" -type f \( -name '*.sh' -o -name '*.conf' \
                -o -name '*.json' -o -name '*.xml' -o -name VERSION \)); do
    if od -c "$f" | grep -q '\\r'; then crlf="$crlf $(basename "$f")"; fi
done
check "$([ -z "$crlf" ] && echo 0 || echo 1)" \
      "no carriage return in anything the Kindle parses${crlf:+ —$crlf}"

# ── Installing it ───────────────────────────────────────────────────────────
echo ""
echo "Installing it into a Kindle that is not there:"

FAKE="$WORK/kindle"
mkdir -p "$FAKE/mnt/us/extensions"
cd "$STAGE"

# A reader that has been set up already: the address it found, and the scan
# list behind Next collector. An installer that loses those has sent somebody
# back to the Find collector screen for no reason.
mkdir -p "$FAKE/mnt/us/extensions/esp32dash"
echo 'HOST=192.168.100.8' > "$FAKE/mnt/us/extensions/esp32dash/dash.conf"
echo '192.168.100.8'      > "$FAKE/mnt/us/extensions/esp32dash/collectors"

( ESP32DASH_PREFIX="$FAKE" . ./install-esp32dash.sh ) > "$WORK/install.log" 2>&1
check "$?" "the installer returns 0"

D="$FAKE/mnt/us/extensions/esp32dash"
check "$([ -f "$D/update_dash.sh" ] && echo 0 || echo 1)" \
      "the extension is on the device"
check "$([ -f "$D/menu.json" ] && [ -f "$D/kual.sh" ] && [ -f "$D/kual-run.sh" ] && echo 0 || echo 1)" \
      "  with the menu and the launcher KUAL will call"
check "$([ -f "$D/icons/1072/fc_-1_61.bmp" ] && echo 0 || echo 1)" \
      "  and the icons, in their subdirectory"
check "$([ "$(cat "$D/dash.conf")" = "HOST=192.168.100.8" ] && echo 0 || echo 1)" \
      "the collector address that was already there survived the install"
check "$([ "$(cat "$D/collectors")" = "192.168.100.8" ] && echo 0 || echo 1)" \
      "  and so did the scan list"
check "$([ "$(cat "$D/VERSION")" = "t-e-s-t" ] && echo 0 || echo 1)" \
      "the version that was built is the version on the device"
check "$(grep -q "installed from an MRPI package" "$D/kual.log" && echo 0 || echo 1)" \
      "the install is recorded in kual.log, next to the scripts"
check "$(grep -q "FBInk" "$D/kual.log" && echo 0 || echo 1)" \
      "  along with FBInk being absent, which is why nothing would draw"

# The updater SOURCES this file, so everything it defines outlives it and is
# in scope for whatever the same package run sources next. Its own header
# promises it leaves nothing behind; this is what holds it to that.
leftovers=$( ESP32DASH_PREFIX="$FAKE" sh -c '
    . ./install-esp32dash.sh > /dev/null 2>&1
    set | grep "^ESP32DASH_" | cut -d= -f1
    command -v esp32dash_log > /dev/null 2>&1 && echo esp32dash_log
    true' 2>/dev/null )
check "$([ -z "$leftovers" ] && echo 0 || echo 1)" \
      "it leaves nothing of its own in the updater's shell${leftovers:+ — $(echo $leftovers)}"

# Installing over an install is the normal case — every update after the first.
( ESP32DASH_PREFIX="$FAKE" . ./install-esp32dash.sh ) >> "$WORK/install.log" 2>&1
check "$?" "installing over an existing install returns 0"
check "$([ "$(cat "$D/dash.conf")" = "HOST=192.168.100.8" ] && echo 0 || echo 1)" \
      "  and still keeps dash.conf"

# A package that arrives without its payload must say so and change nothing,
# rather than leaving an empty extension folder that KUAL will happily list.
BROKEN="$WORK/broken"
mkdir -p "$BROKEN/mnt/us/extensions"
cp "$STAGE/install-esp32dash.sh" "$WORK/"
( cd "$WORK" && ESP32DASH_PREFIX="$BROKEN" . ./install-esp32dash.sh ) \
    > "$WORK/broken.log" 2>&1
check "$([ "$?" != "0" ] && echo 0 || echo 1)" \
      "a package with no payload fails instead of installing nothing quietly"
check "$(grep -q "missing its payload" "$WORK/broken.log" && echo 0 || echo 1)" \
      "  and says which half is missing"

# No /mnt/us at all: the FAT partition is not mounted. Nothing to install onto.
NOMNT="$WORK/nomnt"
mkdir -p "$NOMNT"
( ESP32DASH_PREFIX="$NOMNT" . ./install-esp32dash.sh ) > "$WORK/nomnt.log" 2>&1
check "$([ "$?" != "0" ] && echo 0 || echo 1)" \
      "an unmounted /mnt/us fails rather than creating one"

# ── The zip route ───────────────────────────────────────────────────────────
echo ""
echo "The zip, for a reader without MRPI:"
ZIP=$(ls "$WORK/out"/*.zip 2>/dev/null | head -1)
check "$([ -n "$ZIP" ] && echo 0 || echo 1)" "a zip is built alongside the package"
if [ -n "$ZIP" ]; then
    check "$(unzip -l "$ZIP" | grep -q 'extensions/esp32dash/menu.json' && echo 0 || echo 1)" \
          "  rooted at extensions/, so it unpacks onto the USB volume as-is"
    check "$(unzip -l "$ZIP" | grep -q 'mrpackages/' && echo 0 || echo 1)" \
          "  and it creates mrpackages/, which MRPI reads and which is easy to miss"
fi

echo ""
if [ "$FAILURES" -gt 0 ]; then
    echo "FAIL: $FAILURES of $CHECKS check(s) failed"
    exit 1
fi
echo "OK: $CHECKS checks — the package installs, and keeps what was there"
