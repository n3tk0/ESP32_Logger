@echo off
REM Build standalone executable for ESP32 Logger Deploy GUI
REM Usage: double-click, or run "tools\build_exe.bat" from anywhere.

REM Move to the project root (parent of this script's tools\ folder) so the
REM spec path resolves correctly no matter where the script is launched from.
cd /d "%~dp0.."

echo Installing PyInstaller...
pip install pyinstaller -q
if errorlevel 1 (
    echo ERROR: Failed to install PyInstaller.
    pause
    exit /b 1
)

echo Building executable...
pyinstaller tools\deploy_gui.spec --clean -y
if errorlevel 1 (
    echo ERROR: PyInstaller build failed. See output above.
    pause
    exit /b 1
)

echo.
echo [OK] Windows executable created: dist\ESP32_Deploy.exe
echo.
echo Next steps:
echo   1. Test the executable: dist\ESP32_Deploy.exe
echo   2. Create a release folder
echo   3. Share with users
echo.
pause
