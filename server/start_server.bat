@echo off
chcp 65001 > nul
cd /d "%~dp0"
echo [start] Starting local ESP32 OTA server ...
echo [hint]  If Windows Firewall asks, allow Node.js to access the network.
echo.
node server.js
pause
