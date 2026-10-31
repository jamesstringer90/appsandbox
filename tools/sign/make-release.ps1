<#
  make-release.ps1 - Build AppSandbox, then conditionally sign + package.

  Flow:
    1. Build the solution (Release|$Platform) unless -NoBuild.
    2. Check whether the App Sandbox LLC EV YubiKey is plugged in.
         - NOT present  -> SKIP signing, driver attestation, and ZIP.
                           The (test-signed) dev build is left as-is. Done.
         - present      -> a) EV-sign every binary we build (one PIN prompt),
                           b) ensure the drivers are Microsoft attestation-signed
                              (reuse cached signed drivers, or run sign-drivers.ps1),
                           c) stage bin\Release minus symbols/intermediates and ZIP it.

  So nothing is signed or zipped on an ordinary dev machine without the token.

  USAGE:
    .\make-release.ps1                  # x64: build + (if YubiKey) sign + attest + zip
    .\make-release.ps1 -Platform ARM64  # Windows on ARM (bin\Release-ARM64 -> *-win-arm64.zip)
    .\make-release.ps1 -NoBuild         # package the current bin\Release[-ARM64]
    .\make-release.ps1 -SkipDrivers     # sign app binaries + zip; leave drivers as-is
    .\make-release.ps1 -ForceDriverSign # re-submit drivers for attestation even if cached

  -Platform selects the bin\ output dir (ARM64 -> bin\Release-ARM64), the driver
  attestation OS/signature codes (sign-drivers.ps1 -Platform rewrites the x64 config
  at sign time), and the zip name. The AppSandboxPackage project passes
  -Platform $(Platform) after a Release build.
#>
[CmdletBinding()]
param(
  [string]$Configuration = 'Release',
  [ValidateSet('x64','ARM64')][string]$Platform = 'x64',
  [string]$OS            = 'win',   # OS token in the zip name; not passed to MSBuild
  [string]$Version       = '',     # empty => read from Directory.Build.props (single source)
  [switch]$NoBuild,
  [switch]$BuildOnly,
  [switch]$SkipDrivers,
  [switch]$ForceDriverSign
)
$ErrorActionPreference = 'Stop'

# Re-entry guard: when THIS script builds the solution, the AppSandboxPackage project's
# post-build would otherwise invoke this script again (nested). This env var tells that
# project to stand down during our own build. (A VS "Build Solution", which has no env
# var set, still triggers the project normally.)
$env:ASB_SKIP_PACKAGE_PROJECT = '1'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sln  = Join-Path $repo 'AppSandbox.sln'
# Output dir matches the .vcxproj OutDir convention: x64 -> bin\Release,
# ARM64 -> bin\Release-ARM64.
$binLeaf = if ($Platform -eq 'ARM64') { "$Configuration-ARM64" } else { $Configuration }
$bin  = Join-Path $repo ("bin\{0}" -f $binLeaf)

$cfgPath = Join-Path $PSScriptRoot 'submission-config.json'
$cfg     = if (Test-Path $cfgPath) { Get-Content $cfgPath -Raw | ConvertFrom-Json } else { $null }
$tsUrl   = if ($cfg -and $cfg.timestampUrl) { $cfg.timestampUrl } else { 'http://ts.ssl.com' }

