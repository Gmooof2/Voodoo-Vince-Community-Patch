@echo off
title Voodoo Vince Community Patch
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0VinceAIO.ps1" %*
set "result=%errorlevel%"
echo.
pause
exit /b %result%
