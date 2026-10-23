[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BinaryRoot,
    [Parameter(Mandatory)][string]$InstallerPath,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$Label = 'TEST-SIGNED'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$BinaryRoot = (Resolve-Path -LiteralPath $BinaryRoot).Path
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
& (Join-Path $PSScriptRoot 'verify-signpath.ps1') -BinaryRoot $BinaryRoot
$thumbprint = (Get-AuthenticodeSignature -LiteralPath (Join-Path $BinaryRoot 'CheekyOpenXRLayer.dll')).SignerCertificate.Thumbprint
& (Join-Path $PSScriptRoot 'verify-signpath.ps1') -InstallerPath $InstallerPath -ExpectedThumbprint $thumbprint

# Both packagers receive explicit signed inputs; never copy them over local builds.
& (Join-Path $PSScriptRoot 'package-uevr.ps1') -BinaryRoot $BinaryRoot -OpenXRSetupPath $InstallerPath -Label $Label
& (Join-Path $PSScriptRoot 'package-standalone.ps1') -BinaryRoot $BinaryRoot -OpenXRSetupPath $InstallerPath -Mode Standalone -Label $Label

$versionText = Get-Content -LiteralPath (Join-Path $projectRoot 'shared/version.h') -Raw
if ($versionText -notmatch '#define CHEEKY_VERSION "([0-9.]+)"') { throw 'Version not found.' }
$version = $Matches[1]
$output = Join-Path $projectRoot ('build/signpath-packages-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$checksums = foreach ($mode in @('UEVR', 'Standalone')) {
    $name = "CheekyFoveatedDLSS-$version-$mode-$Label.zip"
    Copy-Item -LiteralPath (Join-Path $BinaryRoot $name) -Destination $output
    $hash = (Get-FileHash -LiteralPath (Join-Path $output $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
$checksums | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
if ($env:GITHUB_OUTPUT) { "directory=$output" >> $env:GITHUB_OUTPUT }
Write-Host "Verified signed packages and checksums: $output"
