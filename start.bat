@echo off
setlocal
cd /d "%~dp0"
if not exist "dist\AirSFTP.exe" (
  call build-exe.bat
  if errorlevel 1 exit /b 1
)
start "" "dist\AirSFTP.exe"
