@echo off
setlocal
cd /d "%~dp0"
if not exist "dist\sshhelper.exe" (
  echo Download the Windows ZIP from https://github.com/itsuki689/sshhelper/releases/latest and extract it completely.
  exit /b 1
)
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\verify-dist.ps1"
if errorlevel 1 (
  echo Release verification failed. Download and extract the complete Release ZIP again.
  exit /b 1
)
start "" "%~dp0dist\sshhelper.exe" %*
