$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $repo 'obj\tests'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products * -property installationPath | Select-Object -First 1
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$cmd = @"
cd /d "$repo"
call "$vcvars" >nul
cl /nologo /W4 /WX /TC /Fo:"$outDir\\" /Fe:"$outDir\test_gamepad_protocol.exe" "$repo\tests\test_gamepad_protocol.c" "$repo\src\core\gamepad_protocol.c" "$repo\src\backend_win\vm_gamepad.c" /link ws2_32.lib || exit /b 1
"$outDir\test_gamepad_protocol.exe" || exit /b 1
cl /nologo /W4 /WX /TC /DVGD_CTRL_NO_WIN32=1 /Fo:"$outDir\\" /Fe:"$outDir\test_vgd_codecs.exe" "$repo\tests\test_vgd_codecs.c" "$repo\tools\vgd\vgd_shm.c" "$repo\tools\vgd\vgd_xusb_codec.c" "$repo\tools\vgd\vgd_hid_codec.c" "$repo\tools\agent\vgd_controller.c" "$repo\src\core\gamepad_protocol.c" || exit /b 1
"$outDir\test_vgd_codecs.exe" || exit /b 1
cl /nologo /W4 /WX /TC /Fo:"$outDir\\" /Fe:"$outDir\test_gamepad_provisioning.exe" "$repo\tests\test_gamepad_provisioning.c" "$repo\tools\provision\win_provision.c" "$repo\src\backend_win\gamepad_provision.c" || exit /b 1
"$outDir\test_gamepad_provisioning.exe" || exit /b 1
"@

$tmpBat = Join-Path $outDir 'run_tests.cmd'
Set-Content -Path $tmpBat -Value $cmd -Encoding ASCII
& cmd.exe /c "`"$tmpBat`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "ALL GAMEPAD SEAM TESTS PASSED."
