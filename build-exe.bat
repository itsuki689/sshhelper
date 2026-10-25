@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul || (echo CMake 3.24+ is required.& pause& exit /b 1)
where ninja >nul 2>nul || (echo Ninja is required.& pause& exit /b 1)
where gcc >nul 2>nul || (echo MinGW-w64 GCC is required.& pause& exit /b 1)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
if errorlevel 1 goto :failed
cmake --build build --config Release
if errorlevel 1 goto :failed
ctest --test-dir build --output-on-failure
if errorlevel 1 goto :failed

if not exist dist mkdir dist
copy /B /Y "build\AirSFTP.exe" "dist\AirSFTP.exe" >nul
if errorlevel 1 goto :failed
copy /B /Y "build\sshhelper-tool.exe" "dist\sshhelper-tool.exe" >nul
if errorlevel 1 goto :failed
for %%I in ("dist\AirSFTP.exe") do echo Portable EXE ready: %%~fI  [%%~zI bytes]
for %%I in ("dist\sshhelper-tool.exe") do echo Codex MCP tool ready: %%~fI  [%%~zI bytes]
exit /b 0

:failed
echo Build failed.
pause
exit /b 1
