[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
    [ValidateSet("Standalone", "OptiScaler", "Both")][string]$Mode = "Both",
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$Label = "build",
    [string]$BinaryRoot = "",
    [switch]$IncludeOpenXRSetup
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BinaryRoot) { $BinaryRoot = Join-Path $projectRoot "bin\$Configuration" }
$BinaryRoot = [IO.Path]::GetFullPath($BinaryRoot)
$versionText = Get-Content -LiteralPath (Join-Path $projectRoot "shared\version.h") -Raw
if ($versionText -notmatch '#define CHEEKY_VERSION "([0-9.]+)"') { throw "Version not found." }
$version = $Matches[1]
$modes = if ($Mode -eq "Both") { @("Standalone", "OptiScaler") } else { @($Mode) }
$head = & git -C $projectRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw "Could not identify source commit." }
$dirty = [bool](& git -C $projectRoot status --porcelain)
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Check all archives before staging any payload. Packaging never replaces an
# earlier artifact and never modifies a game directory or an OptiScaler install.
foreach ($packageMode in $modes) {
    $archive = Join-Path $BinaryRoot "CheekyFoveatedDLSS-$version-$packageMode-$Label.zip"
    if (Test-Path -LiteralPath $archive) { throw "Archive already exists: $archive. Choose a new -Label." }
}

foreach ($packageMode in $modes) {
    $prefix = if ($packageMode -eq "OptiScaler") { "OptiScaler\plugins\" } else { "" }
    $loaderName = if ($packageMode -eq "OptiScaler") { "CheekyFoveatedDLSS.asi" } else { "dxgi.dll" }
    $loaderInput = if ($packageMode -eq "OptiScaler") { $loaderName } else { "standalone-loader\dxgi.dll" }
    $files = [ordered]@{}
    $files[$prefix + $loaderName] = Join-Path $BinaryRoot $loaderInput
    foreach ($dll in @("CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSSRuntime.dll")) {
        $files[$prefix + "CheekyFoveatedDLSS\" + $dll] = Join-Path $BinaryRoot ("CheekyFoveatedDLSS\" + $dll)
    }
    $files["Cheeky-Standalone-README.md"] = Join-Path $projectRoot "standalone\README.md"
    $files["licenses\Cheeky-GPLv3.txt"] = Join-Path $projectRoot "LICENSE"
    $files["licenses\MinHook.txt"] = Join-Path $projectRoot "third_party\reshade\deps\minhook\LICENSE.txt"
    $files["licenses\OpenVR.txt"] = Join-Path $projectRoot "third_party\openvr\LICENSE"
    $files["licenses\DearImGui.txt"] = Join-Path $projectRoot "third_party\reshade\deps\imgui\LICENSE.txt"
    if ($IncludeOpenXRSetup) {
        $files["OpenXR\CheekyOpenXRSetup.exe"] = Join-Path $projectRoot "bin\installer\CheekyOpenXRSetup.exe"
    }
    foreach ($fileSource in $files.Values) {
        if (-not (Test-Path -LiteralPath $fileSource -PathType Leaf)) { throw "Missing input: $fileSource. Build first." }
    }

    $stage = Join-Path $projectRoot ("build\standalone-package-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $manifest = @()
    foreach ($entry in $files.GetEnumerator()) {
        $destination = Join-Path $stage $entry.Key
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $entry.Value -Destination $destination
        $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest += "$hash  $($entry.Key.Replace('\', '/'))"
    }
    $manifest | Set-Content -LiteralPath (Join-Path $stage "SHA256SUMS.txt") -Encoding ascii
    @("Cheeky $version $packageMode $Label ($Configuration)", "Source commit: $head", "Working tree modified: $dirty",
      "No NVIDIA or OptiScaler binaries included.", "Matching optional OpenXR installer included: $IncludeOpenXRSetup") |
        Set-Content -LiteralPath (Join-Path $stage "BUILD.txt") -Encoding utf8
    $archive = Join-Path $BinaryRoot "CheekyFoveatedDLSS-$version-$packageMode-$Label.zip"
    # CreateFromDirectory refuses an existing destination, including a file that
    # appears after the earlier check. No overwrite flag is provided.
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive)

    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $expected = @($files.Keys | ForEach-Object { $_.Replace('\', '/') }) + @("SHA256SUMS.txt", "BUILD.txt")
        $payload = @($zip.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
        if ($payload.Count -ne $expected.Count) { throw "Unexpected ZIP payload count." }
        foreach ($entry in $payload) {
            $name = $entry.FullName.Replace('\', '/')
            if ($expected -notcontains $name) { throw "Unexpected ZIP entry: $name" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            $localHash = (Get-FileHash -LiteralPath (Join-Path $stage $name) -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($hash -ne $localHash) { throw "ZIP checksum mismatch: $name" }
        }
    } finally { $zip.Dispose() }
    Write-Host "Packaged and verified: $archive"
    Get-FileHash -LiteralPath $archive -Algorithm SHA256
}
