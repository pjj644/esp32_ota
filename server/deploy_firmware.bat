@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "BUILD_BIN=%SCRIPT_DIR%..\build\local_test.bin"
set "FW_DIR=%SCRIPT_DIR%firmware"

if not exist "%BUILD_BIN%" (
    echo ERROR: Build output not found: %BUILD_BIN%
    echo Please run "idf.py build" in the local_test directory first.
    pause
    exit /b 1
)

echo Deploying firmware to %FW_DIR% ...
copy /Y "%BUILD_BIN%" "%FW_DIR%\firmware.bin" > nul
if errorlevel 1 (
    echo ERROR: Failed to copy firmware.
    pause
    exit /b 1
)

echo.
echo [OK] Firmware deployed: %FW_DIR%\firmware.bin
echo.
echo IMPORTANT: Update %FW_DIR%\version.json with the new version.
echo            The version must match CONFIG_APP_PROJECT_VER in sdkconfig.defaults.
echo            Example: {"version":"1.0.2"}
echo.
pause
