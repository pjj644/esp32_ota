@echo off
rem double-click wrapper: bypass Windows Restricted policy, pass args through
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_esp32.ps1" %*
