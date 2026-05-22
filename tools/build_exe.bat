@echo off
REM Build standalone executable for ESP32 Logger Deploy GUI
REM Usage: tools\build_exe.bat

echo Installing PyInstaller...
pip install pyinstaller -q

echo Building executable...
pyinstaller tools\deploy_gui.spec --clean -y

echo.
echo ✓ Windows executable created: dist\ESP32_Deploy.exe
echo.
echo Next steps:
echo   1. Test the executable: dist\ESP32_Deploy.exe
echo   2. Create a release folder
echo   3. Share with users
echo.
pause
