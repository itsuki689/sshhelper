@echo off
setlocal
cd /d "%~dp0"

where node.exe >nul 2>nul
if errorlevel 1 (
  echo [AirSFTP] Node.js 22 or newer is required to build the EXE.
  pause
  exit /b 1
)

call npm install
if errorlevel 1 goto :failed
call npm run build:portable
if errorlevel 1 goto :failed

echo.
echo [AirSFTP] Portable EXE created: dist\AirSFTP.exe
pause
exit /b 0

:failed
echo.
echo [AirSFTP] Build failed.
pause
exit /b 1
