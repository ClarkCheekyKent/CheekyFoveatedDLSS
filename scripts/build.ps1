[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$taskPath = $env:Path
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $taskPath

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer's vswhere.exe was not found."
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild was not found. Install the Visual Studio C++ build tools."
}

$msbuildRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $msbuild))
$vcTargetsRoot = Join-Path $msbuildRoot "Microsoft\VC"
$availableToolsets = @(
    Get-ChildItem -LiteralPath $vcTargetsRoot -Directory -Filter "v*" -ErrorAction SilentlyContinue |
        ForEach-Object {
            $platformToolsets = Join-Path $_.FullName "Platforms\x64\PlatformToolsets"
            if (Test-Path -LiteralPath $platformToolsets) {
                Get-ChildItem -LiteralPath $platformToolsets -Directory -ErrorAction SilentlyContinue |
                    Select-Object -ExpandProperty Name
            }
        } |
        Sort-Object -Unique -Descending
)
if ($availableToolsets.Count -eq 0) {
    throw "No x64 Visual C++ platform toolset was found."
}
$toolset = if ($availableToolsets -contains "v143") {
    "v143"
} else {
    $availableToolsets[0]
}

& $msbuild `
    (Join-Path $projectRoot "CheekyFoveatedDLSS.sln") `
    /m `
    /nologo `
    /verbosity:minimal `
    "/p:Configuration=$Configuration" `
    /p:Platform=x64 `
    "/p:PlatformToolset=$toolset"
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$testExecutable = Join-Path $projectRoot "bin\$Configuration\CheekyTests.exe"
& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Tests failed with exit code $LASTEXITCODE."
}

$uevrTest = Join-Path $projectRoot "bin\$Configuration\CheekyUEVRTests.exe"
& $uevrTest
if ($LASTEXITCODE -ne 0) { throw "UEVR host tests failed with exit code $LASTEXITCODE." }
& $uevrTest --conflict
if ($LASTEXITCODE -ne 0) { throw "UEVR ownership test failed with exit code $LASTEXITCODE." }
& $uevrTest --dx11
if ($LASTEXITCODE -ne 0) { throw "UEVR DX11 test failed with exit code $LASTEXITCODE." }
foreach ($mode in @("dx11", "dx11-c", "dx12", "dx12-c", "streamline", "streamline-dx11")) {
    & $uevrTest "--late-$mode"
    if ($LASTEXITCODE -ne 0) { throw "UEVR late attachment ($mode) failed with exit code $LASTEXITCODE." }
}

Write-Host "Built and tested:"
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS.addon64")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyOpenXRLayer.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\XR_APILAYER_CHEEKY_foveated_dlss.json")
