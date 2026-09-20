[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$SkipTests
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

if ($SkipTests) {
    Write-Host "Build completed. Test execution skipped."
    return
}

& (Join-Path $projectRoot "bin\$Configuration\CheekyNrObserverTests.exe")
if ($LASTEXITCODE -ne 0) { throw "NR native observer tests failed with exit code $LASTEXITCODE." }

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
foreach ($mode in @("dx11", "dx11-c", "streamline-dx11", "dx12", "dx12-c", "streamline")) {
    & $uevrTest "--late-$mode" --inactive-afw
    if ($LASTEXITCODE -ne 0) { throw "UEVR inactive AFW ($mode) failed with exit code $LASTEXITCODE." }
}
foreach ($abi in @("022", "027", "028", "029")) {
    & $uevrTest "--openvr-late-$abi"
    if ($LASTEXITCODE -ne 0) { throw "UEVR cached OpenVR compositor ($abi) test failed." }
}
foreach ($mode in @("native", "native-c", "streamline", "missing-lower", "public-first", "public-first-c", "ota", "ota-c", "ota-streamline", "ota-ambiguous")) {
    & $uevrTest "--afw-$mode"
    if ($LASTEXITCODE -ne 0) { throw "UEVR AFW routing ($mode) failed with exit code $LASTEXITCODE." }
}

$runtimeHostTest = Join-Path $projectRoot "bin\$Configuration\CheekyRuntimeHostTests.exe"
foreach ($arguments in @(@(), @('--dx11'), @('--optiscaler'), @('--optiscaler','--dx11'), @('--conflict'), @('--transport'), @('--optiscaler','--transport'), @('--transport-forwarded'), @('--optiscaler','--transport-forwarded'))) {
    & $runtimeHostTest @arguments
    if ($LASTEXITCODE -ne 0) { throw "Generic runtime tests failed: $arguments" }
}
$standaloneHostTest = Join-Path $projectRoot "bin\$Configuration\CheekyStandaloneHostTests.exe"
foreach ($abi in @('022','027','028','029')) {
    & $runtimeHostTest "--openvr-late-$abi"
    if ($LASTEXITCODE -ne 0) { throw "Standalone cached OpenVR compositor failed: $abi" }
    & $runtimeHostTest --optiscaler "--openvr-late-$abi"
    if ($LASTEXITCODE -ne 0) { throw "OptiScaler cached OpenVR compositor failed: $abi" }
}
foreach ($hostKind in @('standalone','optiscaler')) {
    foreach ($mode in @('dx11','dx11-c','dx12','dx12-c','streamline','streamline-dx11')) {
        & $standaloneHostTest $mode $hostKind
        if ($LASTEXITCODE -ne 0) { throw "Standalone host tests failed: $hostKind $mode" }
    }
}
$bootstrapTest = Join-Path $projectRoot "bin\$Configuration\CheekyBootstrapTests.exe"
foreach ($mode in @('proxy','version','asi','missing','chain','broken-chain','loop-chain','device-chain')) {
    $proxyInput = if ($mode -eq 'version') { 'version-loader\version.dll' } else { 'standalone-loader\dxgi.dll' }
    & $bootstrapTest $mode (Join-Path $projectRoot "bin\$Configuration\$proxyInput") (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS.asi") (Join-Path $projectRoot "bin\$Configuration\CheekyBootstrapFakeHost.dll")
    if ($LASTEXITCODE -ne 0) { throw "Bootstrap tests failed: $mode" }
}
$overlayTest = Join-Path $projectRoot "bin\$Configuration\CheekyOverlayTests.exe"
& $overlayTest --ui
if ($LASTEXITCODE -ne 0) { throw "Overlay diagnostic UI tests failed." }
foreach ($api in @('dx11','dx12')) {
    foreach ($color in @('sdr','scrgb','hdr10')) {
        & $overlayTest "--$api" "--$color"
        if ($LASTEXITCODE -ne 0) { throw "Overlay tests failed: $api $color" }
    }
}
$coreDiscoveryTest = Join-Path $projectRoot "bin\$Configuration\CheekyCoreDiscoveryTests.exe"
foreach ($mode in @('accept','reject')) {
    & $coreDiscoveryTest $mode (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll") (Join-Path $projectRoot "bin\$Configuration\test-fixtures\CheekyFakeCore.dll") (Join-Path $projectRoot "bin\$Configuration\test-fixtures\CheekyFakeCoreProxy.dll")
    if ($LASTEXITCODE -ne 0) { throw "NGX core discovery tests failed: $mode" }
}

Write-Host "Built and tested:"
Write-Host (Join-Path $projectRoot "bin\$Configuration\standalone-loader\dxgi.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\version-loader\version.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS.asi")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS\CheekyFoveatedDLSSHost.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyFoveatedDLSS.addon64")
Write-Host (Join-Path $projectRoot "bin\$Configuration\CheekyOpenXRLayer.dll")
Write-Host (Join-Path $projectRoot "bin\$Configuration\XR_APILAYER_CHEEKY_foveated_dlss.json")
