[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $projectRoot ('build/signpath-test-' + [guid]::NewGuid().ToString('N'))
$binaries = Join-Path $fixture 'binaries'
$installer = Join-Path $fixture 'installer/CheekyOpenXRSetup.exe'
New-Item -ItemType Directory -Path (Split-Path -Parent $installer) -Force | Out-Null
Set-Content -LiteralPath $installer -Value 'synthetic signed installer fixture' -Encoding ascii
$relativePaths = @(
    'CheekyFoveatedDLSS.dll',
    'CheekyOpenXRLayer.dll', 'CheekyVulkanLayer.dll',
    'CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll',
    'CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll',
    'standalone-loader/dxgi.dll', 'version-loader/version.dll'
)
foreach ($relative in $relativePaths) {
    $path = Join-Path $binaries $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value "synthetic signed binary: $relative" -Encoding ascii
}
function Assert-Rejected($Action, [string]$Message) {
    $rejected = $false
    try { & $Action } catch {
        if ($_.Exception.Message -notlike "*$Message*") { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw "Expected rejection: $Message" }
}
# Exercise the real Windows check before introducing the certificate stub.
Assert-Rejected { & "$projectRoot/scripts/verify-signpath.ps1" -InstallerPath $installer } 'Signature check failed'

# Only the certificate response is mocked; use actual packagers, ZIP IO and hashes.
$signatureFixture = @{ Status = 'NotTrusted'; Message = 'Synthetic test result'; InstallerThumbprint = 'TEST-CERT'; InstallerPath = $installer }
Set-Item Function:Get-AuthenticodeSignature -Value ({
    param([string]$LiteralPath)
    [pscustomobject]@{
        Status = $signatureFixture.Status
        StatusMessage = $signatureFixture.Message
        SignerCertificate = [pscustomobject]@{
            Thumbprint = $(if ($LiteralPath -eq $signatureFixture.InstallerPath) { $signatureFixture.InstallerThumbprint } else { 'TEST-CERT' })
        }
    }
}.GetNewClosure())
$signatureFixture.Status = 'HashMismatch'
Assert-Rejected { & "$projectRoot/scripts/package-signpath.ps1" -BinaryRoot $binaries -InstallerPath $installer } 'Signature check failed'
$signatureFixture.Status = 'UnknownError'
Assert-Rejected { & "$projectRoot/scripts/verify-signpath.ps1" -InstallerPath $installer } 'Signature check failed'
# Authenticode reports the sentence without the HRESULT suffix or final period.
$signatureFixture.Message = ([Runtime.InteropServices.Marshal]::GetExceptionForHR(-2146762487).Message -replace '\s*\((Exception from HRESULT: )?0x800B0109\)$', '').Trim().TrimEnd('.')
& "$projectRoot/scripts/verify-signpath.ps1" -InstallerPath $installer
$signatureFixture.Message = 'Synthetic test result'
$signatureFixture.Status = 'NotTrusted'
$signatureFixture.InstallerThumbprint = 'OTHER-CERT'
Assert-Rejected { & "$projectRoot/scripts/package-signpath.ps1" -BinaryRoot $binaries -InstallerPath $installer } 'Unexpected signing certificate'
$signatureFixture.InstallerThumbprint = 'TEST-CERT'
$held = Join-Path $binaries 'version-loader/version.dll'
Move-Item -LiteralPath $held -Destination "$held.missing"
Assert-Rejected { & "$projectRoot/scripts/package-signpath.ps1" -BinaryRoot $binaries -InstallerPath $installer } 'Missing signed file'
Move-Item -LiteralPath "$held.missing" -Destination $held

$previousOutput = $env:GITHUB_OUTPUT
try {
    $env:GITHUB_OUTPUT = Join-Path $fixture 'outputs.txt'
    & "$projectRoot/scripts/package-signpath.ps1" -BinaryRoot $binaries -InstallerPath $installer
    $output = ((Get-Content -LiteralPath $env:GITHUB_OUTPUT | Where-Object { $_ -like 'directory=*' }) -replace '^directory=', '')
} finally { $env:GITHUB_OUTPUT = $previousOutput }
$archives = @(Get-ChildItem -LiteralPath $output -Filter '*.zip')
if ($archives.Count -ne 2) { throw 'Expected exactly two final ZIPs.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($archive in $archives) {
    $zip = [IO.Compression.ZipFile]::OpenRead($archive.FullName)
    try {
        $entries = @{}
        foreach ($entry in $zip.Entries) { $entries[$entry.FullName.Replace('\', '/')] = $entry }
        $expected = @{'OpenXR/CheekyOpenXRSetup.exe' = $installer}
        if ($archive.Name -like '*-UEVR-*') {
            $expected['plugins/CheekyFoveatedDLSS.dll'] = Join-Path $binaries 'CheekyFoveatedDLSS.dll'
            $expected['plugins/CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll'] = Join-Path $binaries 'CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll'
        } else {
            $expected['dxgi.dll'] = Join-Path $binaries 'standalone-loader/dxgi.dll'
            $expected['version.dll'] = Join-Path $binaries 'version-loader/version.dll'
            $expected['CheekyVulkanLayer.dll'] = Join-Path $binaries 'CheekyVulkanLayer.dll'
            foreach ($name in @('CheekyFoveatedDLSSHost.dll', 'CheekyFoveatedDLSSRuntime.dll')) {
                $expected["CheekyFoveatedDLSS/$name"] = Join-Path $binaries "CheekyFoveatedDLSS/$name"
            }
        }
        foreach ($name in $expected.Keys) {
            if (-not $entries.ContainsKey($name)) { throw "Missing ZIP path: $name" }
            $stream = $entries[$name].Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($actual -ne (Get-FileHash -LiteralPath $expected[$name] -Algorithm SHA256).Hash) {
                throw "Package used incorrect binary source: $name"
            }
        }
    } finally { $zip.Dispose() }
}
$checksums = @(Get-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt'))
if ($checksums.Count -ne 2) { throw 'Expected two final checksums.' }
foreach ($line in $checksums) {
    $hash, $name = $line -split '  ', 2
    if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $output $name) -Algorithm SHA256).Hash) {
        throw "Incorrect final checksum: $name"
    }
}
Write-Host 'PASS: signature rejection, certificate mismatch, missing input, signed source isolation, both ZIP layouts and final checksums.'
