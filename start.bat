@echo off
setlocal
cd /d "%~dp0"

where node.exe >nul 2>nul
if errorlevel 1 (
  echo [AirSFTP] Node.js is required for this BAT launcher.
  echo Use dist\AirSFTP.exe on a computer without Node.js.
  pause
  exit /b 1
)

if not exist "node_modules\ssh2\package.json" (
  echo [AirSFTP] Installing the SSH dependency...
  call npm install --omit=dev
  if errorlevel 1 (
    echo [AirSFTP] Dependency installation failed.
    pause
    exit /b 1
  )
)

set "AIRSFTP_OPEN=1"
node server.js
if errorlevel 1 pause
endlocal
