@echo off
setlocal
set "BASE=%~dp0"
if exist "%BASE%windows\build.ps1" goto :check_arg
set "BASE=%~dp0..\"
:check_arg
if "%~1"=="" (
    echo Usage: drag and drop your .gbc ROM onto build.bat, or run:
    echo   build.bat path\to\rom.gbc
    echo.
    echo Tip: double-click GhostBabelRecomp.bat for the graphical window.
    pause
    exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%BASE%windows\build.ps1" -RomPath "%~1"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo Build failed. See .build.log for details.
)
pause
exit /b %RC%
