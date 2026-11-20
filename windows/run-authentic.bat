@echo off
setlocal
set "BASE=%~dp0"
if exist "%BASE%windows\build.ps1" goto :run
set "BASE=%~dp0..\"
:run
set "EXE=%BASE%build\ghost-babel-authentic.exe"
if not exist "%EXE%" (
    if exist "%BASE%build\ghost-babel.exe" (
        set "EXE=%BASE%build\ghost-babel.exe"
    ) else (
        echo The game has not been built yet.
        echo Run GhostBabelRecomp.bat or drag your ROM onto build.bat first.
        pause
        exit /b 1
    )
)
pushd "%BASE%build" >nul
"%EXE%" %*
set "RC=%ERRORLEVEL%"
popd >nul
exit /b %RC%
