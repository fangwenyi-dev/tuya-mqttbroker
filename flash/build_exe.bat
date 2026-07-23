@echo off
chcp 65001 >nul
title TuyaOpen Bridge Flash Tool - Build Script

echo ============================================================
echo  TuyaOpen Bridge Flash Tool - PyInstaller Build Script
echo ============================================================
echo.

REM Check Python
where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found, please install Python 3.8+ and add to PATH
    pause
    exit /b 1
)

echo [1/4] Checking dependencies...
python -c "import PyInstaller" 2>nul
if errorlevel 1 (
    echo     Installing PyInstaller...
    pip install pyinstaller
)

python -c "import serial" 2>nul
if errorlevel 1 (
    echo     Installing pyserial...
    pip install pyserial
)

python -c "import qrcode" 2>nul
if errorlevel 1 (
    echo     Installing qrcode...
    pip install "qrcode[pil]"
)

python -c "import PIL" 2>nul
if errorlevel 1 (
    echo     Installing Pillow...
    pip install Pillow
)

python -c "import win32print" 2>nul
if errorlevel 1 (
    echo     Installing pywin32...
    pip install pywin32
)

python -c "import esptool" 2>nul
if errorlevel 1 (
    echo     Installing esptool...
    pip install esptool
)

echo.
echo [2/4] Cleaning old build artifacts...
if exist build rmdir /s /q build
if exist dist rmdir /s /q dist

echo.
echo [3/4] Building (may take a few minutes)...
python -m PyInstaller --noconfirm --clean MatterBridgeFlashTool.spec

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed, check error messages above
    pause
    exit /b 1
)

echo.
echo [4/4] Build complete!
echo.
echo ============================================================
echo  EXE location: dist\TuyaOpenBridgeFlashTool.exe
echo ============================================================
echo.
echo Copy dist\TuyaOpenBridgeFlashTool.exe to any location to use.
echo Config file settings.json will be auto-created next to the exe.
echo.
pause
