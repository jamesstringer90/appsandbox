@echo off
rem Builds everything and runs the tests against this source tree. Windows asks for approval
rem once, to start the disk helper, unless it is running already. Results are in out\chains.
rem Usage: run.cmd [--filter <scenario>] [--keep]
setlocal
set "HERE=%~dp0"
for %%I in ("%HERE%..\..") do set "ROOT=%%~fI"
set "OUT=%HERE%out"
set "CHAINS=%OUT%\chains"
if not exist "%CHAINS%" mkdir "%CHAINS%"
call "%HERE%find-compiler.cmd" || exit /b 1

set "PIPE=\\.\pipe\asb-snapshot-tests-disk-helper"
powershell -NoProfile -Command "exit [int](-not (Test-Path '%PIPE%'))"
if not errorlevel 1 goto :helper_up
call "%HERE%build-helper.cmd" "%OUT%\helper" || exit /b 1
call "%HERE%start-helper.cmd" "%OUT%\helper\asb-disk-helper.exe" "%CHAINS%"
echo Waiting for the disk helper. Approve the prompt if Windows shows one.
powershell -NoProfile -Command "$n=0; while (-not (Test-Path '%PIPE%') -and $n -lt 120) { Start-Sleep 1; $n++ }; exit [int](-not (Test-Path '%PIPE%'))"
if errorlevel 1 (
    echo The disk helper did not start.
    exit /b 1
)
:helper_up
call "%HERE%build-tests.cmd" "%ROOT%" "%OUT%\tests" || exit /b 1
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "RUN=%CHAINS%\%%T"
"%OUT%\tests\snapshot_tests.exe" run "%RUN%" "this tree" %*
exit /b %ERRORLEVEL%
