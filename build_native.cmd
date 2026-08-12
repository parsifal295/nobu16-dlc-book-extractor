@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_native.ps1"
exit /b %errorlevel%
