[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")][string]$Configuration = "Release",
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]*$')][string]$Label = "build",
    [switch]$IncludeOpenXRSetup = $true
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$binaryRoot = Join-Path $projectRoot "bin\$Configuration"
$versionText = Get-Content -LiteralPath (Join-Path $projectRoot "shared\version.h") -Raw
if ($versionText -notmatch '#define CHEEKY_VERSION "([0-9.]+)"') { throw "Version not found." }
$version = $Matches[1]
$stage = Join-Path $projectRoot ("build\uevr-package-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$files = [ordered]@{
    "plugins\CheekyFoveatedDLSS.dll" = Join-Path $binaryRoot "CheekyFoveatedDLSS.dll"
    "plugins\CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll" = Join-Path $binaryRoot "CheekyFoveatedDLSS\CheekyFoveatedDLSSRuntime.dll"
    "scripts\cheeky_foveated_dlss.lua" = Join-Path $projectRoot "uevr\scripts\cheeky_foveated_dlss.lua"
    "licenses\Cheeky-GPLv3.txt" = Join-Path $projectRoot "LICENSE"
    "licenses\MinHook.txt" = Join-Path $projectRoot "third_party\reshade\deps\minhook\LICENSE.txt"
    "licenses\OpenVR.txt" = Join-Path $projectRoot "third_party\openvr\LICENSE"
    "licenses\UEVR-API-MIT.txt" = Join-Path $projectRoot "third_party\uevr\LICENSE.txt"
}
if ($IncludeOpenXRSetup) {
    $files["OpenXR\CheekyOpenXRSetup.exe"] = Join-Path $projectRoot "bin\installer\CheekyOpenXRSetup.exe"
}
foreach ($entry in $files.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { throw "Missing input: $($entry.Value). Build first." }
    $destination = Join-Path $stage $entry.Key
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $destination
}
$archive = Join-Path $binaryRoot "CheekyFoveatedDLSS-$version-UEVR-$Label.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force

# Read back the archive: check every payload hash and reject accidental extras.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $expected = @($files.Keys | ForEach-Object { $_.Replace('\', '/') })
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