# ----------------------------------------------------------------- helpers ---

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        # Prefer the MSBuild whose process architecture matches the HOST. The WDK's post-build
        # ApiValidator (and its aitstatic helper) is invoked at the msbuild process architecture;
        # the x86 validator returns exit 193 (ERROR_BAD_EXE_FORMAT) when checking a driver on an
        # ARM64 host. The 32-bit MSBuild\Current\Bin\MSBuild.exe (which `-find ... | First 1`
        # returns) is x86, so a driver build (e.g. AppSandboxVAD.sys) fails ApiValidation even
        # though it compiled. Selecting the native msbuild makes the native ApiValidator run -
        # exactly what the VS IDE does (which is why a GUI build of the same solution succeeds).
        $root = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
        if ($root) {
            $cands = @()
            switch ($env:PROCESSOR_ARCHITECTURE) {
                'ARM64' { $cands += "$root\MSBuild\Current\Bin\arm64\MSBuild.exe"; $cands += "$root\MSBuild\Current\Bin\amd64\MSBuild.exe" }
                'AMD64' { $cands += "$root\MSBuild\Current\Bin\amd64\MSBuild.exe" }
            }
            $cands += "$root\MSBuild\Current\Bin\MSBuild.exe"   # x86 fallback
            foreach ($c in $cands) { if (Test-Path $c) { return $c } }
        }
        # Last resort: original vswhere -find (may be x86).
        $mb = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
        if ($mb) { return $mb }
    }
    throw "MSBuild not found via vswhere."
}

function Find-SignTool {
    $c = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $c) { throw "signtool.exe not found under Windows Kits\10\bin\*\x64." }
    return $c.FullName
}

# Read the version (the single source of truth) from repo-root Directory.Build.props.
function Get-AsbVersion([string]$repoRoot) {
    $props = Join-Path $repoRoot 'Directory.Build.props'
    if (-not (Test-Path $props)) { return $null }
    [xml]$x = Get-Content $props -Raw
    $pg = @($x.Project.PropertyGroup) | Where-Object { $_.AsbVersionMajor } | Select-Object -First 1
    if (-not $pg) { return $null }
    $p = @("$($pg.AsbVersionMajor)", "$($pg.AsbVersionMinor)", "$($pg.AsbVersionPatch)", "$($pg.AsbVersionRevision)") | ForEach-Object { $_.Trim() }
    return [pscustomobject]@{ Short = ($p[0..2] -join '.'); Full = ($p -join '.') }
}

