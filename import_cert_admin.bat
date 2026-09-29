@echo off
setlocal
cd /d "%~dp0"
echo Requesting Administrator privileges to import certificate...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell.exe -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0import_cert.ps1\"' -Verb RunAs"
