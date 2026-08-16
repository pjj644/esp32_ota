@echo off
rem double-click wrapper: bypass Windows Restricted policy, pass args through
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_stm32.ps1" %*
