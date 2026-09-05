[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+(\.\d+)?$')]
    [string]$Version = '0.1.0',
    [string]$IsccPath,
    # Use only after building Release artifacts (also supports CMake output).
    [switch]$SkipBuild,
    [string]$ArtifactsDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

if (-not $IsccPath) {
    $compiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($compiler) {
        $IsccPath = $compiler.Source
    } else {
        foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:LOCALAPPDATA)) {
            if (-not $base) { continue }
            foreach ($relative in @('Inno Setup 6\ISCC.exe', 'Programs\Inno Setup 6\ISCC.exe')) {
                $candidate = Join-Path $base $relative
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    $IsccPath = $candidate
                    break
                }
            }
            if ($IsccPath) { break }
        }
    }
}
if (-not $IsccPath -or -not (Test-Path -LiteralPath $IsccPath -PathType Leaf)) {
    throw 'Install Inno Setup 6.3 or newer from https://jrsoftware.org/isdl.php, or pass -IsccPath with the full path to ISCC.exe.'
}
$IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
if ($ArtifactsDirectory -and -not $SkipBuild) {
    throw '-ArtifactsDirectory requires -SkipBuild. Supply an already built Release directory.'
}
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
}
if (-not $ArtifactsDirectory) {
    $ArtifactsDirectory = Join-Path $projectRoot 'bin\Release'
}
$ArtifactsDirectory = (Resolve-Path -LiteralPath $ArtifactsDirectory).Path
foreach ($name in @('CheekyOpenXRLayer.dll', 'XR_APILAYER_CHEEKY_foveated_dlss.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ArtifactsDirectory $name) -PathType Leaf)) {
        throw "Missing installer payload: $name. Build the Release OpenXR layer first."
    }
}
$manifest = Get-Content -LiteralPath (Join-Path $ArtifactsDirectory 'XR_APILAYER_CHEEKY_foveated_dlss.json') -Raw | ConvertFrom-Json
if ($manifest.api_layer.name -ne 'XR_APILAYER_CHEEKY_foveated_dlss' -or
    $manifest.api_layer.library_path -ne '.\CheekyOpenXRLayer.dll') {
    throw 'The manifest must identify the Cheeky layer and reference the adjacent CheekyOpenXRLayer.dll.'
}

& $IsccPath "/DAppVersion=$Version" "/DSourceDir=$ArtifactsDirectory" (Join-Path $projectRoot 'installer\CheekyEyeTrackingSetup.iss')
if ($LASTEXITCODE -ne 0) {
    throw "Installer compilation failed with exit code $LASTEXITCODE."
}
Write-Host 'Built standalone OpenXR installer (no ReShade add-on included):'
Write-Host (Join-Path $projectRoot 'bin\installer\CheekyEyeTrackingSetup.exe')
