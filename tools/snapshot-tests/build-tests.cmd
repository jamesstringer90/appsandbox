@echo off
rem Builds the tests against one version of the code.
rem Usage: build-tests.cmd <root of that version's source> <output folder>
rem The test source comes from this folder, whichever version is being tested.
setlocal
set "HERE=%~dp0"
set "SRC=%~f1"
set "OUT=%~f2"
if "%OUT%"=="" (
    echo usage: build-tests.cmd ^<source root^> ^<output folder^>
    exit /b 2
)
if not exist "%SRC%\src\backend_win\snapshot.c" (
    echo No snapshot.c under %SRC%
    exit /b 2
)
if not exist "%OUT%" mkdir "%OUT%"
call "%HERE%find-compiler.cmd" || exit /b 1
cd /d "%OUT%" || exit /b 1
del /q *.obj *.exe 2>nul

rem Cut the product's own vhdx_create_differencing out of its disk_util.c.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$l = Get-Content -LiteralPath '%SRC%\src\backend_win\disk_util.c'; $a = ($l | Select-String -Pattern '^HRESULT vhdx_create_differencing' | Select-Object -First 1).LineNumber; if (-not $a) { exit 1 }; $b = $a; while ($l[$b - 1] -ne '}') { $b++ }; $l[($a - 1)..($b - 1)] | Set-Content -LiteralPath 'vhdx_create_differencing.inc' -Encoding ascii; $h = (Get-FileHash -LiteralPath '%SRC%\src\backend_win\snapshot.c' -Algorithm SHA256).Hash.Substring(0,16); $i = (Get-FileHash -LiteralPath 'vhdx_create_differencing.inc' -Algorithm SHA256).Hash.Substring(0,16); Set-Content -LiteralPath 'source.txt' -Encoding ascii -Value ('snapshot.c ' + $h + ' vhdx_create_differencing ' + $i)"
if errorlevel 1 exit /b 1

set "INC=/I "%HERE%." /I "%SRC%\src\backend_win" /I "%OUT%""
set "FLAGS=/nologo /W3 /MT /Zi /DASB_API="
cl %FLAGS% %INC% /c /FI"%HERE%fault_hooks.h" "%SRC%\src\backend_win\snapshot.c" /Fo:snapshot.obj || exit /b 1
cl %FLAGS% %INC% /c "%HERE%fault_hooks.c" "%HERE%disk_client.c" "%HERE%snapshot_tests.c" || exit /b 1
link /nologo /DEBUG /OUT:snapshot_tests.exe snapshot.obj fault_hooks.obj disk_client.obj snapshot_tests.obj || exit /b 1
del /q *.obj 2>nul
exit /b 0