# A smart-card cert stays in the store with HasPrivateKey=$true even after the YubiKey is unplugged,
# so HasPrivateKey is not proof the token is present - and we must NOT probe the key to find out:
# opening it (GetECDsaPrivateKey / signtool) asks the CNG layer to locate the card and pops a Windows
# "insert smart card" dialog when it's gone. Two UI-free checks instead:
#   AnyCardPresent()    - WinSCard status query: is any smart card in any reader at all?
#   KeyOnPresentCard()  - opens THIS cert's own key container with NCRYPT_SILENT_FLAG, so the KSP
#                         returns an error (never UI) when the specific card bearing it is absent.
#                         This is what tells our token apart from some unrelated card the user inserted.
if (-not ([System.Management.Automation.PSTypeName]'AsbScReader').Type) {
    # Add-Type shells out to the .NET Framework C# compiler, which reads the LIB environment
    # variable and fails the compile (CS1668, warning-as-error) on any entry that does not exist.
    # The VS ARM64 build pushes a non-existent ...\atlmfc\lib\ARM64 onto LIB (the ATL/MFC ARM64
    # libs aren't installed); csc needs nothing from LIB here, so drop missing entries first.
    if ($env:LIB) { $env:LIB = (($env:LIB -split ';') | Where-Object { $_ -and (Test-Path $_) }) -join ';' }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class AsbScReader {
    [DllImport("winscard.dll")] static extern int SCardEstablishContext(uint scope, IntPtr r1, IntPtr r2, out IntPtr ctx);
    [DllImport("winscard.dll")] static extern int SCardReleaseContext(IntPtr ctx);
    [DllImport("winscard.dll", CharSet=CharSet.Unicode, EntryPoint="SCardListReadersW")]
    static extern int SCardListReaders(IntPtr ctx, string groups, [Out] char[] readers, ref int len);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct RDR { public string szReader; public IntPtr pv; public uint cur; public uint evt; public uint cbAtr;
                 [MarshalAs(UnmanagedType.ByValArray, SizeConst=36)] public byte[] atr; }
    [DllImport("winscard.dll", CharSet=CharSet.Unicode, EntryPoint="SCardGetStatusChangeW")]
    static extern int SCardGetStatusChange(IntPtr ctx, uint timeout, [In,Out] RDR[] states, uint count);
    const uint SCOPE_USER = 0; const uint STATE_PRESENT = 0x20;
    public static bool AnyCardPresent() {
        IntPtr ctx;
        if (SCardEstablishContext(SCOPE_USER, IntPtr.Zero, IntPtr.Zero, out ctx) != 0) return false;
        try {
            int len = 0;
            if (SCardListReaders(ctx, null, null, ref len) != 0 || len <= 1) return false;
            char[] buf = new char[len];
            if (SCardListReaders(ctx, null, buf, ref len) != 0) return false;
            var rs = new List<string>(); int s = 0;
            for (int i = 0; i < buf.Length; i++) {
                if (buf[i] == '\0') { if (i > s) rs.Add(new string(buf, s, i - s)); s = i + 1;
                    if (i + 1 >= buf.Length || buf[i+1] == '\0') break; } }
            if (rs.Count == 0) return false;
            var st = new RDR[rs.Count];
            for (int i = 0; i < rs.Count; i++) { st[i].szReader = rs[i]; st[i].cur = 0; st[i].atr = new byte[36]; }
            if (SCardGetStatusChange(ctx, 0, st, (uint)st.Length) != 0) return false;
            foreach (var r in st) if ((r.evt & STATE_PRESENT) != 0) return true;
            return false;
        } finally { SCardReleaseContext(ctx); }
    }
    [DllImport("crypt32.dll", SetLastError=true)]
    static extern bool CertGetCertificateContextProperty(IntPtr ctx, uint propId, IntPtr pvData, ref uint cb);
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct KEYPROV { public string container; public string prov; public uint provType; public uint flags; public uint cParam; public IntPtr rgParam; public uint keySpec; }
    [DllImport("ncrypt.dll", CharSet=CharSet.Unicode)] static extern int NCryptOpenStorageProvider(out IntPtr prov, string name, uint flags);
    [DllImport("ncrypt.dll", CharSet=CharSet.Unicode)] static extern int NCryptOpenKey(IntPtr prov, out IntPtr key, string keyName, uint legacyKeySpec, uint flags);
    [DllImport("ncrypt.dll")] static extern int NCryptFreeObject(IntPtr h);
    const uint KEYPROV_PROP = 2; const uint SILENT = 0x40;
    public static bool KeyOnPresentCard(IntPtr certContext) {
        uint cb = 0;
        if (!CertGetCertificateContextProperty(certContext, KEYPROV_PROP, IntPtr.Zero, ref cb) || cb == 0) return false;
        IntPtr buf = Marshal.AllocHGlobal((int)cb);
        try {
            if (!CertGetCertificateContextProperty(certContext, KEYPROV_PROP, buf, ref cb)) return false;
            var kp = (KEYPROV)Marshal.PtrToStructure(buf, typeof(KEYPROV));
            if (string.IsNullOrEmpty(kp.prov) || string.IsNullOrEmpty(kp.container)) return false;
            IntPtr prov;
            if (NCryptOpenStorageProvider(out prov, kp.prov, 0) != 0) return false;
            try {
                IntPtr key;
                if (NCryptOpenKey(prov, out key, kp.container, kp.keySpec, SILENT) == 0) { NCryptFreeObject(key); return true; }
                return false;
            } finally { NCryptFreeObject(prov); }
        } finally { Marshal.FreeHGlobal(buf); }
    }
}
'@
}
function Test-SmartCardPresent { try { return [AsbScReader]::AnyCardPresent() } catch { return $false } }
function Test-CertOnPresentCard($cert) { try { return [AsbScReader]::KeyOnPresentCard($cert.Handle) } catch { return $false } }

