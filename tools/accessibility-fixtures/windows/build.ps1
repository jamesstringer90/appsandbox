param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)

$ErrorActionPreference = 'Stop'
$framework = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319'
$compiler = Join-Path $framework 'csc.exe'
if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'The .NET Framework 4.8 C# compiler is required.'
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$executable = Join-Path $output 'NativeControlLab.exe'
$references = @(
    'System.dll', 'System.Core.dll', 'System.Windows.Forms.dll', 'System.Drawing.dll',
    'System.Xaml.dll', 'WPF\WindowsBase.dll', 'WPF\PresentationCore.dll',
    'WPF\PresentationFramework.dll', 'WPF\WindowsFormsIntegration.dll',
    'WPF\UIAutomationTypes.dll', 'WPF\UIAutomationClient.dll'
)
$compilerArguments = @('/nologo', '/target:winexe', '/platform:anycpu', '/warn:4', "/out:$executable")
$compilerArguments += @($references | ForEach-Object { '/r:' + (Join-Path $framework $_) })
$compilerArguments += Join-Path $PSScriptRoot 'NativeControlLab.cs'
& $compiler @compilerArguments
if ($LASTEXITCODE -ne 0) {
    throw "Fixture compilation failed with exit code $LASTEXITCODE."
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NativeControlLab.exe.config') -Destination "$executable.config" -Force
Write-Output $executable
