@echo off
REM Build appsandbox-agent.exe (guest-side agent)
REM Run from VS Developer Command Prompt or after calling vcvarsall.bat

cl /nologo /O2 /W3 /Fe:..\..\release\resources\appsandbox-agent.exe agent.c ws2_32.lib advapi32.lib
if %ERRORLEVEL% EQU 0 (
    echo Built: release\resources\appsandbox-agent.exe
) else (
    echo Build failed.
    exit /b 1
)