# The YubiKey gate: returns the EV cert object only if OUR signing token is physically present, else
# $null. Both checks are UI-free: a fast "is any card in a reader" pre-filter, then a silent open of
# the candidate cert's own key container - so an unrelated smart card the user has inserted does NOT
# look like our token, and nothing ever pops the "insert smart card" dialog.
function Get-EvCert {
    if (-not (Test-SmartCardPresent)) { return $null }
    $now = Get-Date
    $thumb = if ($cfg) { $cfg.evCertThumbprint } else { '' }
    if ($thumb) {
        $c = Get-Item "Cert:\CurrentUser\My\$thumb" -ErrorAction SilentlyContinue
        if ($c -and $c.HasPrivateKey -and (Test-CertOnPresentCard $c)) { return $c }
        return $null
    }
    $cands = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.HasPrivateKey -and ($_.Issuer -ne $_.Subject) -and ($_.NotBefore -le $now) -and ($_.NotAfter -gt $now) -and (Test-CertOnPresentCard $_) })
    if ($cands.Count -eq 1) { return $cands[0] }
    if ($cands.Count -gt 1) {
        Write-Warning "Multiple usable code-signing certs found; set 'evCertThumbprint' in submission-config.json to choose one."
        return $null
    }
    return $null
}

# True if a catalog is Microsoft-signed (attestation). Used to detect drivers that are already
# signed (the cache lives in bin\Release\drivers now), so a -NoBuild re-run doesn't re-submit.
function Test-MsSignedCat([string]$catPath) {
    if (-not (Test-Path $catPath)) { return $false }
    $sig = Get-AuthenticodeSignature -LiteralPath $catPath
    return ($sig.Status -eq 'Valid' -and $null -ne $sig.SignerCertificate -and $sig.SignerCertificate.Subject -match 'Microsoft')
}

# Extract the version (the X.X.X.X after the date) from an INF's DriverVer line - used to make
# sure cached/reused MS-signed drivers were built for THIS version, never an older one.
function Get-InfVersion([string]$infPath) {
    if (-not (Test-Path $infPath)) { return $null }
    foreach ($line in Get-Content $infPath) {
        if ($line -match '^\s*DriverVer\s*=\s*[^,]*,\s*([0-9][0-9.]*)') { return $matches[1].Trim() }
    }
    return $null
}

# True if a file is already Authenticode-signed by OUR EV cert. Phase 3 skips such files: re-signing
# an already-EV-signed driver .sys/.dll would change its bytes and break the Microsoft catalog hash.
function Test-OurEvSigned([string]$path, [string]$thumb) {
    $sig = Get-AuthenticodeSignature -LiteralPath $path
    return ($null -ne $sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $thumb)
}

# True if a driver folder holds a Microsoft-signed catalog AT the given version (both must match).
function Test-DriverSigned([string]$dir, [string]$name, [string]$wantVer) {
    $cat = Join-Path $dir "$name.cat"
    $inf = Join-Path $dir "$name.inf"
    return ((Test-MsSignedCat $cat) -and ((Get-InfVersion $inf) -eq $wantVer))
}

# Resolve the version from Directory.Build.props (single source). $asb.Full (X.X.X.X) gates the
# driver cache/reuse so we never reuse or ship MS-signed drivers built for an older version.
$asb = Get-AsbVersion $repo
if (-not $asb) { throw "Could not read the version from Directory.Build.props under $repo." }
if (-not $Version) { $Version = $asb.Short }
Write-Host "Release version: $Version  (DriverVer $($asb.Full), source: Directory.Build.props)"

# ------------------------------------------------------------------- 1. build

if (-not $NoBuild) {
    $msbuild = Find-MSBuild
    Write-Host "Building $Configuration|$Platform ..."
    & $msbuild $sln /t:Build /p:Configuration=$Configuration /p:Platform=$Platform /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }
} else {
    Write-Host "-NoBuild: packaging existing $bin"
}
if (-not (Test-Path $bin)) { throw "Output dir not found: $bin" }

