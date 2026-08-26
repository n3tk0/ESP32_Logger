#!/bin/bash
# Build standalone executable for ESP32 Logger Deploy GUI
# Usage: ./tools/build_exe.sh

set -e

cd "$(dirname "$0")/.."

echo "Installing PyInstaller..."
pip install pyinstaller -q

echo "Building executable..."
pyinstaller tools/deploy_gui.spec --clean -y

# The spec has no BUNDLE section, so PyInstaller emits a plain binary on macOS
# too — not an .app. This used to point at dist/ESP32_Deploy.app/Contents/...,
# a path that has never existed.
if [ "$(uname)" == "Darwin" ]; then
    EXE="dist/ESP32_Deploy"
    echo ""
    echo "✓ macOS binary created: $EXE"
    echo "  Run: ./$EXE"
    echo "  If Gatekeeper blocks it: xattr -d com.apple.quarantine $EXE"
elif [ "$(uname -s)" == "Linux" ]; then
    EXE="dist/ESP32_Deploy"
    echo ""
    echo "✓ Linux executable created: $EXE"
    echo "  Run: ./$EXE"
else
    EXE="dist/ESP32_Deploy.exe"
    echo ""
    echo "✓ Windows executable created: $EXE"
    echo "  Run: .\$EXE"
fi

echo ""
echo "Build successful!"
echo ""
echo "Next steps:"
echo "  1. Test the executable"
echo "  2. Create a release/dist folder"
echo "  3. Share with users"
