@echo off
chcp 65001 > nul
echo Current local IPv4 addresses (non-loopback):
powershell -Command "Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -ne '127.0.0.1' -and $_.IPAddress -notlike '169.254.*' } | Select-Object InterfaceAlias, IPAddress | Format-Table -AutoSize"
echo.
echo Please make sure LOCAL_HOST in main/main.c matches one of the IPs above.
pause
