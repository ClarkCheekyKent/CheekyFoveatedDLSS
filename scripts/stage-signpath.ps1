[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$binaryRoot = Join-Path $projectRoot 'bin/Release'
$stage = Join-Path $projectRoot ('build/signpath-' + [guid]::NewGuid().ToString('N'))
[xml]$configuration = Get-Content -LiteralPath (Join-Path $projectRoot '.github/signpath/binaries.xml') -Raw
$namespaces = New-Object System.Xml.XmlNamespaceManager($configuration.NameTable)
$namespaces.AddNamespace('sp', 'http://signpath.io/artifact-configuration/v1')
$files = $configuration.SelectNodes('/sp:artifact-configuration/sp:zip-file/sp:pe-file', $namespaces)
if ($files.Count -ne 7) { throw 'Expected seven explicitly named project binaries.' }
foreach ($file in $files) {
    $relative = $file.GetAttribute('path')
    $source = Join-Path $binaryRoot $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing Release binary: $relative" }
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
}
# upload-artifact adds the ZIP container. Do not put another ZIP inside it.
if ($env:GITHUB_OUTPUT) { "directory=$stage" >> $env:GITHUB_OUTPUT }
# A local sample helps configure SignPath; CI must upload the staged directory.
$sample = "$stage.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $sample)
Write-Host "Staged binaries: $stage"
Write-Host "SignPath configuration sample: $sample"
