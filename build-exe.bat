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

for %%I in ("build\AirSFTP.exe") do echo Local portable EXE ready: %%~fI  [%%~zI bytes]
for %%I in ("build\sshhelper-tool.exe") do echo Local MCP tool ready: %%~fI  [%%~zI bytes]
echo To update the tracked dist from successful CI, use scripts\update-dist.ps1.
exit /b 0

:failed
echo Build failed.
pause
exit /b 1
