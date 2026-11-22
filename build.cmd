@echo off
rem Builds build\probe_runner.exe with MSVC. Run from a "x64 Native Tools Command Prompt",
rem or let this script find vcvars64.bat through vswhere.
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if not errorlevel 1 goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto nomsvc
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /EHsc /W4 /DUNICODE /D_UNICODE runner\probe_runner.cpp /Fobuild\ /Febuild\probe_runner.exe /link d3d12.lib dxgi.lib || exit /b 1
echo built build\probe_runner.exe
exit /b 0

:nomsvc
echo MSVC not found. Install Visual Studio Build Tools with the C++ workload.
exit /b 1