if ($BuildOnly) { Write-Host "-BuildOnly: build complete; skipping signing + packaging."; return }

# ---------------------------------------------------------------- 2. YubiKey gate

$ev = Get-EvCert
if (-not $ev) {
    Write-Host ""
    Write-Host "================================================================"
    Write-Host " No App Sandbox LLC EV YubiKey detected."
    Write-Host " SKIPPING: app signing, driver attestation, and ZIP packaging."
    Write-Host " The build is complete; drivers remain test-signed (dev build)."
    Write-Host " Insert the YubiKey and re-run to produce a signed release."
    Write-Host "================================================================"
    return
}
Write-Host ("EV YubiKey present: {0} [{1}]" -f $ev.Subject, $ev.Thumbprint)
$signtool = Find-SignTool

# --------------------------------------- 3. EV-sign ALL binaries: app + drivers (1 PIN)
# ONE signtool call over every PE we build - app .exe/.dll AND the driver .sys/.dll in
# bin\Release\drivers - so binaries cost a single PIN. Idempotent: files already signed by our
# cert are skipped (re-signing a driver binary would change its bytes and break the MS catalog
# hash on a reuse / cache-hit run). drivers-signed/_attest/_package are excluded; WebView2Loader.dll
# is left as Microsoft shipped it.
# WebView2Loader.dll and devcon.exe are Microsoft's binaries - never re-sign them with our cert.
$msBinaries = @('WebView2Loader.dll', 'devcon.exe')
$toSign = @(Get-ChildItem $bin -Recurse -Include *.exe, *.dll, *.sys | Where-Object {
    ($_.Name -notin $msBinaries) -and
    ($_.FullName -notmatch '\\(drivers-signed|_attest|_package)\\') -and
    (-not (Test-OurEvSigned $_.FullName $ev.Thumbprint))
}) | ForEach-Object { $_.FullName }

if ($toSign.Count -gt 0) {
    Write-Host "EV-signing $($toSign.Count) binaries (app + drivers) - enter the YubiKey PIN when prompted..."
    & $signtool sign /sha1 $ev.Thumbprint /fd SHA256 /tr $tsUrl /td SHA256 /v @toSign
    if ($LASTEXITCODE -ne 0) { throw "Binary EV-signing failed (exit $LASTEXITCODE)." }
    & $signtool verify /pa @toSign | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Binary signature verify failed." }
    Write-Host "Binaries signed + verified."
} else {
    Write-Host "All binaries already EV-signed by our cert - nothing to re-sign."
}

# ------------------------------------------------- 4. ensure MS-signed drivers (this version)
#
# The MS-signed drivers must replace the WDK test-signed files in bin\Release\drivers - the folder
# the VM provisioning reads (src\backend_win\disk_util.c) and the source for the zip. Drivers count
# as "done" only if MS-signed AND stamped with the version we are building (never an older one).
# Order: use bin\Release\drivers if already good; else reuse a matching prior drivers-signed
# download (no resubmit); else run the Microsoft round-trip. (All YubiKey-gated above.)

