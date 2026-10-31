@echo off
rem Builds CombatMod.asi with MSVC + Ninja (both from Visual Studio 2026 Community).
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
set "NINJADIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%PATH%;%NINJADIR%"
cd /d "%~dp0.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo BUILD OK
