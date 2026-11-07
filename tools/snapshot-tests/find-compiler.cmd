@echo off
rem Puts the newest Visual Studio's x64 compiler on the PATH, unless one is there already.
if defined VCINSTALLDIR exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :none
set "VSROOT="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT goto :none
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul && exit /b 0
:none
echo Cannot find a C compiler. Run this from a Visual Studio x64 developer prompt.
exit /b 1
