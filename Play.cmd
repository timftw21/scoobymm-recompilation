@echo off
setlocal DisableDelayedExpansion
cd /d "%~dp0"
set "game=build\xbe-release\scoobymm.exe"
if not exist "%game%" set "game=build\xbe-compiled\scoobymm.exe"
if not exist "%game%" (
    echo Build the scoobymm target in build\xbe-release or build\xbe-compiled before starting Play.cmd.
    if "%~1"=="" pause
    exit /b 1
)
rem Without options, start the windowed game on its own so this console closes.
if "%~1"=="" (
    start "" "%game%"
    exit /b 0
)
"%game%" %*
exit /b %ERRORLEVEL%
