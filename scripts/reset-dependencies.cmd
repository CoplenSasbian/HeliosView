@echo off
setlocal
cd /d "%~dp0.."
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0reset-dependencies.ps1" %*
exit /b %errorlevel%
