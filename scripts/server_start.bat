@echo off
rem double-click wrapper: bypass Windows Restricted policy, pass args through
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0server_start.ps1" %*
