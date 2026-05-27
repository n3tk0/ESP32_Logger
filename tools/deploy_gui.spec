# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec file for ESP32 Logger Deployment GUI

Build with:
    pyinstaller tools/deploy_gui.spec --clean

Output: dist/deploy_gui.exe (Windows) or dist/deploy_gui (macOS/Linux)
"""

import sys
from pathlib import Path

# Determine the project root
ROOT = Path(__file__).parent.parent

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
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,  # Hide console window (True for debugging)
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,  # Optional: add icon path here
)
