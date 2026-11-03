@echo off
rem Starts the disk helper. Windows asks for approval once, and it then serves every
rem test run until you close its window, the PC restarts, or it has been idle for 12 hours.
rem Usage: start-helper.cmd <helper exe> <test folder>
setlocal
set "EXE=%~f1"
set "FOLDER=%~f2"
if not exist "%EXE%" (
    echo Cannot find the helper: %EXE%
    pause
    exit /b 1
)
if not exist "%FOLDER%" mkdir "%FOLDER%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath '%EXE%' -ArgumentList '\"%FOLDER%\"'"
