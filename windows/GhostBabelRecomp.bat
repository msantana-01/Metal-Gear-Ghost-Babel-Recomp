@echo off
setlocal
set "BASE=%~dp0"
if exist "%BASE%windows\gui.ps1" goto :launch
set "BASE=%~dp0..\"
:launch
powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -STA -File "%BASE%windows\gui.ps1"
if errorlevel 1 pause
