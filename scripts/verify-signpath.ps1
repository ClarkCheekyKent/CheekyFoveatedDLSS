[CmdletBinding(DefaultParameterSetName = 'Binaries')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Binaries')][string]$BinaryRoot,
    [Parameter(Mandatory, ParameterSetName = 'Installer')][string]$InstallerPath,
    [string]$ExpectedThumbprint
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ($PSCmdlet.ParameterSetName -eq 'Binaries') {
    [xml]$configuration = Get-Content -LiteralPath (Join-Path $projectRoot '.github/signpath/binaries.xml') -Raw
    $namespaces = New-Object System.Xml.XmlNamespaceManager($configuration.NameTable)
    $namespaces.AddNamespace('sp', 'http://signpath.io/artifact-configuration/v1')
    $paths = @($configuration.SelectNodes('/sp:artifact-configuration/sp:zip-file/sp:pe-file', $namespaces) |
        ForEach-Object { Join-Path $BinaryRoot $_.GetAttribute('path') })
    if ($paths.Count -ne 7) { throw 'Expected seven explicitly named signed binaries.' }
} else {
    $paths = @($InstallerPath)
}
foreach ($path in $paths) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing signed file: $path" }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    # NotTrusted is expected for the self-signed test certificate. Reject missing,
    # broken, unsupported and otherwise unverifiable signatures.
    if (-not $signature.SignerCertificate -or $signature.Status -notin @('Valid', 'NotTrusted')) {
        throw "Signature check failed for ${path}: $($signature.Status) $($signature.StatusMessage)"
    }
    if (-not $ExpectedThumbprint) { $ExpectedThumbprint = $signature.SignerCertificate.Thumbprint }
    if ($signature.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
        throw "Unexpected signing certificate: $path"
    }
    Write-Host "Signature checked: $path ($($signature.Status))"
}
