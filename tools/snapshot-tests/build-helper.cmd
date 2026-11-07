@echo off
rem Builds the disk helper and its self-test. Usage: build-helper.cmd <output folder>
setlocal
set "HERE=%~dp0"
set "OUT=%~f1"
if "%OUT%"=="" (
    echo usage: build-helper.cmd ^<output folder^>
    exit /b 2
)
if not exist "%OUT%" mkdir "%OUT%"
call "%HERE%find-compiler.cmd" || exit /b 1
cd /d "%OUT%" || exit /b 1
rem Static CRT and a System32-only DLL search: it runs elevated from a user-writable folder.
cl /nologo /W4 /WX /MT /O1 /I "%HERE%." "%HERE%disk_helper.c" /Fe:asb-disk-helper.exe /link /DEPENDENTLOADFLAG:0x800 || exit /b 1
cl /nologo /W3 /MT /I "%HERE%." "%HERE%selftest_main.c" "%HERE%disk_client.c" /Fe:helper-selftest.exe || exit /b 1
del /q *.obj 2>nul
exit /b 0