$signedDir  = Join-Path $bin 'drivers-signed'
$driversDir = Join-Path $bin 'drivers'
if (-not $SkipDrivers) {
    $haveSigned = (Test-DriverSigned $driversDir 'AppSandboxVDD' $asb.Full) -and `
                  (Test-DriverSigned $driversDir 'AppSandboxVAD' $asb.Full) -and `
                  (Test-DriverSigned $driversDir 'AppSandboxSHM' $asb.Full)
    if ($ForceDriverSign -or -not $haveSigned) {
        # Reuse a prior MS-signed download only if it matches THIS version; otherwise submit.
        $vddInf = Get-ChildItem (Join-Path $signedDir 'AppSandboxVDD') -Recurse -Filter 'AppSandboxVDD.inf' -ErrorAction SilentlyContinue | Select-Object -First 1
        $vadInf = Get-ChildItem (Join-Path $signedDir 'AppSandboxVAD') -Recurse -Filter 'AppSandboxVAD.inf' -ErrorAction SilentlyContinue | Select-Object -First 1
        $shmInf = Get-ChildItem (Join-Path $signedDir 'AppSandboxSHM') -Recurse -Filter 'AppSandboxSHM.inf' -ErrorAction SilentlyContinue | Select-Object -First 1
        $reusable = (-not $ForceDriverSign) -and $vddInf -and $vadInf -and $shmInf -and `
                    (Test-DriverSigned $vddInf.DirectoryName 'AppSandboxVDD' $asb.Full) -and `
                    (Test-DriverSigned $vadInf.DirectoryName 'AppSandboxVAD' $asb.Full) -and `
                    (Test-DriverSigned $shmInf.DirectoryName 'AppSandboxSHM' $asb.Full)
        if ($reusable) {
            Write-Host "Reusing existing Microsoft-signed drivers (v$($asb.Full)) from drivers-signed - no resubmit."
        } else {
            Write-Host "Driver attestation signing (sign-drivers.ps1 -Platform $Platform) - Microsoft round-trip..."
            & (Join-Path $PSScriptRoot 'sign-drivers.ps1') -Platform $Platform
            if ($LASTEXITCODE -ne 0) { throw "Driver attestation failed." }
        }
        # Deposit the MS-signed drivers into bin\Release\drivers (deposited .sys/.dll match the MS
        # catalog hash; the .cer + devcon.exe the provisioning also copies are left in place).
        if (Test-Path $signedDir) {
            Get-ChildItem $signedDir -Recurse -Include *.inf, *.cat, *.sys, *.dll | ForEach-Object {
                Copy-Item $_.FullName (Join-Path $driversDir $_.Name) -Force
            }
            Write-Host "Deposited Microsoft-signed drivers into $driversDir."
        }
        # Guard: confirm the deposited drivers are MS-signed AND this version before shipping.
        if (-not ((Test-DriverSigned $driversDir 'AppSandboxVDD' $asb.Full) -and `
                  (Test-DriverSigned $driversDir 'AppSandboxVAD' $asb.Full) -and `
                  (Test-DriverSigned $driversDir 'AppSandboxSHM' $asb.Full))) {
            throw "Drivers in $driversDir are not Microsoft-signed at v$($asb.Full) - aborting (sign-drivers may have failed)."
        }
    } else {
        Write-Host "Drivers in $driversDir already Microsoft-signed at v$($asb.Full) - skipping attestation."
    }
    # Clean up the signing intermediates so they are neither shipped in the zip nor left behind.
    Remove-Item (Join-Path $bin '_attest') -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item $signedDir -Recurse -Force -ErrorAction SilentlyContinue
}

# --------------------------------------------------- 5. stage + zip (explicit allowlist)

$stageRoot = Join-Path $bin '_package'
$stage     = Join-Path $stageRoot 'AppSandbox'
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Explicit allowlist (not "copy everything minus excludes"): the root holds ONLY the host app +
# its direct deps; the guest binaries ship under resources\, the signed drivers under drivers\,
# the web UI under web\.
$rootBins = @('AppSandbox.exe', 'appsandbox_core.dll', 'iso-patch.exe', 'WebView2Loader.dll')
foreach ($b in $rootBins) {
    $src = Join-Path $bin $b
    if (-not (Test-Path $src)) { throw "Missing required root binary $b under $bin." }
    Copy-Item $src (Join-Path $stage $b) -Force
}

# The drivers\, resources\ and web\ subfolders are shipped whole, minus symbols/intermediates
# and the test .cer (the drivers are MS-signed; devcon.exe is Microsoft's and stays).
foreach ($sub in @('resources', 'drivers', 'web')) {
    $srcSub = Join-Path $bin $sub
    if (-not (Test-Path $srcSub)) { throw "Missing required folder $sub under $bin." }
    robocopy $srcSub (Join-Path $stage $sub) /E /XF *.pdb *.lib *.exp *.ilk *.iobj *.ipdb *.log *.cer | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $sub failed (exit $LASTEXITCODE)." }
    $global:LASTEXITCODE = 0   # robocopy 0..7 = success
}

# Ship the headless-api SDK as a sibling folder at the zip root: the Python SDK +
# examples + tests that script the daemon over the HTTP API. The folder is named
# "headless-api" - the tools\ parent is intentionally NOT shipped. __pycache__ and
# *.pyc are local build cruft and are excluded.
$sdkSrc = Join-Path $repo 'tools\headless-api'
$sdkDst = Join-Path $stage 'headless-api'
New-Item -ItemType Directory -Force -Path $sdkDst | Out-Null
foreach ($f in @('asb.py', 'README.md')) {
    $fsrc = Join-Path $sdkSrc $f
    if (-not (Test-Path $fsrc)) { throw "Missing headless-api file $f under $sdkSrc." }
    Copy-Item $fsrc (Join-Path $sdkDst $f) -Force
}
foreach ($sub in @('examples', 'tests')) {
    robocopy (Join-Path $sdkSrc $sub) (Join-Path $sdkDst $sub) /E /XD __pycache__ /XF *.pyc | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy headless-api\$sub failed (exit $LASTEXITCODE)." }
    $global:LASTEXITCODE = 0
}

# Tripwire: the signed drivers come from the deposit (not the build) - fail loudly if any
# expected artifact is missing rather than shipping a partial zip.
$required = @(
    'AppSandbox.exe', 'appsandbox_core.dll', 'iso-patch.exe', 'WebView2Loader.dll',
    'drivers\AppSandboxVDD.dll', 'drivers\AppSandboxVDD.inf', 'drivers\AppSandboxVDD.cat',
    'drivers\AppSandboxVAD.sys', 'drivers\AppSandboxVAD.inf', 'drivers\AppSandboxVAD.cat',
    'drivers\AppSandboxSHM.sys', 'drivers\AppSandboxSHM.inf', 'drivers\AppSandboxSHM.cat',
    'drivers\devcon.exe', 'resources', 'web',
    'headless-api\asb.py', 'headless-api\README.md', 'headless-api\examples', 'headless-api\tests'
)
if ($Platform -eq 'x64') {
    $required += 'resources\nvidia\appsandbox-nvidia-vk-gl-shim.dll', 'resources\nvidia\appsandbox-nvidia-vk-gl-shim32.dll'
    $required += 'resources\nvidia\appsandbox-nvidia-dlss-shim.dll'
    $required += 'resources\nvidia\appsandbox-nvidia-cuda-shim.dll', 'resources\nvidia\appsandbox-nvidia-opencl-shim.dll'
}
$missing = @($required | Where-Object { -not (Test-Path (Join-Path $stage $_)) })
if ($missing.Count) { throw "Refusing to package - missing expected artifacts: $($missing -join ', ')" }

# Manifest: show exactly what is going into the zip.
Write-Host "`n--- package contents ---"
$files = Get-ChildItem $stage -Recurse -File
$files | ForEach-Object { Write-Host ("  " + $_.FullName.Substring($stage.Length + 1)) }
$mb = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host ("--- {0} files, {1} MB ---`n" -f $files.Count, $mb)

$zip = Join-Path $bin ("AppSandbox-{0}-{1}-{2}.zip" -f $Version, $OS, $Platform.ToLower())
if (Test-Path $zip) { Remove-Item $zip -Force }
# Flat layout: the items sit at the zip root (file.zip\AppSandbox.exe, file.zip\drivers\...),
# with the drivers\ / resources\ / web\ subfolders preserved - no extra parent folder.
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item $stageRoot -Recurse -Force

Write-Host ""
Write-Host "==================== RELEASE READY ===================="
Write-Host "Signed ZIP: $zip"
