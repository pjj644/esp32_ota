@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "FW_DIR=%SCRIPT_DIR%firmware"

rem 允许通过参数指定 STM32 bin 路径, 默认用 cmake 构建产物
if "%~1"=="" (
    set "BUILD_BIN=%SCRIPT_DIR%..\stm32\esp32_test\build\esp32_test.bin"
) else (
    set "BUILD_BIN=%~1"
)

if not exist "%BUILD_BIN%" (
    echo ERROR: STM32 bin not found: %BUILD_BIN%
    echo.
    echo Usage:
    echo   deploy_stm32.bat [path\to\stm32.bin]
    echo.
    echo Default expects: stm32\esp32_test\build\esp32_test.bin
    echo Generate it with: arm-none-eabi-objcopy -O binary esp32_test.elf esp32_test.bin
    pause
    exit /b 1
)

echo Deploying STM32 firmware to %FW_DIR% ...
copy /Y "%BUILD_BIN%" "%FW_DIR%\stm32.bin" > nul
if errorlevel 1 (
    echo ERROR: Failed to copy stm32.bin.
    pause
    exit /b 1
)

echo.
echo [OK] STM32 firmware deployed: %FW_DIR%\stm32.bin
echo.
echo IMPORTANT: Update %FW_DIR%\stm32_version.json with the new version.
echo            Example: {"version":"1.0.1"}
echo            ESP32 只有在新版本大于其 NVS 记录时才会触发刷写。
echo.
pause