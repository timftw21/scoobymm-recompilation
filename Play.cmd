@echo off
setlocal DisableDelayedExpansion
cd /d "%~dp0"
set "game=build\xbe-compiled\scoobymm.exe"
if not exist "%game%" (
    echo Build the scoobymm target in build\xbe-compiled before starting Play.cmd.
    if "%~1"=="" pause
    exit /b 1
)
"%game%" %*
set "result=%ERRORLEVEL%"
if not "%result%"=="0" if "%~1"=="" pause
exit /b %result%
