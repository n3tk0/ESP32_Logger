# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec file for ESP32 Logger Deployment GUI

Build with:
    pyinstaller tools/deploy_gui.spec --clean

Output: dist/deploy_gui.exe (Windows) or dist/deploy_gui (macOS/Linux)
"""

import sys
from pathlib import Path

# SPECPATH is set by PyInstaller to the directory containing this spec file.
# __file__ is NOT available in the spec namespace on all platforms.
ROOT = Path(SPECPATH).parent

# Windows version-info resource. Only meaningful on Windows; harmless elsewhere.
# Embedding product metadata reduces antivirus false positives by making the
# binary look like a published app rather than an anonymous PyInstaller stub.
_version_file = ROOT / 'tools' / 'version_info.txt'
version_info = str(_version_file) if sys.platform == 'win32' and _version_file.is_file() else None

a = Analysis(
    [str(ROOT / 'tools' / 'deploy_gui.py')],
    pathex=[str(ROOT / 'tools')],
    binaries=[],
    datas=[],
    hiddenimports=[
        'customtkinter',
        'pyserial',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    # PlatformIO is an external CLI tool (~500MB+ with toolchains) that cannot
    # be bundled. The deploy GUI invokes it via subprocess; users must install
    # PlatformIO separately (pip install platformio, or get the installer).
    excludedimports=['platformio'],
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=None)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='ESP32_Deploy',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    # UPX compression is the single most common antivirus false-positive
    # trigger (heuristics flag packed executables as malware). Disable it.
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,  # Hide console window (True for debugging)
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,  # Optional: add icon path here
    version=version_info,  # Windows version metadata (see version_info.txt)
)

