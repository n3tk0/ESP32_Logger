#!/usr/bin/env bash
# tools/fetch_uplot.sh
#
# Download uPlot.iife.min.js + uPlot.min.css from jsDelivr, place them in
# www/.  After running this, run gzip_www.sh to produce gzipped siblings
# that the ESP32 AsyncWebServer auto-serves with Content-Encoding: gzip.
#
# Usage:
#   chmod +x tools/fetch_uplot.sh
#   ./tools/fetch_uplot.sh [version]
#
# Default version is "1" (latest 1.x).  Pin a specific version by passing it
# as the first argument: ./tools/fetch_uplot.sh 1.6.30

set -euo pipefail

VERSION="${1:-1}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WWW_DIR="$SCRIPT_DIR/../www"

JS_URL="https://cdn.jsdelivr.net/npm/uplot@${VERSION}/dist/uPlot.iife.min.js"
CSS_URL="https://cdn.jsdelivr.net/npm/uplot@${VERSION}/dist/uPlot.min.css"

echo "Fetching uPlot ${VERSION} from jsDelivr..."
curl -fsSL "$JS_URL"  -o "$WWW_DIR/uPlot.iife.min.js"
curl -fsSL "$CSS_URL" -o "$WWW_DIR/uPlot.min.css"

echo ""
echo "Done.  Saved to:"
echo "  $WWW_DIR/uPlot.iife.min.js"
echo "  $WWW_DIR/uPlot.min.css"
echo ""
echo "Next: run tools/gzip_www.sh to produce .gz siblings, then flash www_gz/"
echo "      to LittleFS /www/."
