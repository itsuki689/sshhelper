@echo off
setlocal
cd /d "%~dp0"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\verify-dist.ps1"
if errorlevel 1 (
  echo Distribution verification failed. Download and extract the complete project ZIP again.
  exit /b 1
)
start "" "%~dp0dist\AirSFTP.exe"
