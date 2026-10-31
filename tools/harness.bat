@echo off
rem Builds a test host (build\harness\CrimsonDesert.exe) and stages the plugin next to it. For testing only.
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\harness mkdir build\harness
cl /nologo /W3 /O2 /MT /Fe:build\harness\CrimsonDesert.exe /Fo:build\harness\ tools\harness.cpp || exit /b 1
cl /nologo /W3 /O2 /MT /LD /Fe:build\harness\xinput1_4.dll /Fo:build\harness\fake_ tools\fake_xinput.cpp /link /DEF:tools\fake_xinput.def || exit /b 1
copy /y build\CombatMod.asi build\harness\ >nul
copy /y dist\CombatMod.dev.ini build\harness\CombatMod.ini >nul
copy /y dist\CombatMod.sigs build\harness\ >nul
rem The developer bridge also needs this marker file next to the asi (a release install never has it).
echo developer marker: lets BridgeEnabled=1 open the loopback bridge> build\harness\CombatMod.dev
rem Use port 6401 (never the real game's 6400) and allow `call` for tests.
powershell -NoProfile -Command "(Get-Content build\harness\CombatMod.ini) -replace 'BridgePort=6400','BridgePort=6401' -replace 'AllowCall=0','AllowCall=1' | Set-Content build\harness\CombatMod.ini"
echo HARNESS OK
