#!/usr/bin/env bash
# tools/gzip_www.sh
#
# Gzip www/ assets before flashing to save ~60% LittleFS space.
#
# Usage:
#   chmod +x tools/gzip_www.sh
#   ./tools/gzip_www.sh            # gzip www/ → www_gz/
#   ./tools/gzip_www.sh --restore  # copy original www/ files back
#
# The ESP32 AsyncWebServer transparently serves .gz files when the client
# sends "Accept-Encoding: gzip" — which every modern browser does.
# Files that should NOT be gzipped (e.g. binary blobs) are excluded below.
#
# After running this script, flash the www_gz/ directory contents to LittleFS
# using PlatformIO's data/ folder or the Arduino IDE LittleFS uploader.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WWW_DIR="$SCRIPT_DIR/../www"
OUT_DIR="$SCRIPT_DIR/../www_gz"

# File extensions to gzip (everything else is copied as-is)
GZIP_EXTS="html js css txt json"

# Extensions to SKIP (already compressed or binary)
SKIP_EXTS="gz bin png jpg jpeg gif ico woff woff2 ttf"

if [[ "${1:-}" == "--restore" ]]; then
    echo "Restoring: copying www/ → www_gz/ without compression"
    rm -rf "$OUT_DIR"
    cp -r "$WWW_DIR" "$OUT_DIR"
    echo "Done. www_gz/ contains uncompressed originals."
    exit 0
fi

echo "Gzipping www/ assets → www_gz/"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

shopt -s globstar nullglob

total=0
skipped=0
compressed=0

for src in "$WWW_DIR"/**; do
    [[ -f "$src" ]] || continue
    rel="${src#$WWW_DIR/}"
    dst_dir="$OUT_DIR/$(dirname "$rel")"
    mkdir -p "$dst_dir"
    ext="${src##*.}"

    # Check if extension is in the skip list
    skip=false
    for s in $SKIP_EXTS; do
        if [[ "$ext" == "$s" ]]; then skip=true; break; fi
    done

    # Check if extension should be gzipped
    do_gz=false
    for g in $GZIP_EXTS; do
        if [[ "$ext" == "$g" ]]; then do_gz=true; break; fi
    done

    if $skip; then
        cp "$src" "$OUT_DIR/$rel"
        ((skipped++)) || true
    elif $do_gz; then
        gzip -9 -c "$src" > "$OUT_DIR/${rel}.gz"
        orig=$(wc -c < "$src")
        comp=$(wc -c < "$OUT_DIR/${rel}.gz")
        saving=$(( (orig - comp) * 100 / (orig + 1) ))
        printf "  %-40s %5d → %5d bytes  (%d%% smaller)\n" "$rel" "$orig" "$comp" "$saving"
        ((compressed++)) || true
    else
        cp "$src" "$OUT_DIR/$rel"
        ((skipped++)) || true
    fi
    ((total++)) || true
done

echo ""
echo "Done. $total file(s) processed: $compressed gzipped, $skipped copied as-is."
echo "Flash the contents of www_gz/ to LittleFS /www/"
